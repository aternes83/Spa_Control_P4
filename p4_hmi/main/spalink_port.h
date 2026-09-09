/*
 * SpaLink transport for the P4, on ESP-IDF.
 *
 * Everything ESP-IDF-specific about the link lives here: UART setup, the receive
 * task, and (later) the TWAI variant. The codec and the state model above it
 * stay portable, which is why those two are the parts covered by host tests.
 *
 * Swapping transports is a build-time choice: define SPALINK_USE_TWAI. The
 * message set, sequence numbers and ACK handling do not change.
 */
#ifndef SPALINK_PORT_H
#define SPALINK_PORT_H

#include <stdbool.h>
#include <stdint.h>

#include "spalink_codec.h"

typedef struct {
    int uart_num;
    int tx_gpio;
    int rx_gpio;
    int baud;
} spalink_port_cfg_t;

/* Starts the receive task. Returns ESP_OK-style 0 on success. */
int  spalink_port_init(const spalink_port_cfg_t *cfg);

/* Queue one message. Sequence numbers are managed here; pass want_ack to set the
 * ACK flag. Returns the sequence number used. */
uint8_t spalink_port_send(uint8_t msg_id, const uint8_t *payload, size_t len,
                          bool want_ack);

/* Pop one received message. Returns false if none is waiting. Non-blocking, so
 * it is safe to call from the LVGL tick. */
bool spalink_port_recv(spalink_msg_t *out);

/* Framing errors seen since boot — worth a line on the diagnostics screen, since
 * a rising count is the first sign of a marginal cable or the wrong baud. */
void spalink_port_stats(uint32_t *bad_crc, uint32_t *bad_len, uint32_t *rx_frames);

#endif /* SPALINK_PORT_H */
