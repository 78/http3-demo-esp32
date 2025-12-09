/*
 * UART UHCI DMA Controller
 * 
 * 自定义 UHCI DMA 控制器，提供启动/停止 DMA 接收的功能，
 * 以支持低功耗模式下释放 PM 锁。
 * 
 * 支持两种接收模式：
 * 1. 单缓冲区模式（原有）：调用者提供单个缓冲区，接收完成后停止
 * 2. 缓冲区池模式（新增）：使用预分配的缓冲区池持续接收，通过回调通知
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
    // RX buffer descriptor for buffer pool mode
    // 缓冲区池模式的 RX 缓冲区描述符
    struct RxBuffer {
        uint8_t* data;          // Pointer to buffer data / 缓冲区数据指针
        size_t capacity;        // Buffer capacity / 缓冲区容量
        size_t size;            // Actual received data size / 实际接收数据大小
        uint32_t index;         // Buffer index in pool / 缓冲区在池中的索引
    };

    // RX event data passed to callback
    // 传递给回调的 RX 事件数据
    struct RxEventData {
        RxBuffer* buffer;       // Buffer containing received data / 包含接收数据的缓冲区
        size_t recv_size;       // Number of bytes received / 接收字节数
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

    // Statistics for RX ISR performance monitoring
    // RX ISR 性能监控统计
    struct RxIsrStatistics {
        uint32_t call_count;           // Total number of calls / 累积调用次数
        uint64_t total_time_us;        // Total time spent in ISR (microseconds) / 累积耗时（微秒）
        uint32_t min_time_us;          // Minimum single call time / 单次最小耗时
        uint32_t max_time_us;          // Maximum single call time / 单次最大耗时
    };

    // Buffer pool configuration
    // 缓冲区池配置
    struct BufferPoolConfig {
        size_t buffer_count;        // Number of buffers in pool / 池中缓冲区数量
        size_t buffer_size;         // Size of each buffer / 每个缓冲区大小
    };

    // Configuration structure
    // 配置结构
    struct Config {
        uart_port_t uart_port;          // UART port number / UART 端口号
        size_t tx_queue_depth;          // TX transaction queue depth / TX 事务队列深度
        size_t max_tx_size;             // Maximum TX size per transaction / 单次发送最大字节数
        size_t dma_burst_size;          // DMA burst size (0 to disable, or power of 2) / DMA 突发大小
        BufferPoolConfig rx_pool;       // RX buffer pool config / RX 缓冲区池配置
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

    // Start continuous DMA receive using buffer pool
    // Acquires PM lock to prevent light sleep
    // Buffers are delivered via RxCallback, caller must return them via ReturnBuffer
    // 使用缓冲区池启动持续 DMA 接收
    // 获取 PM 锁以阻止 light sleep
    // 缓冲区通过 RxCallback 传递，调用者必须通过 ReturnBuffer 归还
    esp_err_t StartReceive();

    // Stop DMA receive and release PM lock
    // 停止 DMA 接收并释放 PM 锁
    esp_err_t StopReceive();

    // Return a buffer back to the pool after processing
    // Must be called for each buffer received via RxCallback
    // 处理完成后将缓冲区归还到池中
    // 必须对每个通过 RxCallback 收到的缓冲区调用
    void ReturnBuffer(RxBuffer* buffer);

    // Check if RX is currently running
    // 检查 RX 是否正在运行
    bool IsReceiving() const { return rx_running_.load(); }

    // Get RX ISR statistics
    // 获取 RX ISR 统计信息
    RxIsrStatistics GetRxIsrStatistics() const;

    // Reset RX ISR statistics
    // 重置 RX ISR 统计信息
    void ResetRxIsrStatistics();

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

    // Initialize RX buffer pool
    esp_err_t InitRxBufferPool(const BufferPoolConfig& config);
    void DeinitRxBufferPool();

    // Prepare next DMA receive node
    void PrepareNextRxNode();

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

    // RX direction - buffer pool mode
    gdma_channel_handle_t rx_dma_chan_{nullptr};
    gdma_link_list_handle_t rx_dma_link_{nullptr};
    RxBuffer* rx_buffer_pool_{nullptr};     // Array of buffer descriptors
    size_t rx_pool_size_{0};                // Number of buffers in pool
    size_t rx_buffer_size_{0};              // Size of each buffer
    QueueHandle_t rx_free_queue_{nullptr};  // Queue of free buffer indices
    int32_t* rx_mounted_idx_{nullptr};      // Buffer index mounted at each DMA node (-1 = none)
    size_t rx_mounted_count_{0};            // Number of buffers currently mounted
    size_t rx_active_node_{0};              // Current active DMA node index
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

    // RX ISR statistics (updated atomically from ISR)
    // RX ISR 统计（从 ISR 中原子更新）
    std::atomic<uint32_t> rx_isr_call_count_{0};
    std::atomic<uint64_t> rx_isr_total_time_us_{0};
    std::atomic<uint32_t> rx_isr_min_time_us_{UINT32_MAX};
    std::atomic<uint32_t> rx_isr_max_time_us_{0};

    // Delete copy/move
    UartUhci(const UartUhci&) = delete;
    UartUhci& operator=(const UartUhci&) = delete;
};
