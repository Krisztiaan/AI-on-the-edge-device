idf_component_register(
    SRCS
        "neo_app.cpp"
    INCLUDE_DIRS
        "."
    REQUIRES
        esp_event
        esp_netif
        esp_http_server
        esp_wifi
        nvs_flash
)

