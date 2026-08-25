idf_component_register(
    SRCS "wifi.c"
    INCLUDE_DIRS "."
    REQUIRES freertos esp_wifi esp_event esp_netif nvs_flash
)
