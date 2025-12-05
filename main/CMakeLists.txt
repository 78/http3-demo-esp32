idf_component_register(SRCS "uart_uhci.cc" "uart_eth_modem.cc" "main.cc"
                              "http3_manager.cc"
                              "pmic/pmic.cc"
                              "pmic/axp2101.cc"
                              "pmic/i2c_device.cc"
                    PRIV_REQUIRES esp-http3 esp_driver_i2c esp_driver_uart esp_psram esp_driver_gpio esp_mm esp_pm
                    INCLUDE_DIRS "." "pmic"
                    EMBED_FILES "test.ogg")
