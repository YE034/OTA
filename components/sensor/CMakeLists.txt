idf_component_register(
    SRCS "temp_sensor.c"
    INCLUDE_DIRS "."
    REQUIRES driver log esp_system soc esp_hw_support efuse freertos
)
