#include "quic_test.h"
#include <esp_log.h>
#include "esp_http3.h"
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string>
#include <vector>
#include <esp_ota_ops.h>
#include <esp_app_desc.h>
#include <esp_image_format.h>
#include <cstring>

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
 * 3. Wait 5 seconds
 * 4. Test ChatStream (POST /pocket-sage/chat/stream)
 * 5. Don't close connection, wait for server to close
 */
void TestHttp3ManagerSharedConnection(const char* hostname, uint16_t port) {
    ESP_LOGI(TAG, "===========================================");
    ESP_LOGI(TAG, "=== Http3Client Shared Connection Test ===");
    ESP_LOGI(TAG, "===========================================");
    ESP_LOGI(TAG, "Target: %s:%u", hostname, port);
    
    // 1. Initialize Http3Client
    Http3ClientConfig config;
    config.hostname = hostname;
    config.port = port;
    config.connect_timeout_ms = 10000;
    config.request_timeout_ms = 30000;
    config.idle_timeout_ms = 120000;  // 2 minutes idle timeout
    config.enable_debug = true;
    
    Http3Client client(config);
    
    // 2. Establish connection
    ESP_LOGI(TAG, "Establishing QUIC connection...");
    if (!client.IsConnected()) {
        // For Http3Client, connection is established on first Open() call
        // Let's try opening a simple request
        Http3Request test_req;
        test_req.method = "GET";
        test_req.path = "/";
        auto test_stream = client.Open(test_req, 20000);
        if (!test_stream) {
            ESP_LOGE(TAG, "Failed to connect");
            return;
        }
        test_stream->Close();
    }
    ESP_LOGI(TAG, "Connection established!");
    
    // =========================================
    // Phase 1: Test concurrent requests
    // =========================================
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "=========================================");
    ESP_LOGI(TAG, "Phase 1: Testing Concurrent Requests");
    ESP_LOGI(TAG, "=========================================");
    
    int req1_status = 0, req2_status = 0;
    std::string req1_body, req2_body;
    bool req1_success = false, req2_success = false;
    
    // Request 1: GET / (using separate synchronous call for concurrency)
    Http3Request req1;
    req1.method = "GET";
    req1.path = "/";
    
    auto stream1 = client.Open(req1, 15000);
    if (stream1 && stream1->IsValid()) {
        req1_status = stream1->GetStatus(15000);
        req1_success = true;
        
        ESP_LOGI(TAG, "[Stream %d] GET / - Status: %d", stream1->GetStreamId(), req1_status);
        
        // Read response body
        uint8_t buffer[1024];
        int n;
        while ((n = stream1->Read(buffer, sizeof(buffer), 5000)) > 0) {
            req1_body.append(reinterpret_cast<const char*>(buffer), n);
        }
        
        ESP_LOGI(TAG, "[Stream %d] GET / - Complete (body size: %zu)", stream1->GetStreamId(), req1_body.size());
    } else {
        ESP_LOGE(TAG, "Failed to open GET / request");
        req1_success = false;
    }
    
    // Request 2: GET /pocket-sage/health
    Http3Request req2;
    req2.method = "GET";
    req2.path = "/pocket-sage/health";
    
    auto stream2 = client.Open(req2, 15000);
    if (stream2 && stream2->IsValid()) {
        req2_status = stream2->GetStatus(15000);
        req2_success = true;
        
        ESP_LOGI(TAG, "[Stream %d] GET /health - Status: %d", stream2->GetStreamId(), req2_status);
        
        // Read response body
        uint8_t buffer[1024];
        int n;
        while ((n = stream2->Read(buffer, sizeof(buffer), 5000)) > 0) {
            req2_body.append(reinterpret_cast<const char*>(buffer), n);
        }
        
        ESP_LOGI(TAG, "[Stream %d] GET /health - Complete (body size: %zu)", stream2->GetStreamId(), req2_body.size());
    } else {
        ESP_LOGE(TAG, "Failed to open GET /health request");
        req2_success = false;
    }
    
    // Print concurrent request results
    ESP_LOGI(TAG, "--- Concurrent Request Results ---");
    ESP_LOGI(TAG, "Request 1 (GET /): %s, Status=%d, Body size=%zu", 
             req1_success ? "Success" : "Failed", req1_status, req1_body.size());
    ESP_LOGI(TAG, "Request 2 (GET /health): %s, Status=%d, Body size=%zu", 
             req2_success ? "Success" : "Failed", req2_status, req2_body.size());
    
    if (!client.IsConnected()) {
        ESP_LOGE(TAG, "Connection lost, aborting test");
        return;
    }
    
    // =========================================
    // Phase 2: Wait 5 seconds
    // =========================================
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "=========================================");
    ESP_LOGI(TAG, "Phase 2: Waiting 5 seconds");
    ESP_LOGI(TAG, "=========================================");
    
    for (int waited = 0; waited < 5 && client.IsConnected(); waited += 1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        ESP_LOGI(TAG, "  Waited %d seconds...", waited + 1);
    }
    
    if (!client.IsConnected()) {
        ESP_LOGE(TAG, "Connection lost during wait, aborting test");
        return;
    }
    
    ESP_LOGI(TAG, "5 seconds wait complete, connection still alive");
    
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
    
    auto chat_stream = client.Open(chat_req, 30000);
    if (chat_stream && chat_stream->IsValid()) {
        ESP_LOGI(TAG, "ChatStream opened: stream %d", chat_stream->GetStreamId());
        
        // Upload audio data
        ESP_LOGI(TAG, "Uploading audio data (%zu bytes)...", audio_size);
        int write_result = chat_stream->Write(audio_data, audio_size, 60000);
        if (write_result > 0) {
            ESP_LOGI(TAG, "Wrote %d bytes", write_result);
        } else if (write_result < 0) {
            ESP_LOGE(TAG, "Write failed: %s", chat_stream->GetError().c_str());
        }
        
        // Finish sending
        if (chat_stream->Finish()) {
            ESP_LOGI(TAG, "Upload complete");
        } else {
            ESP_LOGE(TAG, "Failed to finish stream");
        }
        
        // Read response
        std::string chat_body;
        ESP_LOGI(TAG, "Waiting for ChatStream response...");
        uint8_t buffer[1024];
        int n;
        while ((n = chat_stream->Read(buffer, sizeof(buffer), 5000)) > 0) {
            std::string chunk(reinterpret_cast<const char*>(buffer), n);
            chat_body.append(chunk);
            // Print streaming response in real-time
            ESP_LOGI(TAG, "[ChatStream] %s", chunk.c_str());
        }
        
        ESP_LOGI(TAG, "--- ChatStream Results ---");
        ESP_LOGI(TAG, "Status: %d", chat_stream->GetStatus());
        ESP_LOGI(TAG, "Response size: %zu bytes", chat_body.size());
    } else {
        ESP_LOGE(TAG, "Failed to open ChatStream");
    }
    
    // =========================================
    // Phase 4: Wait for server to close connection
    // =========================================
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "=========================================");
    ESP_LOGI(TAG, "Phase 4: Waiting for server to close");
    ESP_LOGI(TAG, "=========================================");
    ESP_LOGI(TAG, "Connection will stay open until server closes it (idle timeout: %u ms)", config.idle_timeout_ms);
    
    // Print connection statistics
    auto stats = client.GetStatistics();
    ESP_LOGI(TAG, "=== Connection Stats ===");
    ESP_LOGI(TAG, "  Packets sent: %u", stats.packets_sent);
    ESP_LOGI(TAG, "  Packets received: %u", stats.packets_received);
    ESP_LOGI(TAG, "  Bytes sent: %u", stats.bytes_sent);
    ESP_LOGI(TAG, "  Bytes received: %u", stats.bytes_received);
    ESP_LOGI(TAG, "  RTT: %u ms", stats.rtt_ms);
    
    // Keep waiting until connection is closed
    int idle_seconds = 0;
    while (client.IsConnected()) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        idle_seconds += 10;
        ESP_LOGI(TAG, "  Connection alive, idle for %d seconds...", idle_seconds);
    }
    
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "=========================================");
    ESP_LOGI(TAG, "=== Test Complete ===");
    ESP_LOGI(TAG, "=========================================");
    ESP_LOGI(TAG, "Connection closed by server after %d seconds idle", idle_seconds);
}

/**
 * Test HTTP3 download speed and write to OTA partition
 * 
 * Downloads a file from the specified URL and measures the speed.
 * Writes the downloaded firmware to OTA partition.
 */
void TestHttp3DownloadSpeed(const char* hostname, uint16_t port, const char* path) {
    ESP_LOGI(TAG, "===========================================");
    ESP_LOGI(TAG, "=== HTTP3 Download Speed Test (with OTA) ===");
    ESP_LOGI(TAG, "===========================================");
    ESP_LOGI(TAG, "Target: https://%s:%u%s", hostname, port, path);
    
    // 1. Initialize Http3Client
    Http3ClientConfig config;
    config.hostname = hostname;
    config.port = port;
    config.connect_timeout_ms = 10000;
    config.request_timeout_ms = 300000;  // 5 minutes for large downloads
    config.idle_timeout_ms = 120000;
    config.receive_buffer_size = 128 * 1024;
    config.enable_debug = false;  // Disable debug for speed test
    
    Http3Client client(config);
    
    // 2. Establish connection
    ESP_LOGI(TAG, "Establishing QUIC connection...");
    int64_t connect_start = esp_timer_get_time();
    
    // Connection is established on first Open() call
    // We'll establish it with a test request if needed
    if (!client.IsConnected()) {
        Http3Request test_req;
        test_req.method = "GET";
        test_req.path = "/";
        auto test_stream = client.Open(test_req, 20000);
        if (!test_stream) {
            ESP_LOGE(TAG, "Failed to connect");
            return;
        }
        test_stream->Close();
    }
    
    int64_t connect_time = esp_timer_get_time() - connect_start;
    ESP_LOGI(TAG, "Connection established in %.2f ms", connect_time / 1000.0);
    
    // 3. Prepare OTA partition
    esp_ota_handle_t update_handle = 0;
    const esp_partition_t* update_partition = esp_ota_get_next_update_partition(NULL);
    
    if (update_partition == NULL) {
        ESP_LOGE(TAG, "Failed to get update partition");
        return;
    }
    
    ESP_LOGI(TAG, "Writing to OTA partition %s at offset 0x%lx", 
             update_partition->label, update_partition->address);
    
    // 4. Download test
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "Starting download...");
    
    // Download statistics
    struct DownloadStats {
        int status = 0;
        size_t total_bytes = 0;
        size_t bytes_since_last_report = 0;
        size_t chunk_count = 0;
        int64_t first_byte_time = 0;  // Time to first byte (us)
        int64_t start_time = 0;
        int64_t last_report_time = 0;
        int64_t end_time = 0;
        bool success = false;
        std::string error;
        std::string content_length;
    } stats;
    
    stats.start_time = esp_timer_get_time();
    stats.last_report_time = stats.start_time;
    
    Http3Request req;
    req.method = "GET";
    req.path = path;
    
    auto stream = client.Open(req, 30000);
    if (stream && stream->IsValid()) {
        stats.status = stream->GetStatus(30000);
        ESP_LOGI(TAG, "[Stream %d] Status: %d", stream->GetStreamId(), stats.status);
        
        // Find content-length header
        for (const auto& h : stream->GetHeaders()) {
            if (h.first == "content-length") {
                stats.content_length = h.second;
                ESP_LOGI(TAG, "[Stream %d] Content-Length: %s", stream->GetStreamId(), h.second.c_str());
            }
        }
        
        // Read response data and write to OTA partition
        static uint8_t buffer[4096];
        int n;
        bool image_header_checked = false;
        std::string image_header;
        bool ota_started = false;
        
        while ((n = stream->Read(buffer, sizeof(buffer), 5000)) > 0) {
            // Record time to first byte
            if (stats.total_bytes == 0) {
                stats.first_byte_time = esp_timer_get_time() - stats.start_time;
            }
            
            // Check image header and begin OTA if not started
            if (!image_header_checked) {
                image_header.append(reinterpret_cast<const char*>(buffer), n);
                
                if (image_header.size() >= sizeof(esp_image_header_t) + 
                    sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t)) {
                    
                    // Verify image header
                    esp_app_desc_t new_app_info;
                    std::memcpy(&new_app_info, 
                           image_header.data() + sizeof(esp_image_header_t) + 
                           sizeof(esp_image_segment_header_t), 
                           sizeof(esp_app_desc_t));
                    
                    auto current_version = esp_app_get_description()->version;
                    ESP_LOGI(TAG, "Current version: %s, New version: %s", 
                             current_version, new_app_info.version);
                    
                    // Begin OTA
                    esp_err_t err = esp_ota_begin(update_partition, 
                                                   OTA_WITH_SEQUENTIAL_WRITES, 
                                                   &update_handle);
                    
                    if (err != ESP_OK) {
                        ESP_LOGE(TAG, "Failed to begin OTA: %s", esp_err_to_name(err));
                        stats.success = false;
                        stats.error = "OTA begin failed";
                        break;
                    }
                    
                    // Write the buffered image header
                    err = esp_ota_write(update_handle, 
                                       reinterpret_cast<const uint8_t*>(image_header.data()), 
                                       image_header.size());
                    
                    if (err != ESP_OK) {
                        ESP_LOGE(TAG, "Failed to write OTA header: %s", esp_err_to_name(err));
                        esp_ota_abort(update_handle);
                        stats.success = false;
                        stats.error = "OTA write header failed";
                        break;
                    }
                    stats.total_bytes += image_header.size();
                    image_header_checked = true;
                    ota_started = true;
                    std::string().swap(image_header);
                    
                    // Give IDLE task chance to run
                    vTaskDelay(pdMS_TO_TICKS(10));
                    
                    // Skip to next iteration since we already wrote the header
                    stats.bytes_since_last_report += n;
                    stats.chunk_count++;
                    continue;
                }
            }
            
            // Write remaining data to OTA partition
            if (ota_started && update_handle != 0) {
                // ESP_LOGI(TAG, "Writing OTA data to partition %s at offset 0x%lx bytes: %zu", 
                //          update_partition->label, update_partition->address, n);
                // esp_err_t err = esp_ota_write(update_handle, buffer, n);
                
                // if (err != ESP_OK) {
                //     ESP_LOGE(TAG, "Failed to write OTA data: %s", esp_err_to_name(err));
                //     esp_ota_abort(update_handle);
                //     stats.success = false;
                //     stats.error = "OTA write data failed";
                //     break;
                // }

                // ESP_LOGI(TAG, "Wrote OTA data to partition %s at offset 0x%lx bytes: %zu", 
                //          update_partition->label, update_partition->address, n);
                vTaskDelay(pdMS_TO_TICKS(1));
            }
            
            stats.total_bytes += n;
            stats.bytes_since_last_report += n;
            stats.chunk_count++;
            
            // Report every second
            int64_t now = esp_timer_get_time();
            int64_t elapsed_since_report = now - stats.last_report_time;
            
            if (elapsed_since_report >= 1000000) {
                float instant_speed_bytes_per_sec = (stats.bytes_since_last_report * 1000000.0f) / elapsed_since_report;
                float overall_speed_bytes_per_sec = (stats.total_bytes * 1000000.0f) / (now - stats.start_time);
                
                ESP_LOGI(TAG, "Downloaded: %zu bytes (%.1f KB), Instant: %.1f KB/s (%.0f B/s), Average: %.1f KB/s (%.0f B/s)",
                         stats.total_bytes, stats.total_bytes / 1024.0f,
                         instant_speed_bytes_per_sec / 1024.0f, instant_speed_bytes_per_sec,
                         overall_speed_bytes_per_sec / 1024.0f, overall_speed_bytes_per_sec);
                
                stats.last_report_time = now;
                stats.bytes_since_last_report = 0;
            }
        }
        
        stats.end_time = esp_timer_get_time();
        
        if (n < 0) {
            // Error occurred
            stats.success = false;
            stats.error = stream->GetError();
            ESP_LOGE(TAG, "[Stream %d] Download failed: %s", stream->GetStreamId(), stats.error.c_str());
            if (update_handle != 0) {
                esp_ota_abort(update_handle);
            }
        } else if (!ota_started) {
            // OTA not started (image header not complete)
            stats.success = false;
            stats.error = "Incomplete image header";
            ESP_LOGE(TAG, "Image header incomplete or download too small");
        } else {
            // Success (n == 0 means EOF)
            ESP_LOGI(TAG, "[Stream %d] Download complete, finalizing OTA", stream->GetStreamId());
            
            // End OTA
            // esp_err_t err = esp_ota_end(update_handle);
            // if (err != ESP_OK) {
            //     if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
            //         ESP_LOGE(TAG, "Image validation failed, image is corrupted");
            //     } else {
            //         ESP_LOGE(TAG, "Failed to end OTA: %s", esp_err_to_name(err));
            //     }
            //     stats.success = false;
            //     stats.error = "OTA end failed";
            // } else {
            //     // Set boot partition
            //     err = esp_ota_set_boot_partition(update_partition);
            //     if (err != ESP_OK) {
            //         ESP_LOGE(TAG, "Failed to set boot partition: %s", esp_err_to_name(err));
            //         stats.success = false;
            //         stats.error = "Set boot partition failed";
            //     } else {
            //         stats.success = true;
            //         ESP_LOGI(TAG, "Firmware upgrade successful");
            //     }
            // }
        }
    } else {
        ESP_LOGE(TAG, "Failed to open download stream");
        stats.success = false;
        stats.error = "Failed to open stream";
        stats.end_time = esp_timer_get_time();
    }
    
    // 5. Print results
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "===========================================");
    ESP_LOGI(TAG, "=== Download Speed Test Results ===");
    ESP_LOGI(TAG, "===========================================");
    ESP_LOGI(TAG, "URL: https://%s:%u%s", hostname, port, path);
    ESP_LOGI(TAG, "Status: %d", stats.status);
    ESP_LOGI(TAG, "Success: %s", stats.success ? "Yes" : "No");
    
    if (!stats.success && !stats.error.empty()) {
        ESP_LOGI(TAG, "Error: %s", stats.error.c_str());
    }
    
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "--- Timing ---");
    ESP_LOGI(TAG, "Connection time: %.2f ms", connect_time / 1000.0);
    ESP_LOGI(TAG, "Time to first byte: %.2f ms", stats.first_byte_time / 1000.0);
    
    int64_t total_time = stats.end_time - stats.start_time;
    ESP_LOGI(TAG, "Total download time: %.2f ms", total_time / 1000.0);
    
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "--- Data ---");
    if (!stats.content_length.empty()) {
        ESP_LOGI(TAG, "Expected size: %s bytes", stats.content_length.c_str());
    }
    ESP_LOGI(TAG, "Downloaded: %zu bytes (%.2f KB, %.2f MB)", 
             stats.total_bytes, 
             stats.total_bytes / 1024.0,
             stats.total_bytes / (1024.0 * 1024.0));
    ESP_LOGI(TAG, "Chunks received: %zu", stats.chunk_count);
    
    if (stats.total_bytes > 0 && total_time > 0) {
        float elapsed_sec = total_time / 1000000.0;
        float speed_bps = stats.total_bytes * 8.0 / elapsed_sec;
        float speed_kbps = speed_bps / 1024.0;
        float speed_mbps = speed_kbps / 1024.0;
        float speed_kb_sec = stats.total_bytes / 1024.0 / elapsed_sec;
        float speed_mb_sec = speed_kb_sec / 1024.0;
        
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "--- Speed ---");
        ESP_LOGI(TAG, "Average speed: %.2f KB/s (%.2f MB/s)", speed_kb_sec, speed_mb_sec);
        ESP_LOGI(TAG, "Average speed: %.2f Kbps (%.2f Mbps)", speed_kbps, speed_mbps);
    }
    
    // Print connection stats
    auto conn_stats = client.GetStatistics();
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "--- Connection Stats ---");
    ESP_LOGI(TAG, "Packets sent: %u", conn_stats.packets_sent);
    ESP_LOGI(TAG, "Packets received: %u", conn_stats.packets_received);
    ESP_LOGI(TAG, "Total bytes sent: %u", conn_stats.bytes_sent);
    ESP_LOGI(TAG, "Total bytes received: %u", conn_stats.bytes_received);
    ESP_LOGI(TAG, "RTT: %u ms", conn_stats.rtt_ms);
    
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "===========================================");
}

