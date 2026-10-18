idf_component_register(
    SRCS "main.c" "display.c" "touch.c" "diag.c" "logbuf.c" "wifi.c" "web.c" "pintest.c"
         "profiles.c" "net.c" "eth.c" "display_font.c" "display_ui.c" "rescue.c" "logstore.c"
         "ui_lvgl_port.c" "mypi_ui/lvgl_view.c" "mypi_font_14.c" "mypi_kbd.c" "opt_config.c"
    INCLUDE_DIRS "."
    EMBED_TXTFILES "web_ui.css" "web_ui.js"
    REQUIRES esp_lcd esp_driver_spi esp_driver_i2c esp_driver_gpio esp_hw_support esp_timer
             esp_wifi esp_netif esp_event nvs_flash esp_http_server app_update esp_app_format
             esp_psram spi_flash log esp_eth lwip fatfs wear_levelling vfs
)

# Compile DEBUG-level logs into the app so debug mode can enable them at runtime
# (the global maximum stays INFO, so ESP-IDF components are not affected).
target_compile_definitions(${COMPONENT_LIB} PRIVATE LOG_LOCAL_LEVEL=ESP_LOG_DEBUG)

# v0.8.6: LVGL's built-in heap (LV_MEM_SIZE, 64 KB static array) goes to PSRAM instead of internal
# DRAM. Needs CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY. LVGL has no Kconfig option for this.
# LwIP / NAT tuning: lwIP reads the NAPT table size (512) and idle TCP timeout (2 h) from
# opt_config.c (lwip_tune.h). Fixed defaults since v0.9.3 (no Optimize UI; the v0.9.2 tcpip
# core-pinning wrapper is gone, the tcpip task uses CONFIG_LWIP_TCPIP_TASK_AFFINITY).
idf_component_get_property(lwip_lib lwip COMPONENT_LIB)
target_compile_options(${lwip_lib} PRIVATE "-include${CMAKE_CURRENT_LIST_DIR}/lwip_tune.h")

idf_component_get_property(lvgl_lib lvgl__lvgl COMPONENT_LIB)
target_compile_definitions(${lvgl_lib} PRIVATE
    "LV_ATTRIBUTE_LARGE_RAM_ARRAY=__attribute__((section(\".ext_ram.bss.lv_mem\")))")
