idf_component_register(
    SRCS "main.c" "bh1750.c"
    INCLUDE_DIRS "."
    REQUIRES 
        nvs_flash
        esp_wifi
        esp_event
        esp_netif
        lwip
        driver
        esp_http_client
        json
)