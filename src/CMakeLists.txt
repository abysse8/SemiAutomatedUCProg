idf_component_register(
  SRCS "proxy_main.c" "usb_update_msc.c"
  INCLUDE_DIRS "."
  PRIV_REQUIRES nvs_flash esp_wifi esp_eth esp_netif lwip ethernet_init esp_http_server esp_tinyusb fatfs sdmmc esp_driver_sdmmc esp_driver_gpio
)

target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--allow-multiple-definition")
