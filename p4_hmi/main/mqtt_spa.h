/*
 * The app's MQTT contract, on the wire.
 *
 * Steps 2 and 3 of docs/MQTT.md. That file is the specification and it was
 * written before this code deliberately, because the SpaControl iOS app already
 * speaks this protocol to the v2.0 firmware: the format is not ours to choose,
 * only ours to honour. Read it before changing anything here, particularly the
 * rule that the payload is snake_case (the app decodes with
 * .convertFromSnakeCase, so a camelCase key does not error — it silently
 * decodes as absent and the app shows a stale value).
 *
 * What goes out is what the plant is ACTUALLY doing, taken from spa_state_t,
 * never what the user selected. The only two fields that come from this board
 * are eco and max_jet, because Eco and Max Jets are applied here.
 *
 * What comes in is a request, not a command. It lands on ui_intent_t through
 * ui_post_remote(), exactly where a finger lands, so the S3 keeps its veto and
 * the app finds out what really happened from the next status publish.
 */
#ifndef MQTT_SPA_H
#define MQTT_SPA_H

#include "spa_state.h"
#include "ui.h"

/* Connect and stay connected, retrying on its own. Call once, after the radio
 * has been started; it does not wait for an address. */
void mqtt_spa_start(void);

/* Publish if anything changed, or if the heartbeat is due. Call from the link
 * task on every UI tick — it is cheap when there is nothing to say. */
void mqtt_spa_publish(const spa_state_t *s, const ui_out_t *o, uint32_t now_ms);

/* Connected to the broker and subscribed. For the diagnostics screen. */
bool mqtt_spa_up(void);

#endif /* MQTT_SPA_H */
