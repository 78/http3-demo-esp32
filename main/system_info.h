#ifndef _SYSTEM_INFO_H_
#define _SYSTEM_INFO_H_

#include <string>

#include <esp_err.h>
#include <freertos/FreeRTOS.h>

class SystemInfo {
public:
    // Flash info
    static size_t GetFlashSize();

    // Heap info (including PSRAM if available)
    static size_t GetTotalHeapSize();
    static size_t GetFreeHeapSize();
    static size_t GetMinimumFreeHeapSize();

    // Internal SRAM info
    static size_t GetTotalSramSize();
    static size_t GetFreeSramSize();
    static size_t GetMinimumFreeSramSize();

    static std::string GetMacAddress();
    static std::string GetChipModelName();
    static std::string GetUserAgent();
    static esp_err_t PrintTaskCpuUsage(TickType_t xTicksToWait);
    static void PrintTaskList();
    static void PrintHeapStats();
    
    // UUID and Client ID management
    static std::string GenerateUuid();
    static std::string GetClientId();
};

#endif // _SYSTEM_INFO_H_
