idf_component_register(
    SRCS "onenet_mqtt.c"
    INCLUDE_DIRS "."
    REQUIRES mqtt esp_event esp_timer log json
)
