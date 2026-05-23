idf_component_register(SRCS "synth-32.c"
                    INCLUDE_DIRS "."
                    REQUIRES
                        nvs_flash
                        esp_wifi
                        esp_event
                        esp_netif
                        esp_http_server
                        esp_driver_i2s
                        esp_driver_gpio
                        esp_hw_support
                        espressif__esp_codec_dev
                        waveshare__esp32_p4_platform
                        )
