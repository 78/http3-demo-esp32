// Copyright 2025 Terrence
// SPDX-License-Identifier: Apache-2.0

#include "uart_eth_modem.h"

#include <cstdio>
#include <cstring>
#include <algorithm>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_rom_sys.h"

// Static member definitions
constexpr uint8_t UartEthModem::kHandshakeRequest[];
constexpr uint8_t UartEthModem::kHandshakeAck[];

UartEthModem::UartEthModem(const Config& config) : config_(config) {
    // Generate MAC address
    esp_read_mac(mac_addr_, ESP_MAC_ETH);
    mac_addr_[5] ^= 0x01;  // Make unique

    // Create event group
    event_group_ = xEventGroupCreate();
    if (!event_group_) {
        ESP_LOGE(kTag, "Failed to create event group");
        abort();  // Constructor cannot fail gracefully
    }

    // Create PM lock handle for CPU frequency control
    esp_err_t ret = esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "modem", &pm_lock_handle_);
    if (ret != ESP_OK) {
        ESP_LOGE(kTag, "Failed to create PM lock: %s", esp_err_to_name(ret));
        abort();  // Constructor cannot fail gracefully
    }

    // Initialize driver structure
    driver_.name = "uart_eth";
    driver_.init = [](iot_eth_driver_t* driver) -> esp_err_t {
        // Already initialized
        return ESP_OK;
    };
    driver_.deinit = [](iot_eth_driver_t* driver) -> esp_err_t {
        return ESP_OK;
    };
    driver_.start = [](iot_eth_driver_t* driver) -> esp_err_t {
        return ESP_OK;
    };
    driver_.stop = [](iot_eth_driver_t* driver) -> esp_err_t {
        return ESP_OK;
    };
    driver_.transmit = [](iot_eth_driver_t* driver, uint8_t* buf, size_t len) -> esp_err_t {
        // Get UartEthModem instance using container_of pattern
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
        auto* self = reinterpret_cast<UartEthModem*>(reinterpret_cast<char*>(driver) - offsetof(UartEthModem, driver_));
#pragma GCC diagnostic pop
        if (!self->connected_.load()) {
            return ESP_ERR_INVALID_STATE;
        }
        return self->SendFrame(buf, len, FrameType::kEthernet);
    };
    driver_.get_addr = [](iot_eth_driver_t* driver, uint8_t* mac) -> esp_err_t {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
        auto* self = reinterpret_cast<UartEthModem*>(reinterpret_cast<char*>(driver) - offsetof(UartEthModem, driver_));
#pragma GCC diagnostic pop
        memcpy(mac, self->mac_addr_, 6);
        return ESP_OK;
    };
    driver_.set_mediator = [](iot_eth_driver_t* driver, iot_eth_mediator_t* mediator) -> esp_err_t {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
        auto* self = reinterpret_cast<UartEthModem*>(reinterpret_cast<char*>(driver) - offsetof(UartEthModem, driver_));
#pragma GCC diagnostic pop
        self->mediator_ = mediator;
        return ESP_OK;
    };
}

UartEthModem::~UartEthModem() {
    Stop();

    // Cleanup resources created in constructor
    if (pm_lock_handle_) {
        esp_pm_lock_delete(pm_lock_handle_);
        pm_lock_handle_ = nullptr;
    }
    if (event_group_) {
        vEventGroupDelete(event_group_);
        event_group_ = nullptr;
    }
}

esp_err_t UartEthModem::Start() {
    ESP_LOGI(kTag, "Starting UartEthModem...");

    if (initialized_.load()) {
        ESP_LOGW(kTag, "Already started");
        return ESP_ERR_INVALID_STATE;
    }

    stop_flag_ = false;
    connected_ = false;
    initializing_ = true;

    // Create event queue FIRST (before GPIO init, since ISR uses it)
    // 首先创建事件队列（在 GPIO 初始化之前，因为 ISR 会使用它）
    event_queue_ = xQueueCreate(32, sizeof(Event));
    if (!event_queue_) {
        ESP_LOGE(kTag, "Failed to create event queue");
        return ESP_ERR_NO_MEM;
    }

    // Initialize UART
    esp_err_t ret = InitUart();
    if (ret != ESP_OK) {
        vQueueDelete(event_queue_);
        event_queue_ = nullptr;
        return ret;
    }

    // Initialize GPIO
    ret = InitGpio();
    if (ret != ESP_OK) {
        DeinitUart();
        vQueueDelete(event_queue_);
        event_queue_ = nullptr;
        return ret;
    }

    // Create semaphore for AT command response
    at_command_response_semaphore_ = xSemaphoreCreateBinary();
    if (!at_command_response_semaphore_) {
        ESP_LOGE(kTag, "Failed to create AT semaphore");
        vQueueDelete(event_queue_);
        DeinitGpio();
        DeinitUart();
        return ESP_ERR_NO_MEM;
    }

    // Allocate DMA receive buffer
    // 分配 DMA 接收缓冲区
    rx_buffer_ = static_cast<uint8_t*>(heap_caps_malloc(config_.rx_buffer_size, MALLOC_CAP_DMA));
    if (!rx_buffer_) {
        ESP_LOGE(kTag, "Failed to allocate DMA buffer");
        vSemaphoreDelete(at_command_response_semaphore_);
        vQueueDelete(event_queue_);
        DeinitGpio();
        DeinitUart();
        return ESP_ERR_NO_MEM;
    }

    // Initialize UART UHCI DMA controller
    // 初始化 UART UHCI DMA 控制器
    UartUhci::Config uhci_cfg = {
        .uart_port = config_.uart_num,
        .tx_queue_depth = 10,
        .max_tx_size = config_.rx_buffer_size,
        .max_rx_buffer_size = config_.rx_buffer_size,
        .dma_burst_size = 32,
        .idle_eof = true,  // Trigger EOF when UART line becomes idle (frame end)
    };

    ret = uart_uhci_.Init(uhci_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(kTag, "Failed to init UHCI: %s", esp_err_to_name(ret));
        free(rx_buffer_);
        vSemaphoreDelete(at_command_response_semaphore_);
        vQueueDelete(event_queue_);
        DeinitGpio();
        DeinitUart();
        return ret;
    }

    // Register UHCI callbacks
    // 注册 UHCI 回调函数
    uart_uhci_.SetRxCallback(UhciRxCallbackStatic, this);
    uart_uhci_.SetTxDoneCallback(UhciTxCallbackStatic, this);

    // Note: Don't start DMA receive here, will be started when entering Active state
    // 注意：不在这里启动 DMA 接收，将在进入 Active 状态时启动（避免持有 uhci 的 NO_LIGHT_SLEEP 锁）

    // Create main task (handles all events, no blocking operations)
    // 创建主任务（处理所有事件，无阻塞操作）
    xTaskCreate([](void* arg) {
        static_cast<UartEthModem*>(arg)->MainTaskRun();
        vTaskDelete(nullptr);
    }, "uart_eth_main", 4096, this, configMAX_PRIORITIES - 1, &main_task_);
    
    // Create init task
    xTaskCreate([](void* arg) {
        static_cast<UartEthModem*>(arg)->InitTaskRun();
        vTaskDelete(nullptr);
    }, "uart_eth_init", 4096, this, 4, &init_task_);

    if (!main_task_ || !init_task_) {
        ESP_LOGE(kTag, "Failed to create tasks");
        stop_flag_ = true;
        vTaskDelay(pdMS_TO_TICKS(100));
        uart_uhci_.Deinit();
        free(rx_buffer_);
        vSemaphoreDelete(at_command_response_semaphore_);
        vQueueDelete(event_queue_);
        DeinitGpio();
        DeinitUart();
        return ESP_ERR_NO_MEM;
    }

    // Signal start
    xEventGroupSetBits(event_group_, kEventStart);

    // Wait for initialization to complete
    auto bits = xEventGroupWaitBits(event_group_, kEventInitDone | kEventStop, pdFALSE, pdFALSE, pdMS_TO_TICKS(180000));
    if (bits & kEventStop) {
        ESP_LOGE(kTag, "Initialization failed");
        // Cleanup: wait for tasks to exit and cleanup resources
        stop_flag_ = true;
        initializing_ = false;
        xEventGroupSetBits(event_group_, kEventStop);
        xEventGroupWaitBits(event_group_, kEventAllTasksDone, pdTRUE, pdTRUE, pdMS_TO_TICKS(3000));
        
        // Cleanup resources (iot_eth not initialized yet, so skip it)
        CleanupResources(false);
        return ESP_FAIL;
    }

    initializing_ = false;
    ESP_LOGI(kTag, "UartEthModem started successfully");
    return ESP_OK;
}

esp_err_t UartEthModem::Stop() {
    if (!initialized_.load() && !initializing_.load()) {
        return ESP_OK;
    }

    ESP_LOGI(kTag, "Stopping UartEthModem...");

    stop_flag_ = true;
    initializing_ = false;
    if (event_group_) {
        xEventGroupSetBits(event_group_, kEventStop);
    }

    // Wait for tasks to finish
    if (event_group_) {
        xEventGroupWaitBits(event_group_, kEventAllTasksDone, pdTRUE, pdTRUE, pdMS_TO_TICKS(3000));
    }

    // Cleanup all resources including iot_eth
    CleanupResources(true);

    SetInitialized(false);

    ESP_LOGI(kTag, "UartEthModem stopped");
    return ESP_OK;
}

esp_err_t UartEthModem::SendAt(const std::string& cmd, std::string& response, uint32_t timeout_ms) {
    std::lock_guard<std::mutex> lock(at_mutex_);

    // Allow AT commands during initialization even if not connected
    if (!connected_.load() && !initializing_.load() && !initialized_.load()) {
        ESP_LOGE(kTag, "Failed to send AT command: not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    at_command_response_.clear();
    waiting_for_at_response_ = true;
    xSemaphoreTake(at_command_response_semaphore_, 0);  // Clear any pending

    // Add \r if not present
    std::string cmd_with_cr = cmd;
    if (cmd_with_cr.empty() || cmd_with_cr.back() != '\r') {
        cmd_with_cr += '\r';
    }

    // Send AT command frame
    esp_err_t ret = SendFrame(reinterpret_cast<const uint8_t*>(cmd_with_cr.c_str()), cmd_with_cr.size(), FrameType::kAtCommand);
    if (ret != ESP_OK) {
        waiting_for_at_response_ = false;
        return ret;
    }

    if (debug_enabled_.load()) {
        ESP_LOGI(kTag, "AT>>> %s", cmd.c_str());
    }

    // Wait for response
    if (xSemaphoreTake(at_command_response_semaphore_, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        ESP_LOGW(kTag, "AT timeout: %s", cmd.c_str());
        waiting_for_at_response_ = false;
        return ESP_ERR_TIMEOUT;
    }

    waiting_for_at_response_ = false;
    response = at_command_response_;

    // Check for OK/ERROR
    if (response.find("OK") != std::string::npos) {
        return ESP_OK;
    } else if (response.find("ERROR") != std::string::npos) {
        return ESP_FAIL;
    }

    return ESP_OK;
}

UartEthModem::NetworkStatus UartEthModem::WaitForNetworkReady(uint32_t timeout_ms) {
    if (initialized_.load() && network_status_.load() == NetworkStatus::Ready) {
        return NetworkStatus::Ready;
    }

    TickType_t wait_time = (timeout_ms == 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    auto bits = xEventGroupWaitBits(event_group_, kEventNetworkReady | kEventStop, pdFALSE, pdFALSE, wait_time);
    
    if (bits & kEventNetworkReady) {
        return network_status_.load();
    }
    
    // Timeout or stopped, return current status
    return network_status_.load();
}

void UartEthModem::SetNetworkStatusCallback(NetworkStatusCallback callback) {
    network_status_callback_ = std::move(callback);
}

void UartEthModem::SetDebug(bool enabled) {
    debug_enabled_.store(enabled);
}

std::string UartEthModem::GetImei() {
    if (imei_.empty()) {
        std::string resp;
        if (SendAt("AT+CGSN=1", resp) == ESP_OK) {
            // Parse +CGSN: "IMEI" using sscanf, consistent with C implementation
            char imei[16] = {0};
            if (sscanf(resp.c_str(), "\r\n+CGSN: \"%15s", imei) == 1) {
                imei_ = imei;
            }
        }
    }
    return imei_;
}

std::string UartEthModem::GetIccid() {
    if (iccid_.empty()) {
        std::string resp;
        if (SendAt("AT+ECICCID", resp) == ESP_OK) {
            // Parse +ECICCID: ICCID using sscanf, consistent with C implementation
            char iccid[21] = {0};
            if (sscanf(resp.c_str(), "\r\n+ECICCID: %20s", iccid) == 1) {
                iccid_ = iccid;
            }
        }
    }
    return iccid_;
}

std::string UartEthModem::GetCarrierName() {
    std::string resp;
    if (SendAt("AT+COPS?", resp) == ESP_OK) {
        // Parse +COPS: mode,format,"operator",act using sscanf
        int mode = 0, format = 0, act = 0;
        char operator_name[64] = {0};
        if (sscanf(resp.c_str(), "\r\n+COPS: %d,%d,\"%63[^\"]\",%d", &mode, &format, operator_name, &act) >= 3) {
            carrier_name_ = operator_name;
        }
    }
    return carrier_name_;
}

std::string UartEthModem::GetModuleRevision() {
    if (module_revision_.empty()) {
        std::string resp;
        if (SendAt("AT+CGMR", resp) == ESP_OK) {
            // Parse response using sscanf, consistent with C implementation
            char revision[128] = {0};
            if (resp.find("+CGMR:") != std::string::npos) {
                // Format: "\r\n+CGMR: \r\n<version>\r\n"
                if (sscanf(resp.c_str(), "\r\n+CGMR: \r\n%127[^\r\n]", revision) == 1) {
                    module_revision_ = revision;
                }
            } else {
                // Format: "\r\n<version>\r\n"
                if (sscanf(resp.c_str(), "\r\n%127[^\r\n]", revision) == 1) {
                    module_revision_ = revision;
                }
            }
        }
    }
    return module_revision_;
}

int UartEthModem::GetSignalStrength() {
    std::string resp;
    if (SendAt("AT+CSQ", resp, 500) == ESP_OK) {
        // Parse +CSQ: rssi,ber
        auto pos = resp.find("+CSQ:");
        if (pos != std::string::npos) {
            int rssi = 99;
            sscanf(resp.c_str() + pos, "+CSQ: %d", &rssi);
            signal_strength_ = rssi;
        }
    }
    return signal_strength_;
}

UartEthModem::CellInfo UartEthModem::GetCellInfo() {
    std::string resp;
    if (SendAt("AT+CEREG?", resp) == ESP_OK) {
        ParseAtResponse(resp);
    }
    return cell_info_;
}

const char* UartEthModem::GetNetworkStatusName(NetworkStatus status) {
    switch (status) {
        case NetworkStatus::Ready: return "Ready";
        case NetworkStatus::LinkUp: return "LinkUp";
        case NetworkStatus::LinkDown: return "LinkDown";
        case NetworkStatus::Searching: return "Searching";
        case NetworkStatus::Error: return "Error";
        case NetworkStatus::NoSimCard: return "NoSimCard";
        case NetworkStatus::RegistrationDenied: return "RegistrationDenied";
        case NetworkStatus::NoCarrier: return "NoCarrier";
        default: return "Unknown";
    }
}

// Private methods

esp_err_t UartEthModem::InitUart() {
    // UHCI DMA 模式：只配置 UART 参数，不安装驱动（UHCI 会接管）
    // UHCI DMA mode: only configure UART params, don't install driver (UHCI takes over)
    uart_config_t uart_config = {
        .baud_rate = config_.baud_rate,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 0,
        .source_clk = UART_SCLK_DEFAULT,
        .flags = {.allow_pd = 0, .backup_before_sleep = 0},
    };
    esp_err_t ret = uart_param_config(config_.uart_num, &uart_config);
    ESP_RETURN_ON_ERROR(ret, kTag, "Failed to configure UART");

    ret = uart_set_pin(config_.uart_num, config_.tx_pin, config_.rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    ESP_RETURN_ON_ERROR(ret, kTag, "Failed to set UART pins");

    gpio_set_pull_mode(config_.rx_pin, GPIO_PULLUP_ONLY);

    return ESP_OK;
}

esp_err_t UartEthModem::InitGpio() {
    // MRDY pin (output) - Master Ready/Busy
    // 初始化为高电平（空闲状态）
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << config_.mrdy_pin),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t ret = gpio_config(&io_conf);
    ESP_RETURN_ON_ERROR(ret, kTag, "Failed to configure MRDY pin");

    // Set MRDY high initially (idle)
    gpio_set_level(config_.mrdy_pin, 1);
    gpio_sleep_sel_dis(config_.mrdy_pin);

    // SRDY pin (input with interrupt) - Slave Ready/Busy
    io_conf.pin_bit_mask = (1ULL << config_.srdy_pin);
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    ret = gpio_config(&io_conf);
    ESP_RETURN_ON_ERROR(ret, kTag, "Failed to configure SRDY pin");

    // Enable GPIO wakeup on low level (for light sleep)
    // 启用低电平唤醒（用于轻度睡眠）
    ret = gpio_wakeup_enable(config_.srdy_pin, GPIO_INTR_LOW_LEVEL);
    ESP_RETURN_ON_ERROR(ret, kTag, "Failed to enable GPIO wakeup");

    // Install ISR handler
    ret = gpio_isr_handler_add(config_.srdy_pin, SrdyIsrHandler, this);
    ESP_RETURN_ON_ERROR(ret, kTag, "Failed to add ISR handler");

    // Initially configure for wakeup (low level trigger)
    // 初始配置为唤醒模式（低电平触发）
    ConfigureSrdyInterrupt(true);

    return ESP_OK;
}

esp_err_t UartEthModem::InitIotEth() {
    // Install iot_eth driver
    iot_eth_config_t eth_cfg = {
        .driver = &driver_,
        .stack_input = nullptr,
        .user_data = this,
    };

    esp_err_t ret = iot_eth_install(&eth_cfg, &eth_handle_);
    ESP_RETURN_ON_ERROR(ret, kTag, "Failed to install iot_eth driver");

    // Create netif
    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    eth_netif_ = esp_netif_new(&netif_cfg);
    if (!eth_netif_) {
        ESP_LOGE(kTag, "Failed to create netif");
        iot_eth_uninstall(eth_handle_);
        eth_handle_ = nullptr;
        return ESP_ERR_NO_MEM;
    }

    // Create glue and attach
    glue_ = iot_eth_new_netif_glue(eth_handle_);
    if (!glue_) {
        ESP_LOGE(kTag, "Failed to create netif glue");
        esp_netif_destroy(eth_netif_);
        eth_netif_ = nullptr;
        iot_eth_uninstall(eth_handle_);
        eth_handle_ = nullptr;
        return ESP_ERR_NO_MEM;
    }

    esp_netif_attach(eth_netif_, glue_);

    // Start iot_eth
    ret = iot_eth_start(eth_handle_);
    if (ret != ESP_OK) {
        ESP_LOGE(kTag, "Failed to start iot_eth");
        iot_eth_del_netif_glue(glue_);
        glue_ = nullptr;
        esp_netif_destroy(eth_netif_);
        eth_netif_ = nullptr;
        iot_eth_uninstall(eth_handle_);
        eth_handle_ = nullptr;
        return ret;
    }

    // Register IP event handler to detect when we get an IP address
    ret = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_ETH_GOT_IP,
                                              &IpEventHandler, this,
                                              &ip_event_handler_instance_);
    if (ret != ESP_OK) {
        ESP_LOGE(kTag, "Failed to register IP event handler");
        iot_eth_stop(eth_handle_);
        iot_eth_del_netif_glue(glue_);
        glue_ = nullptr;
        esp_netif_destroy(eth_netif_);
        eth_netif_ = nullptr;
        iot_eth_uninstall(eth_handle_);
        eth_handle_ = nullptr;
        return ret;
    }

    // Notify iot_eth of link state changes (critical for netif to work)
    // This triggers IOT_ETH_EVENT_CONNECTED which starts DHCP
    if (mediator_) {
        // Notify MAC address available
        mediator_->on_stage_changed(mediator_, IOT_ETH_STAGE_GET_MAC, nullptr);
        // Notify link is up (IOT_ETH_LINK_UP = 0)
        int link_status = 0;  // IOT_ETH_LINK_UP
        mediator_->on_stage_changed(mediator_, IOT_ETH_STAGE_LINK, &link_status);
    }

    return ESP_OK;
}

void UartEthModem::DeinitUart() {
    // UHCI 模式下，UART 没有安装驱动，无需删除
    // In UHCI mode, UART driver is not installed, nothing to delete
}

void UartEthModem::DeinitGpio() {
    gpio_isr_handler_remove(config_.srdy_pin);
}

void UartEthModem::DeinitIotEth() {
    // Unregister IP event handler first
    if (ip_event_handler_instance_) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_ETH_GOT_IP,
                                              ip_event_handler_instance_);
        ip_event_handler_instance_ = nullptr;
    }
    if (eth_handle_) {
        iot_eth_stop(eth_handle_);
    }
    if (glue_) {
        iot_eth_del_netif_glue(glue_);
        glue_ = nullptr;
    }
    if (eth_netif_) {
        esp_netif_destroy(eth_netif_);
        eth_netif_ = nullptr;
    }
    if (eth_handle_) {
        iot_eth_uninstall(eth_handle_);
        eth_handle_ = nullptr;
    }
}

// 主任务：处理所有事件（使用 UHCI DMA，收发都不阻塞）
// Main task: handles all events (using UHCI DMA, no blocking I/O)
void UartEthModem::MainTaskRun() {
    ESP_LOGI(kTag, "Main task started (UHCI DMA mode)");

    // Initialize MRDY to high (not busy, allow slave to sleep)
    // 初始化 MRDY 为高电平（空闲）
    SetMrdy(false);
    working_state_.store(WorkingState::Idle);

    while (!stop_flag_.load()) {
        // Calculate next timeout based on current state
        // 根据当前状态计算下一次超时时间
        TickType_t wait_ticks = CalculateNextTimeout();

        Event event;
        if (xQueueReceive(event_queue_, &event, wait_ticks) == pdTRUE) {
            HandleEvent(event);
        } else {
            // Timeout occurred
            // 发生超时
            HandleIdleTimeout();
        }
    }

    // Ensure we're in idle state before exiting
    if (working_state_.load() != WorkingState::Idle) {
        EnterIdleState();
    }

    ESP_LOGI(kTag, "Main task exiting");
    xEventGroupSetBits(event_group_, kEventMainTaskDone);
}

// UHCI RX 回调静态包装器（在中断上下文中调用）
// UHCI RX callback static wrapper (called from ISR context)
bool IRAM_ATTR UartEthModem::UhciRxCallbackStatic(const UartUhci::RxEventData& data, void* user_data) {
    auto* self = static_cast<UartEthModem*>(user_data);
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;

    // Send RxData event to main task
    // 发送 RxData 事件到主任务
    Event event = {
        .type = EventType::RxData,
        .data = data.data,
        .size = data.recv_size,
        .is_complete = data.is_complete,
    };

    xQueueSendFromISR(self->event_queue_, &event, &xHigherPriorityTaskWoken);

    return xHigherPriorityTaskWoken == pdTRUE;
}

// UHCI TX 回调静态包装器（在中断上下文中调用）
// UHCI TX callback static wrapper (called from ISR context)
bool IRAM_ATTR UartEthModem::UhciTxCallbackStatic(const UartUhci::TxDoneEventData& data, void* user_data) {
    auto* self = static_cast<UartEthModem*>(user_data);
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;

    // Send TxDone event to main task
    // 发送 TxDone 事件到主任务
    Event event = {
        .type = EventType::TxDone,
        .data = nullptr,
        .size = 0,
        .is_complete = true,
    };

    xQueueSendFromISR(self->event_queue_, &event, &xHigherPriorityTaskWoken);

    return xHigherPriorityTaskWoken == pdTRUE;
}

// 启动下一次 DMA 接收
// Start next DMA receive
void UartEthModem::StartDmaReceive() {
    // Only start new DMA receive if in Active or PendingIdle state
    // In Idle state, don't start receive to release UHCI's NO_LIGHT_SLEEP lock
    // 只有在 Active 或 PendingIdle 状态才启动新的 DMA 接收
    // Idle 状态不启动接收，以释放 UHCI 的 NO_LIGHT_SLEEP 锁
    WorkingState state = working_state_.load();
    if (state == WorkingState::Idle) {
        ESP_LOGD(kTag, "Skipping DMA receive in idle state");
        return;
    }

    // Start new DMA receive with buffer
    // 启动新的 DMA 接收
    esp_err_t ret = uart_uhci_.StartReceive(rx_buffer_, config_.rx_buffer_size);
    if (ret != ESP_OK) {
        ESP_LOGE(kTag, "Failed to start DMA receive: %s", esp_err_to_name(ret));
    }
}

// 计算下一次超时时间
// Calculate next timeout based on current working state
TickType_t UartEthModem::CalculateNextTimeout() {
    WorkingState state = working_state_.load();

    if (state == WorkingState::Idle) {
        // In idle state, wait indefinitely for events
        // 空闲状态：无限等待事件
        return portMAX_DELAY;
    }

    if (state == WorkingState::Active) {
        // In active state, calculate remaining time until idle timeout
        // 工作状态：计算距离超时的剩余时间
        int64_t now_us = esp_timer_get_time();
        int64_t elapsed_us = now_us - last_activity_time_us_;
        int64_t remaining_us = kIdleTimeoutUs - elapsed_us;

        if (remaining_us <= 0) {
            return 0;  // Already timed out
        }
        return pdMS_TO_TICKS(remaining_us / 1000);
    }

    if (state == WorkingState::PendingIdle) {
        // In pending idle state, check SRDY periodically
        // 待退出状态：定期检查 SRDY
        return pdMS_TO_TICKS(10);  // Check every 10ms
    }

    return portMAX_DELAY;
}

// 处理事件
// Handle incoming event
void UartEthModem::HandleEvent(const Event& event) {
    switch (event.type) {
        case EventType::TxRequest:
            // 进入工作状态以准备发送
            // Enter active state to prepare for sending
            if (working_state_.load() != WorkingState::Active) {
                EnterActiveState();
            }
            break;

        case EventType::SrdyLow:
            HandleSrdyLow();
            break;

        case EventType::SrdyHigh:
            HandleSrdyHigh();
            break;

        case EventType::RxData:
            HandleRxData(event.data, event.size, event.is_complete);
            break;

        case EventType::TxDone:
            HandleTxDone();
            break;

        case EventType::Stop:
            stop_flag_.store(true);
            break;

        default:
            break;
    }
}

// 处理 DMA 接收到的数据
// Handle data received via DMA
void UartEthModem::HandleRxData(uint8_t* data, size_t size, bool is_complete) {
    // Ensure we're in active state
    if (working_state_.load() != WorkingState::Active) {
        EnterActiveState();
    }

    // Process received frame
    ProcessReceivedFrame(data, size);

    // Update activity time
    last_activity_time_us_ = esp_timer_get_time();

    // If this was a complete frame (idle detected), start next receive
    // 如果这是完整帧（检测到空闲），启动下一次接收
    if (is_complete) {
        // Send ACK pulse to slave
        SendAckPulse();

        // Start next DMA receive
        StartDmaReceive();
    }
}

// 处理 DMA 发送完成
// Handle DMA transmit completion
void UartEthModem::HandleTxDone() {
    if (debug_enabled_.load()) {
        ESP_LOGI(kTag, "TX done");
    }

    // Update activity time
    last_activity_time_us_ = esp_timer_get_time();

    // Wait for slave ACK using polling (very short, 50us pulse)
    // 使用轮询等待 slave 确认（很短，50us 脉冲）
    if (!WaitForSrdyAck(kAckTimeoutUs)) {
        if (debug_enabled_.load()) {
            ESP_LOGW(kTag, "Slave ACK timeout");
        }
    }
}

// 处理 SRDY 拉低事件（Slave 要发数据或唤醒）
// Handle SRDY low event: Slave wants to send data or waking up
void UartEthModem::HandleSrdyLow() {
    WorkingState state = working_state_.load();

    if (state == WorkingState::Idle || state == WorkingState::PendingIdle) {
        // Slave is waking us up, enter active state
        // Slave 正在唤醒我们，进入工作状态
        ESP_LOGI(kTag, "Slave wakeup detected, entering active state");
        EnterActiveState();
    }

    // Configure interrupt to detect SRDY going high (for ACK or idle detection)
    // 配置中断以检测 SRDY 变高
    ConfigureSrdyInterrupt(false);  // Not for wakeup, for detecting high
}

// 处理 SRDY 拉高事件（Slave 确认收到数据或进入休眠）
// Handle SRDY high event: Slave ACK or entering sleep
void UartEthModem::HandleSrdyHigh() {
    WorkingState state = working_state_.load();

    if (state == WorkingState::PendingIdle) {
        // Both sides are now idle, enter idle state
        // 双方都空闲了，进入休眠状态
        ESP_LOGI(kTag, "Slave also idle, entering idle state");
        EnterIdleState();
    }

    // Configure interrupt to detect SRDY going low (for wakeup)
    // 配置中断以检测 SRDY 拉低（用于唤醒）
    ConfigureSrdyInterrupt(true);  // For wakeup
}

// 处理空闲超时
// Handle idle timeout
void UartEthModem::HandleIdleTimeout() {
    WorkingState state = working_state_.load();

    if (state == WorkingState::Active) {
        // Timeout in active state, enter pending idle
        // 工作状态超时，进入待退出状态
        ESP_LOGI(kTag, "Idle timeout, entering pending idle state");
        EnterPendingIdleState();
    } else if (state == WorkingState::PendingIdle) {
        // Check if SRDY is high
        // 检查 SRDY 是否已经变高
        if (!IsSrdyLow()) {
            ESP_LOGI(kTag, "Slave is idle, entering idle state");
            EnterIdleState();
        }
        // If SRDY is still low, slave has data to send
        // Stay in pending idle and wait for SRDY high event
    }
}

// 进入工作状态
// Enter active working state
void UartEthModem::EnterActiveState() {
    WorkingState prev_state = working_state_.load();
    if (prev_state == WorkingState::Active) {
        return;  // Already active
    }

    ESP_LOGD(kTag, "Entering active state");

    // Only acquire PM lock and start DMA receive when transitioning from Idle state
    // PendingIdle -> Active doesn't need these (lock was never released, DMA still running)
    // 只有从 Idle 状态进入时才获取锁和启动 DMA 接收
    // PendingIdle -> Active 不需要（锁从未释放，DMA 仍在运行）
    if (prev_state == WorkingState::Idle) {
        ESP_ERROR_CHECK(esp_pm_lock_acquire(pm_lock_handle_));
        
        // Start DMA receive (this also acquires UHCI's NO_LIGHT_SLEEP lock)
        // 启动 DMA 接收（这也会获取 UHCI 的 NO_LIGHT_SLEEP 锁）
        esp_err_t ret = uart_uhci_.StartReceive(rx_buffer_, config_.rx_buffer_size);
        if (ret != ESP_OK) {
            ESP_LOGE(kTag, "Failed to start UHCI receive: %s", esp_err_to_name(ret));
        }
    }

    // Set MRDY low (busy)
    // 拉低 MRDY（忙）
    SetMrdy(true);

    // Update state
    working_state_.store(WorkingState::Active);
    last_activity_time_us_ = esp_timer_get_time();

    // Configure SRDY interrupt for detecting high level
    // 配置 SRDY 中断以检测高电平
    ConfigureSrdyInterrupt(false);
}

// 进入待退出状态
// Enter pending idle state
void UartEthModem::EnterPendingIdleState() {
    if (working_state_.load() != WorkingState::Active) {
        return;  // Not in active state
    }

    ESP_LOGD(kTag, "Entering pending idle state");

    // Set MRDY high (not busy, allow slave to sleep)
    // 拉高 MRDY（空闲，允许 Slave 休眠）
    SetMrdy(false);

    // Update state
    working_state_.store(WorkingState::PendingIdle);

    // Configure SRDY interrupt for detecting high level
    // 配置 SRDY 中断以检测高电平
    ConfigureSrdyInterrupt(false);
}

// 进入休眠状态
// Enter idle/sleep state
void UartEthModem::EnterIdleState() {
    ESP_LOGD(kTag, "Entering idle state");

    // Ensure MRDY is high
    // 确保 MRDY 为高
    SetMrdy(false);

    // Update state
    working_state_.store(WorkingState::Idle);

    // Stop DMA receive to release UHCI's NO_LIGHT_SLEEP lock
    // 停止 DMA 接收以释放 UHCI 的 NO_LIGHT_SLEEP 锁
    uart_uhci_.StopReceive();

    // Release PM lock to allow CPU frequency scaling
    // 释放电源管理锁，允许 CPU 降频
    ESP_ERROR_CHECK(esp_pm_lock_release(pm_lock_handle_));

    // Configure SRDY interrupt for wakeup (low level trigger)
    // 配置 SRDY 中断用于唤醒（低电平触发）
    ConfigureSrdyInterrupt(true);
}

void UartEthModem::InitTaskRun() {
    ESP_LOGI(kTag, "Init task started");

    // Wait for start signal
    xEventGroupWaitBits(event_group_, kEventStart, pdTRUE, pdTRUE, portMAX_DELAY);

    if (stop_flag_.load()) {
        goto exit;
    }

    // Run initialization sequence
    if (RunInitSequence() != ESP_OK) {
        ESP_LOGE(kTag, "Initialization sequence failed");
        xEventGroupSetBits(event_group_, kEventStop);
        goto exit;
    }

    // Initialize iot_eth
    if (InitIotEth() != ESP_OK) {
        ESP_LOGE(kTag, "Failed to initialize iot_eth");
        xEventGroupSetBits(event_group_, kEventStop);
        goto exit;
    }

    ESP_LOGI(kTag, "Initialization complete");
    xEventGroupSetBits(event_group_, kEventInitDone);

exit:
    ESP_LOGI(kTag, "Init task exiting");
    xEventGroupSetBits(event_group_, kEventInitTaskDone);
}

// 发送帧（公共接口）：使用 UHCI DMA 发送
// Send frame (public interface): send using UHCI DMA
esp_err_t UartEthModem::SendFrame(const uint8_t* data, size_t length, FrameType type) {
    // Allocate frame with header (DMA compatible memory)
    size_t total_len = sizeof(FrameHeader) + length;
    uint8_t* buffer = static_cast<uint8_t*>(heap_caps_malloc(total_len, MALLOC_CAP_DMA));
    if (!buffer) {
        ESP_LOGE(kTag, "Failed to allocate TX buffer");
        return ESP_ERR_NO_MEM;
    }

    // Build header
    FrameHeader* header = reinterpret_cast<FrameHeader*>(buffer);
    *reinterpret_cast<uint32_t*>(header->raw) = 0;
    header->SetPayloadLength(length);
    header->SetSequence(seq_no_++);
    header->SetFlowControl(false);  // XON = 0 (permit to send), XOFF = 1 (shall not send)
    header->SetType(type);
    header->UpdateChecksum();

    // Copy payload
    memcpy(buffer + sizeof(FrameHeader), data, length);

    // Enter active state if not already (for MRDY control)
    // 如果还没进入工作状态，先进入（用于控制 MRDY）
    if (working_state_.load() != WorkingState::Active) {
        // Send event to main task to enter active state
        Event event = {.type = EventType::TxRequest, .data = nullptr, .size = 0, .is_complete = false};
        xQueueSend(event_queue_, &event, pdMS_TO_TICKS(10));
        
        // Wait a bit for state transition
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    // Wait for SRDY to go low if needed (slave ready to receive)
    // 如果需要，等待 SRDY 变低（slave 准备好接收）
    if (!IsSrdyLow()) {
        int64_t start_us = esp_timer_get_time();
        while (!IsSrdyLow()) {
            if (esp_timer_get_time() - start_us > 50 * 1000) {
                ESP_LOGW(kTag, "Slave not responding (SRDY still high)");
                break;
            }
            vTaskDelay(1);
        }
    }

    // Transmit via UHCI DMA (non-blocking, returns immediately)
    // 通过 UHCI DMA 发送（非阻塞，立即返回）
    esp_err_t ret = uart_uhci_.Transmit(buffer, total_len);
    if (ret != ESP_OK) {
        ESP_LOGE(kTag, "UHCI transmit failed: %s", esp_err_to_name(ret));
        free(buffer);
        return ret;
    }

    // Note: buffer will be freed after TX done callback
    // For simplicity, we wait for TX to complete here
    // 注意：缓冲区会在发送完成回调后释放
    // 为简单起见，这里等待发送完成
    ret = uart_uhci_.WaitTxDone(1000);
    free(buffer);

    if (ret != ESP_OK) {
        ESP_LOGE(kTag, "UHCI TX wait failed: %s", esp_err_to_name(ret));
        return ret;
    }

    return ESP_OK;
}

// 处理 DMA 接收到的帧数据
// Process frame data received via DMA
void UartEthModem::ProcessReceivedFrame(uint8_t* data, size_t size) {
    if (size < sizeof(FrameHeader)) {
        if (debug_enabled_.load() && size > 0) {
            ESP_LOGI(kTag, "Not enough data for header, size: %d", size);
        }
        return;
    }

    // Parse header from DMA buffer
    FrameHeader* header = reinterpret_cast<FrameHeader*>(data);

    // Validate checksum
    if (!header->ValidateChecksum()) {
        ESP_LOGW(kTag, "Invalid checksum, raw: %02x %02x %02x %02x", 
                 header->raw[0], header->raw[1], header->raw[2], header->raw[3]);
        return;
    }

    uint16_t payload_len = header->GetPayloadLength();
    if (sizeof(FrameHeader) + payload_len > size) {
        ESP_LOGW(kTag, "Incomplete frame: expected %d, got %d", 
                 sizeof(FrameHeader) + payload_len, size);
        return;
    }

    if (debug_enabled_.load()) {
        ESP_LOGI(kTag, "RX frame: type=%d, len=%d, seq=%d", 
                 static_cast<int>(header->GetType()), payload_len, header->GetSequence());
    }

    // Get payload pointer (data is in DMA buffer, need to copy for ownership)
    uint8_t* payload = data + sizeof(FrameHeader);

    // Handle frame based on type
    if (header->GetType() == FrameType::kEthernet) {
        // For Ethernet frames, we need to copy data since HandleEthFrame takes ownership
        uint8_t* payload_copy = static_cast<uint8_t*>(malloc(payload_len));
        if (payload_copy) {
            memcpy(payload_copy, payload, payload_len);
            HandleEthFrame(payload_copy, payload_len);
        } else {
            ESP_LOGE(kTag, "Failed to allocate RX buffer");
        }
    } else if (header->GetType() == FrameType::kAtCommand) {
        // For AT responses, we can use the data in place (it will be copied in HandleAtResponse)
        HandleAtResponse(reinterpret_cast<char*>(payload), payload_len);
    }
}

void UartEthModem::HandleEthFrame(uint8_t* data, size_t length) {
    if (!connected_.load()) {
        // Check for handshake ACK
        if (length >= sizeof(kHandshakeAck) && memcmp(data, kHandshakeAck, sizeof(kHandshakeAck)) == 0) {
            ESP_LOGI(kTag, "Handshake ACK received");
            connected_ = true;
            xEventGroupSetBits(event_group_, kEventConnected);
            // Mark as initialized, but wait for IP_EVENT_ETH_GOT_IP for network ready
            SetInitialized(true);
            SetNetworkStatus(NetworkStatus::LinkUp);
        }
        free(data);
        return;
    }

    // Forward to iot_eth stack
    if (mediator_) {
        // Note: mediator->stack_input takes ownership of data (frees it)
        mediator_->stack_input(mediator_, data, length);
    } else {
        free(data);
    }
}

void UartEthModem::HandleAtResponse(const char* data, size_t length) {
    std::string response(data, length);

    // Parse URC or response
    ParseAtResponse(response);

    // If waiting for AT response, signal completion
    if (waiting_for_at_response_) {
        at_command_response_ = response;
        xSemaphoreGive(at_command_response_semaphore_);
    }
}

esp_err_t UartEthModem::SendAtWithRetry(const std::string& cmd, std::string& response, uint32_t timeout_ms, int max_retries) {
    esp_err_t ret = ESP_FAIL;
    for (int i = 0; i < max_retries; i++) {
        ret = SendAt(cmd, response, timeout_ms);
        if (ret == ESP_OK) {
            return ESP_OK;
        }
        if (i < max_retries - 1) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
    return ret;
}

void UartEthModem::ParseAtResponse(const std::string& response) {
    if (debug_enabled_.load()) {
        ESP_LOGI(kTag, "AT<<< %s", response.c_str());
    }

    // Parse CEREG
    auto cereg_pos = response.find("+CEREG:");
    if (cereg_pos != std::string::npos) {
        int n = 0, stat = 0;
        char tac[16] = {0}, ci[16] = {0};
        int act = 0;
        
        // Try different formats
        if (sscanf(response.c_str() + cereg_pos, "+CEREG: %d,%d,\"%[^\"]\",\"%[^\"]\",%d", &n, &stat, tac, ci, &act) >= 2) {
            cell_info_.stat = stat;
            cell_info_.tac = tac;
            cell_info_.ci = ci;
            cell_info_.act = act;
        } else if (sscanf(response.c_str() + cereg_pos, "+CEREG: %d", &stat) == 1) {
            cell_info_.stat = stat;
        }

        // Check registration status
        if (cell_info_.stat == 2) {
            SetNetworkStatus(NetworkStatus::Searching);
        } else if (cell_info_.stat == 3) {
            SetNetworkStatus(NetworkStatus::RegistrationDenied);
        }
    } else if (response.find("+ECNETDEVCTL: 1") != std::string::npos) {
        // Network device ready (link up)
        SetNetworkStatus(NetworkStatus::LinkUp);
    } else if (response.find("+ECNETDEVCTL: 0") != std::string::npos) {
        // Network device down (link down)
        SetNetworkStatus(NetworkStatus::LinkDown);
    }
}

esp_err_t UartEthModem::RunInitSequence() {
    std::string resp;
    esp_err_t ret;

    // Step 1: AT test
    ESP_LOGI(kTag, "Detecting modem...");
    ret = SendAtWithRetry("AT", resp, 500, 20);
    if (ret != ESP_OK) {
        ESP_LOGE(kTag, "Modem not detected");
        SetNetworkStatus(NetworkStatus::Error);
        return ret;
    }

    ESP_LOGI(kTag, "Checking network configuration...");
    ret = SendAt("AT+ECNETCFG?", resp, 1000);
    if (ret != ESP_OK || resp.find("+ECNETCFG: \"nat\",1") == std::string::npos) {
        // First-time configuration
        ESP_LOGI(kTag, "Configuring network (first-time setup)...");
        SendAtWithRetry("AT+ECPCFG=\"usbCtrl\",1", resp, 1000, 3);
        SendAtWithRetry("AT+CEREG=1", resp, 1000, 3);
        SendAtWithRetry("AT+ECNETCFG=\"nat\",1,\"192.168.10.2\"", resp, 1000, 3);
        SendAtWithRetry("AT&W", resp, 300, 3);
        
        // Reset after configuration
        // Clear network status changed bit before reset
        xEventGroupClearBits(event_group_, kEventNetworkStatusChanged);
        SendAt("AT+ECRST", resp, 500);
        auto bits = xEventGroupWaitBits(event_group_, kEventNetworkStatusChanged, pdTRUE, pdTRUE, pdMS_TO_TICKS(10000));
        if (bits & kEventNetworkStatusChanged) {
            ESP_LOGI(kTag, "Modem reset completed");
        } else {
            ESP_LOGE(kTag, "Modem reset timed out");
            SetNetworkStatus(NetworkStatus::Error);
            return ESP_ERR_TIMEOUT;
        }
        ret = SendAtWithRetry("AT", resp, 500, 20);
        if (ret != ESP_OK) {
            ESP_LOGE(kTag, "Modem not responding after reset");
            SetNetworkStatus(NetworkStatus::Error);
            return ret;
        }
    }

    ESP_LOGI(kTag, "Querying modem info...");
    QueryModemInfo();

    ESP_LOGI(kTag, "Checking SIM card...");
    if (!CheckSimCard()) {
        ESP_LOGE(kTag, "SIM card not ready");
        SetNetworkStatus(NetworkStatus::NoSimCard);
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(kTag, "Waiting for network registration...");
    SetNetworkStatus(NetworkStatus::Searching);
    
    // Enable CEREG URC
    SendAt("AT+CEREG=2", resp);
    if (!WaitForRegistration(120000)) {
        if (cell_info_.stat == 3) {
            ESP_LOGE(kTag, "Registration denied");
            SetNetworkStatus(NetworkStatus::RegistrationDenied);
        } else {
            ESP_LOGE(kTag, "Registration timeout");
            SetNetworkStatus(NetworkStatus::Error);
        }
        return ESP_ERR_TIMEOUT;
    }

    ESP_LOGI(kTag, "Starting network device...");
    // ECNETDEVCTL may take several seconds to start network device
    ret = SendAt("AT+ECNETDEVCTL=2,1,1", resp, 5000);
    if (ret != ESP_OK) {
        ESP_LOGE(kTag, "Failed to start network device");
        SetNetworkStatus(NetworkStatus::Error);
        return ret;
    }

    // Send handshake request
    ESP_LOGI(kTag, "Starting handshake...");
    ret = SendFrame(kHandshakeRequest, sizeof(kHandshakeRequest), FrameType::kEthernet);
    if (ret != ESP_OK) {
        ESP_LOGE(kTag, "Handshake failed");
        SetNetworkStatus(NetworkStatus::Error);
        return ret;
    }

    // Wait for handshake ACK
    auto bits = xEventGroupWaitBits(event_group_, kEventConnected | kEventStop, pdFALSE, pdFALSE, pdMS_TO_TICKS(kHandshakeTimeoutMs));
    if (bits & kEventConnected) {
        ESP_LOGI(kTag, "Handshake successful");
    } else {
        ESP_LOGE(kTag, "Handshake timeout");
        SetNetworkStatus(NetworkStatus::Error);
        return ESP_ERR_TIMEOUT;
    }

    // Network ready is set in HandleEthFrame when handshake ACK is received
    ESP_LOGI(kTag, "Modem initialization complete!");
    return ESP_OK;
}


bool UartEthModem::CheckSimCard() {
    std::string resp;
    for (int i = 0; i < 10; i++) {
        if (SendAt("AT+CPIN?", resp, 1000) == ESP_OK) {
            if (resp.find("+CPIN: READY") != std::string::npos) {
                return true;
            }
        }
        if (resp.find("+CME ERROR: 10") != std::string::npos) {
            // SIM not inserted
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    return false;
}

bool UartEthModem::WaitForRegistration(uint32_t timeout_ms) {
    std::string resp;
    uint32_t start = xTaskGetTickCount() * portTICK_PERIOD_MS;

    while (true) {
        if (SendAt("AT+CEREG?", resp, 1000) == ESP_OK) {
            ParseAtResponse(resp);
            if (cell_info_.stat == 1 || cell_info_.stat == 5) {
                return true;
            }
            if (cell_info_.stat == 3) {
                return false;  // Registration denied
            }
        }

        uint32_t elapsed = xTaskGetTickCount() * portTICK_PERIOD_MS - start;
        if (elapsed >= timeout_ms) {
            return false;
        }

        // Log progress
        if ((elapsed / 1000) % 10 == 0) {
            ESP_LOGI(kTag, "Waiting for registration... (%lu/%lu ms)", elapsed, timeout_ms);
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void UartEthModem::QueryModemInfo() {
    std::string resp;

    // Get IMEI using sscanf, consistent with C implementation
    if (SendAt("AT+CGSN=1", resp, 300) == ESP_OK) {
        char imei[16] = {0};
        if (sscanf(resp.c_str(), "\r\n+CGSN: \"%15s", imei) == 1) {
            imei_ = imei;
        }
    }

    // Get ICCID using sscanf, consistent with C implementation
    if (SendAt("AT+ECICCID", resp, 300) == ESP_OK) {
        char iccid[21] = {0};
        if (sscanf(resp.c_str(), "\r\n+ECICCID: %20s", iccid) == 1) {
            iccid_ = iccid;
        }
    }

    // Get module revision using sscanf, consistent with C implementation
    if (SendAt("AT+CGMR", resp, 300) == ESP_OK) {
        char revision[128] = {0};
        if (resp.find("+CGMR:") != std::string::npos) {
            // Format: "\r\n+CGMR: \r\n<version>\r\n"
            if (sscanf(resp.c_str(), "\r\n+CGMR: \r\n%127[^\r\n]", revision) == 1) {
                module_revision_ = revision;
            }
        } else {
            // Format: "\r\n<version>\r\n"
            if (sscanf(resp.c_str(), "\r\n%127[^\r\n]", revision) == 1) {
                module_revision_ = revision;
            }
        }
    }

    ESP_LOGD(kTag, "Modem Info - IMEI: %s, ICCID: %s, Rev: %s", imei_.c_str(), iccid_.c_str(), module_revision_.c_str());
}

// 设置 MRDY 电平
// Set MRDY level: low = true (busy), high = false (idle)
void UartEthModem::SetMrdy(bool low) {
    gpio_set_level(config_.mrdy_pin, low ? 0 : 1);
    mrdy_is_low_.store(low);
}

// 检测 SRDY 是否为低
// Check if SRDY is low (slave is busy/has data)
bool UartEthModem::IsSrdyLow() {
    return gpio_get_level(config_.srdy_pin) == 0;
}

// 发送确认脉冲：MRDY 高 50us
// Send ACK pulse: MRDY high for 50us
void UartEthModem::SendAckPulse() {
    // MRDY: low -> high (50us) -> low
    SetMrdy(false);  // High
    esp_rom_delay_us(kAckPulseUs);
    SetMrdy(true);   // Low (back to busy)
}

// 等待 SRDY 确认脉冲（高电平脉冲）
// Wait for SRDY ACK pulse (high level pulse)
// Returns true if ACK received, false if timeout
bool UartEthModem::WaitForSrdyAck(int64_t timeout_us) {
    int64_t start_us = esp_timer_get_time();

    // Wait for SRDY to go high
    while (IsSrdyLow()) {
        if (esp_timer_get_time() - start_us > timeout_us) {
            if (debug_enabled_.load()) {
                ESP_LOGW(kTag, "Wait for SRDY ACK timeout");
            }
            return false;
        }
        // Short delay to avoid busy-waiting
        esp_rom_delay_us(10);
    }

    return true;
}

// 配置 SRDY 中断类型
// Configure SRDY interrupt type
// for_wakeup: true = LOW_LEVEL (for light sleep wakeup), false = edge detection
void UartEthModem::ConfigureSrdyInterrupt(bool for_wakeup) {
    if (for_wakeup) {
        // LOW_LEVEL trigger for light sleep wakeup
        // 低电平触发，用于低功耗唤醒
        gpio_set_intr_type(config_.srdy_pin, GPIO_INTR_LOW_LEVEL);
    } else {
        // Use ANYEDGE to detect both rising and falling edges
        // This allows us to detect ACK pulses and state changes
        // 使用双边沿触发，以检测 ACK 脉冲和状态变化
        gpio_set_intr_type(config_.srdy_pin, GPIO_INTR_ANYEDGE);
    }
    gpio_intr_enable(config_.srdy_pin);
}

// SRDY 中断处理函数
// ISR handler for SRDY pin changes
// 根据当前配置的中断类型，发送对应的事件到主任务队列
void IRAM_ATTR UartEthModem::SrdyIsrHandler(void* arg) {
    auto* self = static_cast<UartEthModem*>(arg);
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;

    // Disable interrupt to avoid repeated triggers
    gpio_intr_disable(self->config_.srdy_pin);

    // Determine event type based on current SRDY level
    // 根据当前SRDY电平确定事件类型
    int level = gpio_get_level(self->config_.srdy_pin);
    Event event = {
        .type = (level == 0) ? EventType::SrdyLow : EventType::SrdyHigh,
        .data = nullptr,
        .size = 0,
        .is_complete = false,
    };

    xQueueSendFromISR(self->event_queue_, &event, &xHigherPriorityTaskWoken);

    if (xHigherPriorityTaskWoken) {
        portYIELD_FROM_ISR();
    }
}

void UartEthModem::SetInitialized(bool initialized) {
    bool old_value = initialized_.exchange(initialized);
    if (old_value != initialized) {
        ESP_LOGI(kTag, "Network initialized: %s -> %s", old_value ? "true" : "false", initialized ? "true" : "false");
    }
}

void UartEthModem::SetNetworkStatus(NetworkStatus status) {
    // Always update the status value
    NetworkStatus old_status = network_status_.exchange(status);
    // Set event bit to notify waiting tasks
    if (event_group_) {
        xEventGroupSetBits(event_group_, kEventNetworkStatusChanged);
    }
    
    // Only trigger callback when initialized
    if (initialized_.load()) {
        if (old_status != status) {
            ESP_LOGI(kTag, "Network status: %s -> %s", GetNetworkStatusName(old_status), GetNetworkStatusName(status));
            if (network_status_callback_) {
                network_status_callback_(status);
            }
        }
    } else {
        // Log status changes even during initialization
        if (old_status != status) {
            ESP_LOGI(kTag, "Network status: %s -> %s", GetNetworkStatusName(old_status), GetNetworkStatusName(status));
        }
    }
}

void UartEthModem::IpEventHandler(void* arg, esp_event_base_t event_base,
                                  int32_t event_id, void* event_data) {
    auto* self = static_cast<UartEthModem*>(arg);
    
    if (event_base == IP_EVENT && event_id == IP_EVENT_ETH_GOT_IP) {
        ip_event_got_ip_t* event = static_cast<ip_event_got_ip_t*>(event_data);
        ESP_LOGI(kTag, "Got IP address: " IPSTR, IP2STR(&event->ip_info.ip));
        
        // Network is ready now
        self->SetNetworkStatus(NetworkStatus::Ready);
        if (self->event_group_) {
            xEventGroupSetBits(self->event_group_, kEventNetworkReady);
        }
    }
}

void UartEthModem::CleanupResources(bool cleanup_iot_eth) {
    // Cleanup iot_eth if requested
    if (cleanup_iot_eth) {
        DeinitIotEth();
    }

    // Cleanup UHCI controller
    uart_uhci_.Deinit();

    // Cleanup DMA buffer
    if (rx_buffer_) {
        free(rx_buffer_);
        rx_buffer_ = nullptr;
    }

    // Cleanup semaphores
    if (at_command_response_semaphore_) {
        vSemaphoreDelete(at_command_response_semaphore_);
        at_command_response_semaphore_ = nullptr;
    }

    // Cleanup event queue
    if (event_queue_) {
        Event event;
        while (xQueueReceive(event_queue_, &event, 0) == pdTRUE) {
            // No dynamic data in events now
        }
        vQueueDelete(event_queue_);
        event_queue_ = nullptr;
    }

    // Cleanup GPIO and UART
    DeinitGpio();
    DeinitUart();
}