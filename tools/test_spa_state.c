/*
 * HMI state-model tests. Host-compiled, no ESP-IDF:
 *
 *     cc -std=c11 -I link -o /tmp/t tools/test_spa_state.c \
 *        p4_hmi/main/spa_state.c link/spalink_codec.c && /tmp/t
 *
 * (tools/run_tests.sh does this for you.)
 *
 * The staleness checks are the ones that matter: a screen that keeps showing a
 * confident temperature after the control board has stopped talking is worse
 * than one that admits it does not know.
 */
#include "../p4_hmi/main/spa_state.h"

#include <stdio.h>
#include <string.h>

static int failed = 0;

static void check(const char *name, bool cond, long detail)
{
    if (cond) {
        printf("  ok   %s\n", name);
    } else {
        printf("  FAIL %s (%ld)\n", name, detail);
        failed++;
    }
}

static spalink_msg_t msg(uint8_t id, const uint8_t *p, uint8_t n)
{
    spalink_msg_t m;
    memset(&m, 0, sizeof(m));
    m.msg_id = id;
    m.hdr = 0;
    m.len = n;
    if (n) memcpy(m.payload, p, n);
    return m;
}

int main(void)
{
    spa_state_t s;

    printf("initial state\n");
    spa_state_init(&s);
    check("link starts down", !s.link_up, s.link_up);
    check("nothing claimed as connected yet", !s.ever_connected, s.ever_connected);
    check("tick before first contact does not fake a connection",
          !spa_state_tick(&s, 10000, 1500) && !s.link_up, s.link_up);

    printf("status decoding\n");
    uint8_t st[3] = { SPALINK_OUT_HEATER | SPALINK_OUT_PUMP1_LOW,
                      SPALINK_IN_FLOW_SWITCH | SPALINK_IN_HIGH_LIMIT_OK,
                      SPALINK_FAULT_ACTIVE | SPA_FAULT_NO_FLOW };
    spalink_msg_t m = msg(SPALINK_MSG_STATUS, st, 3);
    check("first status brings the link up", spa_state_apply(&s, &m, 1000), 0);
    check("heater bit decoded", (s.outputs & SPALINK_OUT_HEATER) != 0, s.outputs);
    check("fault decoded as NO FLOW",
          s.fault_active && s.fault_code == SPA_FAULT_NO_FLOW, s.fault_code);
    /* A live link is not the same as a known temperature. Until a TEMP frame has
     * landed there is no reading to draw, and water_dF is still its zeroed
     * initial value — which is 0 F, a number somebody would believe. */
    check("a status frame alone claims no temperature", !s.have_temp, s.have_temp);
    check("fault text matches the code",
          strcmp(spa_fault_text(s.fault_code), "NO FLOW") == 0, 0);
    check("identical status is not a redraw", !spa_state_apply(&s, &m, 1100), 0);
    check("a three-byte status reports nothing timed out", s.timed_out == 0,
          s.timed_out);

    /* Byte 3 is optional, so both lengths have to decode. A controller that
     * predates it must keep meaning "nothing timed out" rather than leaving a
     * stale mask behind, or the panel would go on releasing requests. */
    uint8_t st4[4] = { st[0], st[1], st[2], SPALINK_OUT_PUMP2 | SPALINK_OUT_LIGHT };
    m = msg(SPALINK_MSG_STATUS, st4, 4);
    check("byte 3 is a redraw when it changes", spa_state_apply(&s, &m, 1150), 0);
    check("and names the timed-out loads",
          s.timed_out == (SPALINK_OUT_PUMP2 | SPALINK_OUT_LIGHT), s.timed_out);
    check("an unchanged byte 3 is not a redraw", !spa_state_apply(&s, &m, 1160), 0);
    m = msg(SPALINK_MSG_STATUS, st, 3);
    check("dropping back to three bytes clears it",
          spa_state_apply(&s, &m, 1170) && s.timed_out == 0, s.timed_out);

    printf("temperature\n");
    uint8_t tp[4];
    spalink_pack_i16(1014, &tp[0]);      /* 101.4 F */
    spalink_pack_i16(1000, &tp[2]);      /* 100.0 F */
    m = msg(SPALINK_MSG_TEMP, tp, 4);
    spa_state_apply(&s, &m, 1200);
    check("water temp rounds to 101 F", spa_state_water_f(&s) == 101, spa_state_water_f(&s));
    check("setpoint rounds to 100 F", spa_state_setpoint_f(&s) == 100, spa_state_setpoint_f(&s));
    check("and now there is a temperature to draw", s.have_temp, 0);

    /* A 0.4 F drift must not force a redraw; crossing the half degree must. */
    spalink_pack_i16(1010, &tp[0]);
    m = msg(SPALINK_MSG_TEMP, tp, 4);
    check("sub-degree drift is not a redraw", !spa_state_apply(&s, &m, 1300), 0);
    spalink_pack_i16(1005, &tp[0]);      /* 100.5 still displays as 101 */
    m = msg(SPALINK_MSG_TEMP, tp, 4);
    check("the rounding boundary itself is not a redraw", !spa_state_apply(&s, &m, 1400), 0);
    spalink_pack_i16(1004, &tp[0]);      /* 100.4 displays as 100 */
    m = msg(SPALINK_MSG_TEMP, tp, 4);
    check("crossing the displayed degree is a redraw", spa_state_apply(&s, &m, 1450), 0);

    /* Negative temperatures must round away from zero, not toward it: a frozen
     * -0.5 F must not display as 0. */
    spalink_pack_i16(-5, &tp[0]);
    m = msg(SPALINK_MSG_TEMP, tp, 4);
    spa_state_apply(&s, &m, 1500);
    check("negative temperature rounds away from zero",
          spa_state_water_f(&s) == -1, spa_state_water_f(&s));

    printf("short frames\n");
    spa_state_t before = s;
    uint8_t junk[2] = { 0xFF, 0xFF };
    m = msg(SPALINK_MSG_STATUS, junk, 2);
    spa_state_apply(&s, &m, 1600);
    check("a truncated STATUS leaves the last good outputs alone",
          s.outputs == before.outputs, s.outputs);
    m = msg(SPALINK_MSG_TEMP, junk, 2);
    spa_state_apply(&s, &m, 1700);
    check("a truncated TEMP leaves the last good reading alone",
          s.water_dF == before.water_dF, s.water_dF);
    check("a truncated frame still counts as liveness", s.link_up, 0);

    printf("staleness\n");
    check("no change while inside the timeout",
          !spa_state_tick(&s, 1700 + 1499, 1500) && s.link_up, 0);
    check("link drops once past the timeout",
          spa_state_tick(&s, 1700 + 1501, 1500) && !s.link_up, 0);
    check("stale_ms reports how old the screen is", s.stale_ms == 1501, s.stale_ms);
    check("the last known temperature is retained for the UI to grey out",
          s.water_dF == -5, s.water_dF);
    uint8_t ping[1] = { 0 };
    m = msg(SPALINK_MSG_ACK, ping, 1);
    check("any frame brings the link back", spa_state_apply(&s, &m, 4000) && s.link_up, 0);
    check("stale_ms resets on reconnect", s.stale_ms == 0, s.stale_ms);

    /* The panel shares one half-duplex pair with the S3, so it can hear its own
     * transmissions. An echoed frame is CRC-valid and decodes perfectly —
     * nothing below this layer can tell it apart. The one thing that gives it
     * away is the id: REQ, SETPOINT, PING and CLEAR_FAULT are only ever sent by
     * this panel, so hearing one is proof of a loopback, not of a peer. */
    printf("self-echo\n");
    spa_state_init(&s);
    uint8_t req[2] = { SPALINK_REQ_JETS, 0 };
    m = msg(SPALINK_MSG_REQ, req, 2);
    check("our own REQ does not bring the link up",
          !spa_state_apply(&s, &m, 1000) && !s.link_up, s.link_up);
    check("...nor claim the panel has ever been connected",
          !s.ever_connected, s.ever_connected);
    check("...but it is counted, so the screen can say why",
          s.self_echo == 1 && s.rx_frames == 1, (long)s.self_echo);

    m = msg(SPALINK_MSG_SETPOINT, req, 2);
    spa_state_apply(&s, &m, 1100);
    m = msg(SPALINK_MSG_PING, req, 0);
    spa_state_apply(&s, &m, 1200);
    m = msg(SPALINK_MSG_CLEAR_FAULT, req, 0);
    spa_state_apply(&s, &m, 1300);
    check("every id this panel sends is treated the same way",
          s.self_echo == 4 && !s.link_up, (long)s.self_echo);
    check("a tick over an echo-only link still reports no connection",
          !spa_state_tick(&s, 9000, 1500) && !s.link_up, s.link_up);

    /* The S3 speaking must still work through the same loopback. */
    m = msg(SPALINK_MSG_STATUS, st, 3);
    check("a real STATUS on the same wire still connects",
          spa_state_apply(&s, &m, 1400) && s.link_up && s.ever_connected, 0);
    m = msg(SPALINK_MSG_REQ, req, 2);
    spa_state_apply(&s, &m, 1500);
    check("an echo afterwards does not refresh the liveness clock",
          spa_state_tick(&s, 1400 + 1501, 1500) && !s.link_up, s.stale_ms);
    check("echoes are counted while the link is up too",
          s.self_echo == 5, (long)s.self_echo);

    /* Unknown ids are deliberately not treated as echo: they are not this
     * panel's voice, and a message a future S3 adds must still look like a live
     * peer to an older panel. */
    spa_state_init(&s);
    m = msg(0x7F, req, 2);
    check("an id outside the protocol still counts as liveness",
          spa_state_apply(&s, &m, 1000) && s.link_up, s.link_up);
    check("...and is not mistaken for an echo", s.self_echo == 0, (long)s.self_echo);

    /* The clock wraps every ~49 days; the tub runs longer than that. */
    printf("clock wrap\n");
    spa_state_init(&s);
    m = msg(SPALINK_MSG_ACK, ping, 1);
    spa_state_apply(&s, &m, 0xFFFFFF00u);
    /* 0xFFFFFF00 -> 0x00000100 is 512 ms of real time, not 4.2 billion. */
    check("no false timeout across a uint32 ms wrap",
          !spa_state_tick(&s, 0x00000100u, 1500) && s.link_up, s.stale_ms);
    check("elapsed time is correct across the wrap", s.stale_ms == 512u, s.stale_ms);
    /* ...and a genuine gap spanning the wrap must still time out. */
    check("a real timeout spanning the wrap is still caught",
          spa_state_tick(&s, 0x00000800u, 1500) && !s.link_up, s.stale_ms);

    printf("\n");
    if (failed) {
        printf("FAILED: %d\n", failed);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
