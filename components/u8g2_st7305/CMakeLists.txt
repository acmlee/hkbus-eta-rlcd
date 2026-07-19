idf_component_register(
    COMPILE_OPTIONS -Wno-error=unused-function
    SRCS "u8g2_st7305.c"
    INCLUDE_DIRS "."
    REQUIRES
        u8g2
        esp_driver_gpio
        esp_driver_spi
)
