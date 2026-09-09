/*
 * Spa_Control_P4 — ESP32-P4 HMI node.
 *
 * This board owns the touchscreen and, through its ESP32-C6 co-processor, all
 * networking. It owns no relays and no interlocks. It asks the S3 for things
 * and draws what the S3 reports.
 *
 * Status: the link half is implemented and its logic is covered by host tests
 * (tools/test_spa_state.c). The UI half is a stub — see ui_render_stub(). It is
 * left deliberately empty rather than filled with a guessed layout, because the
 * panel is 480x800 portrait where the old HMI was 480x320 landscape, so the
 * screen design is a fresh decision rather than a port.
 */
#include <stdio.h>

#include "board_pins.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "spa_state.h"
#include "spalink_codec.h"
#include "spalink_port.h"

static const char *TAG = "spa_hmi";

/* Match the S3's LINK_TIMEOUT_MS: both ends must agree on what "silent" means. */
#define LINK_TIMEOUT_MS 1500
/* Keepalive rate. Four per timeout window, so a single lost frame is harmless. */
#define REQ_TX_MS       400
#define UI_TICK_MS      20

static spa_state_t   s_state;
static spa_request_t s_req;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* ── UI ───────────────────────────────────────────────────────────────────────
 * TODO: LVGL screen for the 480x800 panel. Bring it up on the Waveshare BSP
 * (esp_lcd MIPI-DSI + the touch controller on I2C), then render from s_state
 * only — the UI must never hold plant state of its own, or the screen and the
 * controller will disagree the moment a frame is lost.
 *
 * Two rules the layout has to honour:
 *   1. When !s_state.link_up, everything derived from the S3 is stale. Grey it
 *      and say so. Never show a confident temperature the controller has not
 *      confirmed for seconds.
 *   2. A button press is a *request*, not a state change. Set the bit in s_req
 *      and wait for the S3's STATUS to report the load actually energised. The
 *      S3 can and will refuse (interlocks, faults, run-timer expiry).
 */
static void ui_render_stub(const spa_state_t *s)
{
    static uint32_t last_log;
    uint32_t t = now_ms();
    if (t - last_log < 1000) {
        return;
    }
    last_log = t;

    if (!s->ever_connected) {
        ESP_LOGI(TAG, "waiting for the control board...");
        return;
    }
    ESP_LOGI(TAG, "%s water=%d F set=%d F out=0x%02x %s%s (stale %lu ms)",
             s->link_up ? "[up]  " : "[DOWN]",
             spa_state_water_f(s), spa_state_setpoint_f(s), s->outputs,
             s->fault_active ? "FAULT: " : "",
             s->fault_active ? spa_fault_text(s->fault_code) : "ok",
             (unsigned long)s->stale_ms);
}

static void link_task(void *arg)
{
    (void)arg;
    uint32_t next_req = 0;

    for (;;) {
        uint32_t t = now_ms();

        spalink_msg_t m;
        while (spalink_port_recv(&m)) {
            spa_state_apply(&s_state, &m, t);
        }
        if (spa_state_tick(&s_state, t, LINK_TIMEOUT_MS)) {
            ESP_LOGW(TAG, "link %s", s_state.link_up ? "restored" : "LOST");
        }

        /* Requests are sent continuously, not on edges. The S3 treats silence as
         * "drop everything", so a periodic restatement is also the keepalive —
         * one mechanism, so the two cannot drift apart. */
        if ((int32_t)(t - next_req) >= 0) {
            next_req = t + REQ_TX_MS;
            uint8_t payload[2] = { s_req.requests, s_req.modes };
            spalink_port_send(SPALINK_MSG_REQ, payload, sizeof(payload), false);
        }

        ui_render_stub(&s_state);
        vTaskDelay(pdMS_TO_TICKS(UI_TICK_MS));
    }
}

/* Called by the UI when the user changes the setpoint. Asks for an ACK: unlike
 * the request bitfield, a setpoint is not restated every 400 ms, so it needs to
 * know it landed. */
void spa_hmi_set_setpoint_f(float f)
{
    uint8_t payload[2];
    spalink_pack_i16((int16_t)(f * 10.0f + (f >= 0 ? 0.5f : -0.5f)), payload);
    spalink_port_send(SPALINK_MSG_SETPOINT, payload, sizeof(payload), true);
}

void app_main(void)
{
    ESP_LOGI(TAG, "board: %s", BOARD_REVISION);

    spa_state_init(&s_state);
    s_req.requests = 0;
    s_req.modes = 0;

    const spalink_port_cfg_t cfg = {
        .uart_num = LINK_UART_NUM,
#ifdef SPALINK_BENCH_TTL
        .tx_gpio  = LINK_BENCH_UART_TX,
        .rx_gpio  = LINK_BENCH_UART_RX,
        .baud     = LINK_BAUD,
        .rs485    = false,
#else
        .tx_gpio  = LINK_UART_TX,      /* through the on-board SP485E */
        .rx_gpio  = LINK_UART_RX,
        .baud     = LINK_BAUD,
        .rs485    = true,
#endif
    };
    if (spalink_port_init(&cfg) != 0) {
        ESP_LOGE(TAG, "link init failed");
        return;
    }

    xTaskCreate(link_task, "spa_link", 4096, NULL, 5, NULL);
}
