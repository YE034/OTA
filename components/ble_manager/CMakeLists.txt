idf_component_register(
    SRCS
        "ble_manager.c"
        "ble_gatt.c"
    INCLUDE_DIRS "."
    REQUIRES
        bt
        esp_wifi
        esp_event
        esp_netif
        nvs_flash
        freertos
        log
        wifi
)
