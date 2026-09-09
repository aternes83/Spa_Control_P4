/*
 * BOARD PIN MAP — HMI node.
 *
 * Module:  Guition JC-ESP32P4-M3  (confirmed from the board silkscreen)
 *          ESP32-P4 + ESP32-C6-MINI, 32 MB PSRAM, 16 MB flash.
 * Carrier: 4.3" 480x800 IPS display board, ST7701S over MIPI-DSI, GT911 touch.
 *          Guition's JC4880P443C_I_W / JC4880P433 family — NOT yet confirmed.
 *
 * Same rule as the S3 side: every GPIO this firmware touches is named here and
 * nowhere else.
 *
 * The split below matters. The MODULE pins are fixed by the JC-ESP32P4-M3 itself
 * and hold whatever it is plugged into. The CARRIER pins belong to the 4.3"
 * display board, and Guition ships several carriers for this module that are not
 * pin-compatible — the JC-ESP32P4-M3-DEV board, for instance, puts an Ethernet
 * PHY on GPIO31/50/51/52, which are free header pins on the display carrier.
 * So treat everything under CARRIER as provisional until checked.
 *
 * Carrier data comes from the schematic sheets and pin table published at
 * github.com/ultramcu/guition-jc4880p443c-i-w (community-maintained, derived
 * from vendor schematics). The supplier's own pan.jczn1688.com download is a JS
 * app behind a 403 and could not be read directly.
 */
#ifndef BOARD_PINS_H
#define BOARD_PINS_H

#define BOARD_REVISION "spa-p4-hmi-jc4880p443c-r1"

/* ── SpaLink to the S3 control node ──────────────────────────────────────────
 *
 * Two transports, both plain async UART — which is why the protocol does not
 * change between them (docs/PROTOCOL.md).
 *
 * BENCH: any free JP1 pins, single-ended, short wires.
 * INSTALL: UART1 at GPIO26/27 is wired to an on-board MAX485 on carriers that
 * have one — see the visual check in docs/HARDWARE.md. Its DE/RE is
 * driven from the TX line itself through a 74LVC1G132 + transistor, i.e.
 * automatic direction control — no GPIO, and no turnaround code on this side.
 * That gives a differential, noise-immune pair for the run past the pump
 * contactors, with no parts to add to this board.
 *
 * Set SPALINK_USE_RS485 for the install wiring.
 */
#ifdef SPALINK_USE_RS485
#define LINK_UART_NUM  1
#define LINK_UART_TX   26     /* -> MAX485 DI (also drives DE/RE automatically) */
#define LINK_UART_RX   27     /* <- MAX485 RO */
#else
#define LINK_UART_NUM  1
#define LINK_UART_TX   32     /* JP1 pin 17 — bench bring-up only */
#define LINK_UART_RX   33     /* JP1 pin 4  — bench bring-up only */
#endif
#define LINK_BAUD      115200

/* ── MODULE pins — fixed by the JC-ESP32P4-M3, confirmed ────────────────────
 * ESP32-C6 radio over SDIO: CLK 18, CMD 19, D0-D3 on 14/15/16/17.
 * C6 reset: GPIO54.
 * Not touched by this firmware — esp_hosted owns them — but recorded so no
 * later pin assignment can collide with the radio. */
#define C6_SDIO_CLK   18
#define C6_SDIO_CMD   19
#define C6_SDIO_D0    14
#define C6_SDIO_D1    15
#define C6_SDIO_D2    16
#define C6_SDIO_D3    17
#define C6_RESET_GPIO 54

/* ── CARRIER pins — 4.3" display board, VERIFY before wiring ────────────────
 * Owned by the BSP, listed so nothing double-books them. */
#define LCD_RESET_GPIO      5    /* active-low, 20-120 ms pulse */
#define LCD_BACKLIGHT_GPIO  23   /* MP3202 driver enable; plain GPIO, active high */
#define TOUCH_I2C_SDA       7    /* GT911 @ 0x5D, I2C shared with the ES8311 codec */
#define TOUCH_I2C_SCL       8
#define TOUCH_RESET_GPIO    3    /* some community docs cite GPIO22 — verify */

/* Also on the carrier and not used here:
 *   microSD (SDMMC) : CLK 43, CMD 44, D0-D3 on 39/40/41/42
 *   ES8311 audio    : MCLK 13, BCLK 12, LRCK 10, DOUT 9, DIN 48; amp enable 11
 *   console UART0   : TX 37, RX 38
 *   boot button     : GPIO35  (also listed on JP1 — check before using it) */

/* ── Free on the Expand-IO header (JP1) ──────────────────────────────────────
 * GPIO28, 29, 30, 31, 32, 33, 34, 35(?), 49, 50, 51, 52, plus 3V3, 5V and GND.
 * JP1 also carries the C6's UART0 and boot/reset pins for reflashing the radio
 * co-processor: C6_U0RXD, C6_U0TXD, C6_IO9, C6_CHIP_PU. */

#endif /* BOARD_PINS_H */
