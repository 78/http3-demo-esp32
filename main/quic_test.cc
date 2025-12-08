#include "quic_test.h"
#include "http3_manager.h"
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <string>
#include <vector>

static const char *TAG = "QUIC_TEST";

// Embedded resource: test.ogg
extern const uint8_t _binary_test_ogg_start[] asm("_binary_test_ogg_start");
extern const uint8_t _binary_test_ogg_end[] asm("_binary_test_ogg_end");

const uint8_t* GetTestOggData() {
    return _binary_test_ogg_start;
}

size_t GetTestOggSize() {
    return _binary_test_ogg_end - _binary_test_ogg_start;
}

/**
 * Test Http3Manager shared connection
 * 
 * Test flow:
 * 1. Establish connection
 * 2. Test concurrent requests (GET / and GET /pocket-sage/health)
 * 3. Wait 30 seconds
 * 4. Test ChatStream (POST /pocket-sage/chat/stream)
 * 5. Don't close connection, wait for server to close
 */
void TestHttp3ManagerSharedConnection(const char* hostname, uint16_t port) {
    ESP_LOGI(TAG, "===========================================");
    ESP_LOGI(TAG, "=== Http3Manager Shared Connection Test ===");
    ESP_LOGI(TAG, "===========================================");
    ESP_LOGI(TAG, "Target: %s:%u", hostname, port);
    
    // 1. Initialize Http3Manager
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
    
    // 2. Establish connection
    ESP_LOGI(TAG, "Establishing QUIC connection...");
    if (!manager.EnsureConnected(20000)) {
        ESP_LOGE(TAG, "Failed to connect");
        manager.Deinit();
        return;
    }
    ESP_LOGI(TAG, "Connection established!");
    
    // =========================================
    // Phase 1: Test concurrent requests
    // =========================================
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "=========================================");
    ESP_LOGI(TAG, "Phase 1: Testing Concurrent Requests");
    ESP_LOGI(TAG, "=========================================");
    
    // Track concurrent request completion status
    SemaphoreHandle_t req1_sem = xSemaphoreCreateBinary();
    SemaphoreHandle_t req2_sem = xSemaphoreCreateBinary();
    int req1_status = 0, req2_status = 0;
    std::string req1_body, req2_body;
    
    // Send request 1: GET /
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
    
    // Send request 2: GET /pocket-sage/health
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
    
    // Wait for both requests to complete
    ESP_LOGI(TAG, "Waiting for concurrent responses...");
    bool req1_done = xSemaphoreTake(req1_sem, pdMS_TO_TICKS(15000)) == pdTRUE;
    bool req2_done = xSemaphoreTake(req2_sem, pdMS_TO_TICKS(15000)) == pdTRUE;
    
    vSemaphoreDelete(req1_sem);
    vSemaphoreDelete(req2_sem);
    
    // Cleanup streams
    if (stream1 >= 0) manager.CleanupStream(stream1);
    if (stream2 >= 0) manager.CleanupStream(stream2);
    
    // Print concurrent request results
    ESP_LOGI(TAG, "--- Concurrent Request Results ---");
    ESP_LOGI(TAG, "Request 1 (GET /): %s, Status=%d", req1_done ? "Done" : "Timeout", req1_status);
    ESP_LOGI(TAG, "Request 2 (GET /health): %s, Status=%d", req2_done ? "Done" : "Timeout", req2_status);
    
    if (!manager.IsConnected()) {
        ESP_LOGE(TAG, "Connection lost, aborting test");
        manager.Deinit();
        return;
    }
    
    // =========================================
    // Phase 2: Wait 30 seconds
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
    // Phase 3: Test ChatStream
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
            // Print streaming response in real-time
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
        
        // Upload audio data
        ESP_LOGI(TAG, "Uploading audio data (%zu bytes)...", audio_size);
        manager.QueueWrite(chat_stream, audio_data, audio_size);
        manager.FinishStream(chat_stream);
        
        // Wait for response to complete
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
    // Phase 4: Wait for server to close connection
    // =========================================
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "=========================================");
    ESP_LOGI(TAG, "Phase 4: Waiting for server to close");
    ESP_LOGI(TAG, "=========================================");
    ESP_LOGI(TAG, "Connection will stay open until server closes it (idle timeout: %u ms)", config.idle_timeout_ms);
    
    // Print connection statistics
    auto stats = manager.GetStats();
    ESP_LOGI(TAG, "=== Connection Stats ===");
    ESP_LOGI(TAG, "  Packets sent: %lu", stats.packets_sent);
    ESP_LOGI(TAG, "  Packets received: %lu", stats.packets_received);
    ESP_LOGI(TAG, "  Bytes sent: %lu", stats.bytes_sent);
    ESP_LOGI(TAG, "  Bytes received: %lu", stats.bytes_received);
    ESP_LOGI(TAG, "  RTT: %lu ms", stats.rtt_ms);
    
    // Keep waiting until connection is closed
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

