/*
 * UART UHCI DMA Controller
 * 
 * 自定义 UHCI DMA 控制器，提供启动/停止 DMA 接收的功能，
 * 以支持低功耗模式下释放 PM 锁。
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <cstddef>

#include "esp_err.h"
#include "driver/uart.h"
#include "esp_pm.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

// Forward declarations for ESP-IDF internal types
typedef struct gdma_channel_t *gdma_channel_handle_t;
typedef struct gdma_link_list_t *gdma_link_list_handle_t;
struct uhci_dev_t;

class UartUhci {
public:
    // RX event data passed to callback
    // 传递给回调的 RX 事件数据
    struct RxEventData {
        uint8_t* data;          // Pointer to received data / 接收数据指针
        size_t recv_size;       // Number of bytes received / 接收字节数
        bool is_complete;       // True if frame is complete (EOF) / 帧是否完整
    };

    // TX done event data passed to callback
    // 传递给回调的 TX 完成事件数据
    struct TxDoneEventData {
        void* buffer;           // Pointer to transmitted buffer / 发送缓冲区指针
        size_t sent_size;       // Number of bytes sent / 发送字节数
    };

    // Callback types (return true to yield, called from ISR context)
    // 回调类型（返回 true 表示需要调度，在 ISR 上下文中调用）
    // Note: These are function pointers, not std::function, for ISR safety
    using RxCallback = bool(*)(const RxEventData& data, void* user_data);
    using TxDoneCallback = bool(*)(const TxDoneEventData& data, void* user_data);

    // Configuration structure
    // 配置结构
    struct Config {
        uart_port_t uart_port;          // UART port number / UART 端口号
        size_t tx_queue_depth;          // TX transaction queue depth / TX 事务队列深度
        size_t max_tx_size;             // Maximum TX size per transaction / 单次发送最大字节数
        size_t max_rx_buffer_size;      // Maximum RX buffer size / 最大接收缓冲区大小
        size_t dma_burst_size;          // DMA burst size (0 to disable, or power of 2) / DMA 突发大小
        bool idle_eof;                  // End frame on UART idle / UART 空闲时结束帧
    };

    UartUhci();
    ~UartUhci();

    // Initialize the UHCI controller
    // 初始化 UHCI 控制器
    esp_err_t Init(const Config& config);

    // Deinitialize and release resources
    // 反初始化并释放资源
    void Deinit();

    // Register callbacks (must be called before StartReceive)
    // 注册回调（必须在 StartReceive 之前调用）
    void SetRxCallback(RxCallback callback, void* user_data);
    void SetTxDoneCallback(TxDoneCallback callback, void* user_data);

    // Start DMA receive with user-provided buffer
    // Acquires PM lock to prevent light sleep
    // 启动 DMA 接收，使用用户提供的缓冲区
    // 获取 PM 锁以阻止 light sleep
    esp_err_t StartReceive(uint8_t* buffer, size_t buffer_size);

    // Stop DMA receive and release PM lock
    // 停止 DMA 接收并释放 PM 锁
    esp_err_t StopReceive();

    // Check if RX is currently running
    // 检查 RX 是否正在运行
    bool IsReceiving() const { return rx_running_.load(); }

    // Transmit data via DMA (non-blocking)
    // Buffer must remain valid until TX done callback
    // 通过 DMA 发送数据（非阻塞）
    // 缓冲区必须在 TX 完成回调前保持有效
    esp_err_t Transmit(uint8_t* buffer, size_t size);

    // Wait for all pending TX transactions to complete
    // 等待所有待处理的 TX 事务完成
    esp_err_t WaitTxDone(int timeout_ms);

private:
    // FSM states for TX
    enum class TxFsm {
        Enable,     // Ready for new transaction
        RunWait,    // Waiting to start
        Run,        // Transaction in progress
    };

    // Transaction descriptor
    struct TransDesc {
        void* buffer;
        size_t buffer_size;
    };

    // Initialize GDMA channels
    esp_err_t InitGdma(const Config& config);
    void DeinitGdma();

    // Do actual transmit
    void DoTransmit(TransDesc* trans);

public:
    // Handle GDMA callbacks (called by C-style wrapper functions in .cc file)
    // Note: IRAM_ATTR only in declaration, not definition
    bool IRAM_ATTR HandleGdmaTxDone();
    bool IRAM_ATTR HandleGdmaRxDone(bool is_eof);

private:

    // Member variables
    int uhci_num_{-1};
    uhci_dev_t* uhci_dev_{nullptr};

    // TX direction
    gdma_channel_handle_t tx_dma_chan_{nullptr};
    gdma_link_list_handle_t tx_dma_link_{nullptr};
    TransDesc* tx_trans_pool_{nullptr};
    TransDesc* tx_cur_trans_{nullptr};
    QueueHandle_t tx_ready_queue_{nullptr};
    QueueHandle_t tx_progress_queue_{nullptr};
    QueueHandle_t tx_complete_queue_{nullptr};
    std::atomic<TxFsm> tx_fsm_{TxFsm::Enable};
    std::atomic<int> tx_inflight_{0};
    size_t tx_int_mem_align_{0};
    size_t tx_ext_mem_align_{0};

    // RX direction
    gdma_channel_handle_t rx_dma_chan_{nullptr};
    gdma_link_list_handle_t rx_dma_link_{nullptr};
    size_t* rx_buffer_sizes_{nullptr};      // Size per DMA node
    uint8_t** rx_buffer_ptrs_{nullptr};     // Buffer pointer per node
    size_t rx_num_nodes_{0};
    size_t rx_node_index_{0};
    size_t rx_cache_line_{0};
    size_t rx_int_mem_align_{0};
    size_t rx_ext_mem_align_{0};
    std::atomic<bool> rx_running_{false};

    // PM lock
    esp_pm_lock_handle_t pm_lock_{nullptr};

    // Cache line sizes
    size_t int_mem_cache_line_{0};
    size_t ext_mem_cache_line_{0};

    // Callbacks
    RxCallback rx_callback_{nullptr};
    TxDoneCallback tx_done_callback_{nullptr};
    void* rx_callback_user_data_{nullptr};
    void* tx_callback_user_data_{nullptr};

    // Delete copy/move
    UartUhci(const UartUhci&) = delete;
    UartUhci& operator=(const UartUhci&) = delete;
};

