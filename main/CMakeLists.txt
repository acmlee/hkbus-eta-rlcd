idf_component_register(SRCS "main.c" "display.c" "eta_fetcher.c" "route_config.c"
                          "battery.c" "http_util.c" "weather_hko.c"
                          "fonts/u8g2_font_zhhk_dest_24.c" "fonts/u8g2_font_zhhk_stop_20.c"
                       INCLUDE_DIRS "." "fonts"
                       REQUIRES nvs_flash esp_wifi esp_event esp_http_client spiffs
                                 u8g2 u8g2_st7305 esp_driver_spi esp_driver_gpio
                                 mbedtls esp_adc)

target_compile_definitions(${COMPONENT_LIB} PRIVATE U8G2_USE_LARGE_FONTS)

spiffs_create_partition_image(storage "${CMAKE_SOURCE_DIR}/spiffs_data" FLASH_IN_PROJECT)
