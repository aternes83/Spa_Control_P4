/*
 * The HMI's model of the spa: what the S3 last told us, and how fresh it is.
 *
 * Deliberately free of ESP-IDF and LVGL so it compiles and is tested on a host
 * (tools/test_spa_state.c). The UI reads this struct; nothing else does.
 *
 * The important field is `link_up`. Everything shown on the screen is a claim
 * about a machine at the other end of a wire, and when that wire goes quiet the
 * claims are stale. The UI must render stale state as stale — greyed, with the
 * connection called out — instead of confidently showing a heater that may well
 * have shut off minutes ago.
 *
 * `link_up` means "the S3 spoke recently", not "a frame arrived recently". The
 * two are not the same on a half-duplex bus, where this panel can hear itself;
 * see spa_state_apply() and `self_echo`.
 */
#ifndef SPA_STATE_H
#define SPA_STATE_H

#include <stdbool.h>
#include <stdint.h>

#include "spalink_codec.h"

/* Matches spa_core.FAULT_* on the S3 and the fault text on screen. */
typedef enum {
    SPA_FAULT_NONE = 0,
    SPA_FAULT_NO_FLOW = 1,
    SPA_FAULT_HIGH_LIMIT = 2,
    SPA_FAULT_OVERTEMP = 3,
    SPA_FAULT_ESTOP = 4,
    SPA_FAULT_TEMP_SENSOR = 5,
} spa_fault_t;

typedef struct {
    /* Plant state, as last reported. */
    uint8_t  outputs;          /* SPALINK_OUT_* bits */
    uint8_t  safety_inputs;    /* SPALINK_IN_* bits */
    /* Loads the S3 is holding off because their own runtime ceiling expired
     * rather than because an interlock said no — SPALINK_OUT_* bits again. A
     * controller that predates STATUS byte 3 leaves this 0, which reads as
     * "nothing timed out" and keeps the old behaviour. */
    uint8_t  timed_out;
    bool     fault_active;
    uint8_t  fault_code;
    int16_t  water_dF;         /* deci-Fahrenheit */
    int16_t  setpoint_dF;
    uint16_t spa_remain_s;
    uint16_t light_remain_s;

    /* Peer identity, from MSG_HELLO. */
    uint8_t  peer_proto;
    uint8_t  peer_fw_major;
    uint8_t  peer_fw_minor;

    /* True once a TEMP frame has arrived. `ever_connected` is not enough on its
     * own: an ACK or a HELLO also proves the link is alive, and drawing
     * water_dF before any TEMP has landed would put a confident 0 °F on the
     * dial. The UI shows "--" until this is set. */
    bool     have_temp;

    /* Link health. */
    bool     link_up;
    bool     ever_connected;   /* false until the first frame since boot */
    uint32_t last_rx_ms;
    uint32_t rx_frames;        /* every CRC-valid frame, echoes included */
    /* Frames carrying an id this panel itself sends — see spa_state_apply().
     * Nonzero means the wire is looping back: a transceiver whose driver-enable
     * is stuck on, an auto-direction circuit releasing late, or a bench
     * loopback. If this tracks rx_frames, the S3 is not in the conversation. */
    uint32_t self_echo;
    uint32_t stale_ms;         /* how long the data on screen has been stale */
} spa_state_t;

/* What the UI is allowed to send. Mirrors SPALINK_REQ_* / SPALINK_MODE_*. */
typedef struct {
    uint8_t requests;
    uint8_t modes;
} spa_request_t;

void spa_state_init(spa_state_t *s);

/* Fold one received message into the state. now_ms is a monotonic millisecond
 * clock. Returns true if anything the UI draws actually changed, so the caller
 * can skip a redraw. */
bool spa_state_apply(spa_state_t *s, const spalink_msg_t *m, uint32_t now_ms);

/* Re-evaluate link health. Call every UI tick, not only on receive — that is
 * what notices silence. Returns true if link_up changed. */
bool spa_state_tick(spa_state_t *s, uint32_t now_ms, uint32_t timeout_ms);

const char *spa_fault_text(uint8_t code);

/* Temperature for display, in whole °F, rounded half away from zero. */
int spa_state_water_f(const spa_state_t *s);
int spa_state_setpoint_f(const spa_state_t *s);

#endif /* SPA_STATE_H */
