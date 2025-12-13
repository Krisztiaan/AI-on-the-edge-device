set(app_sources
    md5.cpp
    server_file.cpp
    server_help.cpp
    server_ota.cpp
    third_party/ed25519-donna/ed25519.c
)

if(CONFIG_JOMJOL_ENABLE_ZIP_UPDATES)
    list(APPEND app_sources miniz/miniz.c)
endif()

idf_component_register(SRCS ${app_sources}
                    INCLUDE_DIRS "." "../../include" "miniz"
                    REQUIRES vfs esp_http_server app_update esp_http_client nvs_flash jomjol_tfliteclass jomjol_flowcontroll spiffs jomjol_helper jomjol_controlGPIO json)
