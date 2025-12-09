#include "system_info.h"

#include <freertos/task.h>
#include <esp_log.h>
#include <esp_flash.h>
#include <esp_mac.h>
#include <esp_system.h>
#include <esp_partition.h>
#include <esp_app_desc.h>
#include <esp_ota_ops.h>
#include <esp_random.h>
#include <esp_heap_caps.h>
#include <nvs_flash.h>
#include <nvs.h>
#if CONFIG_IDF_TARGET_ESP32P4
#include "esp_wifi_remote.h"
#endif

#define TAG "SystemInfo"

// NVS namespace and key for client ID storage
static constexpr const char* NVS_NAMESPACE = "pocket-sage";
static constexpr const char* NVS_KEY_CLIENT_ID = "client_id";

size_t SystemInfo::GetFlashSize() {
    uint32_t flash_size;
    if (esp_flash_get_size(NULL, &flash_size) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get flash size");
        return 0;
    }
    return (size_t)flash_size;
}

size_t SystemInfo::GetTotalHeapSize() {
    return heap_caps_get_total_size(MALLOC_CAP_8BIT);
}

size_t SystemInfo::GetFreeHeapSize() {
    return esp_get_free_heap_size();
}

size_t SystemInfo::GetMinimumFreeHeapSize() {
    return esp_get_minimum_free_heap_size();
}

size_t SystemInfo::GetTotalSramSize() {
    return heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
}

size_t SystemInfo::GetFreeSramSize() {
    return heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
}

size_t SystemInfo::GetMinimumFreeSramSize() {
    return heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
}

std::string SystemInfo::GetMacAddress() {
    uint8_t mac[6];
#if CONFIG_IDF_TARGET_ESP32P4
    esp_wifi_get_mac(WIFI_IF_STA, mac);
#else
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
#endif
    char mac_str[18];
    snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return std::string(mac_str);
}

std::string SystemInfo::GetChipModelName() {
    return std::string(CONFIG_IDF_TARGET);
}

std::string SystemInfo::GetUserAgent() {
    auto app_desc = esp_app_get_description();
    auto user_agent = std::string(app_desc->project_name) + "/" + app_desc->version;
    return user_agent;
}

esp_err_t SystemInfo::PrintTaskCpuUsage(TickType_t xTicksToWait) {
    #define ARRAY_SIZE_OFFSET 5
    TaskStatus_t *start_array = NULL, *end_array = NULL;
    UBaseType_t start_array_size, end_array_size;
    configRUN_TIME_COUNTER_TYPE start_run_time, end_run_time;
    esp_err_t ret;
    uint32_t total_elapsed_time;

    //Allocate array to store current task states
    start_array_size = uxTaskGetNumberOfTasks() + ARRAY_SIZE_OFFSET;
    start_array = (TaskStatus_t*)malloc(sizeof(TaskStatus_t) * start_array_size);
    if (start_array == NULL) {
        ret = ESP_ERR_NO_MEM;
        goto exit;
    }
    //Get current task states
    start_array_size = uxTaskGetSystemState(start_array, start_array_size, &start_run_time);
    if (start_array_size == 0) {
        ret = ESP_ERR_INVALID_SIZE;
        goto exit;
    }

    vTaskDelay(xTicksToWait);

    //Allocate array to store tasks states post delay
    end_array_size = uxTaskGetNumberOfTasks() + ARRAY_SIZE_OFFSET;
    end_array = (TaskStatus_t*)malloc(sizeof(TaskStatus_t) * end_array_size);
    if (end_array == NULL) {
        ret = ESP_ERR_NO_MEM;
        goto exit;
    }
    //Get post delay task states
    end_array_size = uxTaskGetSystemState(end_array, end_array_size, &end_run_time);
    if (end_array_size == 0) {
        ret = ESP_ERR_INVALID_SIZE;
        goto exit;
    }

    //Calculate total_elapsed_time in units of run time stats clock period.
    total_elapsed_time = (end_run_time - start_run_time);
    if (total_elapsed_time == 0) {
        ret = ESP_ERR_INVALID_STATE;
        goto exit;
    }

    printf("| Task | Run Time | Percentage\n");
    //Match each task in start_array to those in the end_array
    for (int i = 0; i < start_array_size; i++) {
        int k = -1;
        for (int j = 0; j < end_array_size; j++) {
            if (start_array[i].xHandle == end_array[j].xHandle) {
                k = j;
                //Mark that task have been matched by overwriting their handles
                start_array[i].xHandle = NULL;
                end_array[j].xHandle = NULL;
                break;
            }
        }
        //Check if matching task found
        if (k >= 0) {
            uint32_t task_elapsed_time = end_array[k].ulRunTimeCounter - start_array[i].ulRunTimeCounter;
            uint32_t percentage_time = (task_elapsed_time * 100UL) / (total_elapsed_time * CONFIG_FREERTOS_NUMBER_OF_CORES);
            printf("| %-16s | %8lu | %4lu%%\n", start_array[i].pcTaskName, task_elapsed_time, percentage_time);
        }
    }

    //Print unmatched tasks
    for (int i = 0; i < start_array_size; i++) {
        if (start_array[i].xHandle != NULL) {
            printf("| %s | Deleted\n", start_array[i].pcTaskName);
        }
    }
    for (int i = 0; i < end_array_size; i++) {
        if (end_array[i].xHandle != NULL) {
            printf("| %s | Created\n", end_array[i].pcTaskName);
        }
    }
    ret = ESP_OK;

exit:    //Common return path
    free(start_array);
    free(end_array);
    return ret;
}

void SystemInfo::PrintTaskList() {
    char buffer[1000];
    vTaskList(buffer);
    ESP_LOGI(TAG, "Task list: \n%s", buffer);
}

void SystemInfo::PrintHeapStats() {
    int free_sram = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    int min_free_sram = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "free sram: %u minimal sram: %u", free_sram, min_free_sram);
}

// ==================== UUID 和存储 ====================

std::string SystemInfo::GenerateUuid() {
    char uuid[37];
    uint32_t random[4];

    for (int i = 0; i < 4; i++) {
        random[i] = esp_random();
    }

    snprintf(uuid, sizeof(uuid),
             "%08lx-%04lx-4%03lx-%04lx-%012llx",
             random[0],
             (random[1] >> 16) & 0xFFFF,
             random[1] & 0x0FFF,
             ((random[2] >> 16) & 0x3FFF) | 0x8000,
             ((uint64_t)(random[2] & 0xFFFF) << 32) | random[3]);

    return std::string(uuid);
}

std::string SystemInfo::GetClientId() {
    nvs_handle_t nvs_handle;
    esp_err_t err;

    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition was truncated, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error opening NVS handle: %s", esp_err_to_name(err));
        return "";
    }

    size_t required_size = 0;
    err = nvs_get_str(nvs_handle, NVS_KEY_CLIENT_ID, NULL, &required_size);

    if (err == ESP_OK && required_size > 0) {
        char* value = (char*)malloc(required_size);
        err = nvs_get_str(nvs_handle, NVS_KEY_CLIENT_ID, value, &required_size);
        if (err == ESP_OK) {
            std::string client_id(value);
            free(value);
            nvs_close(nvs_handle);
            return client_id;
        }
        free(value);
    }

    std::string new_uuid = GenerateUuid();
    ESP_LOGI(TAG, "Generated new client ID: %s", new_uuid.c_str());

    err = nvs_set_str(nvs_handle, NVS_KEY_CLIENT_ID, new_uuid.c_str());
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error setting client ID in NVS: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return new_uuid;
    }

    err = nvs_commit(nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error committing NVS: %s", esp_err_to_name(err));
    }

    nvs_close(nvs_handle);
    return new_uuid;
}
