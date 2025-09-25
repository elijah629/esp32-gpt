
#include "cdc.h"
#include "log.h"

#include "esp_err.h"
#include "usb/usb_host.h"
#include "usb/cdc_acm_host.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_system.h"
#include "esp_log.h"
#include <inttypes.h>
#include <string.h>

#define USB_DEVICE_VID      (CDC_HOST_ANY_VID)
#define USB_DEVICE_PID      (CDC_HOST_ANY_PID)

#define USB_HOST_PRIORITY   (20)

#define POOL_SIZE           16
#define BUF_SIZE            512

#define TX_QUEUE_LEN        16
#define TX_TASK_STACK       4096
#define TX_BLOCK_MS         1000

static uint8_t tx_pool[POOL_SIZE][BUF_SIZE];
static QueueHandle_t free_idx_queue;
static QueueHandle_t tx_queue;
static cdc_acm_dev_hdl_t cdc_dev = NULL;

typedef struct {
    uint8_t idx;
    size_t len;
} tx_item_t;

static void usb_lib_task(void *arg)
{
    while (1) {
        uint32_t event_flags;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            ESP_ERROR_CHECK(usb_host_device_free_all());
        }
    }
}

static void tx_task(void *arg)
{
    tx_item_t item;
    while (true) {
        if (xQueueReceive(tx_queue, &item, portMAX_DELAY) == pdTRUE) {
            uint8_t idx = item.idx;
            size_t len = item.len;

            if (cdc_dev) {
                esp_err_t res = cdc_acm_host_data_tx_blocking(cdc_dev,
                                                              tx_pool[idx],
                                                              len,
                                                              TX_BLOCK_MS);
                if (res != ESP_OK) {
                    ESP_LOGW(TAG, "TX failed (res=%d), len=%u", res, (unsigned)len);
                }
            } else {
                ESP_LOGW(TAG, "TX skipped, no device");
            }

            // always recycle buffer
            xQueueSend(free_idx_queue, &idx, 0);
        }
    }
}

esp_err_t cdc_print(const char *data, size_t len) {
    if (!cdc_dev) {
        return ESP_ERR_INVALID_STATE;
    }
    if (data == NULL || len == 0) {
        return ESP_OK;
    }

    size_t offset = 0;
    while (offset < len) {
        size_t chunk_len = (len - offset > BUF_SIZE) ? BUF_SIZE : (len - offset);

        uint8_t idx;
        if (xQueueReceive(free_idx_queue, &idx, pdMS_TO_TICKS(10)) != pdTRUE) {
            return ESP_ERR_NO_MEM; // pool full, give up for now
        }

        memcpy(tx_pool[idx], data + offset, chunk_len);

        tx_item_t item = { .idx = idx, .len = chunk_len };
        if (xQueueSend(tx_queue, &item, pdMS_TO_TICKS(10)) != pdTRUE) {
            xQueueSend(free_idx_queue, &idx, 0); // recycle
            return ESP_ERR_NO_MEM;
        }

        offset += chunk_len;
    }

    return ESP_OK;
}

esp_err_t cdc_print(const char *data) {
    if (data == nullptr || data[0] == '\0') {
        return ESP_OK;
    }

    return cdc_print(data, strlen(data));
}

void usb_install() {
    free_idx_queue = xQueueCreate(POOL_SIZE, sizeof(uint8_t));
    tx_queue = xQueueCreate(TX_QUEUE_LEN, sizeof(tx_item_t));
    assert(free_idx_queue && tx_queue);

    for (uint8_t i = 0; i < POOL_SIZE; i++) {
        xQueueSend(free_idx_queue, &i, 0);
    }

    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };

    ESP_ERROR_CHECK(usb_host_install(&host_config));

    assert(xTaskCreate(usb_lib_task, "usb_lib", 4096, NULL,
                       USB_HOST_PRIORITY, NULL) == pdTRUE);
    assert(xTaskCreate(tx_task, "tx_task", 4096, NULL,
                       tskIDLE_PRIORITY + 5, NULL) == pdTRUE);

    ESP_ERROR_CHECK(cdc_acm_host_install(NULL));
}

esp_err_t usb_open(const cdc_acm_host_device_config_t* dev_config) {
    cdc_dev = NULL; // reset before trying
    return cdc_acm_host_open(USB_DEVICE_VID, USB_DEVICE_PID, 0,
                             dev_config, &cdc_dev);
}

// Call this on disconnect event
void usb_close() {
    if (cdc_dev) {
        cdc_acm_host_close(cdc_dev);
        cdc_dev = NULL;
    }
}
