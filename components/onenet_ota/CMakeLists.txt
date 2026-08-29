idf_component_register(
    SRCS "onenet_ota.c"
    INCLUDE_DIRS "."
    REQUIRES esp_http_client app_update json mbedtls log
)
