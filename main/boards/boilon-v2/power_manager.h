#pragma once
#include <vector>
#include <functional>

#include <esp_timer.h>
#include <driver/gpio.h>
#include <driver/rtc_io.h>
#include <esp_sleep.h>

#include "adc_battery_estimation.h"
#include "power_controller.h"
#include "config.h"

// 电池 ADC 配置（GPIO4 = ADC1_CH3，串联 200kΩ + 100kΩ 分压）
#define JIUCHUAN_ADC_UNIT (ADC_UNIT_1)
#define JIUCHUAN_ADC_BITWIDTH (ADC_BITWIDTH_12)
#define JIUCHUAN_ADC_ATTEN (ADC_ATTEN_DB_12)
#define JIUCHUAN_ADC_CHANNEL (ADC_CHANNEL_3)        // GPIO4 = 电池 ADC
#define JIUCHUAN_VBUS_ADC_CHANNEL (ADC_CHANNEL_4)   // GPIO5 = VBUS ADC（vendor parity）
#define JIUCHUAN_RESISTOR_UPPER (200000)
#define JIUCHUAN_RESISTOR_LOWER (100000)

// VBUS ADC 充电检测阈值：mV 大于该值视为"USB 在供电中"。
// USB 5V 经分压（与电池同 200k/100k 假设 ⇒ 5V × 100/(200+100) ≈ 1.67V at ADC pin）。
// 不接 USB 时 GPIO5 ≈ 0V。1000mV 阈值同时容忍分压比未知 + 噪声，保留约 670mV 余量。
#define JIUCHUAN_VBUS_CHARGING_THRESHOLD_MV (1000)

#undef TAG
#define TAG "PowerManager"

class PowerManager {
private:
    esp_timer_handle_t timer_handle_;
    std::function<void(bool)> on_charging_status_changed_;
    std::function<void(bool)> on_low_battery_status_changed_;
    gpio_num_t charging_pin_ = GPIO_NUM_NC;
    std::vector<uint16_t> adc_values_;
    int32_t battery_level_ = 100;
    bool is_charging_ = false;
    bool is_low_battery_ = false;
    bool is_empty_battery_ = false;
    int ticks_ = 0;
    const int kBatteryAdcInterval = 60;
    const int kBatteryAdcDataCount = 3;
    const int kLowBatteryLevel = 20;

    adc_battery_estimation_handle_t adc_battery_estimation_handle;
    PowerController* power_controller_;
    adc_oneshot_unit_handle_t external_adc_handle_ = NULL;
    adc_cali_handle_t external_adc_cali_handle_ = NULL;

    void CheckBatteryStatus() {
        // 充电检测：用 GPIO5 (VBUS) ADC 电压判断"正在充电"，对齐 vendor 行为。
        //   GPIO16 是 CHARGE_DONE 引脚（"未充满"=HIGH, "充满"=LOW + NEGEDGE）—
        //   把它当 "正在充电" 是错的，曾导致不接 USB 也显示充电中图标。
        int vbus_mv = ReadVbusVoltageMv();
        bool new_charging_status = (vbus_mv > JIUCHUAN_VBUS_CHARGING_THRESHOLD_MV);
        if (new_charging_status != is_charging_) {
            ESP_LOGI(TAG, "Charging state changed: %s (VBUS=%dmV)",
                     new_charging_status ? "charging" : "not charging", vbus_mv);
            is_charging_ = new_charging_status;
            if (on_charging_status_changed_) {
                on_charging_status_changed_(is_charging_);
            }
            ReadBatteryAdcData();
            return;
        }

        if (adc_values_.size() < kBatteryAdcDataCount) {
            ReadBatteryAdcData();
            return;
        }

        ticks_++;
        if (ticks_ % kBatteryAdcInterval == 0) {
            ReadBatteryAdcData();
        }
    }

    // 读 GPIO5 (VBUS) ADC 电压，返回 mV；未配置或读失败时返回 0（按"无 USB"处理）。
    int ReadVbusVoltageMv() {
        if (!external_adc_handle_ || !external_adc_cali_handle_) {
            return 0;
        }
        int raw = 0;
        esp_err_t err = adc_oneshot_read(external_adc_handle_,
                                         JIUCHUAN_VBUS_ADC_CHANNEL, &raw);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "VBUS ADC read failed: %s", esp_err_to_name(err));
            return 0;
        }
        int mv = 0;
        err = adc_cali_raw_to_voltage(external_adc_cali_handle_, raw, &mv);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "VBUS ADC cali failed: %s", esp_err_to_name(err));
            return 0;
        }
        return mv;
    }

    void ReadBatteryAdcData() {
        float battery_capacity_temp = 0;
        adc_battery_estimation_get_capacity(adc_battery_estimation_handle, &battery_capacity_temp);
        ESP_LOGI(TAG, "Battery: %.1f%%, charging=%d",
                 battery_capacity_temp, is_charging_ ? 1 : 0);
        if (battery_capacity_temp > -10 && battery_capacity_temp <= 0) {
            battery_level_ = 0;
        } else {
            battery_level_ = battery_capacity_temp;
        }
    }

public:
    PowerManager(gpio_num_t pin, adc_oneshot_unit_handle_t shared_adc = NULL,
                 adc_cali_handle_t shared_cali = NULL)
        : charging_pin_(pin), external_adc_handle_(shared_adc),
          external_adc_cali_handle_(shared_cali) {
        // PowerController 单例的构造函数会调 LatchPowerOn() 主动驱动 GPIO15 HIGH，
        // 这是"松手即掉电"BUG 的真正修复（详见 power_controller.h）。
        power_controller_ = &PowerController::Instance();

        // 配置 charging DONE 引脚（GPIO16）：与 vendor boot log 一致的参数。
        //   "I (266) gpio: GPIO[16]| InputEn:1| OutputEn:0| Pullup:1| Pulldown:0| Intr:2"
        //   "I (276) PowerManager: 电池充满检测引脚 GPIO16 初始化完成
        //                          (当前状态: 未充满, 下降沿中断)"
        //
        // 注意：vendor 同时装了 ISR (gpio_isr_handler_add(battery_full_pin_, DonePinIsrHandler, this))，
        // 我们目前没装 ISR，所以把 intr_type 设为 DISABLE 而不是 NEGEDGE，避免硬件
        // 中断触发到空 vector。如果未来需要"充满后自动停止充电图标"功能，可以
        // 加 ISR + intr=NEGEDGE。
        //
        // ⚠️ 严禁把 charging_pin 改成 GPIO4（电池 ADC），曾因此踩坑数小时。
        gpio_config_t io_conf = {};
        io_conf.intr_type = GPIO_INTR_DISABLE;
        io_conf.mode = GPIO_MODE_INPUT;
        io_conf.pin_bit_mask = (1ULL << charging_pin_);
        io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io_conf.pull_up_en = GPIO_PULLUP_ENABLE;     // vendor: Pullup=1
        gpio_config(&io_conf);

        // 电池电量检查定时器（1Hz）
        esp_timer_create_args_t timer_args = {
            .callback = [](void* arg) {
                PowerManager* self = static_cast<PowerManager*>(arg);
                self->CheckBatteryStatus();
            },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "battery_check_timer",
            .skip_unhandled_events = true,
        };
        ESP_ERROR_CHECK(esp_timer_create(&timer_args, &timer_handle_));
        ESP_ERROR_CHECK(esp_timer_start_periodic(timer_handle_, 1000000));

        // 电池电量分段表（线性插值）
        static const battery_point_t battery_ponint_table[] = {
            { 4.2,  100 },
            { 4.06,  80 },
            { 3.82,  60 },
            { 3.58,  40 },
            { 3.34,  20 },
            { 3.1,    0 },
            { 3.0,  -10 }
        };

        adc_battery_estimation_t config = {};
        if (external_adc_handle_) {
            config.external.adc_handle = external_adc_handle_;
            config.external.adc_cali_handle = external_adc_cali_handle_;
        } else {
            config.internal.adc_unit = JIUCHUAN_ADC_UNIT;
            config.internal.adc_bitwidth = JIUCHUAN_ADC_BITWIDTH;
            config.internal.adc_atten = JIUCHUAN_ADC_ATTEN;
        }
        config.adc_channel = JIUCHUAN_ADC_CHANNEL;
        config.upper_resistor = JIUCHUAN_RESISTOR_UPPER;
        config.lower_resistor = JIUCHUAN_RESISTOR_LOWER;
        config.battery_points = battery_ponint_table;
        config.battery_points_count = sizeof(battery_ponint_table) / sizeof(battery_ponint_table[0]);

        adc_battery_estimation_handle = adc_battery_estimation_create(&config);

        RegisterAllCallbacks();
    }

    ~PowerManager() {
        if (timer_handle_) {
            esp_timer_stop(timer_handle_);
            esp_timer_delete(timer_handle_);
        }
        if (adc_battery_estimation_handle) {
            adc_battery_estimation_destroy(adc_battery_estimation_handle);
        }
    }

    bool IsCharging() {
        // 满电后不再显示"充电中"图标
        if (battery_level_ == 100) {
            return false;
        }
        return is_charging_;
    }

    bool IsDischarging() {
        return !is_charging_;
    }

    int32_t GetBatteryLevel() {
        return battery_level_;
    }

    void RegisterAllCallbacks() {
        power_controller_->OnStateChange([this](PowerState newState) {
            switch (newState) {
                case PowerState::SHUTDOWN: {
                    ESP_LOGI(TAG, "Shutdown requested");

                    // 进 deep sleep 前必须等用户松开电源键；否则 GPIO3 仍 HIGH 时
                    // ext0_wakeup 会立刻自唤醒，表现为"长按变重启"。
                    int waited_ms = 0;
                    while (gpio_get_level(PWR_BUTTON_GPIO) == 1) {
                        if (waited_ms == 0) {
                            ESP_LOGW(TAG, "Waiting for power button release before deep sleep");
                        }
                        vTaskDelay(10 / portTICK_PERIOD_MS);
                        waited_ms += 10;
                    }
                    if (waited_ms > 0) {
                        ESP_LOGI(TAG, "Power button released after %d ms", waited_ms);
                    }

                    // GPIO3 配置为 ext0 高电平唤醒源（与 vendor 一致）
                    ESP_ERROR_CHECK(rtc_gpio_init(PWR_BUTTON_GPIO));
                    ESP_ERROR_CHECK(rtc_gpio_set_direction(PWR_BUTTON_GPIO, RTC_GPIO_MODE_INPUT_ONLY));
                    ESP_ERROR_CHECK(rtc_gpio_pulldown_dis(PWR_BUTTON_GPIO));
                    ESP_ERROR_CHECK(rtc_gpio_pullup_dis(PWR_BUTTON_GPIO));
                    ESP_ERROR_CHECK(esp_sleep_enable_ext0_wakeup(PWR_BUTTON_GPIO, 1));

                    // 关电源 latch（PWR_EN GPIO15 → LOW），整板进入低功耗
                    power_controller_->PowerOff();

                    vTaskDelay(200 / portTICK_PERIOD_MS);
                    ESP_LOGI(TAG, "Entering deep sleep");
                    esp_deep_sleep_start();
                    break;
                }
                default:
                    ESP_LOGD(TAG, "State changed to %d", static_cast<int>(newState));
                    break;
            }
        });
    }

    void SetPowerState(PowerState newState) {
        power_controller_->SetState(newState);
    }

    void OnLowBatteryStatusChanged(std::function<void(bool)> callback) {
        on_low_battery_status_changed_ = callback;
    }

    void OnChargingStatusChanged(std::function<void(bool)> callback) {
        on_charging_status_changed_ = callback;
    }
};
