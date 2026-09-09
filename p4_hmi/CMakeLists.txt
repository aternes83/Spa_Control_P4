# ESP-IDF project for the Spa_Control_P4 HMI node (ESP32-P4).
#
#   idf.py set-target esp32p4
#   idf.py build flash monitor
#
# Requires ESP-IDF v5.5 or newer for ESP32-P4 support. The MIPI-DSI panel and
# touch controller come from the board BSP — add it via the component manager
# once the display work starts (see docs/README notes in the repo root).
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(spa_control_p4_hmi)
