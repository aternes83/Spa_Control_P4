# ESP-IDF project for the Spa_Control_P4 HMI node (ESP32-P4).
#
#   idf.py set-target esp32p4
#   idf.py build flash monitor
#
# Requires ESP-IDF v5.5 or newer for ESP32-P4 support.
#
# The MIPI-DSI panel and the GT911 touch controller come from the board BSP.
# That BSP is the vendor's own copy, shipped in the documentation package and
# checked in alongside this firmware, NOT the one on the component registry:
# Guition modified it for this panel, putting the 480x800 timings behind the
# misleadingly named CONFIG_BSP_LCD_TYPE_1024_600 and pointing its ST7701 driver
# dependency at a local path. Pulling espressif/esp32_p4_function_ev_board from
# the registry instead would build, and would drive the panel at the wrong
# resolution. See docs/HARDWARE.md.
cmake_minimum_required(VERSION 3.16)

set(VENDOR_COMPONENTS
    "${CMAKE_CURRENT_LIST_DIR}/../JC4880P443C_I_W/1-Demo/idf_examples/ESP-IDF_5.5.4/common_components")
set(EXTRA_COMPONENT_DIRS
    "${VENDOR_COMPONENTS}/espressif__esp32_p4_function_ev_board"
    "${VENDOR_COMPONENTS}/espressif__esp_lcd_st7701")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(spa_control_p4_hmi)
