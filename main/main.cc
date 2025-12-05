#include <esp_log.h>
#include <esp_check.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
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
#include "http3_manager.h"
#include "wifi_station.h"
#include "ssid_manager.h"
#include "uart_eth_modem.h"
#include "pmic/pmic.h"

// 选择网络模式：1 = WiFi模式, 0 = 4G模式
#define USE_WIFI_MODE 0

// Embedded resource: test.ogg
extern const uint8_t _binary_test_ogg_start[] asm("_binary_test_ogg_start");
extern const uint8_t _binary_test_ogg_end[] asm("_binary_test_ogg_end");

static inline const uint8_t* GetTestOggData() {
    return _binary_test_ogg_start;
}

static inline size_t GetTestOggSize() {
    return _binary_test_ogg_end - _binary_test_ogg_start;
}

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

/**
 * 初始化4G网卡并等待网络就绪
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
        .rx_buffer_size = 4096,
    };
    
    g_modem = std::make_unique<UartEthModem>(config);
    g_modem->SetDebug(true);

    g_modem->SetNetworkStatusCallback([](UartEthModem::NetworkStatus status) {
        ESP_LOGI(TAG, "Network status changed: %s", UartEthModem::GetNetworkStatusName(status));
    });
    
    esp_err_t ret = g_modem->Start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start modem: %s", esp_err_to_name(ret));
        g_modem.reset();
        return false;
    }
    
    ESP_LOGI(TAG, "Waiting for network ready...");
    auto status = g_modem->WaitForNetworkReady(120000);
    if (status != UartEthModem::NetworkStatus::Ready) {
        ESP_LOGE(TAG, "Network not ready, status: %s", 
                 UartEthModem::GetNetworkStatusName(status));
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

/**
 * 初始化 WiFi 并等待连接
 */
[[maybe_unused]] static bool InitWifi() {
    ESP_LOGI(TAG, "Initializing WiFi...");
    
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    
    auto& wifi = WifiStation::GetInstance();
    wifi.AddAuth("KFC", "chuheridangwu");
    
    wifi.OnConnected([](const std::string& ssid) {
        ESP_LOGI(TAG, "WiFi connected to: %s", ssid.c_str());
    });
    
    wifi.Start();
    
    if (!wifi.WaitForConnected(30000)) {
        ESP_LOGE(TAG, "WiFi connection timeout");
        return false;
    }
    
    ESP_LOGI(TAG, "WiFi connected! IP: %s", wifi.GetIpAddress().c_str());
    return true;
}


/**
 * 使用 Http3Manager 进行共享连接测试
 * 
 * 测试流程：
 * 1. 建立连接
 * 2. 测试并发请求（GET / 和 GET /pocket-sage/health）
 * 3. 等待30秒
 * 4. 测试 ChatStream（POST /pocket-sage/chat/stream）
 * 5. 不关闭连接，等待服务器关闭
 */
void TestHttp3ManagerSharedConnection(const char* hostname, uint16_t port) {
    ESP_LOGI(TAG, "===========================================");
    ESP_LOGI(TAG, "=== Http3Manager Shared Connection Test ===");
    ESP_LOGI(TAG, "===========================================");
    ESP_LOGI(TAG, "Target: %s:%u", hostname, port);
    
    // 1. 初始化 Http3Manager
    auto& manager = Http3Manager::GetInstance();
    
    Http3ManagerConfig config;
    config.hostname = hostname;
    config.port = port;
    config.connect_timeout_ms = 10000;
    config.request_timeout_ms = 30000;
    config.idle_timeout_ms = 120000;  // 2 minutes idle timeout
    config.enable_debug = true;
    
    if (!manager.Init(config)) {
        ESP_LOGE(TAG, "Failed to init Http3Manager");
        return;
    }
    
    manager.SetIdentifiers("test-device-12345", "test-client-001");
    
    // 2. 建立连接
    ESP_LOGI(TAG, "Establishing QUIC connection...");
    if (!manager.EnsureConnected(20000)) {
        ESP_LOGE(TAG, "Failed to connect");
        manager.Deinit();
        return;
    }
    ESP_LOGI(TAG, "Connection established!");
    
    // =========================================
    // 阶段1：测试并发请求
    // =========================================
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "=========================================");
    ESP_LOGI(TAG, "Phase 1: Testing Concurrent Requests");
    ESP_LOGI(TAG, "=========================================");
    
    // 用于跟踪并发请求完成状态
    SemaphoreHandle_t req1_sem = xSemaphoreCreateBinary();
    SemaphoreHandle_t req2_sem = xSemaphoreCreateBinary();
    int req1_status = 0, req2_status = 0;
    std::string req1_body, req2_body;
    
    // 发送请求1: GET /
    Http3Request req1;
    req1.method = "GET";
    req1.path = "/";
    
    Http3StreamCallbacks cb1;
    cb1.on_headers = [&](int stream_id, int status, const auto& headers) {
        ESP_LOGI(TAG, "[Stream %d] GET / - Status: %d", stream_id, status);
        req1_status = status;
    };
    cb1.on_data = [&](int stream_id, const uint8_t* data, size_t len, bool fin) {
        if (data && len > 0) {
            req1_body.append(reinterpret_cast<const char*>(data), len);
        }
    };
    cb1.on_complete = [&](int stream_id, bool success, const std::string& error) {
        ESP_LOGI(TAG, "[Stream %d] GET / - Complete: %s", stream_id, success ? "OK" : error.c_str());
        xSemaphoreGive(req1_sem);
    };
    
    int stream1 = manager.OpenStream(req1, cb1);
    if (stream1 >= 0) {
        manager.FinishStream(stream1);  // No body for GET
        ESP_LOGI(TAG, "Request 1 (GET /) sent on stream %d", stream1);
    } else {
        ESP_LOGE(TAG, "Failed to open stream for request 1");
    }
    
    // 发送请求2: GET /pocket-sage/health
    Http3Request req2;
    req2.method = "GET";
    req2.path = "/pocket-sage/health";
    
    Http3StreamCallbacks cb2;
    cb2.on_headers = [&](int stream_id, int status, const auto& headers) {
        ESP_LOGI(TAG, "[Stream %d] GET /health - Status: %d", stream_id, status);
        req2_status = status;
    };
    cb2.on_data = [&](int stream_id, const uint8_t* data, size_t len, bool fin) {
        if (data && len > 0) {
            req2_body.append(reinterpret_cast<const char*>(data), len);
        }
    };
    cb2.on_complete = [&](int stream_id, bool success, const std::string& error) {
        ESP_LOGI(TAG, "[Stream %d] GET /health - Complete: %s", stream_id, success ? "OK" : error.c_str());
        xSemaphoreGive(req2_sem);
    };
    
    int stream2 = manager.OpenStream(req2, cb2);
    if (stream2 >= 0) {
        manager.FinishStream(stream2);  // No body for GET
        ESP_LOGI(TAG, "Request 2 (GET /health) sent on stream %d", stream2);
    } else {
        ESP_LOGE(TAG, "Failed to open stream for request 2");
    }
    
    // 等待两个请求完成
    ESP_LOGI(TAG, "Waiting for concurrent responses...");
    bool req1_done = xSemaphoreTake(req1_sem, pdMS_TO_TICKS(15000)) == pdTRUE;
    bool req2_done = xSemaphoreTake(req2_sem, pdMS_TO_TICKS(15000)) == pdTRUE;
    
    vSemaphoreDelete(req1_sem);
    vSemaphoreDelete(req2_sem);
    
    // 清理 stream
    if (stream1 >= 0) manager.CleanupStream(stream1);
    if (stream2 >= 0) manager.CleanupStream(stream2);
    
    // 打印并发请求结果
    ESP_LOGI(TAG, "--- Concurrent Request Results ---");
    ESP_LOGI(TAG, "Request 1 (GET /): %s, Status=%d", req1_done ? "Done" : "Timeout", req1_status);
    ESP_LOGI(TAG, "Request 2 (GET /health): %s, Status=%d", req2_done ? "Done" : "Timeout", req2_status);
    
    if (!manager.IsConnected()) {
        ESP_LOGE(TAG, "Connection lost, aborting test");
        manager.Deinit();
        return;
    }
    
    // =========================================
    // 阶段2：等待30秒
    // =========================================
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "=========================================");
    ESP_LOGI(TAG, "Phase 2: Waiting 30 seconds");
    ESP_LOGI(TAG, "=========================================");
    
    for (int waited = 0; waited < 30 && manager.IsConnected(); waited += 5) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        ESP_LOGI(TAG, "  Waited %d seconds...", waited + 5);
    }
    
    if (!manager.IsConnected()) {
        ESP_LOGE(TAG, "Connection lost during wait, aborting test");
        manager.Deinit();
        return;
    }
    
    ESP_LOGI(TAG, "30 seconds wait complete, connection still alive");
    
    // =========================================
    // 阶段3：测试 ChatStream
    // =========================================
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "=========================================");
    ESP_LOGI(TAG, "Phase 3: Testing ChatStream");
    ESP_LOGI(TAG, "=========================================");
    
    const uint8_t* audio_data = GetTestOggData();
    size_t audio_size = GetTestOggSize();
    ESP_LOGI(TAG, "Audio file size: %zu bytes", audio_size);
    
    SemaphoreHandle_t chat_sem = xSemaphoreCreateBinary();
    int chat_status = 0;
    std::string chat_body;
    bool chat_success = false;
    
    Http3Request chat_req;
    chat_req.method = "POST";
    chat_req.path = "/pocket-sage/chat/stream";
    chat_req.headers = {
        {"content-type", "application/octet-stream"},
        {"x-audio-sample-rate", "16000"},
        {"x-audio-channels", "1"},
        {"x-audio-container", "ogg"},
        {"accept", "text/plain"},
    };
    
    Http3StreamCallbacks chat_cb;
    chat_cb.on_headers = [&](int stream_id, int status, const auto& headers) {
        ESP_LOGI(TAG, "[Stream %d] ChatStream - Status: %d", stream_id, status);
        chat_status = status;
    };
    chat_cb.on_data = [&](int stream_id, const uint8_t* data, size_t len, bool fin) {
        if (data && len > 0) {
            std::string chunk(reinterpret_cast<const char*>(data), len);
            chat_body.append(chunk);
            // 实时打印流式响应
            ESP_LOGI(TAG, "[ChatStream] %s", chunk.c_str());
        }
    };
    chat_cb.on_write_complete = [](int stream_id, size_t total_bytes) {
        ESP_LOGI(TAG, "[Stream %d] Upload complete: %zu bytes", stream_id, total_bytes);
    };
    chat_cb.on_complete = [&](int stream_id, bool success, const std::string& error) {
        ESP_LOGI(TAG, "[Stream %d] ChatStream - Complete: %s", stream_id, success ? "OK" : error.c_str());
        chat_success = success;
        xSemaphoreGive(chat_sem);
    };
    
    int chat_stream = manager.OpenStream(chat_req, chat_cb);
    if (chat_stream >= 0) {
        ESP_LOGI(TAG, "ChatStream opened: stream %d", chat_stream);
        
        // 上传音频数据
        ESP_LOGI(TAG, "Uploading audio data (%zu bytes)...", audio_size);
        manager.QueueWrite(chat_stream, audio_data, audio_size);
        manager.FinishStream(chat_stream);
        
        // 等待响应完成
        ESP_LOGI(TAG, "Waiting for ChatStream response...");
        if (xSemaphoreTake(chat_sem, pdMS_TO_TICKS(60000)) == pdTRUE) {
            ESP_LOGI(TAG, "--- ChatStream Results ---");
            ESP_LOGI(TAG, "Status: %d, Success: %s", chat_status, chat_success ? "Yes" : "No");
        } else {
            ESP_LOGW(TAG, "ChatStream timeout");
        }
        
        manager.CleanupStream(chat_stream);
    } else {
        ESP_LOGE(TAG, "Failed to open ChatStream");
    }
    
    vSemaphoreDelete(chat_sem);
    
    // =========================================
    // 阶段4：等待服务器关闭连接
    // =========================================
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "=========================================");
    ESP_LOGI(TAG, "Phase 4: Waiting for server to close");
    ESP_LOGI(TAG, "=========================================");
    ESP_LOGI(TAG, "Connection will stay open until server closes it (idle timeout: %u ms)", config.idle_timeout_ms);
    
    // 打印连接统计
    auto stats = manager.GetStats();
    ESP_LOGI(TAG, "=== Connection Stats ===");
    ESP_LOGI(TAG, "  Packets sent: %lu", stats.packets_sent);
    ESP_LOGI(TAG, "  Packets received: %lu", stats.packets_received);
    ESP_LOGI(TAG, "  Bytes sent: %lu", stats.bytes_sent);
    ESP_LOGI(TAG, "  Bytes received: %lu", stats.bytes_received);
    ESP_LOGI(TAG, "  RTT: %lu ms", stats.rtt_ms);
    
    // 持续等待，直到断开连接
    int idle_seconds = 0;
    while (manager.IsConnected()) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        idle_seconds += 10;
        ESP_LOGI(TAG, "  Connection alive, idle for %d seconds...", idle_seconds);
    }
    
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "=========================================");
    ESP_LOGI(TAG, "=== Test Complete ===");
    ESP_LOGI(TAG, "=========================================");
    ESP_LOGI(TAG, "Connection closed by server after %d seconds idle", idle_seconds);
    
    manager.Deinit();
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
    
    // 共享连接测试
    TestHttp3ManagerSharedConnection("api.tenclass.net", 443);
    
#else
    // ========== 4G 模式 ==========
    ESP_LOGI(TAG, "Running in 4G mode");
    
    if (!Init4GNetwork()) {
        ESP_LOGE(TAG, "Failed to initialize 4G network");
        return;
    }

    esp_pm_config_t pm_config = {
        .max_freq_mhz = 240,
        .min_freq_mhz = 80,
        .light_sleep_enable = true,
    };
    ESP_ERROR_CHECK(esp_pm_configure(&pm_config));
    
    // 共享连接测试
    TestHttp3ManagerSharedConnection("api.tenclass.net", 443);
    
#endif
    
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
