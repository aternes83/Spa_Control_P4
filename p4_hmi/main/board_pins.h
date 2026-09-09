/*
 * BOARD PIN MAP — ESP32-P4-WIFI6-Touch-LCD-4.3 (HMI node).
 *
 * The same rule as the S3 side: every GPIO this firmware touches is named here
 * and nowhere else.
 *
 * UNVERIFIED — these are header pins chosen from Waveshare's published 40-pin
 * list, not from the schematic. Check them against the board before wiring
 * anything, and correct this file rather than the call sites. Confirmed so far:
 *   - display  : MIPI-DSI, 2 lanes, 480x800 portrait (driven by the BSP, not here)
 *   - touch    : I2C on GPIO7 (SDA) / GPIO8 (SCL)
 *   - C6 radio : SDIO to the on-board ESP32-C6-MINI; not ours to configure
 */
#ifndef BOARD_PINS_H
#define BOARD_PINS_H

#define BOARD_REVISION "spa-p4-hmi-r1"

/* SpaLink to the S3 control node. UART1 — UART0 is the console. */
#define LINK_UART_NUM  1
#define LINK_UART_TX   32     /* VERIFY against the schematic */
#define LINK_UART_RX   33     /* VERIFY against the schematic */
#define LINK_BAUD      115200

/* Reserved for the TWAI/CAN transport (needs a transceiver — docs/HARDWARE.md). */
#define LINK_CAN_TX    34     /* VERIFY against the schematic */
#define LINK_CAN_RX    35     /* VERIFY against the schematic */
#define LINK_CAN_BITRATE 500000

/* Touch I2C, per the vendor documentation. */
#define TOUCH_I2C_SDA  7
#define TOUCH_I2C_SCL  8

#endif /* BOARD_PINS_H */
