/*
 * HTTP/3 Manager - QUIC/HTTP3 Connection Manager
 * 
 * Manages a persistent QUIC connection with background event loop.
 * Supports multiple concurrent streams and stream cancellation.
 */

#pragma once

#include <string>
#include <memory>
#include <functional>
#include <mutex>
#include <vector>
#include <map>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>
#include <freertos/semphr.h>
#include <freertos/queue.h>
#include "esp_http3.h"

/**
 * Stream state for tracking active requests
 */
enum class StreamState {
    IDLE,           // Stream not in use
    OPENING,        // Stream being opened
    UPLOADING,      // Uploading data (for POST requests)
    WAITING,        // Waiting for response
    RECEIVING,      // Receiving response data
    COMPLETED,      // Stream completed successfully
    CANCELLED,      // Stream was cancelled
    ERROR           // Stream encountered an error
};

/**
 * Stream request configuration
 */
struct Http3Request {
    std::string method;
    std::string path;
    std::vector<std::pair<std::string, std::string>> headers;
    
    // For data upload (POST/PUT)
    const uint8_t* body = nullptr;
    size_t body_size = 0;
    
    // For chunked/streaming upload
    bool chunked_upload = false;
};

/**
 * Stream response data
 */
struct Http3Response {
    int status = 0;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;  // Accumulated body (if small) or empty if using stream callback
    bool complete = false;
    std::string error;
};

/**
 * Callbacks for stream events
 */
struct Http3StreamCallbacks {
    // Called when response headers are received
    std::function<void(int stream_id, int status, 
                       const std::vector<std::pair<std::string, std::string>>& headers)> on_headers;
    
    // Called for each chunk of response data (streaming)
    std::function<void(int stream_id, const uint8_t* data, size_t len, bool fin)> on_data;
    
    // Called when request completes (success or error)
    std::function<void(int stream_id, bool success, const std::string& error)> on_complete;
    
    // Called when write completes (for chunked uploads)
    std::function<void(int stream_id, size_t total_bytes)> on_write_complete;
};

/**
 * Connection configuration
 */
struct Http3ManagerConfig {
    std::string hostname;
    uint16_t port = 443;
    
    // Timeouts
    uint32_t connect_timeout_ms = 10000;
    uint32_t request_timeout_ms = 30000;
    uint32_t idle_timeout_ms = 60000;
    
    // Debug logging
    bool enable_debug = false;
};

/**
 * Http3Manager - Manages a persistent QUIC/HTTP3 connection
 * 
 * This class provides:
 * - Persistent QUIC connection with automatic reconnection
 * - Background event loop for processing QUIC events
 * - Multiple concurrent streams support
 * - Stream cancellation capability
 * 
 * Threading model:
 * - Background task handles event loop (receiving UDP, timer ticks)
 * - Public methods can be called from any task
 * - Thread-safe through mutex protection
 */
class Http3Manager {
public:
    /**
     * Get singleton instance
     */
    static Http3Manager& GetInstance();
    
    /**
     * Initialize the manager
     * @param config Connection configuration
     * @return true if initialization successful
     */
    bool Init(const Http3ManagerConfig& config);
    
    /**
     * Deinitialize and cleanup
     */
    void Deinit();
    
    /**
     * Set device and client identifiers for API requests
     */
    void SetIdentifiers(const std::string& device_id, const std::string& client_id);
    
    
    /**
     * Check if connected to server
     */
    bool IsConnected() const;
    
    /**
     * Ensure connection is established (blocking)
     * @param timeout_ms Maximum time to wait for connection
     * @return true if connected
     */
    bool EnsureConnected(uint32_t timeout_ms = 10000);
    
    /**
     * Disconnect from server
     */
    void Disconnect();
    
    // ==================== Request Methods ====================
    
    /**
     * Send a simple request and wait for response (synchronous)
     * Good for OTA registration and other simple requests
     * @param request Request configuration
     * @param response Output response data
     * @param timeout_ms Request timeout
     * @return true if request successful
     */
    bool SendRequest(const Http3Request& request, Http3Response& response, 
                     uint32_t timeout_ms = 30000);
    
    /**
     * Open a stream for chunked upload (asynchronous)
     * Returns stream_id which can be used for writing data
     * @param request Request configuration (body is ignored)
     * @param callbacks Stream event callbacks
     * @return stream_id (>= 0) on success, -1 on failure
     */
    int OpenStream(const Http3Request& request, const Http3StreamCallbacks& callbacks);
    
    /**
     * Queue data for writing to a stream
     * Data will be sent respecting flow control
     * @param stream_id Stream ID from OpenStream
     * @param data Data to send (must remain valid until sent or freed by deleter)
     * @param size Data size
     * @param deleter Optional callback to free the data after it's sent
     * @return true if queued successfully
     */
    bool QueueWrite(int stream_id, const uint8_t* data, size_t size, 
                    std::function<void()> deleter = nullptr);
    
    /**
     * Mark stream upload as finished (send FIN)
     * @param stream_id Stream ID
     * @return true if successful
     */
    bool FinishStream(int stream_id);
    
    /**
     * Cancel a stream
     * @param stream_id Stream ID to cancel
     * @return true if stream was cancelled
     */
    bool CancelStream(int stream_id);
    
    /**
     * Cleanup stream context and free resources
     * Should be called after stream is completed or cancelled
     * @param stream_id Stream ID to cleanup
     */
    void CleanupStream(int stream_id);
    
    /**
     * Check if a stream is active
     * @param stream_id Stream ID
     * @return true if stream exists and is active
     */
    bool IsStreamActive(int stream_id) const;
    
    /**
     * Get stream state
     * @param stream_id Stream ID
     * @return Current stream state
     */
    StreamState GetStreamState(int stream_id) const;
    
    /**
     * Get queued bytes for a stream
     * @param stream_id Stream ID
     * @return Number of bytes waiting to be sent
     */
    size_t GetQueuedBytes(int stream_id) const;
    
    // ==================== Connection Statistics ====================
    
    struct Stats {
        uint32_t packets_sent = 0;
        uint32_t packets_received = 0;
        uint32_t bytes_sent = 0;
        uint32_t bytes_received = 0;
        uint32_t rtt_ms = 0;
        uint32_t active_streams = 0;
    };
    
    Stats GetStats() const;
    
private:
    // Singleton
    Http3Manager();
    ~Http3Manager();
    Http3Manager(const Http3Manager&) = delete;
    Http3Manager& operator=(const Http3Manager&) = delete;
    
    // Stream tracking
    struct StreamContext {
        int stream_id = -1;
        StreamState state = StreamState::IDLE;
        Http3StreamCallbacks callbacks;
        Http3Response response;
        SemaphoreHandle_t complete_sem = nullptr;  // For synchronous requests
        bool sync_request = false;
    };
    
    // Configuration
    Http3ManagerConfig config_;
    std::string device_id_;
    std::string client_id_;
    
    // QUIC connection
    std::unique_ptr<esp_http3::QuicConnection> connection_;
    int udp_socket_ = -1;
    bool connected_ = false;
    bool initialized_ = false;
    volatile bool needs_cleanup_ = false;  // Marked true when connection drops unexpectedly
    
    // Stream management
    std::map<int, std::unique_ptr<StreamContext>> streams_;
    mutable std::mutex streams_mutex_;
    
    // Tasks
    TaskHandle_t event_loop_task_ = nullptr;
    TaskHandle_t udp_recv_task_ = nullptr;
    EventGroupHandle_t event_group_ = nullptr;
    volatile bool stop_tasks_ = false;
    
    // UDP data queue (from recv task to event loop)
    struct UdpPacket {
        uint8_t data[1500];
        size_t len;
    };
    QueueHandle_t udp_queue_ = nullptr;
    static constexpr size_t UDP_QUEUE_SIZE = 64;  // Buffer up to 64 packets
    
    // Event bits (unified event system)
    static constexpr uint32_t EVENT_UDP_DATA = (1 << 0);     // UDP data in queue
    static constexpr uint32_t EVENT_WAKE = (1 << 1);         // External wake-up (work pending)
    static constexpr uint32_t EVENT_STOP = (1 << 2);         // Stop tasks
    static constexpr uint32_t EVENT_CONNECTED = (1 << 3);    // Connection established
    static constexpr uint32_t EVENT_DISCONNECTED = (1 << 4); // Connection lost
    
    // Connection mutex
    mutable std::mutex connection_mutex_;
    
    // Private methods
    bool CreateSocket();
    void CloseSocket();
    bool StartTasks();
    void StopTasks();
    static void EventLoopTask(void* param);
    static void UdpRecvTask(void* param);
    void RunEventLoop();
    void RunUdpRecv();
    void WakeEventLoop();  // Signal event loop to process pending work
    
    // QUIC callbacks
    void OnConnected();
    void OnDisconnected(int error_code, const std::string& reason);
    void OnResponse(int stream_id, const esp_http3::H3Response& response);
    void OnStreamData(int stream_id, const uint8_t* data, size_t len, bool fin);
    void OnWriteComplete(int stream_id, size_t total_bytes);
    void OnWriteError(int stream_id, int error_code, const std::string& reason);
    
    // Stream helpers
    StreamContext* GetStreamContext(int stream_id);
};


