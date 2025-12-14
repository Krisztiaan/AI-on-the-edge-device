#include "neo_app.h"

#include <string.h>

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

static const char *TAG = "neo";

static esp_err_t handle_root(httpd_req_t *req)
{
    static const char kHtml[] =
        "<!doctype html>"
        "<meta charset=\"utf-8\">"
        "<title>AI Meter NEO</title>"
        "<h1>AI Meter NEO</h1>"
        "<p>NEO is in early bring-up.</p>"
        "<p><a href=\"/api/info\">/api/info</a></p>";

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, kHtml, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handle_api_info(httpd_req_t *req)
{
    char buf[256];
    const unsigned int heap = (unsigned int)esp_get_free_heap_size();
    const int n = snprintf(buf, sizeof(buf),
                           "{\"heap_free\":%u}",
                           heap);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, n);
}

static httpd_handle_t start_http()
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;

    httpd_handle_t server = nullptr;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        return nullptr;
    }

    httpd_uri_t root = {};
    root.uri = "/";
    root.method = HTTP_GET;
    root.handler = handle_root;
    httpd_register_uri_handler(server, &root);

    httpd_uri_t info = {};
    info.uri = "/api/info";
    info.method = HTTP_GET;
    info.handler = handle_api_info;
    httpd_register_uri_handler(server, &info);

    return server;
}

static esp_err_t init_nvs()
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

static void wifi_init_softap()
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wifi_config = {};
    strncpy((char *)wifi_config.ap.ssid, CONFIG_NEO_SOFTAP_SSID, sizeof(wifi_config.ap.ssid));
    wifi_config.ap.ssid_len = (uint8_t)strlen(CONFIG_NEO_SOFTAP_SSID);
    wifi_config.ap.channel = 1;
    wifi_config.ap.max_connection = 4;
    wifi_config.ap.authmode = WIFI_AUTH_OPEN;

    if (strlen(CONFIG_NEO_SOFTAP_PASSWORD) > 0) {
        strncpy((char *)wifi_config.ap.password, CONFIG_NEO_SOFTAP_PASSWORD, sizeof(wifi_config.ap.password));
        wifi_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "SoftAP started: ssid=%s", CONFIG_NEO_SOFTAP_SSID);
}

void neo_app_start()
{
    ESP_LOGI(TAG, "Starting NEO");

    ESP_ERROR_CHECK(init_nvs());

#if CONFIG_NEO_ENABLE_SOFTAP
    wifi_init_softap();
#else
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
#endif

#if CONFIG_NEO_ENABLE_HTTP
    (void)start_http();
    ESP_LOGI(TAG, "HTTP server started");
#endif
}

