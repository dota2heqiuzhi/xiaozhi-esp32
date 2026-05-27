#pragma once
#include <mutex>
#include <functional>
#include <driver/gpio.h>
#include <driver/rtc_io.h>
#include <esp_log.h>
#include "config.h"
enum class PowerState {
        ACTIVE,
        LIGHT_SLEEP, 
        DEEP_SLEEP,
        SHUTDOWN
    };
    
class PowerController {
public:
    

    static PowerController& Instance() {
        static PowerController instance;
        return instance;
    }

    void SetState(PowerState newState) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (currentState_ != newState) {
            ESP_LOGI("PowerCtrl", "State change: %d -> %d", 
                    static_cast<int>(currentState_), 
                    static_cast<int>(newState));
            
            currentState_ = newState;
            if (stateChangeCallback_) {
                stateChangeCallback_(newState);
            }
        }
    }

    PowerState GetState() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return currentState_;
    }

    void OnStateChange(std::function<void(PowerState)> callback) {
        stateChangeCallback_ = callback;
    }

    void LatchPowerOn() {
        // 闭源金标准固件 (boilon_v2_pink_827998.bin) 字符串扫描结论：
        //   * GPIO5 是 VBUS ADC 输入 (ADC1_CH4)，不是 PWR_EN 输出
        //   * PowerManager::InitializePowerControl() 不调用任何 gpio_set_level / rtc_gpio_set_level
        //   * 全固件没有 PWR_EN / latch / Self_Hold 字样
        // 结论：电源由主板硬件 PMIC + 长按 RC 自锁电路独立维持，固件不参与电源使能。
        // 早期"PWR_EN 拉高/拉低 + hold_en"诊断版反而把 GPIO5 驱动到 PMIC 的 VBUS 检测线上，
        // 干扰主板原本的电源管理逻辑，导致松手立刻断电。
        //
        // 因此本函数只做一件事：清理可能由历史诊断版烧录残留下来的 RTC GPIO5 hold/output
        // 污染状态，把 GPIO5 完全还回普通 ADC 输入。绝不主动驱动 GPIO5。
        rtc_gpio_hold_dis(PWR_VBUS_ADC_GPIO);
        gpio_hold_dis(PWR_VBUS_ADC_GPIO);
        rtc_gpio_deinit(PWR_VBUS_ADC_GPIO);
        ESP_LOGI("PowerCtrl",
                 "GPIO%d returned to default ADC input; no PWR_EN driven (matches vendor firmware)",
                 PWR_VBUS_ADC_GPIO);
    }

    void PowerOff() {
        // 与闭源固件 PowerManager::HandleShutdown 等价：不操作任何 PWR_EN GPIO。
        // 真正的"关机"由 PowerManager 走 panel sleep + esp_deep_sleep_start 实现，
        // 唤醒源 = GPIO3 高电平 ext0_wakeup。
        ESP_LOGI("PowerCtrl", "PowerOff() no-op; deep sleep handled by PowerManager");
    }

private:
    PowerController(){
        LatchPowerOn();
    }
    ~PowerController() = default;

    PowerState currentState_ = PowerState::ACTIVE;
    std::function<void(PowerState)> stateChangeCallback_;
    mutable std::mutex mutex_;
};