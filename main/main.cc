#include <esp_log.h>
#include <esp_check.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/event_groups.h>
#include <driver/gpio.h>
#include <vector>
#include <set>
#include <map>
#include <memory>
#include <nvs_flash.h>
#include <esp_heap_caps.h>
#include <esp_netif.h>
#include <esp_pm.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <esp_http_client.h>
#include <esp_timer.h>
#include <esp_crt_bundle.h>
#include "http3_manager.h"
#include <wifi_manager.h>
#include <ssid_manager.h>
#include "uart_eth_modem/uart_eth_modem.h"
#include "pmic/pmic.h"
#include "quic_test.h"

// 选择网络模式：1 = WiFi模式, 0 = 4G模式
#define USE_WIFI_MODE 0

static const char *TAG = "HTTPS_DEMO";

// GPIO 配置
static constexpr gpio_num_t RESET_BUTTON_PIN = GPIO_NUM_1;  // 重启按钮引脚
static constexpr gpio_num_t MODEM_RI_PIN = GPIO_NUM_11;
static constexpr gpio_num_t MODEM_DTR_PIN = GPIO_NUM_12;
static constexpr gpio_num_t MODEM_TX_PIN = GPIO_NUM_13;
static constexpr gpio_num_t MODEM_RX_PIN = GPIO_NUM_14;
static constexpr gpio_num_t MODEM_RESET_PIN = GPIO_NUM_17;
static constexpr gpio_num_t MODEM_POWER_PIN = GPIO_NUM_18;

// 4G Modem 全局变量
static std::unique_ptr<UartEthModem> g_modem;

/**
 * GPIO1 低电平中断处理函数
 * 触发后立即重启系统
 */
static void IRAM_ATTR gpio_reset_isr_handler(void* arg) {
    // 在中断中直接调用软重启
    esp_restart();
}

/**
 * 初始化重启按钮 GPIO
 * 配置 GPIO1 为低电平触发中断，并支持低功耗唤醒
 */
static void InitResetButton() {
    ESP_LOGI(TAG, "Initializing reset button on GPIO%d...", RESET_BUTTON_PIN);
    
    // 配置 GPIO1 为输入，启用上拉电阻
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << RESET_BUTTON_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_LOW_LEVEL,  // 低电平触发
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));
    
    // 添加中断处理
    ESP_ERROR_CHECK(gpio_isr_handler_add(RESET_BUTTON_PIN, gpio_reset_isr_handler, nullptr));
    
    // 配置为低功耗唤醒源（支持 light sleep）
    ESP_ERROR_CHECK(gpio_wakeup_enable(RESET_BUTTON_PIN, GPIO_INTR_LOW_LEVEL));
    ESP_ERROR_CHECK(esp_sleep_enable_gpio_wakeup());
    
    ESP_LOGI(TAG, "Reset button initialized, low level will trigger system restart");
}

/**
 * 初始化 Modem 硬件（开机）
 */
static bool InitModemHardware() {
    ESP_LOGI(TAG, "Initializing Modem Hardware...");
    
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << MODEM_RESET_PIN) | (1ULL << MODEM_POWER_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    
    gpio_set_level(MODEM_RESET_PIN, 0);
    gpio_set_level(MODEM_POWER_PIN, 0);
    
    gpio_config_t ri_io_conf = {
        .pin_bit_mask = (1ULL << MODEM_RI_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&ri_io_conf);

    if (gpio_get_level(MODEM_RI_PIN) == 0) {
        ESP_LOGI(TAG, "RI pin is low, powering on modem");
        gpio_set_level(MODEM_POWER_PIN, 1);
        vTaskDelay(pdMS_TO_TICKS(500));
        gpio_set_level(MODEM_POWER_PIN, 0);
    }
    
    ESP_LOGI(TAG, "Modem hardware initialized");
    return true;
}

// Event bits for 4G network initialization
static constexpr uint32_t kEventNetworkConnected = (1 << 0);
static constexpr uint32_t kEventNetworkError = (1 << 1);

/**
 * 初始化4G网卡并等待网络就绪
 * 使用 event group 等待网络连接或错误事件
 */
static bool Init4GNetwork() {
    ESP_LOGI(TAG, "Initializing 4G Network...");
    
    if (!InitModemHardware()) {
        ESP_LOGE(TAG, "Failed to initialize modem hardware");
        return false;
    }
    
    esp_netif_init();
    esp_event_loop_create_default();
    
    UartEthModem::Config config = {
        .uart_num = UART_NUM_1,
        .baud_rate = 3000000,
        .tx_pin = MODEM_TX_PIN,
        .rx_pin = MODEM_RX_PIN,
        .mrdy_pin = MODEM_DTR_PIN,
        .srdy_pin = MODEM_RI_PIN,
    };
    
    g_modem = std::make_unique<UartEthModem>(config);
    g_modem->SetDebug(false);

    // Create event group for waiting network events
    EventGroupHandle_t network_event_group = xEventGroupCreate();
    if (network_event_group == nullptr) {
        ESP_LOGE(TAG, "Failed to create event group");
        g_modem.reset();
        return false;
    }

    // Set callback to notify event group on network events
    g_modem->SetNetworkEventCallback([network_event_group](UartEthModem::UartEthModemEvent event) {
        ESP_LOGI(TAG, "Network event: %s", UartEthModem::GetNetworkEventName(event));
        
        switch (event) {
            case UartEthModem::UartEthModemEvent::Connected:
                xEventGroupSetBits(network_event_group, kEventNetworkConnected);
                break;
            case UartEthModem::UartEthModemEvent::ErrorNoSim:
            case UartEthModem::UartEthModemEvent::ErrorRegistrationDenied:
            case UartEthModem::UartEthModemEvent::ErrorInitFailed:
            case UartEthModem::UartEthModemEvent::ErrorNoCarrier:
                xEventGroupSetBits(network_event_group, kEventNetworkError);
                break;
            case UartEthModem::UartEthModemEvent::Connecting:
            case UartEthModem::UartEthModemEvent::Disconnected:
                // These are informational, no action needed
                break;
        }
    });
    
    esp_err_t ret = g_modem->Start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start modem: %s", esp_err_to_name(ret));
        vEventGroupDelete(network_event_group);
        g_modem.reset();
        return false;
    }
    
    ESP_LOGI(TAG, "Waiting for network ready...");
    
    // Wait for either Connected or Error event with 120s timeout
    EventBits_t bits = xEventGroupWaitBits(
        network_event_group,
        kEventNetworkConnected | kEventNetworkError,
        pdTRUE,   // Clear bits on exit
        pdFALSE,  // Wait for any bit (OR)
        pdMS_TO_TICKS(120000)
    );
    
    if (bits & kEventNetworkError) {
        ESP_LOGE(TAG, "Network initialization failed with error");
        g_modem->Stop();
        g_modem.reset();
        return false;
    }
    
    if (!(bits & kEventNetworkConnected)) {
        ESP_LOGE(TAG, "Network connection timeout");
        g_modem->Stop();
        g_modem.reset();
        return false;
    }
    
    ESP_LOGI(TAG, "=== 4G Modem Info ===");
    ESP_LOGI(TAG, "  IMEI:     %s", g_modem->GetImei().c_str());
    ESP_LOGI(TAG, "  ICCID:    %s", g_modem->GetIccid().c_str());
    ESP_LOGI(TAG, "  Carrier:  %s", g_modem->GetCarrierName().c_str());
    ESP_LOGI(TAG, "  Revision: %s", g_modem->GetModuleRevision().c_str());
    ESP_LOGI(TAG, "  Signal:   %d (CSQ)", g_modem->GetSignalStrength());
    
    auto cell = g_modem->GetCellInfo();
    ESP_LOGI(TAG, "  Cell:     TAC=%s, CI=%s, AcT=%d", 
             cell.tac.c_str(), cell.ci.c_str(), cell.act);
    
    ESP_LOGI(TAG, "4G Network ready!");
    return true;
}

// Event bits for WiFi initialization
static constexpr uint32_t kEventWifiConnected = (1 << 0);
static constexpr uint32_t kEventWifiConfigModeExit = (1 << 1);
static constexpr uint32_t kEventWifiDisconnected = (1 << 2);

/**
 * 初始化 WiFi 并等待连接
 * 参考 managed_components/78__esp-wifi-connect/README.md
 */
[[maybe_unused]] static bool InitWifi() {
    ESP_LOGI(TAG, "Initializing WiFi...");
    
    // Initialize the default event loop
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    
    // Initialize NVS flash for Wi-Fi configuration
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    
    // Get the WifiManager singleton
    auto& wifi_manager = WifiManager::GetInstance();
    
    // Initialize with configuration
    WifiManagerConfig config;
    config.ssid_prefix = "ESP32";  // AP mode SSID prefix
    config.language = "zh-CN";     // Web UI language
    if (!wifi_manager.Initialize(config)) {
        ESP_LOGE(TAG, "Failed to initialize WifiManager");
        return false;
    }
    
    // Create event group for waiting WiFi events
    EventGroupHandle_t wifi_event_group = xEventGroupCreate();
    if (wifi_event_group == nullptr) {
        ESP_LOGE(TAG, "Failed to create WiFi event group");
        return false;
    }
    
    // Set event callback to handle WiFi events
    wifi_manager.SetEventCallback([wifi_event_group](WifiEvent event) {
        switch (event) {
            case WifiEvent::Scanning:
                ESP_LOGI(TAG, "WiFi: Scanning for networks...");
                break;
            case WifiEvent::Connecting:
                ESP_LOGI(TAG, "WiFi: Connecting to network...");
                break;
            case WifiEvent::Connected:
                ESP_LOGI(TAG, "WiFi: Connected successfully!");
                xEventGroupSetBits(wifi_event_group, kEventWifiConnected);
                break;
            case WifiEvent::Disconnected:
                ESP_LOGW(TAG, "WiFi: Disconnected from network");
                xEventGroupSetBits(wifi_event_group, kEventWifiDisconnected);
                break;
            case WifiEvent::ConfigModeEnter:
                ESP_LOGI(TAG, "WiFi: Entered config mode");
                break;
            case WifiEvent::ConfigModeExit:
                ESP_LOGI(TAG, "WiFi: Exited config mode");
                xEventGroupSetBits(wifi_event_group, kEventWifiConfigModeExit);
                break;
        }
    });
    
    // Check if there are saved Wi-Fi credentials
    auto& ssid_list = SsidManager::GetInstance().GetSsidList();
    if (ssid_list.empty()) {
        // No credentials saved, start config AP mode
        ESP_LOGI(TAG, "No saved WiFi credentials, starting config AP mode");
        wifi_manager.StartConfigAp();
        
        // Wait for config mode exit (user configured WiFi)
        ESP_LOGI(TAG, "Waiting for WiFi configuration...");
        EventBits_t bits = xEventGroupWaitBits(
            wifi_event_group,
            kEventWifiConfigModeExit | kEventWifiConnected,
            pdTRUE,   // Clear bits on exit
            pdFALSE,  // Wait for any bit (OR)
            portMAX_DELAY  // Wait indefinitely
        );
        
        if (bits & kEventWifiConfigModeExit) {
            ESP_LOGI(TAG, "Config mode exited, starting station mode");
            // After config mode exit, start station mode to connect to configured WiFi
            wifi_manager.StartStation();
        } else if (bits & kEventWifiConnected) {
            // Already connected during config (shouldn't happen, but handle it)
            ESP_LOGI(TAG, "WiFi connected during config mode");
        }
    } else {
        // Try to connect to the saved Wi-Fi network
        ESP_LOGI(TAG, "Found %zu saved WiFi credential(s), starting station mode", ssid_list.size());
        wifi_manager.StartStation();
    }
    
    // Wait for WiFi connection with 30s timeout
    ESP_LOGI(TAG, "Waiting for WiFi connection...");
    EventBits_t bits = xEventGroupWaitBits(
        wifi_event_group,
        kEventWifiConnected,
        pdTRUE,   // Clear bits on exit
        pdFALSE,  // Wait for any bit (OR)
        pdMS_TO_TICKS(30000)  // 30 second timeout
    );
    
    vEventGroupDelete(wifi_event_group);
    
    if (!(bits & kEventWifiConnected)) {
        ESP_LOGE(TAG, "WiFi connection timeout");
        return false;
    }
    
    ESP_LOGI(TAG, "WiFi connected! SSID: %s, IP: %s", 
             wifi_manager.GetSsid().c_str(), 
             wifi_manager.GetIpAddress().c_str());
    return true;
}


/**
 * HTTP 客户端下载测试
 * 使用 ESP-IDF 自带的 esp_http_client 下载文件并打印速度
 */
static void TestHttpClientDownload(const char* url) {
    ESP_LOGI(TAG, "===========================================");
    ESP_LOGI(TAG, "=== HTTP Client Download Speed Test ===");
    ESP_LOGI(TAG, "===========================================");
    ESP_LOGI(TAG, "URL: %s", url);

    // Configure HTTP client
    esp_http_client_config_t config = {};
    config.url = url;
    config.timeout_ms = 30000;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.buffer_size = 4096;
    config.buffer_size_tx = 1024;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        return;
    }

    // Open connection
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open HTTP connection: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return;
    }

    // Fetch headers
    int content_length = esp_http_client_fetch_headers(client);
    int status_code = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "HTTP Status: %d, Content-Length: %d", status_code, content_length);

    if (status_code != 200) {
        ESP_LOGE(TAG, "HTTP request failed with status %d", status_code);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return;
    }

    // Allocate read buffer
    static constexpr size_t kReadBufferSize = 8192;
    uint8_t* buffer = static_cast<uint8_t*>(malloc(kReadBufferSize));
    if (buffer == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate read buffer");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return;
    }

    // Start downloading and measure speed
    int64_t start_time = esp_timer_get_time();
    int64_t last_report_time = start_time;
    size_t total_bytes = 0;
    size_t bytes_since_last_report = 0;
    int read_len;

    ESP_LOGI(TAG, "Starting download...");

    while ((read_len = esp_http_client_read(client, reinterpret_cast<char*>(buffer), kReadBufferSize)) > 0) {
        total_bytes += read_len;
        bytes_since_last_report += read_len;

        int64_t now = esp_timer_get_time();
        int64_t elapsed_since_report = now - last_report_time;

        // Report every second
        if (elapsed_since_report >= 1000000) {
            float instant_speed_bytes_per_sec = (bytes_since_last_report * 1000000.0f) / elapsed_since_report;  // bytes/s
            float overall_speed_bytes_per_sec = (total_bytes * 1000000.0f) / (now - start_time);  // bytes/s

            ESP_LOGI(TAG, "Downloaded: %zu bytes (%.1f KB), Instant: %.1f KB/s (%.0f B/s), Average: %.1f KB/s (%.0f B/s)",
                     total_bytes, total_bytes / 1024.0f, 
                     instant_speed_bytes_per_sec / 1024.0f, instant_speed_bytes_per_sec,
                     overall_speed_bytes_per_sec / 1024.0f, overall_speed_bytes_per_sec);

            last_report_time = now;
            bytes_since_last_report = 0;
        }
    }

    int64_t end_time = esp_timer_get_time();
    float elapsed_sec = (end_time - start_time) / 1000000.0f;
    float speed_bytes_per_sec = total_bytes / elapsed_sec;  // bytes/s
    float speed_kb_per_sec = speed_bytes_per_sec / 1024.0f;  // KB/s
    float speed_mb_per_sec = speed_bytes_per_sec / (1024.0f * 1024.0f);  // MB/s

    ESP_LOGI(TAG, "===========================================");
    ESP_LOGI(TAG, "=== Download Complete ===");
    ESP_LOGI(TAG, "===========================================");
    ESP_LOGI(TAG, "Total bytes: %zu (%.2f KB, %.2f MB)", total_bytes, total_bytes / 1024.0f, total_bytes / (1024.0f * 1024.0f));
    ESP_LOGI(TAG, "Time elapsed: %.2f seconds", elapsed_sec);
    ESP_LOGI(TAG, "Average speed: %.0f B/s (%.2f KB/s, %.2f MB/s)", speed_bytes_per_sec, speed_kb_per_sec, speed_mb_per_sec);

    if (read_len < 0) {
        ESP_LOGW(TAG, "Read error occurred at the end: %d", read_len);
    }

    // Cleanup
    free(buffer);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    ESP_LOGI(TAG, "HTTP download test completed");
}



extern "C" void app_main(void) {
    ESP_LOGI(TAG, "HTTPS Demo Starting... Press GPIO1 to restart!!!");
    auto& pmic = Pmic::GetInstance();
    pmic.DisableCellCharge();

    gpio_install_isr_service(ESP_INTR_FLAG_LEVEL3 | ESP_INTR_FLAG_IRAM);
    
    // 初始化重启按钮（GPIO1 低电平触发重启，支持低功耗唤醒）
    InitResetButton();
    
#if USE_WIFI_MODE
    // ========== WiFi 模式 ==========
    ESP_LOGI(TAG, "Running in WiFi mode");
    
    if (!InitWifi()) {
        ESP_LOGE(TAG, "Failed to connect WiFi");
        return;
    }
    
#else
    // ========== 4G 模式 ==========
    ESP_LOGI(TAG, "Running in 4G mode");
    
    if (!Init4GNetwork()) {
        ESP_LOGE(TAG, "Failed to initialize 4G network");
        return;
    }

    // esp_pm_config_t pm_config = {
    //     .max_freq_mhz = 240,
    //     .min_freq_mhz = 80,
    //     .light_sleep_enable = true,
    // };
    // ESP_ERROR_CHECK(esp_pm_configure(&pm_config));
    
#endif
    
    // 共享连接测试
    // TestHttp3ManagerSharedConnection("api.tenclass.net", 443);
    
    // HTTP 下载速度测试
    TestHttpClientDownload("https://xiaozhi-voice-assistant.oss-cn-shenzhen.aliyuncs.com/firmwares/v1.9.4_xmini-c3/xiaozhi.bin");
    
    ESP_LOGI(TAG, "HTTPS Demo completed");

    static char buffer[1024];
    vTaskList(buffer);
    ESP_LOGI(TAG, "Task list:\n%s", buffer);
    
    ESP_LOGI(TAG, "Free SRAM: %d bytes, minimal SRAM: %d bytes", heap_caps_get_free_size(MALLOC_CAP_INTERNAL), 
        heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
