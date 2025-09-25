#include <cstdio>
#include <cstring>
#include <string>
#include <deque>
#include <algorithm>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_wifi.h"
#include "nvs_flash.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_http_client.h"

#include "usb/usb_host.h"
#include "usb/cdc_acm_host.h"

#include "cJSON.h"

#include "cdc.h"
#include "wifi.h"
#include "log.h"

static const char *AI_ENDPOINT = "https://ai.hackclub.com/chat/completions";

static const size_t MAX_MSG_HISTORY = 8;
static const int MAX_SEND_RETRIES = 4;
static const TickType_t INITIAL_BACKOFF_MS = 500;
static const TickType_t MAX_BACKOFF_MS = 8000;

static const char *rootCACertificate = R"string_literal(
-----BEGIN CERTIFICATE-----
MIIFazCCA1OgAwIBAgIRAIIQz7DSQONZRGPgu2OCiwAwDQYJKoZIhvcNAQELBQAw
TzELMAkGA1UEBhMCVVMxKTAnBgNVBAoTIEludGVybmV0IFNlY3VyaXR5IFJlc2Vh
cmNoIEdyb3VwMRUwEwYDVQQDEwxJU1JHIFJvb3QgWDEwHhcNMTUwNjA0MTEwNDM4
WhcNMzUwNjA0MTEwNDM4WjBPMQswCQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJu
ZXQgU2VjdXJpdHkgUmVzZWFyY2ggR3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBY
MTCCAiIwDQYJKoZIhvcNAQEBBQADggIPADCCAgoCggIBAK3oJHP0FDfzm54rVygc
h77ct984kIxuPOZXoHj3dcKi/vVqbvYATyjb3miGbESTtrFj/RQSa78f0uoxmyF+
0TM8ukj13Xnfs7j/EvEhmkvBioZxaUpmZmyPfjxwv60pIgbz5MDmgK7iS4+3mX6U
A5/TR5d8mUgjU+g4rk8Kb4Mu0UlXjIB0ttov0DiNewNwIRt18jA8+o+u3dpjq+sW
T8KOEUt+zwvo/7V3LvSye0rgTBIlDHCNAymg4VMk7BPZ7hm/ELNKjD+Jo2FR3qyH
B5T0Y3HsLuJvW5iB4YlcNHlsdu87kGJ55tukmi8mxdAQ4Q7e2RCOFvu396j3x+UC
B5iPNgiV5+I3lg02dZ77DnKxHZu8A/lJBdiB3QW0KtZB6awBdpUKD9jf1b0SHzUv
KBds0pjBqAlkd25HN7rOrFleaJ1/ctaJxQZBKT5ZPt0m9STJEadao0xAH0ahmbWn
OlFuhjuefXKnEgV4We0+UXgVCwOPjdAvBbI+e0ocS3MFEvzG6uBQE3xDk3SzynTn
jh8BCNAw1FtxNrQHusEwMFxIt4I7mKZ9YIqioymCzLq9gwQbooMDQaHWBfEbwrbw
qHyGO0aoSCqI3Haadr8faqU9GY/rOPNk3sgrDQoo//fb4hVC1CLQJ13hef4Y53CI
rU7m2Ys6xt0nUW7/vGT1M0NPAgMBAAGjQjBAMA4GA1UdDwEB/wQEAwIBBjAPBgNV
HRMBAf8EBTADAQH/MB0GA1UdDgQWBBR5tFnme7bl5AFzgAiIyBpY9umbbjANBgkq
hkiG9w0BAQsFAAOCAgEAVR9YqbyyqFDQDLHYGmkgJykIrGF1XIpu+ILlaS/V9lZL
ubhzEFnTIZd+50xx+7LSYK05qAvqFyFWhfFQDlnrzuBZ6brJFe+GnY+EgPbk6ZGQ
3BebYhtF8GaV0nxvwuo77x/Py9auJ/GpsMiu/X1+mvoiBOv/2X/qkSsisRcOj/KK
NFtY2PwByVS5uCbMiogziUwthDyC3+6WVwW6LLv3xLfHTjuCvjHIInNzktHCgKQ5
ORAzI4JMPJ+GslWYHb4phowim57iaztXOoJwTdwJx4nLCgdNbOhdjsnvzqvHu7Ur
TkXWStAmzOVyyghqpZXjFaH3pO3JLF+l+/+sKAIuvtd7u+Nxe5AW0wdeRlN8NwdC
jNPElpzVmbUq4JUagEiuTDkHzsxHpFKVK7q4+63SM1N95R1NbdWhscdCb+ZAJzVc
oyi3B43njTOQ5yOf+1CceWxG1bQVs5ZufpsMljq4Ui0/1lvh+wjChP4kqKOJ2qxq
4RgqsahDYVvTH9w7jXbyLeiNdd8XM2w9U/t7y0Ff/9yi0GE44Za4rF2LN9d11TPA
mRGunUHBcnWEvgJBQl9nJEiU0Zsnvgc/ubhPgXRR4Xq37Z0j4r7g1SgEEzwxA57d
emyPxgcYxn/eR44/KJ4EBs+lVDR3veyJm+kXQ99b21/+jh5Xos1AnX5iItreGCc=
-----END CERTIFICATE-----
)string_literal";

static SemaphoreHandle_t device_disconnected_sem = nullptr;
static esp_http_client_handle_t client = nullptr;
static QueueHandle_t gpt_queue = nullptr; // holds pointers to GptRequest
static SemaphoreHandle_t http_mutex = nullptr; // ensure single HTTP usage
static SemaphoreHandle_t messages_mutex = nullptr; // protect messages history

static std::string input_buffer; // echo buffer for typed chars
static std::deque<std::string> messages_history; // stored JSON objects for messages

static const char system_json[] = "{\"role\":\"system\",\"content\":\"You are a helpful assistant. Output in ASCII only. No special symbols. No emojis. No markdown. Please keep responses to ~2 sentences.\"}";

struct GptRequest {
    std::string message;
};

static void replaceAll(std::string &str, const std::string &from, const std::string &to) {
    size_t pos = 0;
    while ((pos = str.find(from, pos)) != std::string::npos) {
        str.replace(pos, from.length(), to);
        pos += to.length();
    }
}

static void addMessageToHistory(const char *role, const std::string &content) {
    std::string escaped = content;

    replaceAll(escaped, "\\", "\\\\");
    replaceAll(escaped, "\"", "\\\"");
    replaceAll(escaped, "\n", "\\n");

    std::string entry = std::string("{\"role\":\"") + role + "\",\"content\":\"" + escaped + "\"}";

    if (xSemaphoreTake(messages_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        messages_history.push_back(entry);
        while (messages_history.size() > MAX_MSG_HISTORY) {
            messages_history.pop_front();
        }
        xSemaphoreGive(messages_mutex);
    } else {
        ESP_LOGW(TAG, "addMessageToHistory: failed to take messages_mutex");
    }
}

static std::string buildMessagesJsonArray() {
    std::string out = std::string("[") + system_json;
    if (xSemaphoreTake(messages_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        for (const auto &m : messages_history) {
            out += ",";
            out += m;
        }
        xSemaphoreGive(messages_mutex);
    } else {
        ESP_LOGW(TAG, "buildMessagesJsonArray: failed to take messages_mutex");
    }
    out += "]";
    return out;
}

struct StreamState {
    std::string assistantReply;
    std::string partialLine;
    bool done;
    StreamState(): assistantReply(), partialLine(), done(false) {}
};

static esp_err_t _http_event_handler(esp_http_client_event_t *evt) {
    StreamState *state = reinterpret_cast<StreamState*>(evt->user_data);

    switch (evt->event_id) {
        case HTTP_EVENT_ON_FINISH:
            if (state) {
                if (!state->assistantReply.empty()) {
                    addMessageToHistory("assistant", state->assistantReply);
                }
                delete state;
                esp_http_client_set_user_data(evt->client, nullptr);
            }
            break;

        case HTTP_EVENT_ON_DATA:
            if (!state) break;
            if (state->done) break;

            if (evt->data && evt->data_len > 0) {
                for (int i = 0; i < evt->data_len; ++i) {
                    char c = ((const char*)evt->data)[i];
                    if (c == '\r') continue;
                    if (c == '\n') {
                        if (!state->partialLine.empty()) {
                            std::string line = state->partialLine;
                            state->partialLine.clear();

                            if (line.size() < 6 || line.compare(0, 6, "data: ") != 0) {
                                continue;
                            }

                            if (line == "data: [DONE]") {
                                state->done = true;
                                break;
                            }

                            std::string json_text = line.substr(6);

                            cJSON *doc = cJSON_Parse(json_text.c_str());
                            if (!doc) continue;

                            cJSON *choices = cJSON_GetObjectItemCaseSensitive(doc, "choices");
                            if (cJSON_IsArray(choices) && cJSON_GetArraySize(choices) > 0) {
                                cJSON *first_choice = cJSON_GetArrayItem(choices, 0);
                                if (cJSON_IsObject(first_choice)) {
                                    cJSON *delta = cJSON_GetObjectItemCaseSensitive(first_choice, "delta");
                                    if (cJSON_IsObject(delta)) {
                                        cJSON *content = cJSON_GetObjectItemCaseSensitive(delta, "content");
                                        if (cJSON_IsString(content) && content->valuestring) {
                                            const char *chunk = content->valuestring;
                                            cdc_print(chunk, strlen(chunk));
                                            state->assistantReply += chunk;
                                        }
                                    }
                                }
                            }
                            cJSON_Delete(doc);
                        }
                    } else {
                        state->partialLine.push_back(c);
                    }
                }
            }
            break;

        default:
            break;
    }

    return ESP_OK;
}

struct SendResult {
    esp_err_t err;    // last transport-level error (ESP_OK if none)
    int http_status;  // last HTTP status code (-1 if not available)
    int attempts;
};

static esp_err_t sendStreamOnce(const std::string &post_data) {
    StreamState *state = new StreamState();
    if (!state) return ESP_ERR_NO_MEM;

    esp_http_client_set_post_field(client, post_data.c_str(), (int)post_data.length());
    esp_http_client_set_user_data(client, state);

    esp_err_t err = esp_http_client_perform(client);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "sendStreamOnce: perform failed %s (%d)", esp_err_to_name(err), err);
        delete state;
        esp_http_client_set_user_data(client, nullptr);
        return err;
    }
    //  ON_FINISH will free state
    return ESP_OK;
}

static SendResult sendStreamWithRetries(const std::string &post_data) {
    TickType_t backoff = INITIAL_BACKOFF_MS;
    esp_err_t last_err = ESP_FAIL;
    int last_status = -1;

    for (int attempt = 1; attempt <= MAX_SEND_RETRIES; ++attempt) {
        if (xSemaphoreTake(http_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
            last_err = ESP_ERR_TIMEOUT;
            ESP_LOGW(TAG, "sendStreamWithRetries: could not take http_mutex");
            return SendResult{ last_err, last_status, attempt - 1 };
        }

        esp_err_t err = sendStreamOnce(post_data);
        xSemaphoreGive(http_mutex);

        last_err = err;

        if (err == ESP_OK) {
            last_status = esp_http_client_get_status_code(client);
            if (last_status >= 200 && last_status < 300) {
                return SendResult{ ESP_OK, last_status, attempt };
            }

            // treat 429 and 5xx as retryable
            if (last_status == 429 || last_status >= 500) {
                ESP_LOGW(TAG, "sendStreamWithRetries: HTTP %d retryable on attempt %d", last_status, attempt);
            } else {
                ESP_LOGW(TAG, "sendStreamWithRetries: HTTP %d fatal on attempt %d", last_status, attempt);
                return SendResult{ ESP_FAIL, last_status, attempt };
            }
        } else {
            ESP_LOGW(TAG, "sendStreamWithRetries: perform error %s (%d) on attempt %d",
                     esp_err_to_name(err), err, attempt);
        }

        if (attempt < MAX_SEND_RETRIES) {
            TickType_t delay_ms = backoff;
            if (delay_ms > MAX_BACKOFF_MS) delay_ms = MAX_BACKOFF_MS;
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
            backoff = (backoff * 2) > MAX_BACKOFF_MS ? MAX_BACKOFF_MS : (backoff * 2);
            ESP_LOGI(TAG, "sendStreamWithRetries: retrying attempt %d after %u ms", attempt + 1, (unsigned)delay_ms);
        }
    }

    return SendResult{ last_err, last_status, MAX_SEND_RETRIES };
}

static void notifySendFailure(const SendResult &res) {
    char msg[256];
    if (res.err != ESP_OK) {
        const char *ename = esp_err_to_name(res.err);
        int wrote = snprintf(msg, sizeof(msg), "\r\n[gpt] send failed: %s (err=%d) after %d attempts\r\n",
                             ename ? ename : "ERR", (int)res.err, res.attempts);
        if (wrote > 0) cdc_print(msg, (size_t)wrote);
        ESP_LOGW(TAG, "send failed: %s (err=%d) attempts=%d", ename ? ename : "ERR", (int)res.err, res.attempts);
    } else {
        int status = res.http_status;
        int wrote = snprintf(msg, sizeof(msg), "\r\n[gpt] send failed: HTTP %d after %d attempts\r\n",
                             status, res.attempts);
        if (wrote > 0) cdc_print(msg, (size_t)wrote);
        ESP_LOGW(TAG, "send failed: HTTP %d attempts=%d", status, res.attempts);
    }
}

static void gpt_task(void *arg) {
    while (true) {
        GptRequest *req_ptr = nullptr;
        if (xQueueReceive(gpt_queue, &req_ptr, portMAX_DELAY) == pdTRUE && req_ptr) {
            addMessageToHistory("user", req_ptr->message);

            std::string messages_array = buildMessagesJsonArray();
            std::string post_data = std::string("{\"model\":\"qwen/qwen3-32b\",\"include_reasoning\":false,\"messages\":") +
                                    messages_array + ",\"stream\":true}";

            cdc_print("\r\n[gpt] sending\r\n");

            SendResult res = sendStreamWithRetries(post_data);

            if (res.err != ESP_OK || !(res.http_status >= 200 && res.http_status < 300)) {
                notifySendFailure(res);
            } else {
                cdc_print("\r\n[gpt] done\r\n");
            }

            delete req_ptr;
        }
    }
}

static bool handle_rx(const uint8_t *data, size_t data_len, void *arg) {
    for (size_t i = 0; i < data_len; ++i) {
        uint8_t ch = data[i];

        if (ch == '\b' || ch == 127) {
            if (!input_buffer.empty()) {
                input_buffer.pop_back();
                cdc_print("\b \b", 3);
            }
            continue;
        }

        if (ch == '\n') {
            if (input_buffer.empty()) continue;

            if (input_buffer == "CLS") {
                if (xSemaphoreTake(messages_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                    messages_history.clear();
                    xSemaphoreGive(messages_mutex);
                }
                input_buffer.clear();
                cdc_print("\r\n[gpt] cleared conversation\r\n");
                continue;
            }

            GptRequest *req_ptr = new GptRequest();
            req_ptr->message = input_buffer;
            input_buffer.clear();

            if (xQueueSend(gpt_queue, &req_ptr, pdMS_TO_TICKS(50)) != pdTRUE) {
                delete req_ptr;
                cdc_print("\r\n[gpt] queue full, dropped message\r\n");
            } else {
                cdc_print("\r\n[gpt] added to queue\r\n");
            }
            continue;
        }

        // echo and accumulate
        cdc_print((const char *)&ch, 1);
        input_buffer.push_back((char)ch);
    }
    return true;
}

static void handle_event(const cdc_acm_host_dev_event_data_t *event, void *user_ctx) {
    switch (event->type) {
        case CDC_ACM_HOST_ERROR:
            ESP_LOGE(TAG, "CDC-ACM error, err_no=%d", event->data.error);
            break;
        case CDC_ACM_HOST_DEVICE_DISCONNECTED:
            ESP_LOGI(TAG, "Device disconnected");
            usb_close();
            xSemaphoreGive(device_disconnected_sem);
            break;
        case CDC_ACM_HOST_SERIAL_STATE:
            ESP_LOGI(TAG, "Serial state 0x%04X", event->data.serial_state.val);
            break;
        default:
            ESP_LOGW(TAG, "Unsupported CDC event: %d", event->type);
            break;
    }
}

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "Starting CalcGPT");

    device_disconnected_sem = xSemaphoreCreateBinary();
    assert(device_disconnected_sem);

    messages_mutex = xSemaphoreCreateMutex();
    assert(messages_mutex);
    http_mutex = xSemaphoreCreateMutex();
    assert(http_mutex);

    gpt_queue = xQueueCreate(8, sizeof(GptRequest*));
    assert(gpt_queue);

    usb_install();

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    wifi_init_sta();

    esp_http_client_config_t config = {
        .url = AI_ENDPOINT,
        .cert_pem = rootCACertificate,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 60000,
        .event_handler = _http_event_handler,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
    };

    client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Connection", "keep-alive");

    BaseType_t r = xTaskCreate(gpt_task, "gpt_task", 8192, nullptr, tskIDLE_PRIORITY + 4, nullptr);
    assert(r == pdTRUE);

    const cdc_acm_host_device_config_t dev_config = {
        .connection_timeout_ms = TX_TIMEOUT_MS,
        .out_buffer_size = 512,
        .in_buffer_size = 512,
        .event_cb = handle_event,
        .data_cb = handle_rx,
        .user_arg = NULL,
    };

    while (true) {
        esp_err_t err = usb_open(&dev_config);
        if (err != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        vTaskDelay(pdMS_TO_TICKS(100));

        cdc_print("[cdc] setup complete\r\n[wifi] connected\r\n[calcgpt] ready\r\n");

        // block until device disconnects
        xSemaphoreTake(device_disconnected_sem, portMAX_DELAY);
    }
}
