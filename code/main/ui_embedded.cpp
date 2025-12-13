#include "ui_embedded.h"

#include <string.h>

static const char *content_type_for_name(const char *name)
{
    const char *ext = strrchr(name, '.');
    if (!ext) return "application/octet-stream";
    ext++;

    if (strcmp(ext, "html") == 0) return "text/html";
    if (strcmp(ext, "js") == 0) return "application/javascript";
    if (strcmp(ext, "css") == 0) return "text/css";
    if (strcmp(ext, "txt") == 0) return "text/plain";
    return "application/octet-stream";
}

struct EmbeddedFile
{
    const char *name; // e.g. "index.html"
    const uint8_t *start;
    const uint8_t *end;
};

extern const uint8_t _binary_common_js_start[];            extern const uint8_t _binary_common_js_end[];
extern const uint8_t _binary_data_html_start[];            extern const uint8_t _binary_data_html_end[];
extern const uint8_t _binary_edit_alignment_html_start[];  extern const uint8_t _binary_edit_alignment_html_end[];
extern const uint8_t _binary_edit_analog_html_start[];     extern const uint8_t _binary_edit_analog_html_end[];
extern const uint8_t _binary_edit_config_raw_html_start[]; extern const uint8_t _binary_edit_config_raw_html_end[];
extern const uint8_t _binary_edit_config_template_html_start[]; extern const uint8_t _binary_edit_config_template_html_end[];
extern const uint8_t _binary_edit_digits_html_start[];     extern const uint8_t _binary_edit_digits_html_end[];
extern const uint8_t _binary_edit_reference_html_start[];  extern const uint8_t _binary_edit_reference_html_end[];
extern const uint8_t _binary_edit_style_css_start[];       extern const uint8_t _binary_edit_style_css_end[];
extern const uint8_t _binary_file_server_css_start[];      extern const uint8_t _binary_file_server_css_end[];
extern const uint8_t _binary_file_server_js_start[];       extern const uint8_t _binary_file_server_js_end[];
extern const uint8_t _binary_firework_css_start[];         extern const uint8_t _binary_firework_css_end[];
extern const uint8_t _binary_firework_js_start[];          extern const uint8_t _binary_firework_js_end[];
extern const uint8_t _binary_index_html_start[];           extern const uint8_t _binary_index_html_end[];
extern const uint8_t _binary_info_html_start[];            extern const uint8_t _binary_info_html_end[];
extern const uint8_t _binary_log_html_start[];             extern const uint8_t _binary_log_html_end[];
extern const uint8_t _binary_md5_min_js_start[];           extern const uint8_t _binary_md5_min_js_end[];
extern const uint8_t _binary_ota_page_html_start[];        extern const uint8_t _binary_ota_page_html_end[];
extern const uint8_t _binary_overview_html_start[];        extern const uint8_t _binary_overview_html_end[];
extern const uint8_t _binary_prevalue_set_html_start[];    extern const uint8_t _binary_prevalue_set_html_end[];
extern const uint8_t _binary_readconfigcommon_js_start[];  extern const uint8_t _binary_readconfigcommon_js_end[];
extern const uint8_t _binary_readconfigparam_js_start[];   extern const uint8_t _binary_readconfigparam_js_end[];
extern const uint8_t _binary_reboot_page_html_start[];     extern const uint8_t _binary_reboot_page_html_end[];
extern const uint8_t _binary_style_css_start[];            extern const uint8_t _binary_style_css_end[];
extern const uint8_t _binary_timezones_html_start[];       extern const uint8_t _binary_timezones_html_end[];

static const EmbeddedFile kFiles[] = {
    {"common.js", _binary_common_js_start, _binary_common_js_end},
    {"data.html", _binary_data_html_start, _binary_data_html_end},
    {"edit_alignment.html", _binary_edit_alignment_html_start, _binary_edit_alignment_html_end},
    {"edit_analog.html", _binary_edit_analog_html_start, _binary_edit_analog_html_end},
    {"edit_config_raw.html", _binary_edit_config_raw_html_start, _binary_edit_config_raw_html_end},
    {"edit_config_template.html", _binary_edit_config_template_html_start, _binary_edit_config_template_html_end},
    {"edit_digits.html", _binary_edit_digits_html_start, _binary_edit_digits_html_end},
    {"edit_reference.html", _binary_edit_reference_html_start, _binary_edit_reference_html_end},
    {"edit_style.css", _binary_edit_style_css_start, _binary_edit_style_css_end},
    {"file_server.css", _binary_file_server_css_start, _binary_file_server_css_end},
    {"file_server.js", _binary_file_server_js_start, _binary_file_server_js_end},
    {"firework.css", _binary_firework_css_start, _binary_firework_css_end},
    {"firework.js", _binary_firework_js_start, _binary_firework_js_end},
    {"index.html", _binary_index_html_start, _binary_index_html_end},
    {"info.html", _binary_info_html_start, _binary_info_html_end},
    {"log.html", _binary_log_html_start, _binary_log_html_end},
    {"md5.min.js", _binary_md5_min_js_start, _binary_md5_min_js_end},
    {"ota_page.html", _binary_ota_page_html_start, _binary_ota_page_html_end},
    {"overview.html", _binary_overview_html_start, _binary_overview_html_end},
    {"prevalue_set.html", _binary_prevalue_set_html_start, _binary_prevalue_set_html_end},
    {"readconfigcommon.js", _binary_readconfigcommon_js_start, _binary_readconfigcommon_js_end},
    {"readconfigparam.js", _binary_readconfigparam_js_start, _binary_readconfigparam_js_end},
    {"reboot_page.html", _binary_reboot_page_html_start, _binary_reboot_page_html_end},
    {"style.css", _binary_style_css_start, _binary_style_css_end},
    {"timezones.html", _binary_timezones_html_start, _binary_timezones_html_end},
};

static const EmbeddedFile *find_embedded(const char *name)
{
    for (size_t i = 0; i < sizeof(kFiles) / sizeof(kFiles[0]); i++) {
        if (strcmp(kFiles[i].name, name) == 0) return &kFiles[i];
    }
    return NULL;
}

esp_err_t ui_embedded_handler(httpd_req_t *req)
{
    const char *uri = req->uri;
    if (strncmp(uri, "/ui", 3) != 0) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
    }

    const char *name = uri + 3;
    if (*name == '\0' || strcmp(name, "/") == 0) {
        name = "index.html";
    } else {
        if (*name == '/') name++;
    }

    // Strip query string if any
    char name_buf[96];
    const char *q = strchr(name, '?');
    if (q) {
        size_t n = (size_t)(q - name);
        if (n >= sizeof(name_buf)) {
            return httpd_resp_send_err(req, HTTPD_414_URI_TOO_LONG, "URI too long");
        }
        memcpy(name_buf, name, n);
        name_buf[n] = '\0';
        name = name_buf;
    }

    if (strstr(name, "..")) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad path");
    }

    const EmbeddedFile *file = find_embedded(name);
    if (!file) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
    }

    httpd_resp_set_type(req, content_type_for_name(file->name));
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, (const char *)file->start, (size_t)(file->end - file->start));
}
