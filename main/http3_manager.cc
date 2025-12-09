/*
 * HTTP/3 Manager Implementation
 * 
 * Manages a persistent QUIC/HTTP3 connection with background event loop.
 */

#include "http3_manager.h"
#include <esp_log.h>
#include <esp_timer.h>
#include <lwip/sockets.h>
#include <lwip/netdb.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <errno.h>

#define TAG "Http3Manager"

// Singleton instance
Http3Manager& Http3Manager::GetInstance() {
    static Http3Manager instance;
    return instance;
}

Http3Manager::Http3Manager() = default;

Http3Manager::~Http3Manager() {
    Deinit();
}

// ==================== Initialization ====================

bool Http3Manager::Init(const Http3ManagerConfig& config) {
    if (initialized_) {
        ESP_LOGW(TAG, "Already initialized");
        return true;
    }
    
    ESP_LOGI(TAG, "Initializing HTTP/3 Manager for %s:%u", 
             config.hostname.c_str(), config.port);
    
    config_ = config;
    
    // Create event group for unified event handling
    event_group_ = xEventGroupCreate();
    if (!event_group_) {
        ESP_LOGE(TAG, "Failed to create event group");
        return false;
    }
    
    // Create UDP packet queue (recv task -> event loop, passes pointers)
    udp_queue_ = xQueueCreate(UDP_QUEUE_SIZE, sizeof(UdpPacket*));
    if (!udp_queue_) {
        ESP_LOGE(TAG, "Failed to create UDP queue");
        vEventGroupDelete(event_group_);
        event_group_ = nullptr;
        return false;
    }
    
    initialized_ = true;
    return true;
}

void Http3Manager::Deinit() {
    if (!initialized_) {
        return;
    }
    
    ESP_LOGI(TAG, "Deinitializing HTTP/3 Manager");
    
    // Stop tasks
    StopTasks();
    
    // Disconnect
    Disconnect();
    
    // Cleanup event group
    if (event_group_) {
        vEventGroupDelete(event_group_);
        event_group_ = nullptr;
    }
    
    // Cleanup UDP queue
    if (udp_queue_) {
        vQueueDelete(udp_queue_);
        udp_queue_ = nullptr;
    }
    
    // Cleanup streams
    {
        std::lock_guard<std::mutex> lock(streams_mutex_);
        for (auto& [id, ctx] : streams_) {
            if (ctx->complete_sem) {
                vSemaphoreDelete(ctx->complete_sem);
            }
        }
        streams_.clear();
    }
    
    initialized_ = false;
    ESP_LOGI(TAG, "HTTP/3 Manager deinitialized");
}

void Http3Manager::SetIdentifiers(const std::string& device_id, const std::string& client_id) {
    device_id_ = device_id;
    client_id_ = client_id;
}


// ==================== Connection Management ====================

bool Http3Manager::CreateSocket() {
    ESP_LOGD(TAG, "Creating UDP socket for %s:%u", 
             config_.hostname.c_str(), config_.port);
    
    // DNS lookup
    struct hostent* he = gethostbyname(config_.hostname.c_str());
    if (!he) {
        ESP_LOGE(TAG, "DNS lookup failed for %s", config_.hostname.c_str());
        return false;
    }
    
    struct in_addr* addr = (struct in_addr*)he->h_addr;
    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, addr, ip_str, sizeof(ip_str));
    ESP_LOGI(TAG, "Resolved %s to %s", config_.hostname.c_str(), ip_str);
    
    // Create UDP socket (blocking mode - recv will be unblocked by closing socket)
    udp_socket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (udp_socket_ < 0) {
        ESP_LOGE(TAG, "Failed to create socket: %d", errno);
        return false;
    }
    
    // Connect to server
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(config_.port);
    server_addr.sin_addr = *addr;
    
    if (connect(udp_socket_, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "Failed to connect socket: %d", errno);
        close(udp_socket_);
        udp_socket_ = -1;
        return false;
    }
    
    return true;
}

void Http3Manager::CloseSocket() {
    if (udp_socket_ >= 0) {
        close(udp_socket_);
        udp_socket_ = -1;
        ESP_LOGD(TAG, "UDP socket closed");
    }
}

bool Http3Manager::IsConnected() const {
    std::lock_guard<std::mutex> lock(connection_mutex_);
    return connected_;
}

bool Http3Manager::EnsureConnected(uint32_t timeout_ms) {
    // Check if already connected
    if (IsConnected()) {
        return true;
    }
    
    // Use unique_lock so we can release the lock before waiting
    std::unique_lock<std::mutex> lock(connection_mutex_);
    
    // Double-check after acquiring lock
    if (connected_) {
        return true;
    }
    
    // Cleanup stale connection resources if marked for cleanup
    if (needs_cleanup_) {
        ESP_LOGI(TAG, "Cleaning up stale connection resources...");
        
        // Stop tasks first (release lock temporarily)
        lock.unlock();
        StopTasks();
        lock.lock();
        
        // Reset connection and close socket
        connection_.reset();
        CloseSocket();
        needs_cleanup_ = false;
        
        // Clear stale event bits (important: EVENT_DISCONNECTED may have been set by idle timeout)
        xEventGroupClearBits(event_group_, EVENT_CONNECTED | EVENT_DISCONNECTED);
        
        ESP_LOGI(TAG, "Stale connection cleanup completed");
    }
    
    ESP_LOGI(TAG, "Establishing QUIC connection...");
    
    // Create socket if needed
    if (udp_socket_ < 0) {
        if (!CreateSocket()) {
            return false;
        }
    }
    
    // Create QUIC configuration
    esp_http3::QuicConfig quic_config;
    quic_config.hostname = config_.hostname;
    quic_config.port = config_.port;
    quic_config.handshake_timeout_ms = config_.connect_timeout_ms;
    quic_config.idle_timeout_ms = config_.idle_timeout_ms;
    quic_config.response_timeout_ms = config_.request_timeout_ms;
    quic_config.enable_debug = config_.enable_debug;
    
    // Create QUIC connection with send callback
    // Capture 'this' to check socket validity (socket may be closed during disconnect)
    connection_ = std::make_unique<esp_http3::QuicConnection>(
        [this](const uint8_t* data, size_t len) -> int {
            // Check if socket is still valid before sending
            if (udp_socket_ < 0) {
                // Socket already closed (likely during disconnect), silently fail
                return -1;
            }
            int sent = send(udp_socket_, data, len, 0);
            if (sent < 0) {
                // Only log if socket is still valid (not just closed)
                // errno 9 (EBADF) is expected when socket is closed
                if (udp_socket_ >= 0 && errno != EBADF) {
                    ESP_LOGW(TAG, "Socket send failed: %d", errno);
                }
            }
            return sent;
        },
        quic_config
    );
    
    // Set callbacks
    connection_->SetOnConnected([this]() {
        OnConnected();
    });
    
    connection_->SetOnDisconnected([this](int code, const std::string& reason) {
        OnDisconnected(code, reason);
    });
    
    connection_->SetOnResponse([this](int stream_id, const esp_http3::H3Response& resp) {
        OnResponse(stream_id, resp);
    });
    
    connection_->SetOnStreamData([this](int stream_id, const uint8_t* data, size_t len, bool fin) {
        OnStreamData(stream_id, data, len, fin);
    });
    
    connection_->SetOnWriteComplete([this](int stream_id, size_t total_bytes) {
        OnWriteComplete(stream_id, total_bytes);
    });
    
    connection_->SetOnWriteError([this](int stream_id, int error_code, const std::string& reason) {
        OnWriteError(stream_id, error_code, reason);
    });
    
    // Start tasks if not running
    if (!event_loop_task_) {
        if (!StartTasks()) {
            ESP_LOGE(TAG, "Failed to start tasks");
            connection_.reset();
            CloseSocket();
            return false;
        }
    }
    
    // Start handshake
    ESP_LOGI(TAG, "Starting QUIC handshake...");
    if (!connection_->StartHandshake()) {
        ESP_LOGE(TAG, "Failed to start handshake");
        StopTasks();
        connection_.reset();
        CloseSocket();
        return false;
    }
    
    // IMPORTANT: Release the lock before waiting, so RunEventLoop can process data
    lock.unlock();
    
    // Wait for connection with timeout
    EventBits_t bits = xEventGroupWaitBits(
        event_group_,
        EVENT_CONNECTED | EVENT_DISCONNECTED,
        pdTRUE,  // Clear bits
        pdFALSE, // Wait for any bit
        pdMS_TO_TICKS(timeout_ms)
    );
    
    if (bits & EVENT_CONNECTED) {
        ESP_LOGI(TAG, "QUIC connection established");
        return true;
    }
    
    if (bits & EVENT_DISCONNECTED) {
        ESP_LOGE(TAG, "Connection failed");
    } else {
        ESP_LOGE(TAG, "Connection timeout");
    }
    
    // Cleanup on failure - need to re-acquire lock
    lock.lock();
    StopTasks();
    connection_.reset();
    CloseSocket();
    return false;
}

void Http3Manager::Disconnect() {
    std::lock_guard<std::mutex> lock(connection_mutex_);
    
    if (connection_) {
        ESP_LOGI(TAG, "Closing QUIC connection");
        connection_->Close(0, "Client disconnect");
        connection_.reset();
    }
    
    CloseSocket();
    connected_ = false;
}

// ==================== Tasks ====================

bool Http3Manager::StartTasks() {
    if (event_loop_task_) {
        return true;  // Already running
    }
    
    stop_tasks_ = false;
    xEventGroupClearBits(event_group_, EVENT_STOP);
    
    // Create UDP receive task (small stack, only does recv)
    BaseType_t result = xTaskCreate(
        UdpRecvTask,
        "http3_udp_recv",
        2048,  // Small stack for UDP recv only
        this,
        6,     // Higher priority than event loop
        &udp_recv_task_
    );
    
    if (result != pdPASS) {
        ESP_LOGE(TAG, "Failed to create UDP recv task");
        return false;
    }
    
    // Create event loop task
    result = xTaskCreate(
        EventLoopTask,
        "http3_event_loop",
        6144,  // Reduced since UDP recv is separate
        this,
        5,     // Priority
        &event_loop_task_
    );
    
    if (result != pdPASS) {
        ESP_LOGE(TAG, "Failed to create event loop task");
        // Stop UDP task
        stop_tasks_ = true;
        xEventGroupSetBits(event_group_, EVENT_STOP);
        vTaskDelay(pdMS_TO_TICKS(100));
        if (udp_recv_task_) {
            vTaskDelete(udp_recv_task_);
            udp_recv_task_ = nullptr;
        }
        return false;
    }
    
    ESP_LOGI(TAG, "Tasks started (UDP recv + event loop)");
    return true;
}

void Http3Manager::StopTasks() {
    if (!event_loop_task_ && !udp_recv_task_) {
        return;
    }
    
    ESP_LOGI(TAG, "Stopping tasks");
    stop_tasks_ = true;
    xEventGroupSetBits(event_group_, EVENT_STOP);
    
    // Close socket to unblock recv() in UDP task
    CloseSocket();
    
    // Wait for tasks to finish (with timeout)
    for (int i = 0; i < 100 && (event_loop_task_ || udp_recv_task_); i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    
    if (event_loop_task_) {
        ESP_LOGW(TAG, "Event loop task did not finish, deleting");
        vTaskDelete(event_loop_task_);
        event_loop_task_ = nullptr;
    }
    
    if (udp_recv_task_) {
        ESP_LOGW(TAG, "UDP recv task did not finish, deleting");
        vTaskDelete(udp_recv_task_);
        udp_recv_task_ = nullptr;
    }
    
    // Drain queue and free any remaining packets
    UdpPacket* packet;
    while (xQueueReceive(udp_queue_, &packet, 0) == pdTRUE) {
        heap_caps_free(packet);
    }
}

void Http3Manager::EventLoopTask(void* param) {
    Http3Manager* self = static_cast<Http3Manager*>(param);
    self->RunEventLoop();
    self->event_loop_task_ = nullptr;
    vTaskDelete(nullptr);
}

void Http3Manager::UdpRecvTask(void* param) {
    Http3Manager* self = static_cast<Http3Manager*>(param);
    self->RunUdpRecv();
    self->udp_recv_task_ = nullptr;
    vTaskDelete(nullptr);
}

void Http3Manager::RunUdpRecv() {
    ESP_LOGD(TAG, "UDP recv task started");
    
    while (!stop_tasks_) {
        if (udp_socket_ < 0) {
            // Socket closed or not created yet - exit loop
            break;
        }
        
        // Allocate packet on heap
        UdpPacket* packet = static_cast<UdpPacket*>(heap_caps_malloc(sizeof(UdpPacket), MALLOC_CAP_SPIRAM));
        if (!packet) {
            ESP_LOGW(TAG, "Failed to allocate UDP packet");
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        
        // Blocking recv - will return error when socket is closed
        int recv_len = recv(udp_socket_, packet->data, sizeof(packet->data), 0);
        if (recv_len > 0) {
            packet->len = static_cast<size_t>(recv_len);
            // Send pointer to queue (event loop has larger stack for processing)
            if (xQueueSend(udp_queue_, &packet, 0) == pdTRUE) {
                xEventGroupSetBits(event_group_, EVENT_UDP_DATA);
            } else {
                ESP_LOGW(TAG, "UDP queue full, dropping packet");
                heap_caps_free(packet);
            }
        } else {
            // recv_len <= 0: socket closed or error - exit loop
            heap_caps_free(packet);
            break;
        }
    }
    
    ESP_LOGD(TAG, "UDP recv task stopped");
}

void Http3Manager::RunEventLoop() {
    ESP_LOGD(TAG, "Event loop started");
    
    // Default wait time when no connection
    static constexpr uint32_t kDefaultWaitMs = 60000;
    // Minimum wait to avoid busy loop when PTO is very short
    static constexpr uint32_t kMinWaitMs = 1;
    uint32_t next_timer_ms = kDefaultWaitMs;
    int64_t last_tick_time = esp_timer_get_time() / 1000;  // in ms
    
    UdpPacket* packet;
    
    // Events to wait for
    const EventBits_t wait_bits = EVENT_UDP_DATA | EVENT_WAKE | EVENT_STOP;
    
    while (!stop_tasks_) {
        // 1. Process all queued UDP packets
        while (xQueueReceive(udp_queue_, &packet, 0) == pdTRUE) {
            {
                std::lock_guard<std::mutex> lock(connection_mutex_);
                if (connection_) {
                    connection_->ProcessReceivedData(packet->data, packet->len);
                }
            }
            heap_caps_free(packet);
        }
        
        // 2. Calculate elapsed time since last tick
        int64_t current_time = esp_timer_get_time() / 1000;
        uint32_t elapsed_ms = static_cast<uint32_t>(current_time - last_tick_time);
        last_tick_time = current_time;
        next_timer_ms = kDefaultWaitMs;
        
        // 3. Timer tick - process QUIC timers and get next interval
        {
            std::lock_guard<std::mutex> lock(connection_mutex_);
            // Only call OnTimerTick if connection exists and not marked for cleanup
            // (needs_cleanup_ means connection is already closed due to idle timeout etc.)
            if (connection_ && !needs_cleanup_) {
                next_timer_ms = connection_->OnTimerTick(elapsed_ms);
            }
        }
        
        // 4. Wait for events or timeout (timeout acts as timer)
        if (next_timer_ms < kMinWaitMs) {
            next_timer_ms = kMinWaitMs;
        } else if (next_timer_ms > kDefaultWaitMs) {
            next_timer_ms = kDefaultWaitMs;
        }

        if (config_.enable_debug) {
            ESP_LOGW(TAG, "Waiting for events or timeout, next_timer_ms=%d", next_timer_ms);
        }
        
        EventBits_t bits = xEventGroupWaitBits(
            event_group_,
            wait_bits,
            pdTRUE,   // Clear bits on exit
            pdFALSE,  // Wait for any bit
            pdMS_TO_TICKS(next_timer_ms)
        );
        
        if (bits & EVENT_STOP) {
            break;
        }
    }
    
    ESP_LOGD(TAG, "Event loop stopped");
}

void Http3Manager::WakeEventLoop() {
    if (event_group_) {
        xEventGroupSetBits(event_group_, EVENT_WAKE);
    }
}

// ==================== QUIC Callbacks ====================

void Http3Manager::OnConnected() {
    connected_ = true;
    xEventGroupSetBits(event_group_, EVENT_CONNECTED);
}

void Http3Manager::OnDisconnected(int error_code, const std::string& reason) {
    ESP_LOGW(TAG, "QUIC disconnected: code=%d, reason=%s", error_code, reason.c_str());
    connected_ = false;
    xEventGroupSetBits(event_group_, EVENT_DISCONNECTED);
    
    // Collect callbacks to invoke outside the lock to prevent deadlock
    // (callbacks may call CleanupStream which needs streams_mutex_)
    std::vector<std::pair<int, std::function<void(int, bool, const std::string&)>>> callbacks_to_invoke;
    std::vector<SemaphoreHandle_t> sems_to_give;
    
    // Notify all active streams of disconnection and cleanup
    {
        std::lock_guard<std::mutex> lock(streams_mutex_);
        for (auto& [id, ctx] : streams_) {
            if (ctx->state != StreamState::COMPLETED && 
                ctx->state != StreamState::CANCELLED &&
                ctx->state != StreamState::ERROR) {
                ctx->state = StreamState::ERROR;
                ctx->response.error = reason;
                
                if (ctx->callbacks.on_complete) {
                    callbacks_to_invoke.emplace_back(id, ctx->callbacks.on_complete);
                }
                
                if (ctx->complete_sem) {
                    sems_to_give.push_back(ctx->complete_sem);
                }
            }
        }
        
        // Clear all streams on disconnect (prevents memory leak)
        for (auto& [id, ctx] : streams_) {
            if (ctx->complete_sem) {
                // Note: semaphores will be given below, then deleted
                ctx->complete_sem = nullptr;
            }
        }
        streams_.clear();
    }
    
    // Invoke callbacks outside the lock
    for (const auto& [id, callback] : callbacks_to_invoke) {
        callback(id, false, reason);
    }
    
    // Give and delete semaphores
    for (auto sem : sems_to_give) {
        xSemaphoreGive(sem);
        vSemaphoreDelete(sem);
    }
    
    // Mark connection as needing cleanup and stop tasks to free resources
    // Close socket first to unblock recv() in UDP task
    needs_cleanup_ = true;
    CloseSocket();
    
    // Signal tasks to stop (event loop will exit after this callback returns)
    stop_tasks_ = true;
    xEventGroupSetBits(event_group_, EVENT_STOP);
    
    ESP_LOGD(TAG, "Connection closed, tasks will stop");
}

void Http3Manager::OnResponse(int stream_id, const esp_http3::H3Response& resp) {
    ESP_LOGI(TAG, "Response on stream %d: status=%d", stream_id, resp.status);
    
    // Copy callback and data to invoke outside the lock
    std::function<void(int, int, const std::vector<std::pair<std::string, std::string>>&)> headers_callback;
    int status = resp.status;
    std::vector<std::pair<std::string, std::string>> headers = resp.headers;
    
    {
        std::lock_guard<std::mutex> lock(streams_mutex_);
        auto it = streams_.find(stream_id);
        if (it == streams_.end()) {
            ESP_LOGW(TAG, "Unknown stream %d", stream_id);
            return;
        }
        
        auto& ctx = it->second;
        ctx->response.status = resp.status;
        ctx->response.headers = resp.headers;
        ctx->state = StreamState::RECEIVING;
        
        headers_callback = ctx->callbacks.on_headers;
    }
    
    // Invoke callback outside the lock to prevent potential deadlock
    if (headers_callback) {
        headers_callback(stream_id, status, headers);
    }
}

void Http3Manager::OnStreamData(int stream_id, const uint8_t* data, size_t len, bool fin) {
    // Variables to hold callbacks and data to invoke outside the lock
    std::function<void(int, const uint8_t*, size_t, bool)> data_callback;
    std::function<void(int, bool, const std::string&)> complete_callback;
    SemaphoreHandle_t complete_sem = nullptr;
    bool should_complete = false;
    bool success = false;
    std::string error_msg;
    
    {
        std::lock_guard<std::mutex> lock(streams_mutex_);
        auto it = streams_.find(stream_id);
        if (it == streams_.end()) {
            return;
        }
        
        auto& ctx = it->second;
        
        // Accumulate body for sync requests, or call callback for async
        if (ctx->sync_request && data && len > 0) {
            ctx->response.body.append(reinterpret_cast<const char*>(data), len);
        }
        
        // Copy callback to invoke outside lock
        data_callback = ctx->callbacks.on_data;
        
        if (fin) {
            ctx->state = StreamState::COMPLETED;
            ctx->response.complete = true;
            
            // Check if HTTP status is in success range (200-299)
            success = (ctx->response.status >= 200 && ctx->response.status < 300);
            if (!success) {
                // Use response body as error message if available, otherwise use status code
                if (!ctx->response.body.empty()) {
                    error_msg = ctx->response.body;
                } else {
                    error_msg = "HTTP error: " + std::to_string(ctx->response.status);
                }
                ESP_LOGW(TAG, "Stream %d completed with HTTP error: status=%d", 
                         stream_id, ctx->response.status);
            }
            
            complete_callback = ctx->callbacks.on_complete;
            complete_sem = ctx->complete_sem;
            should_complete = true;
        }
    }
    
    // Invoke callbacks outside the lock to prevent deadlock
    // (callbacks may call CleanupStream which needs streams_mutex_)
    if (data_callback) {
        data_callback(stream_id, data, len, fin);
    }
    
    if (should_complete) {
        if (complete_callback) {
            complete_callback(stream_id, success, error_msg);
        }
        
        if (complete_sem) {
            xSemaphoreGive(complete_sem);
        }
    }
}

void Http3Manager::OnWriteComplete(int stream_id, size_t total_bytes) {
    ESP_LOGI(TAG, "Write complete on stream %d: %zu bytes", stream_id, total_bytes);
    
    // Copy callback to invoke outside the lock
    std::function<void(int, size_t)> write_complete_callback;
    
    {
        std::lock_guard<std::mutex> lock(streams_mutex_);
        auto it = streams_.find(stream_id);
        if (it == streams_.end()) {
            return;
        }
        
        auto& ctx = it->second;
        write_complete_callback = ctx->callbacks.on_write_complete;
    }
    
    // Invoke callback outside the lock for consistency
    if (write_complete_callback) {
        write_complete_callback(stream_id, total_bytes);
    }
}

void Http3Manager::OnWriteError(int stream_id, int error_code, const std::string& reason) {
    ESP_LOGE(TAG, "Write error on stream %d: code=%d, reason=%s", 
             stream_id, error_code, reason.c_str());
    
    // Copy callback to invoke outside the lock
    std::function<void(int, bool, const std::string&)> complete_callback;
    SemaphoreHandle_t complete_sem = nullptr;
    
    {
        std::lock_guard<std::mutex> lock(streams_mutex_);
        auto it = streams_.find(stream_id);
        if (it == streams_.end()) {
            return;
        }
        
        auto& ctx = it->second;
        ctx->state = StreamState::ERROR;
        ctx->response.error = reason;
        
        complete_callback = ctx->callbacks.on_complete;
        complete_sem = ctx->complete_sem;
    }
    
    // Invoke callback outside the lock to prevent deadlock
    if (complete_callback) {
        complete_callback(stream_id, false, reason);
    }
    
    if (complete_sem) {
        xSemaphoreGive(complete_sem);
    }
}

// ==================== Request Methods ====================

bool Http3Manager::SendRequest(const Http3Request& request, Http3Response& response,
                                uint32_t timeout_ms) {
    // Ensure connected
    if (!EnsureConnected(config_.connect_timeout_ms)) {
        response.error = "Failed to connect";
        return false;
    }
    
    // Create stream context with semaphore for sync wait
    auto ctx = std::make_unique<StreamContext>();
    ctx->sync_request = true;
    ctx->complete_sem = xSemaphoreCreateBinary();
    if (!ctx->complete_sem) {
        response.error = "Failed to create semaphore";
        return false;
    }
    
    int stream_id;
    
    // Open stream and send request
    {
        std::lock_guard<std::mutex> lock(connection_mutex_);
        
        if (!connection_ || !connected_) {
            vSemaphoreDelete(ctx->complete_sem);
            response.error = "Not connected";
            return false;
        }
        
        // Add common headers
        auto headers = request.headers;
        if (!device_id_.empty()) {
            headers.push_back({"device-id", device_id_});
        }
        if (!client_id_.empty()) {
            headers.push_back({"client-id", client_id_});
        }
        
        if (request.body && request.body_size > 0) {
            // Request with body
            stream_id = connection_->SendRequest(
                request.method, request.path, headers,
                request.body, request.body_size
            );
        } else {
            // Request without body
            stream_id = connection_->SendRequest(request.method, request.path, headers);
        }
        
        if (stream_id < 0) {
            vSemaphoreDelete(ctx->complete_sem);
            response.error = "Failed to send request";
            return false;
        }
        
        ctx->stream_id = stream_id;
        ctx->state = StreamState::WAITING;
    }
    
    // Register stream
    SemaphoreHandle_t sem = ctx->complete_sem;
    {
        std::lock_guard<std::mutex> lock(streams_mutex_);
        streams_[stream_id] = std::move(ctx);
    }
    
    // Wait for response
    if (xSemaphoreTake(sem, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        // Timeout - cancel stream
        CancelStream(stream_id);
        response.error = "Request timeout";
        return false;
    }
    
    // Get response
    {
        std::lock_guard<std::mutex> lock(streams_mutex_);
        auto it = streams_.find(stream_id);
        if (it != streams_.end()) {
            response = std::move(it->second->response);
            vSemaphoreDelete(it->second->complete_sem);
            streams_.erase(it);
        }
    }
    
    return response.complete && response.error.empty();
}

int Http3Manager::OpenStream(const Http3Request& request, const Http3StreamCallbacks& callbacks) {
    // Ensure connected
    if (!EnsureConnected(config_.connect_timeout_ms)) {
        ESP_LOGE(TAG, "Failed to connect for stream");
        return -1;
    }
    
    std::lock_guard<std::mutex> lock(connection_mutex_);
    
    if (!connection_ || !connected_) {
        ESP_LOGE(TAG, "Not connected");
        return -1;
    }
    
    // Add common headers
    auto headers = request.headers;
    if (!device_id_.empty()) {
        headers.push_back({"device-id", device_id_});
    }
    if (!client_id_.empty()) {
        headers.push_back({"client-id", client_id_});
    }
    
    // Open stream
    int stream_id = connection_->OpenStream(request.method, request.path, headers);
    if (stream_id < 0) {
        ESP_LOGE(TAG, "Failed to open stream");
        return -1;
    }
    
    // Create stream context
    auto ctx = std::make_unique<StreamContext>();
    ctx->stream_id = stream_id;
    ctx->state = StreamState::UPLOADING;
    ctx->callbacks = callbacks;
    ctx->sync_request = false;
    
    {
        std::lock_guard<std::mutex> streams_lock(streams_mutex_);
        streams_[stream_id] = std::move(ctx);
    }
    
    ESP_LOGI(TAG, "Opened stream %d for %s %s", stream_id, request.method.c_str(), request.path.c_str());
    return stream_id;
}

bool Http3Manager::QueueWrite(int stream_id, const uint8_t* data, size_t size,
                               std::function<void()> deleter) {
    std::lock_guard<std::mutex> lock(connection_mutex_);
    
    if (!connection_) {
        if (deleter) deleter();  // Clean up on failure
        return false;
    }
    
    bool result = connection_->QueueWrite(stream_id, data, size, std::move(deleter));
    if (result) {
        WakeEventLoop();  // Wake up to process queued data immediately
    }
    return result;
}

bool Http3Manager::FinishStream(int stream_id) {
    std::lock_guard<std::mutex> lock(connection_mutex_);
    
    if (!connection_) {
        return false;
    }
    
    bool result = connection_->QueueFinish(stream_id);
    if (result) {
        WakeEventLoop();  // Wake up to send FIN immediately
    }
    return result;
}

bool Http3Manager::CancelStream(int stream_id) {
    ESP_LOGI(TAG, "Cancelling stream %d", stream_id);
    
    // Update local stream state
    {
        std::lock_guard<std::mutex> lock(streams_mutex_);
        auto it = streams_.find(stream_id);
        if (it != streams_.end()) {
            it->second->state = StreamState::CANCELLED;
            if (it->second->complete_sem) {
                xSemaphoreGive(it->second->complete_sem);
            }
        }
    }
    
    // Reset the stream in QUIC connection (sends RESET_STREAM frame)
    bool result = false;
    {
        std::lock_guard<std::mutex> lock(connection_mutex_);
        if (connection_) {
            result = connection_->ResetStream(stream_id);
        }
    }
    
    // Wake up event loop to send RESET_STREAM immediately
    WakeEventLoop();
    
    return result;
}

bool Http3Manager::IsStreamActive(int stream_id) const {
    std::lock_guard<std::mutex> lock(streams_mutex_);
    auto it = streams_.find(stream_id);
    if (it == streams_.end()) {
        return false;
    }
    
    auto state = it->second->state;
    return state != StreamState::COMPLETED && 
           state != StreamState::CANCELLED && 
           state != StreamState::ERROR;
}

StreamState Http3Manager::GetStreamState(int stream_id) const {
    std::lock_guard<std::mutex> lock(streams_mutex_);
    auto it = streams_.find(stream_id);
    if (it == streams_.end()) {
        return StreamState::IDLE;
    }
    return it->second->state;
}

size_t Http3Manager::GetQueuedBytes(int stream_id) const {
    std::lock_guard<std::mutex> lock(connection_mutex_);
    if (!connection_) {
        return 0;
    }
    return connection_->GetQueuedBytes(stream_id);
}

Http3Manager::StreamContext* Http3Manager::GetStreamContext(int stream_id) {
    auto it = streams_.find(stream_id);
    if (it == streams_.end()) {
        return nullptr;
    }
    return it->second.get();
}

void Http3Manager::CleanupStream(int stream_id) {
    std::lock_guard<std::mutex> lock(streams_mutex_);
    
    auto it = streams_.find(stream_id);
    if (it != streams_.end()) {
        if (it->second->complete_sem) {
            vSemaphoreDelete(it->second->complete_sem);
        }
        streams_.erase(it);
    }
}

Http3Manager::Stats Http3Manager::GetStats() const {
    Stats stats;
    
    std::lock_guard<std::mutex> lock(connection_mutex_);
    if (connection_) {
        auto quic_stats = connection_->GetStats();
        stats.packets_sent = quic_stats.packets_sent;
        stats.packets_received = quic_stats.packets_received;
        stats.bytes_sent = quic_stats.bytes_sent;
        stats.bytes_received = quic_stats.bytes_received;
        stats.rtt_ms = quic_stats.rtt_ms;
    }
    
    {
        std::lock_guard<std::mutex> streams_lock(streams_mutex_);
        for (const auto& [id, ctx] : streams_) {
            if (ctx->state != StreamState::COMPLETED && 
                ctx->state != StreamState::CANCELLED &&
                ctx->state != StreamState::ERROR) {
                stats.active_streams++;
            }
        }
    }
    
    return stats;
}


