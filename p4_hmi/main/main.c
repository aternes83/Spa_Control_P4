/*
 * Spa_Control_P4 — ESP32-P4 HMI node.
 *
 * This board owns the touchscreen and, through its ESP32-C6 co-processor, all
 * networking. It owns no relays and no interlocks. It asks the S3 for things and
 * draws what the S3 reports.
 *
 * The screen is laid out after the SpaControl iOS app — same palette, same
 * cards, same temperature dial — on a 480x800 portrait panel. It lives in ui/;
 * the part of it worth testing lives in ui/ui_model.c and is covered by
 * tools/test_ui_model.c on a host.
 *
 * Two rules the whole file exists to keep:
 *
 *   1. A button press is a *request*. It moves a bit in the UI's intent and
 *      nothing else. Whether the load actually runs is learned from the S3's
 *      STATUS, never assumed from the press — the S3 can and will refuse on an
 *      interlock, a fault, or run-timer expiry.
 *   2. When the link is down, everything on screen derived from the S3 is stale.
 *      It is greyed and called out rather than shown as though it were current.
 */
#include "ble_prov.h"
#include "bench_peer.h"
#include "board_pins.h"
#include "bsp/display.h"
#include "bsp/esp-bsp.h"
#include <time.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "mqtt_spa.h"
#include "net_link.h"
#include "spa_state.h"
#include "spalink_codec.h"
#include "spalink_port.h"
#include "ui.h"
#include "ui_theme.h"

static const char *TAG = "spa_hmi";

/* Match the S3's LINK_TIMEOUT_MS: both ends must agree on what "silent" means. */
#define LINK_TIMEOUT_MS 1500
/* Keepalive rate. Four per timeout window, so a single lost frame is harmless. */
#define REQ_TX_MS       400
/* How often the screen is refreshed from state. The S3 reports at 5 Hz, so
 * anything faster only costs LVGL lock contention. */
#define UI_REFRESH_MS   100
#define POLL_MS         20

static spa_state_t   s_state;
static spa_request_t s_req;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* The one thing the UI needs from the board. Passing it as a callback is what
 * keeps ui/ free of the BSP, and so host-compilable. */
static void set_brightness(int percent)
{
    bsp_display_brightness_set(percent);
}

/* Asks the S3 to change the setpoint. Unlike the request bitfield, a setpoint is
 * not restated every 400 ms, so it is the one message that asks for an ACK. */
static void send_setpoint_f(float f)
{
    uint8_t payload[2];
    int16_t dF = (int16_t)(f * 10.0f + (f >= 0 ? 0.5f : -0.5f));
    spalink_pack_i16(dF, payload);
    spalink_port_send(SPALINK_MSG_SETPOINT, payload, sizeof(payload), true);
#if CONFIG_SPA_HMI_BENCH_PEER
    bench_peer_setpoint_dF(dF);
#endif
}

/* A line a second on the console, so the bench bring-up can be read without a
 * camera pointed at the panel. */
static void log_state(const spa_state_t *s)
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
    ESP_LOGI(TAG, "%s water=%d F set=%d F out=0x%02x req=0x%02x %s%s (stale %lu ms)",
             s->link_up ? "[up]  " : "[DOWN]",
             spa_state_water_f(s), spa_state_setpoint_f(s), s->outputs,
             s_req.requests,
             s->fault_active ? "FAULT: " : "",
             s->fault_active ? spa_fault_text(s->fault_code) : "ok",
             (unsigned long)s->stale_ms);
}

/* The carrier has an RTC on the shared I2C bus and the C6 can fetch NTP, and
 * neither is driven yet. Until one is, the status strip shows "--:--" rather
 * than counting up from the epoch, which would be a wrong time rather than no
 * time. Nothing else here needs to change when a source appears. */
static void update_clock(void)
{
    time_t raw = time(NULL);
    struct tm tm;
    localtime_r(&raw, &tm);
    /* Two separate questions, and getting them confused put 20:36 GMT on a panel
     * standing in a 16:36 EDT garden. A valid clock is not the same as a known
     * timezone: the RTC keeps running across a reflash, so the time can be
     * perfectly correct while nothing yet knows how to render it locally. Show
     * nothing until both are true — a wrong time is worse than no time, and an
     * hours-out clock looks exactly like a right one. */
    if (tm.tm_year + 1900 < 2024 || net_link_tz()[0] == '\0') {
        ui_set_clock(-1, 0);
    } else {
        static bool announced;
        if (!announced) {
            announced = true;
            /* Once, when SNTP first lands. A clock that is wrong by a whole
             * timezone looks exactly like a clock that is right, so print what
             * the panel actually believes rather than only that it synced. */
            ESP_LOGI(TAG, "clock set: %04d-%02d-%02d %02d:%02d %s",
                     tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                     tm.tm_hour, tm.tm_min, tzname[tm.tm_isdst > 0 ? 1 : 0]);
        }
        ui_set_clock(tm.tm_hour, tm.tm_min);
    }
}

static void link_task(void *arg)
{
    (void)arg;
    uint32_t next_req = 0;
    uint32_t next_ui = 0;

    for (;;) {
        uint32_t t = now_ms();

        spalink_msg_t m;
        while (spalink_port_recv(&m)) {
            spa_state_apply(&s_state, &m, t);
        }
#if CONFIG_SPA_HMI_BENCH_PEER
        /* Same path, same state model — the UI cannot tell the difference, which
         * is the point of it and also why it must never ship. */
        bench_peer_request(s_req.requests, s_req.modes);
        while (bench_peer_recv(&m, t)) {
            spa_state_apply(&s_state, &m, t);
        }
#endif
        if (spa_state_tick(&s_state, t, LINK_TIMEOUT_MS)) {
            ESP_LOGW(TAG, "link %s", s_state.link_up ? "restored" : "LOST");
        }

        /* The screen is redrawn here rather than on an LVGL timer so that the
         * state it draws from and the requests that go on the wire are taken in
         * the same breath: what is on the panel is exactly what was last asked
         * for, with no window in which the two can disagree. */
        if ((int32_t)(t - next_ui) >= 0) {
            next_ui = t + UI_REFRESH_MS;
            ui_out_t out;
            if (bsp_display_lock(50)) {
                update_clock();
                ui_set_radio(net_link_up(), ble_prov_connected());
                ui_tick(&s_state, t, &out);
                bsp_display_unlock();

                /* A press should not wait out the keepalive slot. Sending on the
                 * change costs one extra frame on an idle wire and takes the
                 * worst case from ~500 ms to ~100 ms, which is the difference
                 * between a button that feels wired and one that feels polled.
                 * It changes nothing about the protocol: REQ still carries the
                 * complete desired state, so an extra one is a restatement. */
                if (out.requests != s_req.requests || out.modes != s_req.modes) {
                    s_req.requests = out.requests;
                    s_req.modes = out.modes;
                    uint8_t payload[2] = { s_req.requests, s_req.modes };
                    spalink_port_send(SPALINK_MSG_REQ, payload, sizeof(payload), false);
                    next_req = t + REQ_TX_MS;
                }
                if (out.send_setpoint) {
                    send_setpoint_f((float)out.setpoint_f);
                }

                /* Outside the display lock: a broker that has stopped reading
                 * must never be able to hold the panel's redraw. The publisher
                 * is a no-op when nothing changed. */
                mqtt_spa_publish(&s_state, &out, t);
            }
        }

        /* Requests are sent continuously, not on edges. The S3 treats silence as
         * "drop everything", so a periodic restatement is also the keepalive —
         * one mechanism, so the two cannot drift apart. */
        if ((int32_t)(t - next_req) >= 0) {
            next_req = t + REQ_TX_MS;
            uint8_t payload[2] = { s_req.requests, s_req.modes };
            spalink_port_send(SPALINK_MSG_REQ, payload, sizeof(payload), false);
        }

        log_state(&s_state);
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

/* board_pins.h records the angle in degrees, because that is a fact about how
 * the module sits in the enclosure rather than about LVGL. */
#if LCD_ROTATE_DEGREES == 0
#define LCD_ROTATION LV_DISPLAY_ROTATION_0
#elif LCD_ROTATE_DEGREES == 90
#define LCD_ROTATION LV_DISPLAY_ROTATION_90
#elif LCD_ROTATE_DEGREES == 180
#define LCD_ROTATION LV_DISPLAY_ROTATION_180
#elif LCD_ROTATE_DEGREES == 270
#define LCD_ROTATION LV_DISPLAY_ROTATION_270
#else
#error "LCD_ROTATE_DEGREES must be 0, 90, 180 or 270"
#endif

static lv_display_t *s_disp;

static void start_display(void)
{
    const bsp_display_cfg_t cfg = {
        .lvgl_port_cfg = {
            .task_priority   = 4,
            .task_stack      = 16384,
            .task_affinity   = -1,
            .task_max_sleep_ms = 500,
            .task_stack_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_DEFAULT,
            .timer_period_ms = 5,
        },
        .buffer_size   = BSP_LCD_DRAW_BUFF_SIZE,
        .double_buffer = BSP_LCD_DRAW_BUFF_DOUBLE,
        .flags = {
            .buff_dma    = false,
            .buff_spiram = false,
            /* The glass is 480x800 portrait and the UI is 800x480 landscape, so
             * the frame has to be turned somewhere. The panel is driven in DPI
             * mode over MIPI-DSI and scans in its native orientation, so there
             * is no controller-side rotation to ask for: LVGL does it on the way
             * to the framebuffer. That costs a copy per redraw, which is why
             * ui.c goes to the trouble of only writing what changed — a screen
             * that redraws on every tick would pay for it ten times a second.
             * This is also why the tear-avoidance modes stay off; the BSP cannot
             * do both. */
            .sw_rotate   = true,
        },
    };
    s_disp = bsp_display_start_with_config(&cfg);
    if (s_disp != NULL) {
        bsp_display_rotate(s_disp, LCD_ROTATION);
    }
    bsp_display_backlight_on();
}

void app_main(void)
{
    ESP_LOGI(TAG, "board: %s  panel %dx%d, UI %dx%d landscape (rotated %d)",
             BOARD_REVISION, LCD_H_RES, LCD_V_RES, UI_SCREEN_W, UI_SCREEN_H,
             LCD_ROTATE_DEGREES);
    /* The layout derives these rather than hard-coding them, so print what the
     * arithmetic actually produced — it is the quickest way to catch a metric
     * change that silently shrank a control. */
    ESP_LOGI(TAG, "layout: rows %d px, dial %d px in a %d box, step keys %d px",
             UI_ROW_H, UI_DIAL_SIZE, UI_DIAL_BOX, UI_STEPPER_H);
#if CONFIG_SPA_HMI_BENCH_PEER
    ESP_LOGW(TAG, "BENCH PEER ENABLED — this panel is answering its own requests.");
    ESP_LOGW(TAG, "Nothing on this screen reflects a real plant. Never fit to a tub.");
#endif

    spa_state_init(&s_state);
    s_req.requests = 0;
    s_req.modes = 0;

    start_display();
    bsp_display_lock(0);
    ui_init(set_brightness);
    bsp_display_unlock();

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
        /* The screen stays up and says so, rather than the board looking dead.
         * spa_state_t has never been connected, so the UI is already drawing
         * "waiting for the control board". */
        ESP_LOGE(TAG, "link init failed — the panel will stay in its no-link state");
        return;
    }

    /* After the link, and at a lower priority than it. The tub comes first: if
     * the radio is slow, absent or misconfigured, the panel still runs the
     * plant, and nothing below this line can block the control path. */
    net_link_start();
    mqtt_spa_start();
    ble_prov_start();

    xTaskCreate(link_task, "spa_link", 4096, NULL, 5, NULL);
}
