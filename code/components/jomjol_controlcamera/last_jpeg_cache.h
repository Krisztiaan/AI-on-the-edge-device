#pragma once

#include <stddef.h>
#include <stdint.h>

#include <esp_err.h>
#include <esp_http_server.h>

namespace last_jpeg_cache {

bool has();
void clear();
esp_err_t set(const uint8_t* data, size_t len);
esp_err_t send(httpd_req_t* req);

} // namespace last_jpeg_cache

