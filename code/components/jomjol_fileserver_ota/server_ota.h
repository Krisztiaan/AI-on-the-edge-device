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

// Downloads a model from `url` to `/spiffs/models/<name>`, optionally verifies SHA-256,
// and optionally marks it as the active model.
esp_err_t DownloadModel(const std::string &url,
                        const std::string &name,
                        const std::string &expected_sha256,
                        bool overwrite,
                        bool set_active,
                        std::string &out_sha256,
                        size_t &out_bytes,
                        std::string &out_error);

#endif //SERVEROTA_H
