idf_component_register(
    SRC_DIRS     "."
    INCLUDE_DIRS "."
    REQUIRES     driver esp_driver_i2c esp_driver_jpeg esp_eth esp_event
                 esp_http_server esp_lcd esp_netif esp_timer json mdns nvs_flash
)
