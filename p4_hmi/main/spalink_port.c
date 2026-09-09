#include "spalink_port.h"

#include <string.h>

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

static void rx_task(void *arg)
{
    (void)arg;
    uint8_t buf[128];
    spalink_msg_t msg;

    for (;;) {
        int n = uart_read_bytes(s_uart, buf, sizeof(buf), pdMS_TO_TICKS(20));
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
    err = uart_set_pin(cfg->uart_num, cfg->tx_gpio, cfg->rx_gpio,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err) return err;

    s_uart = cfg->uart_num;
    if (xTaskCreate(rx_task, "spalink_rx", 3072, NULL, 10, NULL) != pdPASS) {
        return -1;
    }
    ESP_LOGI(TAG, "link up on uart%d tx=%d rx=%d @%d",
             cfg->uart_num, cfg->tx_gpio, cfg->rx_gpio, cfg->baud);
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

void spalink_port_stats(uint32_t *bad_crc, uint32_t *bad_len, uint32_t *rx_frames)
{
    if (bad_crc)   *bad_crc = s_dec.bad_crc;
    if (bad_len)   *bad_len = s_dec.bad_len;
    if (rx_frames) *rx_frames = s_rx_frames;
}
