idf_component_register(SRCS   "uart_eth_modem/uart_uhci.cc"
                             "uart_eth_modem/uart_eth_modem.cc"
                             "main.cc"
                             "quic_test.cc"
                             "pmic/pmic.cc"
                             "pmic/axp2101.cc"
                             "pmic/i2c_device.cc"
                             "system_info.cc"
                    PRIV_REQUIRES esp-http3 esp_driver_i2c esp_driver_uart esp_psram esp_driver_gpio esp_mm esp_pm esp_http_client spi_flash esp_app_format esp_timer app_update
                    INCLUDE_DIRS "." "pmic" "uart_eth_modem"
                    EMBED_FILES "test.ogg")
