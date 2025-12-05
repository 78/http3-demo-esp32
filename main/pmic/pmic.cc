#include "pmic.h"
#include <esp_log.h>
#include <esp_err.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>
#include <driver/gpio.h>
#include <driver/uart.h>

#define TAG "PMIC"

// 静态成员变量初始化
Pmic* Pmic::instance_ = nullptr;
i2c_master_bus_handle_t Pmic::i2c_bus_ = nullptr;
bool Pmic::initialized_ = false;

/**
 * 获取PMIC单例实例
 */
Pmic& Pmic::GetInstance() {
    if (!initialized_) {
        if (!InitI2cBus()) {
            ESP_LOGE(TAG, "Failed to initialize I2C bus for PMIC");
            // 返回一个临时的空实例，但这会导致问题，实际应用中应该处理错误
            static Pmic dummy(nullptr, AXP2101_I2C_ADDR);
            return dummy;
        }

        instance_ = new Pmic(i2c_bus_, AXP2101_I2C_ADDR);
        initialized_ = true;
        ESP_LOGI(TAG, "PMIC singleton initialized successfully");
    }
    return *instance_;
}

/**
 * 初始化I2C总线
 */
bool Pmic::InitI2cBus() {
    if (i2c_bus_ != nullptr) {
        return true; // 已初始化
    }

    i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PMIC_I2C_SDA_PIN,
        .scl_io_num = PMIC_I2C_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags = {
            .enable_internal_pullup = 1,
            .allow_pd = 0,
        },
    };

    if (i2c_new_master_bus(&bus_config, &i2c_bus_) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create I2C bus for PMIC");
        return false;
    }

    return true;
}

Pmic::Pmic(i2c_master_bus_handle_t i2c_bus, uint8_t addr) 
    : Axp2101(i2c_bus, addr), event_group_handle_(nullptr), irq_task_handle_(nullptr), irq_callback_(nullptr) {
    event_group_handle_ = xEventGroupCreate();

    // ** EFUSE defaults **

    WriteReg(0x22, 0b110); // PWRON > OFFLEVEL as POWEROFF Source enable

    WriteReg(0x27, 0x16);  // hold 6s to power off, 1s to power on
    
    // Disable all LDOs
    WriteReg(0x90, 0);
    WriteReg(0x91, 0);

    WriteReg(0x92, (3.3-0.5) / 0.1); // 配置 aldo1 输出为 3.3V
    WriteReg(0x94, (3.3-0.5) / 0.1); // 配置 aldo3 输出为 3.3V
    
    // Enable DCDC1 only
    WriteReg(0x80, 0x01);

    WriteReg(0x64, 0x03); // CV charger voltage setting to 4.2V

    WriteReg(0x61, 0x05); // set Main battery precharge current to 125mA
    WriteReg(0x62, 0x0B); // set Main battery charger current to 500mA ( 0x08-200mA, 0x09-300mA, 0x0A-400mA )
    WriteReg(0x63, 0x15); // set Main battery term charge current to 125mA

    WriteReg(0x14, 0x00); // set minimum system voltage to 4.2V (default 4.7V), for poor USB cables
    WriteReg(0x15, 0x00); // set input voltage limit to 3.96v, for poor USB cables
    WriteReg(0x16, 0x05); // set input current limit to 2000mA

    WriteReg(0x24, (3.0-2.6) / 0.1); // set Vsys for PWROFF threshold to 3.0V (default - 2.6V and kill battery)
    WriteReg(0x50, 0x14); // disable temperature sensor (FIXME: should not disable in production)

    uint8_t irq_en_0 = 0;
    irq_en_0 |= (1 << 7);  // Enable SOC drop to Warning Level IRQ
    irq_en_0 |= (1 << 6);  // Enable SOC drop to Shutdown Level IRQ
    irq_en_0 |= (1 << 5);  // Enable Gauge Watchdog Timeout IRQ
    irq_en_0 |= (1 << 4);  // Enable Gauge New SOC IRQ
    irq_en_0 |= (1 << 3);  // Enable Battery Over Temperature in Charge mode IRQ
    irq_en_0 |= (1 << 2);  // Enable Battery Under Temperature in Charge mode IRQ
    irq_en_0 |= (1 << 1);  // Enable Battery Over Temperature in Work mode IRQ
    irq_en_0 |= (1 << 0);  // Enable Battery Under Temperature in Work mode IRQ
    WriteReg(0x40, irq_en_0);

    uint8_t irq_en_1 = 0;
    irq_en_1 |= (1 << 7);  // Enable VBUS insert IRQ
    irq_en_1 |= (1 << 6);  // Enable VBUS remove IRQ
    irq_en_1 |= (1 << 5);  // Enable Battery Insert IRQ
    irq_en_1 |= (1 << 4);  // Enable Battery Remove IRQ
    irq_en_1 |= (1 << 3);  // Enable POWERON Short PRESS IRQ
    irq_en_1 |= (1 << 2);  // Enable POWERON Long PRESS IRQ
    WriteReg(0x41, irq_en_1);

    uint8_t irq_en_2 = 0;
    irq_en_2 |= (1 << 6);  // Enable LDO Over Current IRQ
    irq_en_2 |= (1 << 4);  // Enable Battery charge done IRQ
    irq_en_2 |= (1 << 3);  // Enable Charger start IRQ
    irq_en_2 |= (1 << 2);  // Enable DIE Over Temperature level1 IRQ
    irq_en_2 |= (1 << 1);  // Enable Charger Safety Timer1/2 expire IRQ
    irq_en_2 |= (1 << 0);  // Enable Battery Over Voltage Protection IRQ
    WriteReg(0x42, irq_en_2);

    ESP_LOGI(TAG, "IRQ_EN_0: 0x%02X", ReadReg(0x40));
    ESP_LOGI(TAG, "IRQ_EN_1: 0x%02X", ReadReg(0x41));
    ESP_LOGI(TAG, "IRQ_EN_2: 0x%02X", ReadReg(0x42));

    // Clear irq status registers
    WriteReg(0x48, 0xFF);
    WriteReg(0x49, 0xFF);
    WriteReg(0x4A, 0xFF);
}

Pmic::~Pmic() {
    // Clean up IRQ resources if initialized
    if (event_group_handle_ != nullptr) {
        // Remove IRQ pin ISR handler
        gpio_isr_handler_remove(PMIC_IRQ_PIN);
        
        // Delete task if exists
        if (irq_task_handle_) {
            vTaskDelete(irq_task_handle_);
            irq_task_handle_ = nullptr;
        }
        
        // Delete event group
        vEventGroupDelete(event_group_handle_);
        event_group_handle_ = nullptr;
    }
}

void Pmic::EnablePdmPower() {
    uint8_t value = ReadReg(0x90); // XPOWERS_AXP2101_LDO_ONOFF_CTRL0
    value = value | 0x01; // set bit 1 (ALDO1)
    WriteReg(0x90, value);  // and power channels now enabled
}

void Pmic::DisablePdmPower() {
    uint8_t value = ReadReg(0x90); // XPOWERS_AXP2101_LDO_ONOFF_CTRL0
    value = value & ~0x01; // clear bit 1 (ALDO1)
    WriteReg(0x90, value);  // and power channels now enabled
}

void Pmic::EnableCellCharge() {
    uint8_t value = ReadReg(0x18); // CHARGER_CONTROL_1
    value |= 0x02; // set bit 1 to enable cell charge
    WriteReg(0x18, value);
}

void Pmic::DisableCellCharge() {
    uint8_t value = ReadReg(0x18); // CHARGER_CONTROL_1
    value &= ~0x02; // clear bit 1 to disable cell charge
    WriteReg(0x18, value);
}

void Pmic::EnableDisplayPower() {
    uint8_t value = ReadReg(0x90); // XPOWERS_AXP2101_LDO_ONOFF_CTRL0
    value = value | 0x04; // set bit 2 (ALDO3)
    WriteReg(0x90, value);
}

void Pmic::DisableDisplayPower() {
    uint8_t value = ReadReg(0x90); // XPOWERS_AXP2101_LDO_ONOFF_CTRL0
    value = value & ~0x04; // clear bit 2 (ALDO3)
    WriteReg(0x90, value);
}

bool Pmic::InitIrqPin() {
    // Configure IRQ pin as input with interrupt
    gpio_config_t irq_config = {};
    irq_config.pin_bit_mask = (1ULL << PMIC_IRQ_PIN);
    irq_config.mode = GPIO_MODE_INPUT;
    irq_config.pull_up_en = GPIO_PULLUP_DISABLE;
    irq_config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    irq_config.intr_type = GPIO_INTR_LOW_LEVEL;
    esp_err_t ret = gpio_config(&irq_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure IRQ pin: %s", esp_err_to_name(ret));
        return false;
    }

    // Enable GPIO wakeup
    ret = gpio_wakeup_enable(PMIC_IRQ_PIN, GPIO_INTR_LOW_LEVEL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable GPIO wakeup: %s", esp_err_to_name(ret));
        gpio_reset_pin(PMIC_IRQ_PIN);
        return false;
    }

    // Add ISR handler for IRQ pin
    ret = gpio_isr_handler_add(PMIC_IRQ_PIN, IrqPinIsrHandler, this);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add ISR handler: %s", esp_err_to_name(ret));
        gpio_wakeup_disable(PMIC_IRQ_PIN);
        gpio_reset_pin(PMIC_IRQ_PIN);
        return false;
    }

    // Create IRQ handling task
    BaseType_t task_ret = xTaskCreatePinnedToCore([](void* arg) {
        auto pmic = static_cast<Pmic*>(arg);
        pmic->IrqTask();
        vTaskDelete(NULL);
    }, "pmic_irq", 2048, this, 1, &irq_task_handle_, 1);
    
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create IRQ task");
        gpio_isr_handler_remove(PMIC_IRQ_PIN);
        gpio_wakeup_disable(PMIC_IRQ_PIN);
        gpio_reset_pin(PMIC_IRQ_PIN);
        return false;
    }

    ESP_LOGI(TAG, "IRQ pin initialized: GPIO%d", PMIC_IRQ_PIN);
    return true;
}

void Pmic::SetIrqCallback(PmicIrqCallback callback) {
    irq_callback_ = callback;
}

void Pmic::ClearIrqCallback() {
    irq_callback_ = nullptr;
}

void Pmic::IrqTask() {
    while (true) {
        // Wait for IRQ pin interrupt event
        auto bits = xEventGroupWaitBits(
            event_group_handle_,
            PMIC_EVENT_IRQ_PIN_INT,
            pdTRUE,  // Clear bits on exit
            pdFALSE,
            portMAX_DELAY
        );

        if (bits & PMIC_EVENT_IRQ_PIN_INT) {
            // Read IRQ status registers: 0x48, 0x49, 0x4A
            uint8_t irq_status_0x48 = ReadReg(0x48);
            uint8_t irq_status_0x49 = ReadReg(0x49);
            uint8_t irq_status_0x4A = ReadReg(0x4A);

            // Clear irq status registers by writing back the read values
            if (irq_status_0x48 != 0) {
                WriteReg(0x48, irq_status_0x48);
            }
            if (irq_status_0x49 != 0) {
                WriteReg(0x49, irq_status_0x49);
            }
            if (irq_status_0x4A != 0) {
                WriteReg(0x4A, irq_status_0x4A);
            }

            // Combine IRQ status registers into event mask
            // REG 48H (bits 0-7) -> events bits 0-7
            // REG 49H (bits 0-7) -> events bits 8-15
            // REG 4AH (bits 0-7) -> events bits 16-23
            PmicIrqEvents events = 0;
            
            // Map REG 48H bits to events (bits 0-7)
            if (irq_status_0x48 & (1 << 7)) events |= PMIC_IRQ_SOCWL2_IRQ;
            if (irq_status_0x48 & (1 << 6)) events |= PMIC_IRQ_SOCWL1_IRQ;
            if (irq_status_0x48 & (1 << 5)) events |= PMIC_IRQ_GWDT_IRQ;
            if (irq_status_0x48 & (1 << 4)) events |= PMIC_IRQ_LOWSOC_IRQ;
            if (irq_status_0x48 & (1 << 3)) events |= PMIC_IRQ_BCOT_IRQ;
            if (irq_status_0x48 & (1 << 2)) events |= PMIC_IRQ_BCUT_IRQ;
            if (irq_status_0x48 & (1 << 1)) events |= PMIC_IRQ_BWOT_IRQ;
            if (irq_status_0x48 & (1 << 0)) events |= PMIC_IRQ_BWUT_IRQ;
            
            // Map REG 49H bits to events (bits 8-15)
            if (irq_status_0x49 & (1 << 7)) events |= PMIC_IRQ_VINSERT_IRQ;
            if (irq_status_0x49 & (1 << 6)) events |= PMIC_IRQ_VREMOVE_IRQ;
            if (irq_status_0x49 & (1 << 5)) events |= PMIC_IRQ_BINSERT_IRQ;
            if (irq_status_0x49 & (1 << 4)) events |= PMIC_IRQ_BREMOVE_IRQ;
            if (irq_status_0x49 & (1 << 3)) events |= PMIC_IRQ_PONSP_IRQ;
            if (irq_status_0x49 & (1 << 2)) events |= PMIC_IRQ_PONLP_IRQ;
            if (irq_status_0x49 & (1 << 1)) events |= PMIC_IRQ_PONNE_IRQ;
            if (irq_status_0x49 & (1 << 0)) events |= PMIC_IRQ_PONPE_IRQ;
            
            // Map REG 4AH bits to events (bits 16-23)
            if (irq_status_0x4A & (1 << 7)) events |= PMIC_IRQ_WDEXP_IRQ;
            if (irq_status_0x4A & (1 << 6)) events |= PMIC_IRQ_LDOOC_IRQ;
            // Bit 5 is reserved, skip
            if (irq_status_0x4A & (1 << 4)) events |= PMIC_IRQ_CHGDN_IRQ;
            if (irq_status_0x4A & (1 << 3)) events |= PMIC_IRQ_CHGST_IRQ;
            if (irq_status_0x4A & (1 << 2)) events |= PMIC_IRQ_DOTL1_IRQ;
            if (irq_status_0x4A & (1 << 1)) events |= PMIC_IRQ_CHGTE_IRQ;
            if (irq_status_0x4A & (1 << 0)) events |= PMIC_IRQ_BOVP_IRQ;

            // FIXME: **This is a temporary solution to disable frequently charging start IRQ**
            // Handle charge start IRQ: disable the IRQ when received
            if (irq_status_0x4A & (1 << 3)) {
                // Disable charger start IRQ by clearing bit 3 in IRQ_EN_2 register (0x42)
                uint8_t irq_en_2 = ReadReg(0x42);
                irq_en_2 &= ~(1 << 3);  // Clear bit 3 to disable charger start IRQ
                WriteReg(0x42, irq_en_2);
                ESP_LOGI(TAG, "Charger start IRQ received, disabled charger start IRQ. IRQ_EN_2: 0x%02X", irq_en_2);
            }

            // Handle VBUS remove IRQ: re-enable charger start IRQ when received
            if (irq_status_0x49 & (1 << 6)) {
                // Re-enable charger start IRQ by setting bit 3 in IRQ_EN_2 register (0x42)
                uint8_t irq_en_2 = ReadReg(0x42);
                irq_en_2 |= (1 << 3);  // Set bit 3 to enable charger start IRQ
                WriteReg(0x42, irq_en_2);
                ESP_LOGI(TAG, "VBUS remove IRQ received, re-enabled charger start IRQ. IRQ_EN_2: 0x%02X", irq_en_2);
            }

            if (events > 0) {
                // Print IRQ status to console
                ESP_LOGI(TAG, "PMIC IRQ triggered - Status registers:");
                ESP_LOGI(TAG, "  0x48: 0x%02X", irq_status_0x48);
                ESP_LOGI(TAG, "  0x49: 0x%02X", irq_status_0x49);
                ESP_LOGI(TAG, "  0x4A: 0x%02X", irq_status_0x4A);
            }

            // Call callback function if registered
            if (irq_callback_ && events != 0) {
                irq_callback_(events);
            }
            
            gpio_intr_enable(PMIC_IRQ_PIN);
        }
    }
}

void IRAM_ATTR Pmic::IrqPinIsrHandler(void* arg) {
    Pmic* pmic = static_cast<Pmic*>(arg);
    // Disable interrupt
    gpio_intr_disable(PMIC_IRQ_PIN);
    // Notify the task to handle the interrupt
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xEventGroupSetBitsFromISR(pmic->event_group_handle_, PMIC_EVENT_IRQ_PIN_INT, &xHigherPriorityTaskWoken);
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}
