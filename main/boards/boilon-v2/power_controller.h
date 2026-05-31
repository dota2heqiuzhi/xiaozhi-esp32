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

    // ============================================================
    // 硬件电源 latch（"松手即掉电" BUG 真正修复，2026-05-31）
    //
    // 博亿朗二代主板的电源自保持机制：电源键（GPIO3）按下时硬件 latch
    // 给 PMIC EN 提供瞬时支撑；松手后 GPIO3 LOW 失去外部支撑，必须由
    // 固件主动驱动 PWR_EN_GPIO（GPIO15）HIGH 接管 EN 信号，否则整板掉电。
    //
    // 反编译铁证：
    //   vendor binary 的 PowerManager::InitializePowerControl() 在最开头做：
    //     rtc_gpio_init(GPIO_NUM_15);
    //     rtc_gpio_set_direction(GPIO_NUM_15, RTC_GPIO_MODE_OUTPUT_ONLY);
    //     rtc_gpio_hold_dis(GPIO_NUM_15);
    //     rtc_gpio_set_level(GPIO_NUM_15, 1);
    //   见 firmware/reverse/disasm/vendor_pwrmgr_initialize_pwrctl_4202c23e.txt
    //   反汇编 0x4202c1dc 至 0x4202c206。
    //
    // ⚠️ 严禁简化或删除这 4 行。曾经因为错误假设"V2 是纯硬件 PMIC"把
    // LatchPowerOn() 改成空函数，导致松手后 < 50ms 整板掉电（PROBE 实测）。
    // ============================================================
    void LatchPowerOn() {
        ESP_ERROR_CHECK(rtc_gpio_init(PWR_EN_GPIO));
        ESP_ERROR_CHECK(rtc_gpio_set_direction(PWR_EN_GPIO, RTC_GPIO_MODE_OUTPUT_ONLY));
        ESP_ERROR_CHECK(rtc_gpio_hold_dis(PWR_EN_GPIO));
        ESP_ERROR_CHECK(rtc_gpio_set_level(PWR_EN_GPIO, 1));
        ESP_LOGI("PowerCtrl", "GPIO%d driven HIGH (hardware power latch)", PWR_EN_GPIO);
    }

    // 关机：把 PWR_EN 拉低，硬件 PMIC 失去 EN 信号 -> 整板断电。
    // 需要先解 hold（rtc_gpio_hold_en 之后才能改 level）。
    void PowerOff() {
        rtc_gpio_hold_dis(PWR_EN_GPIO);
        rtc_gpio_set_level(PWR_EN_GPIO, 0);
        ESP_LOGI("PowerCtrl", "GPIO%d driven LOW (power off)", PWR_EN_GPIO);
    }

private:
    PowerController() {
        LatchPowerOn();
    }
    ~PowerController() = default;

    PowerState currentState_ = PowerState::ACTIVE;
    std::function<void(PowerState)> stateChangeCallback_;
    mutable std::mutex mutex_;
};
