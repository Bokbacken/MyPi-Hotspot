idf_component_register(
    SRCS "main.c" "display.c" "touch.c" "diag.c" "logbuf.c" "wifi.c" "web.c" "pintest.c"
    INCLUDE_DIRS "."
    REQUIRES esp_lcd esp_driver_spi esp_driver_i2c esp_driver_gpio esp_hw_support esp_timer
             esp_wifi esp_netif esp_event nvs_flash esp_http_server app_update esp_app_format
             esp_psram spi_flash log
)
