# Top-level ESP-IDF project file for MyPi-Hotspot.
# Build with: idf.py set-target esp32p4 && idf.py build
cmake_minimum_required(VERSION 3.16)

# Firmware version shown in the boot banner, web UI and OTA image header.
set(PROJECT_VER "0.6.1")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(mypi_hotspot)
