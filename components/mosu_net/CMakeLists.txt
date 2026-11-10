idf_component_register(
    SRCS "src/mosu_net.c"
    INCLUDE_DIRS "include"
    REQUIRES
        mosu_config
        mosu_time
        esp_wifi
        esp_netif
        esp_event
        esp_http_client
        esp_timer
        mbedtls
        lwip
)
