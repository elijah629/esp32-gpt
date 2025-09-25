idf_component_register(SRCS "main.cpp" "cdc.cpp" "log.cpp" "wifi.cpp" INCLUDE_DIRS "." PRIV_REQUIRES usb json esp_wifi nvs_flash esp_http_client)
