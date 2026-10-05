idf_component_register(SRCS "main.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES esp_driver_cam esp_driver_isp esp_driver_i2c
                             esp_driver_jpeg esp_http_server esp_wifi esp_event
                             esp_netif esp_hw_support esp_mm nvs_flash sensor_init)
