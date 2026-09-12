/*
 * BOARD PIN MAP — HMI node.
 *
 * Module:  Guition JC-ESP32P4-M3 (ESP32-P4 + ESP32-C6-MINI, 32 MB PSRAM, 16 MB flash)
 * Carrier: Guition JC4880P443C_I_W — 4.3" 480x800 IPS, ST7701S over MIPI-DSI,
 *          GT911 capacitive touch, SP485E RS-485, ES8311 codec, microSD.
 *
 * VERIFIED against the vendor documentation package (schematic sheets
 * JC4880P443_V1.0, the esp32_p4_function_ev_board BSP and the uart_echo_rs485
 * example shipped in 1-Demo/idf_examples). The source is named per group below.
 * Nothing here is inferred from a product listing any more.
 *
 * Same rule as the S3 side: every GPIO this firmware touches is named here and
 * nowhere else.
 */
#ifndef BOARD_PINS_H
#define BOARD_PINS_H

#define BOARD_REVISION "spa-p4-hmi-jc4880p443c-r1"

/* ── SpaLink to the S3 control node ──────────────────────────────────────────
 * Schematic sheet 3 (module): GPIO26 = TX1, GPIO27 = RX1.
 * Schematic sheet 5 (485): those nets reach an SP485E — the MAX485-compatible
 * transceiver at U8 — whose DE and /RE are tied together and driven from the TX
 * line through a 74LVC1G132 (U7) and a transistor (U9). Direction switching is
 * therefore automatic: no GPIO, no turnaround code, and the receiver is disabled
 * while transmitting so there is no self-echo to filter.
 *
 * The vendor's own uart_echo_rs485 example agrees exactly: TXD 26, RXD 27, RTS
 * UART_PIN_NO_CHANGE, and uart_set_mode(UART_MODE_RS485_HALF_DUPLEX).
 *
 * The bus appears on J4, a 4-pin connector carrying two A/B pairs so nodes can
 * be daisy-chained, with SMAJ6.5CA TVS clamps and 0.1 A resettable fuses.
 *
 * The SP485E is a 5 V part, so its RO output swings 0-5 V into GPIO27; the
 * carrier limits that with R70, a 1 k series resistor into the P4's clamp diode.
 * Fit a 3.3 V transceiver at the S3 end rather than copying that trick.
 */
#define LINK_UART_NUM  1
#define LINK_UART_TX   26     /* -> SP485E DI, and drives DE//RE automatically */
#define LINK_UART_RX   27     /* <- SP485E RO, through R70 1k */
#define LINK_UART_RTS  UART_PIN_NO_CHANGE   /* direction is done in hardware */
#define LINK_BAUD      115200

/* Bench alternative: any free JP1 pin, TTL, no transceiver at either end. */
#define LINK_BENCH_UART_TX 32
#define LINK_BENCH_UART_RX 33

/* ── Display — ST7701S, MIPI-DSI (BSP: esp32_p4_function_ev_board) ──────────
 * The BSP owns these; they are recorded so no later assignment collides.
 * Timings are the ones the vendor ships for this panel. Note the BSP selects
 * them under CONFIG_BSP_LCD_TYPE_1024_600, whose name is misleading — the
 * values inside it are this board's 480x800.
 */
#define LCD_H_RES              480
#define LCD_V_RES              800     /* portrait; the old ST7796 HMI was 480x320 landscape */
#define LCD_MIPI_DSI_LANES     2
#define LCD_MIPI_LANE_MBPS     1000
#define LCD_BITS_PER_PIXEL     16      /* RGB565 */
#define LCD_MIPI_HSYNC         1344
#define LCD_MIPI_HBP           160
#define LCD_MIPI_HFP           160
#define LCD_MIPI_VSYNC         635
#define LCD_MIPI_VBP           23
#define LCD_MIPI_VFP           12
#define LCD_DSI_PHY_LDO_CHAN   3       /* LDO_VO3 feeds VDD_MIPI_DPHY */
#define LCD_DSI_PHY_LDO_MV     2500

/* How the frame is turned on its way to the panel. The glass is 480x800
 * portrait and the UI is 800x480, so it has to rotate either way; 270 rather
 * than 90 because of which way up the module is mounted in this enclosure —
 * at 90 the picture is correct but upside down. Verified on the board, not
 * inferred: if the module is ever remounted, this is the one line to change. */
#define LCD_ROTATE_DEGREES  270

#define LCD_RESET_GPIO      5     /* sheet 2, on the panel FPC */
#define LCD_BACKLIGHT_GPIO  23    /* LCD_PWM -> MP3202 enable; PWM it for dimming */

/* ── Touch — GT911 on I2C, address 0x5D ─────────────────────────────────────
 * Sheet 2: the panel FPC carries TOUCH_RST and TOUCH_INT, and the touch I2C is
 * the RTC_DAT/SDA1 + RTC_CLK/SCL1 pair with 5k1 pull-ups to 3V3. Sheet 3 maps
 * that pair to GPIO7/GPIO8, shared with the ES8311 codec and the RTC.
 * The vendor BSP leaves RST and INT unconfigured (GPIO_NUM_NC) and drives the
 * controller over I2C alone, which is why they are listed but not used.
 */
#define TOUCH_I2C_SDA       7
#define TOUCH_I2C_SCL       8
#define TOUCH_INT_GPIO      21    /* wired, but BSP treats it as NC */
#define TOUCH_RESET_GPIO    22    /* wired, but BSP treats it as NC */

/* ── MODULE pins — fixed by the JC-ESP32P4-M3 ───────────────────────────────
 * The P4<->C6 SDIO link is internal to the module and never appears on a
 * carrier pin. The module does break out the C6's UART0 and boot/reset so the
 * radio can be reflashed; those land on JP1.
 */
#define C6_SDIO_CLK   18
#define C6_SDIO_CMD   19
#define C6_SDIO_D0    14
#define C6_SDIO_D1    15
#define C6_SDIO_D2    16
#define C6_SDIO_D3    17
#define C6_RESET_GPIO 54

/* ── Carrier peripherals not used by this firmware (sheets 3 and 6) ─────────
 *   microSD (SDMMC) : CLK 43, CMD 44, D0-D3 on 39/40/41/42
 *   ES8311 codec    : MCLK 13, BCLK 12, LRCK 10, DOUT 9; ES7210 DIN 48
 *   audio amp enable: GPIO11 (PA_CTRL)
 *   console UART0   : TX 37, RX 38
 *   camera (CSI)    : dedicated MIPI-CSI pins + CSI_IO0/IO1
 */

/* ── Free user I/O on the Expand-IO header JP1 (26-pin, sheet 4) ────────────
 * JP1 pin 1 = 3V3, pin 2 = 5V, with GND on both sides. It breaks out:
 *
 *   GPIO28 29 30 31 32 33 34 49 50 51 52     <- 11 usable pins
 *   GPIO35                                    <- DO NOT USE: BOOTMODE strap
 *   ES_I2C_SDA / ES_I2C_SCL                   <- GPIO7/8, the shared I2C bus
 *   C6_U0RXD / C6_U0TXD / C6_IO9 / C6_CHIP_PU <- for reflashing the C6
 *
 * Sheet 3 also labels GPIO28-35 and 49-52 with RMII Ethernet names
 * (RMII_TXD0/1, MDC, MDIO, PHY_RSTN, RMII_CLK, RMII_TXEN...). That is the
 * module's alternate-function legend, inherited from the reference design and
 * used on Guition's JC-ESP32P4-M3-DEV carrier. There is NO Ethernet PHY on this
 * carrier — sheets 1-7 contain only power, LCD/CSI, the module, USB/IO, RS-485,
 * codec/TF-card and the microphone — so on this board they are plain GPIO.
 */
#define FREE_GPIO_COUNT 11

#endif /* BOARD_PINS_H */
