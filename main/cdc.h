#pragma once

#include "esp_err.h"
#include <inttypes.h>
#include "usb/usb_host.h"
#include "usb/cdc_acm_host.h"

#define TX_TIMEOUT_MS       (1000)

esp_err_t cdc_print(const char* data, size_t len);
esp_err_t cdc_print(const char* data);

esp_err_t usb_open(const cdc_acm_host_device_config_t* dev_config);
void usb_close();

void usb_install();
