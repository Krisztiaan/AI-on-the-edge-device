#include "last_jpeg_cache.h"

#include <cstring>
#include <string>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "psram.h"

static const char* TAG = "last_jpeg_cache";

namespace last_jpeg_cache {

static SemaphoreHandle_t mutex_handle = nullptr;
static uint8_t* cached = nullptr;
static size_t cached_len = 0;
static size_t cached_cap = 0;

static void ensure_mutex()
{
    if (mutex_handle) {
        return;
    }
    mutex_handle = xSemaphoreCreateMutex();
}

bool has()
{
    ensure_mutex();
    if (!mutex_handle) {
        return false;
    }

    if (xSemaphoreTake(mutex_handle, pdMS_TO_TICKS(50)) != pdTRUE) {
        return false;
    }
    bool result = cached && cached_len > 0;
    xSemaphoreGive(mutex_handle);
    return result;
}

void clear()
{
    ensure_mutex();
    if (!mutex_handle) {
        return;
    }

    if (xSemaphoreTake(mutex_handle, pdMS_TO_TICKS(250)) != pdTRUE) {
        return;
    }

    if (cached) {
        free_psram_heap(std::string(TAG) + "->cached", cached);
    }
    cached = nullptr;
    cached_len = 0;
    cached_cap = 0;

    xSemaphoreGive(mutex_handle);
}

esp_err_t set(const uint8_t* data, size_t len)
{
    ensure_mutex();
    if (!mutex_handle) {
        return ESP_FAIL;
    }
    if (!data || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(mutex_handle, pdMS_TO_TICKS(250)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    if (len > cached_cap) {
        if (cached) {
            free_psram_heap(std::string(TAG) + "->cached", cached);
            cached = nullptr;
            cached_len = 0;
            cached_cap = 0;
        }

        cached = (uint8_t*)malloc_psram_heap(std::string(TAG) + "->cached", len, MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
        if (!cached) {
            xSemaphoreGive(mutex_handle);
            ESP_LOGW(TAG, "Failed to allocate %u bytes for cached JPEG", (unsigned)len);
            return ESP_ERR_NO_MEM;
        }
        cached_cap = len;
    }

    memcpy(cached, data, len);
    cached_len = len;

    xSemaphoreGive(mutex_handle);
    return ESP_OK;
}

esp_err_t send(httpd_req_t* req)
{
    if (!req) {
        return ESP_ERR_INVALID_ARG;
    }

    ensure_mutex();
    if (!mutex_handle) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    if (xSemaphoreTake(mutex_handle, pdMS_TO_TICKS(250)) != pdTRUE) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, "Image cache busy");
        return ESP_ERR_TIMEOUT;
    }

    if (!cached || cached_len == 0) {
        xSemaphoreGive(mutex_handle);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "No cached image available");
        return ESP_ERR_NOT_FOUND;
    }

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t res = httpd_resp_send(req, (const char*)cached, cached_len);

    xSemaphoreGive(mutex_handle);
    return res;
}

} // namespace last_jpeg_cache
