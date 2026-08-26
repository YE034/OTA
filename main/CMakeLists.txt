idf_component_register(SRCS "main.c"
                        INCLUDE_DIRS "."
                        REQUIRES freertos nvs_flash esp_wifi esp_event esp_netif mqtt log driver
                                 wifi onenet_mqtt sensor rgb)
