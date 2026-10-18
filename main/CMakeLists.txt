idf_component_register(
    SRCS "main.c" "display.c" "touch.c" "diag.c" "logbuf.c" "wifi.c" "web.c" "pintest.c"
         "profiles.c" "net.c" "eth.c" "display_font.c" "display_ui.c" "rescue.c" "logstore.c"
         "ui_lvgl_port.c" "mypi_ui/lvgl_view.c" "mypi_font_14.c"
    INCLUDE_DIRS "."
    EMBED_TXTFILES "web_ui.css" "web_ui.js"
    REQUIRES esp_lcd esp_driver_spi esp_driver_i2c esp_driver_gpio esp_hw_support esp_timer
             esp_wifi esp_netif esp_event nvs_flash esp_http_server app_update esp_app_format
             esp_psram spi_flash log esp_eth lwip fatfs wear_levelling vfs
)

# Compile DEBUG-level logs into the app so debug mode can enable them at runtime
# (the global maximum stays INFO, so ESP-IDF components are not affected).
target_compile_definitions(${COMPONENT_LIB} PRIVATE LOG_LOCAL_LEVEL=ESP_LOG_DEBUG)
