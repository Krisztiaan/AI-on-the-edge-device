#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

esp_err_t ui_embedded_handler(httpd_req_t *req);
