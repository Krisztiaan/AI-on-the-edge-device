#pragma once

#ifndef SERVEROTA_H
#define SERVEROTA_H

#include <esp_log.h>

#include <esp_http_server.h>

#include <string>


void register_server_ota_uri(httpd_handle_t server);
void CheckOTAUpdate();
void StartOTAVerifyMonitor();
void doReboot();
void doRebootOTA();
void hard_restart();
void CheckUpdate();

// Downloads a model from `url` to `/spiffs/models/<name>` and optionally verifies SHA-256.
// Endpoint is exposed via `/models/download?url=...&name=...&sha256=...&overwrite=1`.

#endif //SERVEROTA_H
