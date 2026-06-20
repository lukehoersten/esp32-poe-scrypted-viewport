idf_component_register(
    SRC_DIRS     "."
    INCLUDE_DIRS "."
    REQUIRES     driver esp_app_format esp_driver_i2c esp_driver_jpeg esp_eth
                 esp_event esp_http_client esp_http_server esp_hw_support
                 esp_lcd esp_netif esp_pm esp_timer hal json mdns nvs_flash
)
