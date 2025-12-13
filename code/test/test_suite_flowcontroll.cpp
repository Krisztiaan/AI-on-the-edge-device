#include <unity.h>

// Storage ////////////////////
#include "nvs_flash.h"
#include "esp_vfs_fat.h"
#include "wear_levelling.h"
//static const char *TAG = "MAIN TEST";
#include "server_GPIO.h"


//*****************************************************************************
// Include files with functions to test
//*****************************************************************************
#include "components/jomjol-flowcontroll/test_flow_postrocess_helper.cpp"
#include "components/jomjol-flowcontroll/test_flowpostprocessing.cpp"
#include "components/jomjol-flowcontroll/test_flow_pp_negative.cpp"
#include "components/jomjol-flowcontroll/test_PointerEvalAnalogToDigitNew.cpp"
#include "components/jomjol-flowcontroll/test_getReadoutRawString.cpp"
#include "components/jomjol-flowcontroll/test_cnnflowcontroll.cpp"
#include "components/openmetrics/test_openmetrics.cpp"
#include "components/jomjol_mqtt/test_server_mqtt.cpp"

static bool Init_NVS_Storage()
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %d", (int)ret);
        return false;
    }

    static wl_handle_t wl_handle = WL_INVALID_HANDLE;
    const esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = true,
        .max_files = 12,
        .allocation_unit_size = 4096,
        .disk_status_check_enable = 0,
        .use_one_fat = false,
    };

    ret = esp_vfs_fat_spiflash_mount_rw_wl("/spiffs", "storage", &mount_config, &wl_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "FATFS (wear-levelled) mount failed: %d", (int)ret);
        return false;
    }

    ESP_LOGI(TAG, "Mounted FATFS (wear-levelled) at /spiffs");
    return true;
}


void initGPIO()
{
    gpio_config_t io_conf;
    //set as output mode
    io_conf.mode = gpio_mode_t::GPIO_MODE_INPUT;
    //bit mask of the pins that you want to set,e.g.GPIO18/19
     io_conf.pull_down_en =  gpio_pulldown_t::GPIO_PULLDOWN_ENABLE;
    //set pull-up mode
    io_conf.pull_up_en =  gpio_pullup_t::GPIO_PULLUP_DISABLE;
    //configure GPIO with the given settings
    gpio_config(&io_conf);
}


/**
 * @brief startup the test. Like a test-suite 
 * all test methods must be called here
 */
void task_UnityTesting(void *pvParameter)
{
    vTaskDelay( 5000 / portTICK_PERIOD_MS ); // 5s delay to ensure established serial connection

    (void)Init_NVS_Storage();
    UNITY_BEGIN();
        RUN_TEST(test_getReadoutRawString);
        printf("---------------------------------------------------------------------------\n");
        
        RUN_TEST(test_ZeigerEval);
        printf("---------------------------------------------------------------------------\n");
        RUN_TEST(test_ZeigerEvalHybrid);
        printf("---------------------------------------------------------------------------\n");
        
        RUN_TEST(testNegative_Issues);
        printf("---------------------------------------------------------------------------\n");
        RUN_TEST(testNegative);
        printf("---------------------------------------------------------------------------\n");

        RUN_TEST(test_analogToDigit_Standard);
        printf("---------------------------------------------------------------------------\n");
        RUN_TEST(test_analogToDigit_Transition);
        printf("---------------------------------------------------------------------------\n");

        RUN_TEST(test_doFlowPP);
        printf("---------------------------------------------------------------------------\n");
        RUN_TEST(test_doFlowPP1);
        printf("---------------------------------------------------------------------------\n");
        RUN_TEST(test_doFlowPP2);
        printf("---------------------------------------------------------------------------\n");
        RUN_TEST(test_doFlowPP3);
        printf("---------------------------------------------------------------------------\n");
        RUN_TEST(test_doFlowPP4);
    UNITY_END();

    while(1);
}


/**
 * @brief main task
 */
extern "C" void app_main()
{
  initGPIO();
  (void)Init_NVS_Storage();
  esp_log_level_set("*", ESP_LOG_ERROR);        // set all components to ERROR level

  UNITY_BEGIN();
    RUN_TEST(testNegative_Issues);
   RUN_TEST(testNegative);
   
    RUN_TEST(test_analogToDigit_Standard);
    RUN_TEST(test_analogToDigit_Transition);
    RUN_TEST(test_doFlowPP);
    RUN_TEST(test_doFlowPP1);
    RUN_TEST(test_doFlowPP2);
    RUN_TEST(test_doFlowPP3);
    RUN_TEST(test_doFlowPP4);

    // getReadoutRawString test
    RUN_TEST(test_getReadoutRawString);
    RUN_TEST(test_openmetrics);
    RUN_TEST(test_mqtt);
  
  UNITY_END();
}
