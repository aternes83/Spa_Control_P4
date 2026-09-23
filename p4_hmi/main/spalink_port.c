#include "spalink_port.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "spalink";

#define RX_BUF_SIZE   512
#define RX_QUEUE_LEN  16

static QueueHandle_t s_rx_queue;
static int           s_uart = -1;
static uint8_t       s_seq;
static portMUX_TYPE  s_seq_lock = portMUX_INITIALIZER_UNLOCKED;

static spalink_decoder_t s_dec;
static uint32_t s_rx_frames;

/* Raw bytes off the wire, counted before framing. bad_crc only counts frames
 * the decoder managed to delimit, so it reads zero both when nothing arrives
 * and when what arrives is too mangled to start a frame — different faults
 * that look identical without this. */
static uint32_t s_rx_bytes;

/* Edges on the RX pad, counted by a GPIO interrupt rather than by the UART. The
 * GPIO input path stays live while the pin is muxed to a peripheral, so this
 * reports what physically arrives at the pin whatever the UART makes of it.
 *
 * With the two above it separates the three cases that a dead link collapses
 * into: no edges means nothing reaches the pin; edges without bytes means the
 * signal is there and the UART cannot sample it; bytes without frames means it
 * samples fine and the framing is wrong. Bringing this link up cost a day for
 * want of exactly that distinction. */
static volatile uint32_t s_rx_edges;

static void IRAM_ATTR rx_pin_isr(void *arg)
{
    (void)arg;
    s_rx_edges++;
}

static void rx_task(void *arg)
{
    (void)arg;
    uint8_t buf[128];
    spalink_msg_t msg;

    for (;;) {
        int n = uart_read_bytes(s_uart, buf, sizeof(buf), pdMS_TO_TICKS(20));
        if (n > 0) {
            s_rx_bytes += (uint32_t)n;
        }
        for (int i = 0; i < n; i++) {
            if (!spalink_decoder_feed(&s_dec, buf[i], &msg)) {
                continue;
            }
            s_rx_frames++;
            /* Drop the oldest rather than block: the link carries periodic state,
             * so a stale frame is worth less than the one arriving now. */
            if (xQueueSend(s_rx_queue, &msg, 0) != pdTRUE) {
                spalink_msg_t discard;
                xQueueReceive(s_rx_queue, &discard, 0);
                xQueueSend(s_rx_queue, &msg, 0);
            }
        }
    }
}

int spalink_port_init(const spalink_port_cfg_t *cfg)
{
    spalink_decoder_init(&s_dec);
    s_rx_queue = xQueueCreate(RX_QUEUE_LEN, sizeof(spalink_msg_t));
    if (!s_rx_queue) {
        return -1;
    }

    const uart_config_t uc = {
        .baud_rate = cfg->baud,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    int err = uart_driver_install(cfg->uart_num, RX_BUF_SIZE, 0, 0, NULL, 0);
    if (err) return err;
    err = uart_param_config(cfg->uart_num, &uc);
    if (err) return err;
    /* RTS stays unassigned even on RS485: the carrier's SP485E switches its own
     * DE//RE from the TX line through a 74LVC1G132, so there is no direction pin
     * to drive. The vendor's uart_echo_rs485 example does exactly the same. */
    err = uart_set_pin(cfg->uart_num, cfg->tx_gpio, cfg->rx_gpio,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err) return err;

    /* Count edges on the RX pad. Attaching a GPIO interrupt to a pin the UART
     * owns is fine: the mux hands the pad to the peripheral, but the GPIO input
     * path still sees it. ESP_ERR_INVALID_STATE only means something else
     * installed the ISR service first. */
    gpio_set_intr_type(cfg->rx_gpio, GPIO_INTR_ANYEDGE);
    int isr_err = gpio_install_isr_service(0);
    if (isr_err != 0 && isr_err != ESP_ERR_INVALID_STATE) {
        return isr_err;
    }
    gpio_isr_handler_add(cfg->rx_gpio, rx_pin_isr, NULL);
    gpio_intr_enable(cfg->rx_gpio);

    if (cfg->rs485) {
#ifdef SPALINK_UART_RS485_MODE
        err = uart_set_mode(cfg->uart_num, UART_MODE_RS485_HALF_DUPLEX);
        if (err) return err;
#else
        /* Not switching the UART into RS-485 half-duplex mode, although the
         * transceiver is one. Define SPALINK_UART_RS485_MODE to put it back.
         *
         * Its only job here was self-echo suppression, and that is redundant:
         * the carrier drives DE//RE from the TX line through U7/U9 (sheet 5),
         * so the receiver is off while the driver is on, in hardware. The
         * diagnostics screen agrees — Self echo has never left 0.
         *
         * Worth being honest about why this changed. It was tried as a fix for
         * a receive path that had never decoded a frame, on the theory that
         * IDF's half-duplex mode was gating the receiver. It was not: the fault
         * was a short from A to VCC on the far node, and plain mode made no
         * difference. It is kept because the simpler configuration is the
         * better default, not because it fixed anything. */
#endif
    }

    s_uart = cfg->uart_num;
    if (xTaskCreate(rx_task, "spalink_rx", 3072, NULL, 10, NULL) != pdPASS) {
        return -1;
    }
    ESP_LOGI(TAG, "link up on uart%d tx=%d rx=%d @%d (%s)",
             cfg->uart_num, cfg->tx_gpio, cfg->rx_gpio, cfg->baud,
             cfg->rs485 ?
#ifdef SPALINK_UART_RS485_MODE
                 "RS485, uart in half-duplex mode"
#else
                 "RS485, uart in plain mode"
#endif
                 : "TTL");
    return 0;
}

uint8_t spalink_port_send(uint8_t msg_id, const uint8_t *payload, size_t len,
                          bool want_ack)
{
    taskENTER_CRITICAL(&s_seq_lock);
    s_seq = (uint8_t)((s_seq + 1) & SPALINK_SEQ_MASK);
    uint8_t seq = s_seq;
    taskEXIT_CRITICAL(&s_seq_lock);

    uint8_t hdr = seq | (want_ack ? SPALINK_FLAG_ACK : 0);
    uint8_t frame[SPALINK_MAX_FRAME];
    size_t n = spalink_encode_uart(msg_id, hdr, payload, len, frame);
    if (n) {
        uart_write_bytes(s_uart, (const char *)frame, n);
    }
    return seq;
}

bool spalink_port_recv(spalink_msg_t *out)
{
    if (!s_rx_queue) {
        return false;
    }
    return xQueueReceive(s_rx_queue, out, 0) == pdTRUE;
}

uint32_t spalink_port_rx_bytes(void)
{
    return s_rx_bytes;
}

uint32_t spalink_port_rx_edges(void)
{
    return s_rx_edges;
}

void spalink_port_stats(uint32_t *bad_crc, uint32_t *bad_len, uint32_t *rx_frames)
{
    if (bad_crc)   *bad_crc = s_dec.bad_crc;
    if (bad_len)   *bad_len = s_dec.bad_len;
    if (rx_frames) *rx_frames = s_rx_frames;
}
