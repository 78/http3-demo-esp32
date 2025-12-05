/*
 * UART UHCI DMA Controller Implementation
 */

#include "uart_uhci.h"

#include <cstring>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_attr.h"
#include "esp_cache.h"
#include "esp_memory_utils.h"
#include "esp_private/gdma.h"
#include "esp_private/gdma_link.h"
#include "esp_private/periph_ctrl.h"
#include "esp_private/esp_cache_private.h"
#include "hal/uhci_hal.h"
#include "hal/uhci_ll.h"
#include "hal/dma_types.h"
#include "soc/uhci_periph.h"
#include "soc/soc_caps.h"

static const char* kTag = "UartUhci";

// Alignment helper macros
#define ALIGN_UP(num, align) (((num) + ((align) - 1)) & ~((align) - 1))
#define MAX_OF(a, b) (((a) > (b)) ? (a) : (b))

// C-style GDMA callback wrappers (must match gdma_event_callback_t signature)
static bool IRAM_ATTR gdma_tx_callback_wrapper(gdma_channel_handle_t dma_chan, gdma_event_data_t* event_data, void* user_data) {
    (void)dma_chan;
    (void)event_data;
    auto* self = static_cast<UartUhci*>(user_data);
    return self->HandleGdmaTxDone();
}

static bool IRAM_ATTR gdma_rx_callback_wrapper(gdma_channel_handle_t dma_chan, gdma_event_data_t* event_data, void* user_data) {
    (void)dma_chan;
    auto* self = static_cast<UartUhci*>(user_data);
    return self->HandleGdmaRxDone(event_data->flags.normal_eof);
}

// Platform singleton for UHCI controller management
static struct {
    _lock_t mutex;
    void* controllers[SOC_UHCI_NUM];
} s_platform = {};

UartUhci::UartUhci() = default;

UartUhci::~UartUhci() {
    Deinit();
}

esp_err_t UartUhci::Init(const Config& config) {
    esp_err_t ret = ESP_OK;

    // Find a free UHCI controller
    bool found = false;
    _lock_acquire(&s_platform.mutex);
    for (int i = 0; i < SOC_UHCI_NUM; i++) {
        if (s_platform.controllers[i] == nullptr) {
            s_platform.controllers[i] = this;
            uhci_num_ = i;
            found = true;
            break;
        }
    }
    _lock_release(&s_platform.mutex);
    ESP_RETURN_ON_FALSE(found, ESP_ERR_NOT_FOUND, kTag, "no free UHCI controller");

    // Enable UHCI bus clock
    PERIPH_RCC_ATOMIC() {
        uhci_ll_enable_bus_clock(uhci_num_, true);
        uhci_ll_reset_register(uhci_num_);
    }

    // Get UHCI hardware device
    uhci_dev_ = UHCI_LL_GET_HW(uhci_num_);
    ESP_GOTO_ON_FALSE(uhci_dev_, ESP_ERR_INVALID_STATE, err, kTag, "failed to get UHCI device");

    // Initialize UHCI hardware
    uhci_ll_init(uhci_dev_);
    uhci_ll_attach_uart_port(uhci_dev_, config.uart_port);

    // Disable separator character (otherwise UHCI may lose data)
    {
        uhci_seper_chr_t seper_chr = {};
        seper_chr.sub_chr_en = 0;
        uhci_ll_set_seper_chr(uhci_dev_, &seper_chr);
    }

    // Set EOF mode
    if (config.idle_eof) {
        uhci_ll_rx_set_eof_mode(uhci_dev_, UHCI_RX_IDLE_EOF);
    }

    // Create PM lock
#if CONFIG_PM_ENABLE
    char pm_lock_name[16];
    snprintf(pm_lock_name, sizeof(pm_lock_name), "uhci_%d", uhci_num_);
    ESP_GOTO_ON_ERROR(esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, pm_lock_name, &pm_lock_),
                      err, kTag, "failed to create PM lock");
#endif

    // Get cache line sizes
    esp_cache_get_alignment(MALLOC_CAP_SPIRAM, &ext_mem_cache_line_);
    esp_cache_get_alignment(MALLOC_CAP_INTERNAL, &int_mem_cache_line_);

    // Initialize GDMA
    ESP_GOTO_ON_ERROR(InitGdma(config), err, kTag, "failed to initialize GDMA");

    // Create TX transaction queues
    tx_ready_queue_ = xQueueCreateWithCaps(config.tx_queue_depth, sizeof(TransDesc*), MALLOC_CAP_INTERNAL);
    tx_progress_queue_ = xQueueCreateWithCaps(config.tx_queue_depth, sizeof(TransDesc*), MALLOC_CAP_INTERNAL);
    tx_complete_queue_ = xQueueCreateWithCaps(config.tx_queue_depth, sizeof(TransDesc*), MALLOC_CAP_INTERNAL);
    ESP_GOTO_ON_FALSE(tx_ready_queue_ && tx_progress_queue_ && tx_complete_queue_,
                      ESP_ERR_NO_MEM, err, kTag, "failed to create TX queues");

    // Create TX transaction pool
    tx_trans_pool_ = static_cast<TransDesc*>(heap_caps_calloc(config.tx_queue_depth, sizeof(TransDesc), MALLOC_CAP_INTERNAL));
    ESP_GOTO_ON_FALSE(tx_trans_pool_, ESP_ERR_NO_MEM, err, kTag, "failed to allocate TX pool");

    // Fill ready queue with transaction descriptors
    for (size_t i = 0; i < config.tx_queue_depth; i++) {
        TransDesc* p = &tx_trans_pool_[i];
        xQueueSend(tx_ready_queue_, &p, 0);
    }

    ESP_LOGI(kTag, "UHCI %d initialized (UART %d)", uhci_num_, config.uart_port);
    return ESP_OK;

err:
    Deinit();
    return ret;
}

void UartUhci::Deinit() {
    // Stop any ongoing RX
    if (rx_running_.load()) {
        StopReceive();
    }

    // Wait for TX to complete
    if (tx_dma_chan_) {
        WaitTxDone(100);
    }

    // Deinitialize GDMA
    DeinitGdma();

    // Delete queues
    if (tx_ready_queue_) {
        vQueueDeleteWithCaps(tx_ready_queue_);
        tx_ready_queue_ = nullptr;
    }
    if (tx_progress_queue_) {
        vQueueDeleteWithCaps(tx_progress_queue_);
        tx_progress_queue_ = nullptr;
    }
    if (tx_complete_queue_) {
        vQueueDeleteWithCaps(tx_complete_queue_);
        tx_complete_queue_ = nullptr;
    }

    // Free TX pool
    if (tx_trans_pool_) {
        heap_caps_free(tx_trans_pool_);
        tx_trans_pool_ = nullptr;
    }

    // Free RX tracking arrays
    if (rx_buffer_sizes_) {
        free(rx_buffer_sizes_);
        rx_buffer_sizes_ = nullptr;
    }
    if (rx_buffer_ptrs_) {
        free(rx_buffer_ptrs_);
        rx_buffer_ptrs_ = nullptr;
    }

    // Delete PM lock
    if (pm_lock_) {
        esp_pm_lock_delete(pm_lock_);
        pm_lock_ = nullptr;
    }

    // Disable UHCI clock
    if (uhci_num_ >= 0) {
        PERIPH_RCC_ATOMIC() {
            uhci_ll_enable_bus_clock(uhci_num_, false);
        }

        // Release controller slot
        _lock_acquire(&s_platform.mutex);
        s_platform.controllers[uhci_num_] = nullptr;
        _lock_release(&s_platform.mutex);
        uhci_num_ = -1;
    }

    uhci_dev_ = nullptr;
}

esp_err_t UartUhci::InitGdma(const Config& config) {
    // Allocate TX DMA channel
    gdma_channel_alloc_config_t tx_alloc = {};
    tx_alloc.direction = GDMA_CHANNEL_DIRECTION_TX;
    ESP_RETURN_ON_ERROR(gdma_new_ahb_channel(&tx_alloc, &tx_dma_chan_), kTag, "TX DMA alloc failed");
    gdma_connect(tx_dma_chan_, GDMA_MAKE_TRIGGER(GDMA_TRIG_PERIPH_UHCI, 0));

    gdma_transfer_config_t transfer_cfg = {};
    transfer_cfg.max_data_burst_size = config.dma_burst_size;
    transfer_cfg.access_ext_mem = true;
    ESP_RETURN_ON_ERROR(gdma_config_transfer(tx_dma_chan_, &transfer_cfg), kTag, "TX DMA config failed");

    gdma_strategy_config_t strategy = {};
    strategy.auto_update_desc = true;
    strategy.owner_check = true;
    strategy.eof_till_data_popped = true;
    gdma_apply_strategy(tx_dma_chan_, &strategy);

    // Get TX alignment constraints
    gdma_get_alignment_constraints(tx_dma_chan_, &tx_int_mem_align_, &tx_ext_mem_align_);
    // Calculate number of DMA nodes needed
    size_t tx_num_nodes = (config.max_tx_size + DMA_DESCRIPTOR_BUFFER_MAX_SIZE - 1) / DMA_DESCRIPTOR_BUFFER_MAX_SIZE;
    if (tx_num_nodes == 0) tx_num_nodes = 1;

    // Create TX DMA link list
    gdma_link_list_config_t tx_link_cfg = {};
    tx_link_cfg.item_alignment = 4;
    tx_link_cfg.num_items = tx_num_nodes;
    ESP_RETURN_ON_ERROR(gdma_new_link_list(&tx_link_cfg, &tx_dma_link_), kTag, "TX link list failed");

    // Allocate RX DMA channel
    gdma_channel_alloc_config_t rx_alloc = {};
    rx_alloc.direction = GDMA_CHANNEL_DIRECTION_RX;
    ESP_RETURN_ON_ERROR(gdma_new_ahb_channel(&rx_alloc, &rx_dma_chan_), kTag, "RX DMA alloc failed");
    gdma_connect(rx_dma_chan_, GDMA_MAKE_TRIGGER(GDMA_TRIG_PERIPH_UHCI, 0));
    ESP_RETURN_ON_ERROR(gdma_config_transfer(rx_dma_chan_, &transfer_cfg), kTag, "RX DMA config failed");

    // Get RX alignment constraints
    gdma_get_alignment_constraints(rx_dma_chan_, &rx_int_mem_align_, &rx_ext_mem_align_);
    rx_num_nodes_ = (config.max_rx_buffer_size + DMA_DESCRIPTOR_BUFFER_MAX_SIZE - 1) / DMA_DESCRIPTOR_BUFFER_MAX_SIZE;
    if (rx_num_nodes_ == 0) rx_num_nodes_ = 1;

    // Create RX DMA link list
    gdma_link_list_config_t rx_link_cfg = {};
    rx_link_cfg.item_alignment = 4;
    rx_link_cfg.num_items = rx_num_nodes_;
    ESP_RETURN_ON_ERROR(gdma_new_link_list(&rx_link_cfg, &rx_dma_link_), kTag, "RX link list failed");

    // Allocate RX tracking arrays
    rx_buffer_sizes_ = static_cast<size_t*>(heap_caps_calloc(rx_num_nodes_, sizeof(size_t), MALLOC_CAP_INTERNAL));
    rx_buffer_ptrs_ = static_cast<uint8_t**>(heap_caps_calloc(rx_num_nodes_, sizeof(uint8_t*), MALLOC_CAP_INTERNAL));
    ESP_RETURN_ON_FALSE(rx_buffer_sizes_ && rx_buffer_ptrs_, ESP_ERR_NO_MEM, kTag, "RX arrays alloc failed");

    // Register TX callback
    gdma_tx_event_callbacks_t tx_cbs = {};
    tx_cbs.on_trans_eof = gdma_tx_callback_wrapper;
    ESP_RETURN_ON_ERROR(gdma_register_tx_event_callbacks(tx_dma_chan_, &tx_cbs, this), kTag, "TX callback failed");

    // Register RX callback
    gdma_rx_event_callbacks_t rx_cbs = {};
    rx_cbs.on_recv_done = gdma_rx_callback_wrapper;
    ESP_RETURN_ON_ERROR(gdma_register_rx_event_callbacks(rx_dma_chan_, &rx_cbs, this), kTag, "RX callback failed");

    return ESP_OK;
}

void UartUhci::DeinitGdma() {
    if (tx_dma_chan_) {
        gdma_disconnect(tx_dma_chan_);
        gdma_del_channel(tx_dma_chan_);
        tx_dma_chan_ = nullptr;
    }
    if (rx_dma_chan_) {
        gdma_disconnect(rx_dma_chan_);
        gdma_del_channel(rx_dma_chan_);
        rx_dma_chan_ = nullptr;
    }
    if (tx_dma_link_) {
        gdma_del_link_list(tx_dma_link_);
        tx_dma_link_ = nullptr;
    }
    if (rx_dma_link_) {
        gdma_del_link_list(rx_dma_link_);
        rx_dma_link_ = nullptr;
    }
}

void UartUhci::SetRxCallback(RxCallback callback, void* user_data) {
    rx_callback_ = callback;
    rx_callback_user_data_ = user_data;
}

void UartUhci::SetTxDoneCallback(TxDoneCallback callback, void* user_data) {
    tx_done_callback_ = callback;
    tx_callback_user_data_ = user_data;
}

esp_err_t UartUhci::StartReceive(uint8_t* buffer, size_t buffer_size) {
    ESP_RETURN_ON_FALSE(buffer && buffer_size > 0, ESP_ERR_INVALID_ARG, kTag, "invalid buffer");
    ESP_RETURN_ON_FALSE(!rx_running_.load(), ESP_ERR_INVALID_STATE, kTag, "RX already running");

    // Calculate cache line and alignment
    size_t cache_line = esp_ptr_external_ram(buffer) ? ext_mem_cache_line_ : int_mem_cache_line_;
    size_t max_align = MAX_OF(MAX_OF(rx_int_mem_align_, rx_ext_mem_align_), cache_line);
    rx_cache_line_ = cache_line;

    // Align buffer if needed
    if (max_align > 0 && (((uintptr_t)buffer) & (max_align - 1)) != 0) {
        uintptr_t aligned = ((uintptr_t)buffer + max_align - 1) & ~(max_align - 1);
        size_t offset = aligned - (uintptr_t)buffer;
        ESP_RETURN_ON_FALSE(buffer_size > offset, ESP_ERR_INVALID_ARG, kTag, "buffer too small for alignment");
        buffer = (uint8_t*)aligned;
        buffer_size -= offset;
    }

    // Calculate per-node buffer sizes
    size_t usable = (max_align == 0) ? buffer_size : (buffer_size / max_align) * max_align;
    size_t base_size = (max_align == 0) ? usable / rx_num_nodes_ : (usable / rx_num_nodes_ / max_align) * max_align;
    size_t remaining = usable - (base_size * rx_num_nodes_);

    gdma_buffer_mount_config_t mount_configs[rx_num_nodes_];
    memset(mount_configs, 0, sizeof(mount_configs));

    for (size_t i = 0; i < rx_num_nodes_; i++) {
        rx_buffer_sizes_[i] = base_size;
        rx_buffer_ptrs_[i] = buffer;

        // Distribute remaining to first nodes
        if (remaining >= max_align) {
            rx_buffer_sizes_[i] += max_align;
            remaining -= max_align;
        }

        size_t buf_align = esp_ptr_internal(buffer) ? rx_int_mem_align_ : rx_ext_mem_align_;
        mount_configs[i] = {};
        mount_configs[i].buffer = buffer;
        mount_configs[i].buffer_alignment = buf_align;
        mount_configs[i].length = rx_buffer_sizes_[i];
        mount_configs[i].flags.mark_final = false;

        ESP_RETURN_ON_FALSE(rx_buffer_sizes_[i] > 0, ESP_ERR_INVALID_STATE, kTag, "node size is 0");
        buffer += rx_buffer_sizes_[i];
    }

    // Acquire PM lock
    if (pm_lock_) {
        esp_pm_lock_acquire(pm_lock_);
    }

    rx_node_index_ = 0;
    rx_running_.store(true);

    // Mount buffers and start DMA
    gdma_link_mount_buffers(rx_dma_link_, 0, mount_configs, rx_num_nodes_, nullptr);
    gdma_reset(rx_dma_chan_);
    gdma_start(rx_dma_chan_, gdma_link_get_head_addr(rx_dma_link_));

    ESP_LOGD(kTag, "RX started, %d nodes", rx_num_nodes_);
    return ESP_OK;
}

esp_err_t UartUhci::StopReceive() {
    if (!rx_running_.load()) {
        return ESP_OK;  // Already stopped
    }

    // Stop and reset DMA
    gdma_stop(rx_dma_chan_);
    gdma_reset(rx_dma_chan_);

    rx_running_.store(false);

    // Release PM lock
    if (pm_lock_) {
        esp_pm_lock_release(pm_lock_);
    }

    ESP_LOGD(kTag, "RX stopped");
    return ESP_OK;
}

esp_err_t UartUhci::Transmit(uint8_t* buffer, size_t size) {
    ESP_RETURN_ON_FALSE(buffer && size > 0, ESP_ERR_INVALID_ARG, kTag, "invalid arguments");

    // Check alignment
    size_t align = esp_ptr_external_ram(buffer) ? tx_ext_mem_align_ : tx_int_mem_align_;
    size_t cache_line = esp_ptr_external_ram(buffer) ? ext_mem_cache_line_ : int_mem_cache_line_;
    ESP_RETURN_ON_FALSE((((uintptr_t)buffer) & (align - 1)) == 0 && ((size & (align - 1)) == 0),
                        ESP_ERR_INVALID_ARG, kTag, "buffer not aligned to %d", align);

    // Sync cache if needed
    if (cache_line > 0) {
        ESP_RETURN_ON_ERROR(esp_cache_msync(buffer, size, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED),
                            kTag, "cache sync failed");
    }

    // Get a free transaction descriptor
    TransDesc* t = nullptr;
    if (xQueueReceive(tx_ready_queue_, &t, 0) != pdTRUE) {
        if (xQueueReceive(tx_complete_queue_, &t, 0) == pdTRUE) {
            tx_inflight_.fetch_sub(1);
        }
    }
    ESP_RETURN_ON_FALSE(t, ESP_ERR_INVALID_STATE, kTag, "no free TX descriptor");

    t->buffer = buffer;
    t->buffer_size = size;

    // Queue the transaction
    ESP_RETURN_ON_FALSE(xQueueSend(tx_progress_queue_, &t, 0) == pdTRUE, ESP_ERR_NO_MEM, kTag, "TX queue full");
    tx_inflight_.fetch_add(1);

    // Try to start transmission
    TxFsm expected = TxFsm::Enable;
    if (tx_fsm_.compare_exchange_strong(expected, TxFsm::RunWait)) {
        if (xQueueReceive(tx_progress_queue_, &t, 0) == pdTRUE) {
            tx_fsm_.store(TxFsm::Run);
            DoTransmit(t);
        } else {
            tx_fsm_.store(TxFsm::Enable);
        }
    }

    return ESP_OK;
}

void UartUhci::DoTransmit(TransDesc* trans) {
    tx_cur_trans_ = trans;

    size_t buf_align = esp_ptr_internal(trans->buffer) ? tx_int_mem_align_ : tx_ext_mem_align_;
    gdma_buffer_mount_config_t mount = {};
    mount.buffer = trans->buffer;
    mount.buffer_alignment = buf_align;
    mount.length = trans->buffer_size;
    mount.flags.mark_eof = true;
    mount.flags.mark_final = true;

    // Acquire PM lock for TX
    if (pm_lock_) {
        esp_pm_lock_acquire(pm_lock_);
    }

    gdma_link_mount_buffers(tx_dma_link_, 0, &mount, 1, nullptr);
    gdma_start(tx_dma_chan_, gdma_link_get_head_addr(tx_dma_link_));
}

esp_err_t UartUhci::WaitTxDone(int timeout_ms) {
    TickType_t wait = timeout_ms < 0 ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    TransDesc* t = nullptr;

    int cnt = tx_inflight_.load();
    for (int i = 0; i < cnt; i++) {
        ESP_RETURN_ON_FALSE(xQueueReceive(tx_complete_queue_, &t, wait) == pdTRUE, ESP_ERR_TIMEOUT, kTag, "timeout");
        ESP_RETURN_ON_FALSE(xQueueSend(tx_ready_queue_, &t, 0) == pdTRUE, ESP_ERR_INVALID_STATE, kTag, "queue full");
        tx_inflight_.fetch_sub(1);
    }

    return ESP_OK;
}

bool UartUhci::HandleGdmaTxDone() {
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    bool need_yield = false;

    // Transaction complete
    TxFsm expected = TxFsm::Run;
    if (tx_fsm_.compare_exchange_strong(expected, TxFsm::Enable)) {
        TransDesc* trans = tx_cur_trans_;
        xQueueSendFromISR(tx_complete_queue_, &trans, &xHigherPriorityTaskWoken);
        if (xHigherPriorityTaskWoken) need_yield = true;
    }

    // Release PM lock
    if (pm_lock_) {
        esp_pm_lock_release(pm_lock_);
    }

    // User callback
    if (tx_done_callback_) {
        TxDoneEventData evt = {
            .buffer = tx_cur_trans_->buffer,
            .sent_size = tx_cur_trans_->buffer_size,
        };
        if (tx_done_callback_(evt, tx_callback_user_data_)) {
            need_yield = true;
        }
    }

    // Check for more pending transactions
    expected = TxFsm::Enable;
    if (tx_fsm_.compare_exchange_strong(expected, TxFsm::RunWait)) {
        TransDesc* trans = nullptr;
        if (xQueueReceiveFromISR(tx_progress_queue_, &trans, &xHigherPriorityTaskWoken) == pdTRUE) {
            tx_fsm_.store(TxFsm::Run);
            DoTransmit(trans);
            if (xHigherPriorityTaskWoken) need_yield = true;
        } else {
            tx_fsm_.store(TxFsm::Enable);
        }
    }

    return need_yield;
}

bool UartUhci::HandleGdmaRxDone(bool is_eof) {
    bool need_yield = false;

    if (!is_eof) {
        // Partial data received (DMA node full)
        size_t rx_size = rx_buffer_sizes_[rx_node_index_];
        uint8_t* buf = rx_buffer_ptrs_[rx_node_index_];

        // Sync cache if needed
        bool need_cache_sync = esp_ptr_internal(buf) ? (int_mem_cache_line_ > 0) : (ext_mem_cache_line_ > 0);
        if (need_cache_sync) {
            esp_cache_msync(buf, rx_size, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
        }

        // User callback
        if (rx_callback_) {
            RxEventData data = {
                .data = buf,
                .recv_size = rx_size,
                .is_complete = false,
            };
            if (rx_callback_(data, rx_callback_user_data_)) {
                need_yield = true;
            }
        }

        // Move to next node
        rx_node_index_++;
        if (rx_node_index_ >= rx_num_nodes_) {
            rx_node_index_ = 0;
        }
    } else {
        // EOF received (frame complete)
        size_t rx_size = gdma_link_count_buffer_size_till_eof(rx_dma_link_, rx_node_index_);
        uint8_t* buf = rx_buffer_ptrs_[rx_node_index_];

        // Sync cache if needed
        bool need_cache_sync = esp_ptr_internal(buf) ? (int_mem_cache_line_ > 0) : (ext_mem_cache_line_ > 0);
        size_t sync_size = ALIGN_UP(rx_size, rx_cache_line_);
        if (need_cache_sync && sync_size > 0) {
            esp_cache_msync(buf, sync_size, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
        }

        // Release PM lock
        if (pm_lock_) {
            esp_pm_lock_release(pm_lock_);
        }

        // User callback
        if (rx_callback_) {
            RxEventData data = {
                .data = buf,
                .recv_size = rx_size,
                .is_complete = true,
            };
            if (rx_callback_(data, rx_callback_user_data_)) {
                need_yield = true;
            }
        }

        // Stop DMA and reset state
        gdma_stop(rx_dma_chan_);
        gdma_reset(rx_dma_chan_);
        rx_running_.store(false);
    }

    return need_yield;
}

