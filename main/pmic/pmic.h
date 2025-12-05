#ifndef __PMIC_H__
#define __PMIC_H__

#include "axp2101.h"
#include <driver/i2c_master.h>
#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>
#include <functional>
#include <cstdint>

// PMIC 配置参数
#define AXP2101_I2C_ADDR    0x34          // AXP2101 I2C 地址
#define PMIC_I2C_SDA_PIN    GPIO_NUM_42   // PMIC I2C SDA 引脚
#define PMIC_I2C_SCL_PIN    GPIO_NUM_41   // PMIC I2C SCL 引脚
#define PMIC_IRQ_PIN        GPIO_NUM_4    // PMIC IRQ 引脚

// PMIC IRQ 事件位定义
#define PMIC_EVENT_IRQ_PIN_INT    BIT0    // IRQ pin interrupt event

// REG 48H: IRQ Status 0 事件定义
#define PMIC_IRQ_SOCWL2_IRQ       (1ULL << 7)   // Bit 7: SOC drop to Warning Level IRQ
#define PMIC_IRQ_SOCWL1_IRQ       (1ULL << 6)   // Bit 6: SOC drop to Shutdown Level IRQ
#define PMIC_IRQ_GWDT_IRQ         (1ULL << 5)   // Bit 5: Gauge Watchdog Timeout IRQ
#define PMIC_IRQ_LOWSOC_IRQ       (1ULL << 4)   // Bit 4: Gauge New SOC IRQ
#define PMIC_IRQ_BCOT_IRQ         (1ULL << 3)   // Bit 3: Battery Over Temperature in Charge mode IRQ
#define PMIC_IRQ_BCUT_IRQ         (1ULL << 2)   // Bit 2: Battery Under Temperature in Charge mode IRQ
#define PMIC_IRQ_BWOT_IRQ         (1ULL << 1)   // Bit 1: Battery Over Temperature in Work mode IRQ
#define PMIC_IRQ_BWUT_IRQ         (1ULL << 0)   // Bit 0: Battery Under Temperature in Work mode IRQ

// REG 49H: IRQ Status 1 事件定义
#define PMIC_IRQ_VINSERT_IRQ      (1ULL << 15)  // Bit 7: VBUS Insert IRQ
#define PMIC_IRQ_VREMOVE_IRQ      (1ULL << 14)  // Bit 6: VBUS Remove IRQ
#define PMIC_IRQ_BINSERT_IRQ      (1ULL << 13)  // Bit 5: Battery Insert IRQ
#define PMIC_IRQ_BREMOVE_IRQ      (1ULL << 12)  // Bit 4: Battery Remove IRQ
#define PMIC_IRQ_PONSP_IRQ        (1ULL << 11)  // Bit 3: POWERON Short PRESS IRQ
#define PMIC_IRQ_PONLP_IRQ        (1ULL << 10)  // Bit 2: POWERON Long PRESS IRQ
#define PMIC_IRQ_PONNE_IRQ        (1ULL << 9)   // Bit 1: POWERON Negative Edge IRQ
#define PMIC_IRQ_PONPE_IRQ        (1ULL << 8)   // Bit 0: POWERON Positive Edge IRQ

// REG 4AH: IRQ Status 2 事件定义
#define PMIC_IRQ_WDEXP_IRQ        (1ULL << 23)  // Bit 7: Watchdog Expire IRQ
#define PMIC_IRQ_LDOOC_IRQ        (1ULL << 22)  // Bit 6: LDO Over Current IRQ
// Bit 5: Reserved (no function)
#define PMIC_IRQ_CHGDN_IRQ        (1ULL << 20)  // Bit 4: Battery charge done IRQ
#define PMIC_IRQ_CHGST_IRQ        (1ULL << 19)  // Bit 3: Charger start IRQ
#define PMIC_IRQ_DOTL1_IRQ        (1ULL << 18)  // Bit 2: DIE Over Temperature level1 IRQ
#define PMIC_IRQ_CHGTE_IRQ        (1ULL << 17)  // Bit 1: Charger Safety Timer1/2 expire IRQ
#define PMIC_IRQ_BOVP_IRQ         (1ULL << 16)  // Bit 0: Battery Over Voltage Protection IRQ

// IRQ事件掩码类型
using PmicIrqEvents = uint64_t;

// IRQ回调函数类型定义
// 参数 events: 发生的事件位掩码，可以使用上述PMIC_IRQ_*常量进行位运算检查
using PmicIrqCallback = std::function<void(PmicIrqEvents events)>;

// PMIC 类实现（单例模式）
class Pmic : public Axp2101 {
public:
    /**
     * 获取单例实例
     * @return PMIC单例实例
     */
    static Pmic& GetInstance();

    /**
     * 禁用拷贝构造和赋值操作
     */
    Pmic(const Pmic&) = delete;
    Pmic& operator=(const Pmic&) = delete;

    void EnablePdmPower();
    void DisablePdmPower();
    void EnableCellCharge();
    void DisableCellCharge();
    void EnableDisplayPower();
    void DisableDisplayPower();

    /**
     * 初始化 IRQ 引脚和中断处理
     * @return true表示成功，false表示失败
     */
    bool InitIrqPin();

    /**
     * 设置IRQ事件回调函数
     * @param callback 回调函数，当IRQ事件发生时会被调用
     *                  参数 events 表示发生的事件位掩码
     */
    void SetIrqCallback(PmicIrqCallback callback);

    /**
     * 清除IRQ事件回调函数
     */
    void ClearIrqCallback();

    /**
     * 析构函数
     */
    ~Pmic();

private:
    /**
     * 私有构造函数（单例模式）
     */
    Pmic(i2c_master_bus_handle_t i2c_bus, uint8_t addr);

    /**
     * 初始化I2C总线
     * @return true表示成功，false表示失败
     */
    static bool InitI2cBus();

    /**
     * IRQ 处理任务
     */
    void IrqTask();

    /**
     * IRQ 引脚 ISR 处理函数（静态，在 IRAM 中运行）
     */
    static void IRAM_ATTR IrqPinIsrHandler(void* arg);

    // 静态成员变量
    static Pmic* instance_;
    static i2c_master_bus_handle_t i2c_bus_;
    static bool initialized_;

    // IRQ 相关成员变量
    EventGroupHandle_t event_group_handle_;
    TaskHandle_t irq_task_handle_;
    PmicIrqCallback irq_callback_;
};

#endif
