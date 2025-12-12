#include "server_ota.h"

#include <string>
#include "string.h"

/* TODO Rethink the usage of the int watchdog. It is no longer to be used, see
https://docs.espressif.com/projects/esp-idf/en/latest/esp32/migration-guides/release-5.x/5.0/system.html?highlight=esp_int_wdt */
#include "esp_private/esp_int_wdt.h"

#include <esp_task_wdt.h>


#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include <esp_ota_ops.h>
#include "esp_http_client.h"
#include "esp_flash_partitions.h"
#include "esp_partition.h"
#include <nvs.h>
#include "esp_app_format.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
// #include "protocol_examples_common.h"
#include "errno.h"

#include <sys/stat.h>
#include <unistd.h>

#include "MainFlowControl.h"
#include "server_file.h"
#include "server_GPIO.h"
#ifdef ENABLE_MQTT
    #include "interface_mqtt.h"
#endif //ENABLE_MQTT
#include "ClassControllCamera.h"
#include "connect_wlan.h"


#include "ClassLogFile.h"

#include "Helper.h"
#include "statusled.h"
#include "basic_auth.h"
#include "../../include/defines.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "mbedtls/sha256.h"

/*an ota data write buffer ready to write to the flash*/
static char ota_write_data[SERVER_OTA_SCRATCH_BUFSIZE + 1] = { 0 };

static const char *TAG = "OTA";

esp_err_t handler_reboot(httpd_req_t *req);
static bool ota_update_task(std::string fn);

std::string _file_name_update;
bool initial_setup = false;

static bool hex_to_bytes(const std::string &hex, uint8_t *out, size_t out_len)
{
    if (hex.size() != out_len * 2) {
        return false;
    }

    auto from_hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
        if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
        return -1;
    };

    for (size_t i = 0; i < out_len; i++) {
        int hi = from_hex(hex[i * 2]);
        int lo = from_hex(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }

    return true;
}

static bool is_valid_sha256_hex(const std::string &hex)
{
    uint8_t tmp[32];
    return hex_to_bytes(hex, tmp, sizeof(tmp));
}

static std::string bytes_to_hex(const uint8_t *bytes, size_t len)
{
    static const char *digits = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; i++) {
        out.push_back(digits[(bytes[i] >> 4) & 0xF]);
        out.push_back(digits[bytes[i] & 0xF]);
    }
    return out;
}

static esp_err_t http_get_to_string(const std::string &url, std::string &out, size_t max_bytes)
{
    out.clear();

    esp_http_client_config_t cfg = {};
    cfg.url = url.c_str();
    cfg.method = HTTP_METHOD_GET;
    cfg.timeout_ms = 15000;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return err;
    }

    int status = esp_http_client_fetch_headers(client);
    (void)status;

    char buf[512];
    while (true) {
        int r = esp_http_client_read(client, buf, sizeof(buf));
        if (r < 0) {
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return ESP_FAIL;
        }
        if (r == 0) {
            break;
        }

        if (out.size() + (size_t)r > max_bytes) {
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return ESP_ERR_INVALID_SIZE;
        }
        out.append(buf, buf + r);
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    return ESP_OK;
}

static esp_err_t http_download_to_file_with_sha256(const std::string &url,
                                                   const std::string &dest_path,
                                                   const std::string &expected_sha256_hex,
                                                   size_t max_bytes)
{
    const std::string tmp_path = dest_path + ".part";

    esp_http_client_config_t cfg = {};
    cfg.url = url.c_str();
    cfg.method = HTTP_METHOD_GET;
    cfg.timeout_ms = 30000;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        return ESP_FAIL;
    }

    FILE *fp = fopen(tmp_path.c_str(), "wb");
    if (!fp) {
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        fclose(fp);
        esp_http_client_cleanup(client);
        unlink(tmp_path.c_str());
        return err;
    }

    int http_status = esp_http_client_fetch_headers(client);
    (void)http_status;

    char buf[1024];
    size_t total = 0;
    while (true) {
        int r = esp_http_client_read(client, buf, sizeof(buf));
        if (r < 0) {
            err = ESP_FAIL;
            break;
        }
        if (r == 0) {
            err = ESP_OK;
            break;
        }

        total += (size_t)r;
        if (total > max_bytes) {
            err = ESP_ERR_INVALID_SIZE;
            break;
        }

        if (fwrite(buf, 1, (size_t)r, fp) != (size_t)r) {
            err = ESP_FAIL;
            break;
        }

        mbedtls_sha256_update(&sha, (const unsigned char *)buf, (size_t)r);
    }

    uint8_t digest[32];
    mbedtls_sha256_finish(&sha, digest);
    mbedtls_sha256_free(&sha);

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        fclose(fp);
        unlink(tmp_path.c_str());
        return err;
    }

    const std::string actual_hex = bytes_to_hex(digest, sizeof(digest));
    if (!expected_sha256_hex.empty() && actual_hex != expected_sha256_hex) {
        LogFile.WriteToFile(ESP_LOG_ERROR, TAG, "SHA256 mismatch for " + dest_path + " expected=" + expected_sha256_hex + " actual=" + actual_hex);
        fclose(fp);
        unlink(tmp_path.c_str());
        return ESP_ERR_INVALID_CRC;
    }

    if (fflush(fp) != 0) {
        fclose(fp);
        unlink(tmp_path.c_str());
        return ESP_FAIL;
    }
    if (fsync(fileno(fp)) != 0) {
        fclose(fp);
        unlink(tmp_path.c_str());
        return ESP_FAIL;
    }
    fclose(fp);

    if (rename(tmp_path.c_str(), dest_path.c_str()) != 0) {
        unlink(dest_path.c_str());
        if (rename(tmp_path.c_str(), dest_path.c_str()) != 0) {
            unlink(tmp_path.c_str());
            return ESP_FAIL;
        }
    }

    return ESP_OK;
}

static bool parse_manifest_update_zip(const std::string &manifest_json, std::string &out_url, std::string &out_sha256)
{
    out_url.clear();
    out_sha256.clear();

    cJSON *root = cJSON_Parse(manifest_json.c_str());
    if (!root) return false;

    cJSON *update = cJSON_GetObjectItemCaseSensitive(root, "update");
    cJSON *url = update ? cJSON_GetObjectItemCaseSensitive(update, "url") : NULL;
    cJSON *sha = update ? cJSON_GetObjectItemCaseSensitive(update, "sha256") : NULL;

    bool ok = cJSON_IsString(url) && (url->valuestring != NULL) && cJSON_IsString(sha) && (sha->valuestring != NULL);
    if (ok) {
        out_url = url->valuestring;
        out_sha256 = sha->valuestring;
    }
    cJSON_Delete(root);
    return ok;
}

static bool parse_manifest_model(const std::string &manifest_json, const std::string &model_name, std::string &out_url, std::string &out_sha256)
{
    out_url.clear();
    out_sha256.clear();

    cJSON *root = cJSON_Parse(manifest_json.c_str());
    if (!root) return false;

    cJSON *models = cJSON_GetObjectItemCaseSensitive(root, "models");
    if (!cJSON_IsArray(models)) {
        cJSON_Delete(root);
        return false;
    }

    bool ok = false;
    cJSON *it = NULL;
    cJSON_ArrayForEach(it, models) {
        cJSON *name = cJSON_GetObjectItemCaseSensitive(it, "name");
        cJSON *url = cJSON_GetObjectItemCaseSensitive(it, "url");
        cJSON *sha = cJSON_GetObjectItemCaseSensitive(it, "sha256");
        if (!cJSON_IsString(name) || !cJSON_IsString(url) || !cJSON_IsString(sha)) continue;
        if (model_name == name->valuestring) {
            out_url = url->valuestring;
            out_sha256 = sha->valuestring;
            ok = true;
            break;
        }
    }

    cJSON_Delete(root);
    return ok;
}

static void send_bad_gateway(httpd_req_t *req, const char *message)
{
    httpd_resp_set_status(req, "502 Bad Gateway");
    httpd_resp_set_type(req, HTTPD_TYPE_TEXT);
    httpd_resp_send(req, message, HTTPD_RESP_USE_STRLEN);
}


static void infinite_loop(void)
{
    int i = 0;
    LogFile.WriteToFile(ESP_LOG_INFO, TAG, "When a new firmware is available on the server, press the reset button to download it");
    while(1) {
        LogFile.WriteToFile(ESP_LOG_INFO, TAG, "Waiting for a new firmware... (" + to_string(++i) + ")");
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}


void task_do_Update_ZIP(void *pvParameter)
{
    StatusLED(AP_OR_OTA, 1, true);  // Signaling an OTA update
    
    std::string filetype = toUpper(getFileType(_file_name_update));

  	LogFile.WriteToFile(ESP_LOG_INFO, TAG, "File: " + _file_name_update + " Filetype: " + filetype);

    if (filetype == "ZIP")
    {
        std::string in, outHtml, outHtmlTmp, outHtmlOld, outbin, zw, retfirmware;

        outHtml = "/sdcard/html";
        outHtmlTmp = "/sdcard/html_tmp";
        outHtmlOld = "/sdcard/html_old";
        outbin = "/sdcard/firmware";

        /* Remove the old and tmp html folder in case they still exist */
        removeFolder(outHtmlTmp.c_str(), TAG);
        removeFolder(outHtmlOld.c_str(), TAG);

        /* Extract the ZIP file. The content of the html folder gets extracted to the temporar folder html-temp. */
        LogFile.WriteToFile(ESP_LOG_INFO, TAG, "Extracting ZIP file " + _file_name_update + "...");
        retfirmware = unzip_new(_file_name_update, outHtmlTmp+"/", outHtml+"/", outbin+"/", "/sdcard/", initial_setup);
    	LogFile.WriteToFile(ESP_LOG_INFO, TAG, "Files unzipped.");

        /* ZIP file got extracted, replace the old html folder with the new one */
        LogFile.WriteToFile(ESP_LOG_INFO, TAG, "Renaming folder " + outHtml + " to " + outHtmlOld + "...");
        RenameFolder(outHtml, outHtmlOld);
        LogFile.WriteToFile(ESP_LOG_INFO, TAG, "Renaming folder " + outHtmlTmp + " to " + outHtml + "...");
        RenameFolder(outHtmlTmp, outHtml);
        LogFile.WriteToFile(ESP_LOG_INFO, TAG, "Deleting folder " + outHtmlOld + "...");
        removeFolder(outHtmlOld.c_str(), TAG);

        if (retfirmware.length() > 0)
        {
            LogFile.WriteToFile(ESP_LOG_INFO, TAG, "Found firmware.bin");
            ota_update_task(retfirmware);
        }

        LogFile.WriteToFile(ESP_LOG_INFO, TAG, "Trigger reboot due to firmware update");
        doRebootOTA();
    } else if (filetype == "BIN")
    {
       	LogFile.WriteToFile(ESP_LOG_INFO, TAG, "Do firmware update - file: " + _file_name_update);
        ota_update_task(_file_name_update);
        LogFile.WriteToFile(ESP_LOG_INFO, TAG, "Trigger reboot due to firmware update");
        doRebootOTA();
    }
    else
    {
    	LogFile.WriteToFile(ESP_LOG_ERROR, TAG, "Only ZIP-Files support for update during startup!");
    }
}


void CheckUpdate()
{
 	FILE *pfile;
    if ((pfile = fopen("/sdcard/update.txt", "r")) == NULL)
    {
		LogFile.WriteToFile(ESP_LOG_DEBUG, TAG, "No pending update");
        return;
	}

	char zw[1024] = "";
	fgets(zw, 1024, pfile);
    _file_name_update = std::string(zw);
    if (fgets(zw, 1024, pfile))
	{
		std::string _szw = std::string(zw);
        if (_szw == "init")
        {
       		LogFile.WriteToFile(ESP_LOG_INFO, TAG, "Inital Setup triggered");
        }
	}

    fclose(pfile);
    DeleteFile("/sdcard/update.txt");   // Prevent Boot Loop!!!
	LogFile.WriteToFile(ESP_LOG_INFO, TAG, "Start update process (" + _file_name_update + ")");


    xTaskCreate(&task_do_Update_ZIP, "task_do_Update_ZIP", configMINIMAL_STACK_SIZE * 35, NULL, tskIDLE_PRIORITY+1, NULL);
    while(1) { // wait until reboot within task_do_Update_ZIP
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}


static bool ota_update_task(std::string fn)
{
    esp_err_t err;
    /* update handle : set by esp_ota_begin(), must be freed via esp_ota_end() */
    esp_ota_handle_t update_handle = 0 ;
    const esp_partition_t *update_partition = NULL;

    ESP_LOGI(TAG, "Starting OTA update");

    const esp_partition_t *configured = esp_ota_get_boot_partition();
    const esp_partition_t *running = esp_ota_get_running_partition();

    if (configured != running) {        
        LogFile.WriteToFile(ESP_LOG_ERROR, TAG, "Configured OTA boot partition at offset " + to_string(configured->address) + 
                ", but running from offset " + to_string(running->address));
        LogFile.WriteToFile(ESP_LOG_ERROR, TAG, "(This can happen if either the OTA boot data or preferred boot image become somehow corrupted.)");
    }
    ESP_LOGI(TAG, "Running partition type %d subtype %d (offset 0x%08x)",
             running->type, running->subtype, (unsigned int)running->address);


    update_partition = esp_ota_get_next_update_partition(NULL);
    ESP_LOGI(TAG, "Writing to partition subtype %d at offset 0x%x",
             update_partition->subtype, (unsigned int)update_partition->address);
//    assert(update_partition != NULL);

    int binary_file_length = 0;

    // deal with all receive packet 
    bool image_header_was_checked = false;

    int data_read;     

    FILE* f = fopen(fn.c_str(), "rb");     // previously only "r

    if (f == NULL) { // File does not exist
        return false;
    }

    data_read = fread(ota_write_data, 1, SERVER_OTA_SCRATCH_BUFSIZE, f);

    while (data_read > 0) {
        if (data_read < 0) {
            LogFile.WriteToFile(ESP_LOG_ERROR, TAG, "Error: SSL data read error");
            return false;
        } else if (data_read > 0) {
            if (image_header_was_checked == false) {
                esp_app_desc_t new_app_info;
                if (data_read > sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t)) {
                    // check current version with downloading
                    memcpy(&new_app_info, &ota_write_data[sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t)], sizeof(esp_app_desc_t));
                    ESP_LOGI(TAG, "New firmware version: %s", new_app_info.version);

                    esp_app_desc_t running_app_info;
                    if (esp_ota_get_partition_description(running, &running_app_info) == ESP_OK) {
                        ESP_LOGI(TAG, "Running firmware version: %s", running_app_info.version);
                    }

                    const esp_partition_t* last_invalid_app = esp_ota_get_last_invalid_partition();
                    esp_app_desc_t invalid_app_info;
                    if (esp_ota_get_partition_description(last_invalid_app, &invalid_app_info) == ESP_OK) {
                        ESP_LOGI(TAG, "Last invalid firmware version: %s", invalid_app_info.version);
                    }

                    // check current version with last invalid partition
                    if (last_invalid_app != NULL) {
                        if (memcmp(invalid_app_info.version, new_app_info.version, sizeof(new_app_info.version)) == 0) {
                            LogFile.WriteToFile(ESP_LOG_WARN, TAG, "New version is the same as invalid version");
                            LogFile.WriteToFile(ESP_LOG_WARN, TAG, "Previously, there was an attempt to launch the firmware with " + 
                                    string(invalid_app_info.version) + " version, but it failed");
                            LogFile.WriteToFile(ESP_LOG_WARN, TAG, "The firmware has been rolled back to the previous version");
                            infinite_loop();
                        }
                    }

/*
                    if (memcmp(new_app_info.version, running_app_info.version, sizeof(new_app_info.version)) == 0) {
                        LogFile.WriteToFile(ESP_LOG_WARN, TAG, "Current running version is the same as a new. We will not continue the update");
                        infinite_loop();
                    }
*/
                    image_header_was_checked = true;

                    err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &update_handle);
                    if (err != ESP_OK) {
                        LogFile.WriteToFile(ESP_LOG_ERROR, TAG, "esp_ota_begin failed (" + string(esp_err_to_name(err)) + ")");
                        return false;
                    }
                    ESP_LOGI(TAG, "esp_ota_begin succeeded");
                } else {
                    LogFile.WriteToFile(ESP_LOG_ERROR, TAG, "received package is not fit len");
                    return false;
                }
            }            
            err = esp_ota_write( update_handle, (const void *)ota_write_data, data_read);
            if (err != ESP_OK) {
                return false;
            }
            binary_file_length += data_read;
            ESP_LOGD(TAG, "Written image length %d", binary_file_length);
        } else if (data_read == 0) {
           //
           // * As esp_http_client_read never returns negative error code, we rely on
           // * `errno` to check for underlying transport connectivity closure if any
           //
            if (errno == ECONNRESET || errno == ENOTCONN) {
                LogFile.WriteToFile(ESP_LOG_ERROR, TAG, "Connection closed, errno = " + to_string(errno));
                break;
            }
        }
        data_read = fread(ota_write_data, 1, SERVER_OTA_SCRATCH_BUFSIZE, f);
    }
    fclose(f);  

    ESP_LOGI(TAG, "Total Write binary data length: %d", binary_file_length);

    err = esp_ota_end(update_handle);
    if (err != ESP_OK) {
        if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
            LogFile.WriteToFile(ESP_LOG_ERROR, TAG, "Image validation failed, image is corrupted");
        }
        LogFile.WriteToFile(ESP_LOG_ERROR, TAG, "esp_ota_end failed (" + string(esp_err_to_name(err)) + ")!");
        return false;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        LogFile.WriteToFile(ESP_LOG_ERROR, TAG, "esp_ota_set_boot_partition failed (" + string(esp_err_to_name(err)) + ")!");

    }
//    ESP_LOGI(TAG, "Prepare to restart system!");
//    esp_restart();

    return true ;
}


static void print_sha256 (const uint8_t *image_hash, const char *label)
{
    char hash_print[HASH_LEN * 2 + 1];
    hash_print[HASH_LEN * 2] = 0;
    for (int i = 0; i < HASH_LEN; ++i) {
        sprintf(&hash_print[i * 2], "%02x", image_hash[i]);
    }
    ESP_LOGI(TAG, "%s: %s", label, hash_print);
}


static bool diagnostic(void)
{
    return true;
}

static bool is_running_partition_pending_verify()
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!running) {
        return false;
    }

    esp_ota_img_states_t ota_state;
    if (esp_ota_get_state_partition(running, &ota_state) != ESP_OK) {
        return false;
    }

    return ota_state == ESP_OTA_IMG_PENDING_VERIFY;
}

static TaskHandle_t ota_verify_task_handle = NULL;

static void ota_verify_monitor_task(void *pvParameter)
{
    const int timeout_seconds = (int)(intptr_t)pvParameter;
    const TickType_t delay = 1000 / portTICK_PERIOD_MS;

    LogFile.WriteToFile(ESP_LOG_INFO, TAG, "OTA: running image is pending verify; waiting up to " + std::to_string(timeout_seconds) +
                                           "s before confirming");

    bool saw_wifi = false;
    for (int elapsed = 0; elapsed < timeout_seconds; elapsed++) {
        if (getWIFIisConnected()) {
            saw_wifi = true;
            break;
        }
        vTaskDelay(delay);
    }

    if (!is_running_partition_pending_verify()) {
        LogFile.WriteToFile(ESP_LOG_INFO, TAG, "OTA: running image no longer pending verify; nothing to do");
        ota_verify_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    const bool diagnostic_is_ok = diagnostic();
    if (!diagnostic_is_ok) {
        LogFile.WriteToFile(ESP_LOG_ERROR, TAG, "OTA: diagnostics failed; rolling back to previous firmware");
        esp_ota_mark_app_invalid_rollback_and_reboot();
        ota_verify_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    if (saw_wifi) {
        LogFile.WriteToFile(ESP_LOG_INFO, TAG, "OTA: Wi-Fi connected; confirming new firmware");
    } else {
        LogFile.WriteToFile(ESP_LOG_WARN, TAG, "OTA: Wi-Fi not connected within timeout; confirming new firmware anyway");
    }

    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err != ESP_OK) {
        LogFile.WriteToFile(ESP_LOG_ERROR, TAG, "OTA: failed to mark app valid (" + std::string(esp_err_to_name(err)) + ")");
    } else {
        LogFile.WriteToFile(ESP_LOG_INFO, TAG, "OTA: marked app valid; rollback canceled");
    }

    ota_verify_task_handle = NULL;
    vTaskDelete(NULL);
}

void StartOTAVerifyMonitor()
{
    if (!is_running_partition_pending_verify()) {
        return;
    }

    if (ota_verify_task_handle != NULL) {
        return;
    }

    static constexpr int kTimeoutSeconds = 180;
    xTaskCreate(&ota_verify_monitor_task, "ota_verify", 4096, (void *)(intptr_t)kTimeoutSeconds, 5, &ota_verify_task_handle);
}


void CheckOTAUpdate(void)
{
    ESP_LOGI(TAG, "Start CheckOTAUpdateCheck...");

    uint8_t sha_256[HASH_LEN] = { 0 };
    esp_partition_t partition;

    // get sha256 digest for the partition table
    partition.address   = ESP_PARTITION_TABLE_OFFSET;
    partition.size      = ESP_PARTITION_TABLE_MAX_LEN;
    partition.type      = ESP_PARTITION_TYPE_DATA;
    esp_partition_get_sha256(&partition, sha_256);
    print_sha256(sha_256, "SHA-256 for the partition table: ");

    // get sha256 digest for bootloader
    partition.address   = ESP_BOOTLOADER_OFFSET;
    partition.size      = ESP_PARTITION_TABLE_OFFSET;
    partition.type      = ESP_PARTITION_TYPE_APP;
    esp_partition_get_sha256(&partition, sha_256);
    print_sha256(sha_256, "SHA-256 for bootloader: ");

    // get sha256 digest for running partition
    esp_partition_get_sha256(esp_ota_get_running_partition(), sha_256);
    print_sha256(sha_256, "SHA-256 for current firmware: ");

    if (is_running_partition_pending_verify()) {
        LogFile.WriteToFile(ESP_LOG_WARN, TAG, "OTA: running image is pending verification; will confirm after web server start");
    }
}


esp_err_t handler_ota_update(httpd_req_t *req)
{
#ifdef DEBUG_DETAIL_ON     
    LogFile.WriteHeapInfo("handler_ota_update - Start");    
#endif

    LogFile.WriteToFile(ESP_LOG_DEBUG, TAG, "handler_ota_update");
    char _query[600];
    char _filename[100];
    char _valuechar[30];
    char _manifest_url[420] = {0};
    char _model_name[100] = {0};
    std::string fn = "/sdcard/firmware/";
    bool _file_del = false;
    std::string _task = "";

    if (httpd_req_get_url_query_str(req, _query, sizeof(_query)) == ESP_OK)
    {
        ESP_LOGD(TAG, "Query: %s", _query);
        
        if (httpd_query_key_value(_query, "task", _valuechar, 30) == ESP_OK)
        {
            ESP_LOGD(TAG, "task is found: %s", _valuechar);
            _task = std::string(_valuechar);
        }

        if (httpd_query_key_value(_query, "file", _filename, 100) == ESP_OK)
        {
            fn.append(_filename);
            ESP_LOGD(TAG, "File: %s", fn.c_str());
        }
        if (httpd_query_key_value(_query, "delete", _filename, 100) == ESP_OK)
        {
            fn.append(_filename);
            _file_del = true;
            ESP_LOGD(TAG, "Delete Default File: %s", fn.c_str());
        }

        httpd_query_key_value(_query, "manifest", _manifest_url, sizeof(_manifest_url) - 1);
        httpd_query_key_value(_query, "model", _model_name, sizeof(_model_name) - 1);

    }

    if (_task.compare("download_update") == 0)
    {
        const std::string manifest_url = UrlDecode(std::string(_manifest_url));
        if (manifest_url.empty()) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing manifest URL (?manifest=...)");
            return ESP_OK;
        }

        LogFile.WriteToFile(ESP_LOG_INFO, TAG, "Downloading update manifest: " + manifest_url);
        std::string manifest;
        esp_err_t err = http_get_to_string(manifest_url, manifest, 32 * 1024);
        if (err != ESP_OK) {
            LogFile.WriteToFile(ESP_LOG_ERROR, TAG, "Failed to fetch manifest: " + std::to_string(err));
            send_bad_gateway(req, "Failed to fetch manifest");
            return ESP_OK;
        }

        std::string update_url;
        std::string update_sha256;
        if (!parse_manifest_update_zip(manifest, update_url, update_sha256)) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid manifest (expected keys: update.url, update.sha256)");
            return ESP_OK;
        }

        if (!is_valid_sha256_hex(update_sha256)) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid update sha256 in manifest");
            return ESP_OK;
        }

        const std::string dest = "/sdcard/firmware/github_update.zip";
        LogFile.WriteToFile(ESP_LOG_INFO, TAG, "Downloading update: " + update_url);
        err = http_download_to_file_with_sha256(update_url, dest, update_sha256, MAX_FILE_SIZE);
        if (err != ESP_OK) {
            LogFile.WriteToFile(ESP_LOG_ERROR, TAG, "Update download failed: " + std::to_string(err));
            send_bad_gateway(req, "Update download failed");
            return ESP_OK;
        }

        FILE *pfile = fopen("/sdcard/update.txt", "w");
        if (!pfile) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to schedule update");
            return ESP_OK;
        }
        fwrite(dest.c_str(), dest.length(), 1, pfile);
        fclose(pfile);

        httpd_resp_sendstr_chunk(req, "reboot\n");
        httpd_resp_sendstr_chunk(req, NULL);
        return ESP_OK;
    }

    if (_task.compare("download_model") == 0)
    {
        const std::string manifest_url = UrlDecode(std::string(_manifest_url));
        const std::string model_name = UrlDecode(std::string(_model_name));
        if (manifest_url.empty() || model_name.empty()) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing manifest URL or model name (?manifest=...&model=...)");
            return ESP_OK;
        }

        if (model_name.find('/') != std::string::npos || model_name.find('\\') != std::string::npos) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid model name");
            return ESP_OK;
        }

        LogFile.WriteToFile(ESP_LOG_INFO, TAG, "Downloading model manifest: " + manifest_url);
        std::string manifest;
        esp_err_t err = http_get_to_string(manifest_url, manifest, 64 * 1024);
        if (err != ESP_OK) {
            send_bad_gateway(req, "Failed to fetch manifest");
            return ESP_OK;
        }

        std::string model_url;
        std::string model_sha256;
        if (!parse_manifest_model(manifest, model_name, model_url, model_sha256)) {
            httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Model not found in manifest");
            return ESP_OK;
        }

        if (!is_valid_sha256_hex(model_sha256)) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid model sha256 in manifest");
            return ESP_OK;
        }

        const std::string dest = "/sdcard/config/" + model_name;
        LogFile.WriteToFile(ESP_LOG_INFO, TAG, "Downloading model: " + model_url);
        err = http_download_to_file_with_sha256(model_url, dest, model_sha256, 3 * 1024 * 1024);
        if (err != ESP_OK) {
            send_bad_gateway(req, "Model download failed");
            return ESP_OK;
        }

        httpd_resp_sendstr_chunk(req, "ok\n");
        httpd_resp_sendstr_chunk(req, NULL);
        return ESP_OK;
    }

    if (_task.compare("emptyfirmwaredir") == 0)
    {
        ESP_LOGD(TAG, "Start empty directory /firmware");
        delete_all_in_directory("/sdcard/firmware");
        std::string zw = "firmware directory deleted - v2\n";
        ESP_LOGD(TAG, "%s", zw.c_str());
        printf("Ausgabe: %s\n", zw.c_str());
    
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        httpd_resp_send(req, zw.c_str(), strlen(zw.c_str())); 
        /* Respond with an empty chunk to signal HTTP response completion */
        httpd_resp_send_chunk(req, NULL, 0);  

        ESP_LOGD(TAG, "Done empty directory /firmware");
        return ESP_OK;
    }

    if (_task.compare("update") == 0)
    {
        std::string filetype = toUpper(getFileType(fn));
        if (filetype.length() == 0)
        {
            std::string zw = "Update failed - no file specified (zip, bin, tfl, tlite)";
            httpd_resp_sendstr_chunk(req, zw.c_str());
            httpd_resp_sendstr_chunk(req, NULL);  
            return ESP_OK;        
        }

        if ((filetype == "TFLITE") || (filetype == "TFL"))
        {
            std::string out = "/sdcard/config/" + getFileFullFileName(fn);
            DeleteFile(out);
            CopyFile(fn, out);
            DeleteFile(fn);

            const char*  resp_str = "Neural Network File copied.";
            httpd_resp_sendstr_chunk(req, resp_str);
            httpd_resp_sendstr_chunk(req, NULL);  
            return ESP_OK;
        }


        if ((filetype == "ZIP") || (filetype == "BIN"))
        {
           	FILE *pfile;
            LogFile.WriteToFile(ESP_LOG_INFO, TAG, "Update for reboot");
            pfile = fopen("/sdcard/update.txt", "w");
            fwrite(fn.c_str(), fn.length(), 1, pfile);
            fclose(pfile);

            std::string zw = "reboot\n";
            httpd_resp_sendstr_chunk(req, zw.c_str());
            httpd_resp_sendstr_chunk(req, NULL);  
            ESP_LOGD(TAG, "Send reboot");
            return ESP_OK;                

        }

/*
        if (filetype == "BIN")
        {
            const char* resp_str; 

            DeleteMainFlowTask();
            gpio_handler_deinit();
            if (ota_update_task(fn))
            {
                std::string zw = "reboot\n";
                httpd_resp_sendstr_chunk(req, zw.c_str());
                httpd_resp_sendstr_chunk(req, NULL);  
                ESP_LOGD(TAG, "Send reboot");
                return ESP_OK;                
            }

            resp_str = "Error during Firmware Update!!!\nPlease check output of console.";
            httpd_resp_send(req, resp_str, strlen(resp_str));  

            #ifdef DEBUG_DETAIL_ON 
                LogFile.WriteHeapInfo("handler_ota_update - Done");    
            #endif

            return ESP_OK;
        }
*/

        std::string zw = "Update failed - no valid file specified (zip, bin, tfl, tlite)!";
        httpd_resp_sendstr_chunk(req, zw.c_str());
        httpd_resp_sendstr_chunk(req, NULL);  
        return ESP_OK;        
    }


    if (_task.compare("unziphtml") == 0)
    {
        ESP_LOGD(TAG, "Task unziphtml");
        std::string in, out, zw;

        in = "/sdcard/firmware/html.zip";
        out = "/sdcard/html";

        delete_all_in_directory(out);

        unzip(in, out+"/");
        zw = "Web Interface Update Successfull!\nNo reboot necessary";
        httpd_resp_send(req, zw.c_str(), strlen(zw.c_str()));
        httpd_resp_sendstr_chunk(req, NULL);  
        return ESP_OK;        
    }

    if (_file_del)
    {
        ESP_LOGD(TAG, "Delete !! _file_del: %s", fn.c_str());
        struct stat file_stat;
        int _result = stat(fn.c_str(), &file_stat);
        ESP_LOGD(TAG, "Ergebnis %d\n", _result);
        if (_result == 0) {
            ESP_LOGD(TAG, "Deleting file: %s", fn.c_str());
            /* Delete file */
            unlink(fn.c_str());
        }
        else
        {
            LogFile.WriteToFile(ESP_LOG_ERROR, TAG, "File does not exist: " + fn);
        }
        /* Respond with an empty chunk to signal HTTP response completion */
        std::string zw = "file deleted\n";
        ESP_LOGD(TAG, "%s", zw.c_str());
        httpd_resp_send(req, zw.c_str(), strlen(zw.c_str()));
        httpd_resp_send_chunk(req, NULL, 0);
        return ESP_OK;
    }

    string zw = "ota without parameter - should not be the case!";
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, zw.c_str(), strlen(zw.c_str())); 
    httpd_resp_send_chunk(req, NULL, 0);  

    LogFile.WriteToFile(ESP_LOG_ERROR, TAG, "ota without parameter - should not be the case!");

/*  
    const char* resp_str;    

    DeleteMainFlowTask();
    gpio_handler_deinit();
    if (ota_update_task(fn))
    {
        resp_str = "Firmware Update Successfull! You can restart now.";
    }
    else
    {
        resp_str = "Error during Firmware Update!!! Please check console output.";
    }

    httpd_resp_send(req, resp_str, strlen(resp_str));  
*/

    #ifdef DEBUG_DETAIL_ON 
        LogFile.WriteHeapInfo("handler_ota_update - Done");    
    #endif

    return ESP_OK;
}


void hard_restart() 
{
  esp_task_wdt_config_t twdt_config = {
    .timeout_ms = 1,
    .idle_core_mask = (1 << portNUM_PROCESSORS) - 1,    // Bitmask of all cores
    .trigger_panic = true,
  };
  ESP_ERROR_CHECK(esp_task_wdt_init(&twdt_config));

  esp_task_wdt_add(NULL);
  while(true);
}


void task_reboot(void *DeleteMainFlow)
{
    // write a reboot, to identify a reboot by purpouse
    FILE* pfile = fopen("/sdcard/reboot.txt", "w");
    std::string _s_zw= "reboot";
    fwrite(_s_zw.c_str(), strlen(_s_zw.c_str()), 1, pfile);
    fclose(pfile);

    vTaskDelay(3000 / portTICK_PERIOD_MS);

    if ((bool)DeleteMainFlow) {
        DeleteMainFlowTask();  // Kill autoflow task if executed in extra task, if not don't kill parent task
    }

    Camera.LightOnOff(false);
    StatusLEDOff();

    /* Stop service tasks */
    #ifdef ENABLE_MQTT
        MQTTdestroy_client(true);
    #endif //ENABLE_MQTT
    gpio_handler_destroy();
    esp_camera_deinit();
    WIFIDestroy();

    vTaskDelay(3000 / portTICK_PERIOD_MS);
    esp_restart();      // Reset type: CPU reset (Reset both CPUs)

    vTaskDelay(5000 / portTICK_PERIOD_MS);
    hard_restart();     // Reset type: System reset (Triggered by watchdog), if esp_restart stalls (WDT needs to be activated)

    LogFile.WriteToFile(ESP_LOG_ERROR, TAG, "Reboot failed!");
    vTaskDelete(NULL); //Delete this task if it comes to this point
}


void doReboot()
{
    LogFile.WriteToFile(ESP_LOG_INFO, TAG, "Reboot triggered by Software (5s)");
    LogFile.WriteToFile(ESP_LOG_WARN, TAG, "Reboot in 5sec");

    BaseType_t xReturned = xTaskCreate(&task_reboot, "task_reboot", configMINIMAL_STACK_SIZE * 4, (void*) true, 10, NULL);
    if( xReturned != pdPASS )
    {
        LogFile.WriteToFile(ESP_LOG_ERROR, TAG, "task_reboot not created -> force reboot without killing flow");
        task_reboot((void*) false);
    }
    vTaskDelay(10000 / portTICK_PERIOD_MS); // Prevent serving web client fetch response until system is shuting down
}


void doRebootOTA()
{
    LogFile.WriteToFile(ESP_LOG_WARN, TAG, "Reboot in 5sec");

    Camera.LightOnOff(false);
    StatusLEDOff();
    esp_camera_deinit();

    vTaskDelay(5000 / portTICK_PERIOD_MS);
    esp_restart();      // Reset type: CPU reset (Reset both CPUs)

    vTaskDelay(5000 / portTICK_PERIOD_MS);
    hard_restart();     // Reset type: System reset (Triggered by watchdog), if esp_restart stalls (WDT needs to be activated)
}


esp_err_t handler_reboot(httpd_req_t *req)
{
    #ifdef DEBUG_DETAIL_ON     
        LogFile.WriteHeapInfo("handler_reboot - Start");
    #endif    

    LogFile.WriteToFile(ESP_LOG_DEBUG, TAG, "handler_reboot");
    LogFile.WriteToFile(ESP_LOG_INFO, TAG, "!!! System will restart within 5 sec!!!");

    std::string response = 
        "<html><head><script>"
            "function m(h) {"
                "document.getElementById('t').innerHTML=h;"
                "setInterval(function (){h +='.'; document.getElementById('t').innerHTML=h;"
                "fetch('reboot_page.html',{mode: 'no-cors'}).then(r=>{parent.location.href=('index.html');})}, 1000);"
            "}</script></head></html><body style='font-family: arial'><h3 id=t></h3>"
            "<script>m('Rebooting!<br>The page will automatically reload in around 25..60s.<br><br>');</script>"
            "</body></html>";

    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, response.c_str(), strlen(response.c_str()));
    
    doReboot();

    #ifdef DEBUG_DETAIL_ON 
        LogFile.WriteHeapInfo("handler_reboot - Done");    
    #endif

    return ESP_OK;
}


void register_server_ota_sdcard_uri(httpd_handle_t server)
{
    ESP_LOGI(TAG, "Registering URI handlers");
    
    httpd_uri_t camuri = { };
    camuri.method    = HTTP_GET;
    camuri.uri       = "/ota";
    camuri.handler = APPLY_BASIC_AUTH_FILTER(handler_ota_update);
    camuri.user_ctx  = (void*) "Do OTA";    
    httpd_register_uri_handler(server, &camuri);

    camuri.method    = HTTP_GET;
    camuri.uri       = "/reboot";
    camuri.handler = APPLY_BASIC_AUTH_FILTER(handler_reboot);
    camuri.user_ctx  = (void*) "Reboot";    
    httpd_register_uri_handler(server, &camuri);

}
