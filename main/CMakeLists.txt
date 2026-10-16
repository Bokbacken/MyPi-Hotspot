idf_component_register(
    SRCS "main.c" "display.c" "touch.c"
    INCLUDE_DIRS "."
    REQUIRES esp_lcd esp_driver_spi esp_driver_i2c esp_driver_gpio esp_hw_support esp_timer
)
