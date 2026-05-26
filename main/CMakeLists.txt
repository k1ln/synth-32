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
                        esp_driver_i2c
                        esp_hw_support
                        esp_mm
                        esp_driver_ledc
                        esp_lcd
                        espressif__esp_codec_dev
                        waveshare__esp32_p4_platform
                        espressif__esp_lcd_touch
                        espressif__esp_lcd_touch_gt911
                        )
