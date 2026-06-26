idf_component_register(
    SRCS "ssd1683.c" "epd_canvas.c" "epd_text.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_spi esp_driver_gpio
)
