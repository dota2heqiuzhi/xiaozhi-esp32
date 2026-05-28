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
        // 博亿朗 V2 主板的电源自保持是纯硬件 PMIC + RC 电路完成的，固件不需要
        // 主动驱动任何 GPIO 来维持电源（与 vendor 闭源固件运行行为一致）。
        // 真正会破坏 PMIC 自保持环路的是 GPIO4（电池 ADC）的 GPIO 配置 ——
        // 见 power_manager.h 顶部注释和 firmware/docs/hardware.md 中
        // "2026-05-28 松手即掉电" 一节。
    }

    void PowerOff() {
        // "关机"由 PowerManager 走 panel sleep + esp_deep_sleep_start 实现，
        // 唤醒源 = GPIO3 高电平 ext0_wakeup。这里不需要做任何事。
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