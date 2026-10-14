#include "http_get.h"

#include <cstring>
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"

static char const TAG[] = "adsb http";

int http_get_body(const char* url, char** out_buf, size_t* out_len, size_t max_len) {
    *out_buf = nullptr;
    *out_len = 0;

    esp_http_client_config_t config = {};
    config.url               = url;
    config.timeout_ms        = 10000;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.keep_alive_enable = false;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        ESP_LOGE(TAG, "Failed to init HTTP client");
        return -1;
    }

    int status = -1;

    if (esp_http_client_open(client, 0) != ESP_OK) {
        ESP_LOGE(TAG, "Open failed: internal heap %lu bytes free, PSRAM %lu bytes free",
                 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        esp_http_client_cleanup(client);
        return -1;
    }

    int64_t content_length = esp_http_client_fetch_headers(client);
    status                 = esp_http_client_get_status_code(client);

    if (status != 200) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return status;
    }

    size_t capacity = max_len;
    if (content_length > 0 && (size_t)content_length + 1 < capacity) {
        capacity = (size_t)content_length + 1;
    }

    char* buf = (char*)heap_caps_malloc(capacity, MALLOC_CAP_SPIRAM);
    if (buf == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate %u bytes for response body", (unsigned)capacity);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return -1;
    }

    size_t total = 0;
    while (total + 1 < capacity) {
        int read = esp_http_client_read(client, buf + total, (int)(capacity - 1 - total));
        if (read <= 0) {
            break;
        }
        total += (size_t)read;
    }
    buf[total] = '\0';

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    *out_buf = buf;
    *out_len = total;
    return status;
}
