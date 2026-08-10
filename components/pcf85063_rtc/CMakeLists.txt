idf_component_register(
    SRCS "rtc_wrap.cpp"
         "port/i2c_bsp.cpp"
         "port/i2c_equipment.cpp"
         "src/platform/SensorCommDebug.cpp"
         "src/platform/SensorCommStatic.cpp"
    INCLUDE_DIRS "include" "src" "port"
    PRIV_REQUIRES driver esp_driver_i2c esp_driver_gpio esp_driver_spi
                  esp_timer log freertos)
