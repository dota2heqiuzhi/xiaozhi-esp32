#pragma once
#include <vector>
#include <functional>

#include <esp_timer.h>
#include <driver/gpio.h>
#include "adc_battery_estimation.h"
#include "power_controller.h"
#include <driver/rtc_io.h>
#include <esp_sleep.h>

#define JIUCHUAN_ADC_UNIT (ADC_UNIT_1)
#define JIUCHUAN_ADC_BITWIDTH (ADC_BITWIDTH_12)
#define JIUCHUAN_ADC_ATTEN (ADC_ATTEN_DB_12)
#define JIUCHUAN_ADC_CHANNEL (ADC_CHANNEL_3)
#define JIUCHUAN_RESISTOR_UPPER (200000)
#define JIUCHUAN_RESISTOR_LOWER (100000)

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
    adc_oneshot_unit_handle_t external_adc_handle_ = NULL;  // 外部共享 ADC handle
    adc_cali_handle_t external_adc_cali_handle_ = NULL;    // 外部校准 handle
    
    

    void CheckBatteryStatus() {
        // Get charging status
        bool new_charging_status = gpio_get_level(charging_pin_) == 1;
        if (new_charging_status != is_charging_) {
            is_charging_ = new_charging_status;
            if (on_charging_status_changed_) {
                on_charging_status_changed_(is_charging_);
            }
            ReadBatteryAdcData();
            return;
        }

        // 如果电池电量数据不足，则读取电池电量数据
        if (adc_values_.size() < kBatteryAdcDataCount) {
            ReadBatteryAdcData();
            return;
        }

        // 如果电池电量数据充足，则每 kBatteryAdcInterval 个 tick 读取一次电池电量数据
        ticks_++;
        if (ticks_ % kBatteryAdcInterval == 0) {
            ReadBatteryAdcData();
        }
    }

    void ReadBatteryAdcData() {
        float battery_capacity_temp = 0;
        adc_battery_estimation_get_capacity(adc_battery_estimation_handle, &battery_capacity_temp);
        ESP_LOGI("PowerManager", "Battery level: %.1f%%", battery_capacity_temp);
        if(battery_capacity_temp > -10 && battery_capacity_temp <= 0){
            battery_level_ = 0;
        }else{
            battery_level_ = battery_capacity_temp;
        }
    }

public:
    PowerManager(gpio_num_t pin, adc_oneshot_unit_handle_t shared_adc = NULL, adc_cali_handle_t shared_cali = NULL) 
        : charging_pin_(pin), external_adc_handle_(shared_adc), external_adc_cali_handle_(shared_cali) {
        power_controller_ = &PowerController::Instance();
        // 初始化充电完成 DONE 检测引脚 (GPIO16)。
        // 配置参数严格对齐 vendor 闭源固件实测 boot log：
        //   "I (266) gpio: GPIO[16]| InputEn:1| OutputEn:0| Pullup:1| Pulldown:0| Intr:2"
        //   "I (276) PowerManager: 电池充满检测引脚 GPIO16 初始化完成
        //                          (当前状态: 未充满, 下降沿中断)"
        // ⚠️ 严禁把这里改成传 GPIO4 (电池 ADC 引脚)，否则会高阻化电池检测线
        //    → 破坏 PMIC 自保持 → 松手即掉电（曾在此踩坑数小时）。
        gpio_config_t io_conf = {};
        io_conf.intr_type = GPIO_INTR_NEGEDGE;       // vendor: Intr=2 = 下降沿
        io_conf.mode = GPIO_MODE_INPUT;
        io_conf.pin_bit_mask = (1ULL << charging_pin_);
        io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io_conf.pull_up_en = GPIO_PULLUP_ENABLE;     // vendor: Pullup=1
        gpio_config(&io_conf);

        // 创建电池电量检查定时器
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

        static const battery_point_t battery_ponint_table[]={
            { 4.2 ,  100},
            { 4.06 ,  80},
            { 3.82 ,  60},
            { 3.58 ,  40},
            { 3.34 ,  20},
            { 3.1 ,  0},
            { 3.0 ,  -10}
        };

        adc_battery_estimation_t config = {};
        if (external_adc_handle_) {
            // 使用外部共享的 ADC handle
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
        // 如果电量已经满了，则不再显示充电中
        if (battery_level_ == 100) {
            //ESP_LOGI(TAG, "电量已满，不再显示充电中");
            return false;
        }
        return is_charging_;
    }

    bool IsDischarging() {
        // 没有区分充电和放电，所以直接返回相反状态
        return !is_charging_;
    }

    int32_t GetBatteryLevel() {
        return battery_level_;
    }

    void RegisterAllCallbacks() {
        //注册电源状态变更回调函数（优化版）
        power_controller_->OnStateChange([this](PowerState newState) {
            switch(newState) {
                case PowerState::SHUTDOWN: {

                    ESP_LOGD(TAG, "关机");

                    // ⚠️ 博亿朗二代 GPIO3 是 ADC 按键（不按 ~185mV=LOW，按下 ~3200mV=HIGH）
                    //    不是标准数字 GPIO 按键！极性和大多数板子相反。
                    //
                    // 错误版本（会导致"松开立即重启、无限循环"）：
                    //   esp_sleep_enable_ext0_wakeup(PWR_BUTTON_GPIO, 0);  // 低电平唤醒
                    //   rtc_gpio_pulldown_en(PWR_BUTTON_GPIO);             // 启用内部下拉
                    // 失败原因：ADC 按键不按时 GPIO 电平就是 LOW，再叠加内部下拉，
                    //          deep sleep 的瞬间立即满足"低电平唤醒"条件 → 无限重启。
                    //
                    // 正确版本（学卖家金标准固件）：高电平唤醒 + 禁用所有内部上下拉
                    //   - 用户再次按下电源键 → GPIO3 升到 ~3200mV=HIGH → 满足唤醒条件
                    //   - 禁用内部上下拉：ADC 按键外部已有偏置电路，内部拉会和它打架
                    //   - 参见 diff-analysis.md "按键相关差异"一节（卖家用 pulldown_dis）
                    //
                    // 调用约定：本分支只能在用户已经松开电源键（GPIO3=LOW）后被触发，
                    // 由 boilon_v2_board.cc 的 shutdown_pending_ 两阶段确认机制保证。
                    // 如果在 GPIO3 仍为 HIGH 时进入 deep sleep，会被 ext0_wakeup 立刻自唤醒
                    // 表现为"长按变重启"的死循环。这里再加一道防线：等 GPIO3 稳定到 LOW 才 sleep。
                    const int kMaxWaitReleaseMs = 200;
                    int waited_ms = 0;
                    while (gpio_get_level(PWR_BUTTON_GPIO) == 1 && waited_ms < kMaxWaitReleaseMs) {
                        vTaskDelay(10 / portTICK_PERIOD_MS);
                        waited_ms += 10;
                    }
                    if (waited_ms > 0) {
                        ESP_LOGW(TAG, "PWR_BUTTON still HIGH; waited %d ms before deep sleep", waited_ms);
                    }

                    ESP_ERROR_CHECK(esp_sleep_enable_ext0_wakeup(PWR_BUTTON_GPIO, 1));  // 高电平唤醒
                    ESP_ERROR_CHECK(rtc_gpio_pulldown_dis(PWR_BUTTON_GPIO));            // 禁用下拉（学卖家）
                    ESP_ERROR_CHECK(rtc_gpio_pullup_dis(PWR_BUTTON_GPIO));              // 禁用上拉
                    /* 关闭电源使能 */
                    power_controller_->PowerOff();
                    
                    // 确保所有外设已关闭
                    vTaskDelay(200 / portTICK_PERIOD_MS);
                    ESP_LOGI(TAG, "Initiating deep sleep");

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