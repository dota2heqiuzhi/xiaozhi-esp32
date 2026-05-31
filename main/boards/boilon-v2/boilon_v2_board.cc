/**
 * 博亿朗二代 (Boilon V2) 自定义板级代码
 *
 * 基于 jiuchuan-s3 修改，关键变更：
 * 1. LCD 驱动：GC9309NA → JD9853 (240x296)
 * 2. 音频：Es8311AudioCodec（与原版 jiuchuan-s3 一致）
 * 3. 按键配置保持与闭源固件一致
 * 4. PowerController override：PWR_EN_GPIO 改为 GPIO15（vendor parity）
 *    详见 power_controller.h::LatchPowerOn() 注释。
 */
#include "wifi_board.h"
#include "codecs/box_audio_codec.h"  // ES8311+ES7210 双芯片 TDM 模式
#include "display/lcd_display.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "i2c_device.h"

#include <esp_log.h>
#include <esp_lcd_panel_vendor.h>
#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include "led/single_led.h"
#include "assets/lang_config.h"
#include "esp_lcd_jd9853.h"  // JD9853 驱动

#include "power_save_timer.h"
#include "power_manager.h"
#include "boot_emoji.h"  // 开机表情 PNG 数据
#include "power_controller.h"
#include "gpio_manager.h"
#include <driver/rtc_io.h>
#include <esp_sleep.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>
#include <esp_heap_caps.h>


#define BOARD_TAG "BoilonV2Board"
// jiuchuan-s3 父类用 __USER_GPIO_PWRDOWN__ 区分"用 GPIO 控制电源" vs "纯 deep sleep"
// 关机路径。boilon-v2 走 GPIO 电源 latch（GPIO15），所以保留这个 macro。
#define __USER_GPIO_PWRDOWN__

// 自定义LCD显示器类（与闭源固件一致的布局）
class BoilonLcdDisplay : public SpiLcdDisplay
{
public:
    BoilonLcdDisplay(esp_lcd_panel_io_handle_t io_handle,
                     esp_lcd_panel_handle_t panel_handle,
                     int width, int height,
                     int offset_x, int offset_y,
                     bool mirror_x, bool mirror_y, bool swap_xy)
        : SpiLcdDisplay(io_handle, panel_handle, width, height,
                        offset_x, offset_y, mirror_x, mirror_y, swap_xy)
    {
    }

    virtual void SetupUI() override {
        SpiLcdDisplay::SetupUI();
        // 注意：不做圆形屏幕的偏移适配（博亿朗二代是方形屏）
    }
};

class BoilonV2Board : public WifiBoard {
private:
    i2c_master_bus_handle_t codec_i2c_bus_;
    adc_oneshot_unit_handle_t shared_adc_handle_ = NULL;  // ADC1 共享 handle
    adc_cali_handle_t shared_adc_cali_handle_ = NULL;     // ADC1 校准 handle
    Button boot_button_;
    Button* pwr_button_;  // GPIO Button (GPIO3, active_high)，在 InitializeButtons 中 new
    Button wifi_button;   // WIFI_BUTTON_GPIO = GPIO6（音量+ 也复用此键）
    Button cmd_button;    // CMD_BUTTON_GPIO = GPIO7（音量-）
    LcdDisplay* display_;
    PowerSaveTimer* power_save_timer_;
    PowerManager* power_manager_;
    esp_lcd_panel_io_handle_t panel_io = NULL;
    esp_lcd_panel_handle_t panel = NULL;

    // 音量映射函数：将内部音量(0-80)映射为显示音量(0-100%)
    int MapVolumeForDisplay(int internal_volume) {
        if (internal_volume < 0) internal_volume = 0;
        if (internal_volume > 80) internal_volume = 80;
        return (internal_volume * 100) / 80;
    }

    void InitializePowerManager() {
        // PowerManager 监听的 charging_pin 必须是 PWR_CHARGING_DONE_GPIO (GPIO16)，
        // 不能误传 PWR_BATTERY_ADC_GPIO (GPIO4)，否则会把 GPIO4 重新配置为高阻
        // 普通 INPUT，破坏电池 ADC 电路（曾踩坑数小时）。
        power_manager_ = new PowerManager(PWR_CHARGING_DONE_GPIO,
                                          shared_adc_handle_,
                                          shared_adc_cali_handle_);
        // 充电时禁用省电 timer，未充电时启用（vendor 行为）。
        power_manager_->OnChargingStatusChanged([this](bool is_charging) {
            ESP_LOGI(TAG, "Charging status changed: %s", is_charging ? "charging" : "not charging");
            power_save_timer_->SetEnabled(!is_charging);
        });
    }

    void InitializePowerSaveTimer() {
        // 5 分钟无操作触发 shutdown_request
        power_save_timer_ = new PowerSaveTimer(-1, (60 * 5), -1);
        power_save_timer_->OnEnterSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(true);
            GetBacklight()->SetBrightness(1);
        });
        power_save_timer_->OnExitSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(false);
            GetBacklight()->RestoreBrightness();
        });
        power_save_timer_->OnShutdownRequest([this]() {
            ESP_LOGI(TAG, "Power save timer expired - shutting down");
            power_manager_->SetPowerState(PowerState::SHUTDOWN);
        });
        power_save_timer_->SetEnabled(true);
    }

    void InitializeI2c() {
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = (i2c_port_t)1,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &codec_i2c_bus_));

        // I2C 扫描（启动时打印一次，便于检查音频芯片连接）
        int device_count = 0;
        for (uint8_t addr = 0x08; addr < 0x78; addr++) {
            esp_err_t ret = i2c_master_probe(codec_i2c_bus_, addr, 50);
            if (ret == ESP_OK) {
                ESP_LOGI(TAG, "I2C device found at 0x%02X", addr);
                device_count++;
            }
        }
        ESP_LOGI(TAG, "I2C scan complete: %d device(s) found", device_count);
    }

    void InitializeButtons() {
        // 行为对齐 vendor binary 的 JiuchuanDevBoard::InitializeButtons()。
        //
        // 关键设计点（务必保持，曾踩坑数次）：
        // 1. 先 gpio_get_level(GPIO3) 读电平，若用户按住开机就把
        //    pwrbutton_unreleased 置 true，避免"长按开机"被误判为"长按关机"。
        // 2. GPIO Button（不是 ADC Button）：ADC Button 把 GPIO3 配为高阻 ADC
        //    输入，会破坏主板电源自保持电路。
        // 3. Button 5 参数构造：enable_power_save=true，对齐 vendor binary 的
        //    Button::Button(gpio_num_t, bool, uint16_t, uint16_t, bool) 签名。
        // 4. 不注册 OnPressUp 回调：vendor binary 完全没有 BUTTON_PRESS_UP
        //    字符串，对齐 vendor parity（守则 1）。
        // 5. 关机走 OnLongPress + 5 次防抖确认 GPIO3 仍 HIGH。

        static bool pwrbutton_unreleased = false;
        if (gpio_get_level(GPIO_NUM_3) == 1) {
            pwrbutton_unreleased = true;
            ESP_LOGI(TAG, "Power button held HIGH at boot - long press shutdown disabled until first release");
        }

        // GPIO3 = 电源键，GPIO Button，按下=HIGH (active_high)，enable_power_save=true。
        // ⚠️ 不要自己调 gpio_install_isr_service(0)！iot_button 库内部用静态 flag
        //    自己装 ISR，外部已装会触发库的 fatal 错误（曾导致 reboot loop）。
        pwr_button_ = new Button(PWR_BUTTON_GPIO, true /* active_high */,
                                 0, 0, true /* enable_power_save - vendor parity */);

        // iot_button_new_gpio_device(active_high) 默认开内部下拉；vendor 实测：
        //   "I (866) gpio: GPIO[3]| InputEn:1| OutputEn:0| Pullup:0| Pulldown:0| Intr:0"
        // 所以创建后立刻 dis 上下拉对齐。
        ESP_ERROR_CHECK(gpio_pulldown_dis(PWR_BUTTON_GPIO));
        ESP_ERROR_CHECK(gpio_pullup_dis(PWR_BUTTON_GPIO));

        // BOOT 按键单击：唤醒省电定时器
        boot_button_.OnClick([this]() {
            ESP_LOGI(TAG, "Boot button clicked");
            power_save_timer_->WakeUp();
        });

        // 电源键按下：清除 pwrbutton_unreleased 标志（首次完整 down 边沿到来时）。
        pwr_button_->OnPressDown([this]() {
            pwrbutton_unreleased = false;
            Application::GetInstance().ReportClientEvent(
                "button", "press_down", "{\"btn\":\"power\"}");
        });

        // ⚠️ 不注册 OnPressUp 回调（vendor parity，守则 1）：
        // vendor binary 完全没有 BUTTON_PRESS_UP 相关字符串。

        // 电源键长按：5 次 100ms 防抖确认，确认后走 SHUTDOWN 路径。
        // 关机流程内部由 PowerManager 走 esp_deep_sleep_start()。
        pwr_button_->OnLongPress([this]() {
            auto& app = Application::GetInstance();
            ESP_LOGI(TAG, "Power button long press detected");
            if (pwrbutton_unreleased) {
                ESP_LOGI(TAG, "开机后电源键未松开，忽略长按关机");
                app.ReportClientEvent("button", "long_press_ignored_boot_guard",
                                      "{\"btn\":\"power\"}");
                return;
            }
            for (int i = 0; i < 5; i++) {
                if (gpio_get_level(PWR_BUTTON_GPIO) == 0) {
                    ESP_LOGW(TAG, "Power button released during confirmation - abort shutdown");
                    app.ReportClientEvent("button", "long_press_aborted",
                                          "{\"btn\":\"power\"}");
                    return;
                }
                vTaskDelay(100 / portTICK_PERIOD_MS);
            }
            ESP_LOGI(TAG, "Confirmed power button long-pressed - initiating shutdown");
            app.ReportClientEvent("button", "long_press_shutdown",
                                  "{\"btn\":\"power\"}");
            power_manager_->SetPowerState(PowerState::SHUTDOWN);
        });

        // 电源键单击：单轮对话入口（儿童使用场景）
        // 设计目标：点一下开始听一句，AI 回复完自动回 Idle；下次要说话再点一下。
        //
        // 特殊路径：如果当前正在显示笔顺 GIF（用户问"X字怎么写"后），按键
        // 的语义改为"退出 GIF 回到默认 emoji"，不进入新一轮对话。原因：
        //   1. GIF 是模态视图，符合"按键退出全屏"的用户心智
        //   2. 给孩子留消化空隙（看完 → 屏幕变回 emoji → 再决定要不要问下一个）
        //   3. 顺便规避 protocol.cc 120s timeout BUG：在 GIF 期间按键不去
        //      重连，避免"重连失败 → 静默回 Idle"的错觉。
        pwr_button_->OnClick([this]() {
            auto& app = Application::GetInstance();
            auto lcd = dynamic_cast<LcdDisplay*>(GetDisplay());

            // 早返回路径：preview 显示中 → 关 preview + 唤醒省电定时器即返回
            if (lcd && lcd->IsShowingPreview()) {
                ESP_LOGI(TAG, "Power button click during preview - dismiss preview only");
                lcd->SetPreviewImage(nullptr);
                power_save_timer_->WakeUp();
                app.ReportClientEvent("button", "click_dismiss_preview",
                                      "{\"btn\":\"power\"}");
                return;
            }

            auto current_state = app.GetDeviceState();
            ESP_LOGI(TAG, "Power button click, state: %d", current_state);

            // 上报"按键 → 服务端"现场诊断信息
            const char* branch = "ignored";
            if (current_state == kDeviceStateIdle) branch = "start_single_turn";
            else if (current_state == kDeviceStateListening) branch = "stop_listening";
            else if (current_state == kDeviceStateSpeaking) branch = "abort_to_idle";
            std::string data = std::string("{\"btn\":\"power\",\"state\":")
                               + std::to_string(static_cast<int>(current_state))
                               + ",\"branch\":\"" + branch + "\"}";
            app.ReportClientEvent("button", "click", data);

            power_save_timer_->WakeUp();

            if (current_state == kDeviceStateIdle) {
                app.StartSingleTurn();
            } else if (current_state == kDeviceStateListening) {
                app.StopListening();
            } else if (current_state == kDeviceStateSpeaking) {
                app.AbortToIdle();
            }
        });

        // 电源键三击：重置 WiFi
        pwr_button_->OnMultipleClick([this]() {
            ESP_LOGI(TAG, "Power button triple click: reset WiFi");
            Application::GetInstance().ReportClientEvent(
                "button", "triple_click",
                "{\"btn\":\"power\",\"action\":\"reset_wifi\"}");
            power_save_timer_->WakeUp();
            EnterWifiConfigMode();
        }, 3);

        // 音量+
        wifi_button.OnPressDown([this]() {
            ESP_LOGI(TAG, "Volume up");
            power_save_timer_->WakeUp();
            auto codec = GetAudioCodec();
            int current_vol = codec->output_volume();
            current_vol = (current_vol + 8 > 80) ? 80 : current_vol + 8;
            codec->SetOutputVolume(current_vol);
            int display_volume = MapVolumeForDisplay(current_vol);
            GetDisplay()->ShowNotification(Lang::Strings::VOLUME +
                                           std::to_string(display_volume) + "%");
        });

        // 音量-
        cmd_button.OnPressDown([this]() {
            ESP_LOGI(TAG, "Volume down");
            power_save_timer_->WakeUp();
            auto codec = GetAudioCodec();
            int current_vol = codec->output_volume();
            current_vol = (current_vol - 8 < 0) ? 0 : current_vol - 8;
            codec->SetOutputVolume(current_vol);
            if (current_vol == 0) {
                GetDisplay()->ShowNotification(Lang::Strings::MUTED);
            } else {
                int display_volume = MapVolumeForDisplay(current_vol);
                GetDisplay()->ShowNotification(Lang::Strings::VOLUME +
                                               std::to_string(display_volume) + "%");
            }
        });
    }

    void InitializeDisplay() {
        // SPI 总线初始化
        ESP_LOGI(TAG, "Initialize SPI bus for LCD");
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = DISPLAY_SPI_MOSI_PIN;
        buscfg.sclk_io_num = DISPLAY_SPI_SCK_PIN;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));

        // SPI Panel IO
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_SPI_CS_PIN;
        io_config.dc_gpio_num = DISPLAY_DC_PIN;
        io_config.spi_mode = 0;  // JD9853 用 SPI mode 0
        io_config.pclk_hz = 40 * 1000 * 1000;  // JD9853 默认 40MHz
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io);

        // JD9853 面板初始化
        ESP_LOGI(TAG, "Install JD9853 LCD driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = GPIO_NUM_NC;
        panel_config.rgb_ele_order = LCD_RGB_ENDIAN_BGR;
        panel_config.bits_per_pixel = 16;
        ESP_ERROR_CHECK(esp_lcd_new_panel_jd9853(panel_io, &panel_config, &panel));

        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
        ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, true));
        // MADCTL=0x4A (MY=0 MX=1 MV=0 ML=0 BGR=1 MH=0)：硬件层控制方向，
        // LVGL 层不再做 mirror。
        ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(panel_io, 0x36, (uint8_t[]){0x4A}, 1));

        display_ = new BoilonLcdDisplay(panel_io, panel,
                                        DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                        DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
                                        false, false, false);
    }

public:
    BoilonV2Board() :
        boot_button_(BOOT_BUTTON_GPIO),
        pwr_button_(nullptr),
        wifi_button(WIFI_BUTTON_GPIO),
        cmd_button(CMD_BUTTON_GPIO) {

        // 创建共享的 ADC1 handle + 校准 handle（PowerManager 会复用）
        adc_oneshot_unit_init_cfg_t adc_cfg = {
            .unit_id = ADC_UNIT_1,
            .ulp_mode = ADC_ULP_MODE_DISABLE,
        };
        ESP_ERROR_CHECK(adc_oneshot_new_unit(&adc_cfg, &shared_adc_handle_));

        // CH3 = GPIO4 电池电压；CH4 = GPIO5 VBUS（充电检测）
        adc_oneshot_chan_cfg_t chan_cfg = {
            .atten = ADC_ATTEN_DB_12,
            .bitwidth = ADC_BITWIDTH_12,
        };
        ESP_ERROR_CHECK(adc_oneshot_config_channel(shared_adc_handle_, ADC_CHANNEL_3, &chan_cfg));
        ESP_ERROR_CHECK(adc_oneshot_config_channel(shared_adc_handle_, ADC_CHANNEL_4, &chan_cfg));

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
        adc_cali_curve_fitting_config_t cali_config = {
            .unit_id = ADC_UNIT_1,
            .atten = ADC_ATTEN_DB_12,
            .bitwidth = ADC_BITWIDTH_12,
        };
        ESP_ERROR_CHECK(adc_cali_create_scheme_curve_fitting(&cali_config, &shared_adc_cali_handle_));
#endif

        InitializeI2c();
        InitializeButtons();         // GPIO Button (power) + GPIO Buttons (boot/volume)
        InitializePowerManager();    // 内部触发 PowerController::Instance() → LatchPowerOn(GPIO15)
        InitializePowerSaveTimer();
        InitializeDisplay();
        GetBacklight()->RestoreBrightness();

        // 开机表情（SetupUI 时自动显示到 emoji_image_）
        auto lcd = dynamic_cast<LcdDisplay*>(GetDisplay());
        if (lcd) {
            auto* perm_copy = (char*)heap_caps_malloc(boot_emoji_png_size,
                                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (perm_copy) {
                memcpy(perm_copy, boot_emoji_png, boot_emoji_png_size);
                try {
                    lcd->boot_emoji_dsc_ = std::make_unique<LvglAllocatedImage>(
                        perm_copy, boot_emoji_png_size);
                } catch (...) {
                    heap_caps_free(perm_copy);
                    ESP_LOGW(TAG, "Failed to load boot emoji");
                }
            }
        }
    }

    virtual Led* GetLed() override {
        static SingleLed led(BUILTIN_LED_GPIO);
        return &led;
    }

    virtual AudioCodec* GetAudioCodec() override {
        // BoxAudioCodec: ES8311 (DAC) + ES7210 (4ch ADC), TDM 模式
        // ES7210 地址实测为 0x41（非默认 0x40）
        static BoxAudioCodec audio_codec(
            codec_i2c_bus_,
            AUDIO_INPUT_SAMPLE_RATE,
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK,
            AUDIO_I2S_GPIO_BCLK,
            AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT,
            AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN,
            AUDIO_CODEC_ES8311_ADDR,
            AUDIO_CODEC_ES7210_ADDR,
            AUDIO_INPUT_REFERENCE);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }

    virtual bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        static bool last_discharging = false;
        charging = power_manager_->IsCharging();
        discharging = power_manager_->IsDischarging();
        if (discharging != last_discharging) {
            power_save_timer_->SetEnabled(discharging);
            last_discharging = discharging;
        }
        level = power_manager_->GetBatteryLevel();
        return true;
    }

    // 把 LOW_POWER 重映射到 BALANCED（即 WIFI_PS_MIN_MODEM）。
    //
    // 反编译铁证：vendor 的 WifiStation::SetPowerSaveMode(bool enabled) 实现是
    //   esp_wifi_set_ps(enabled ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE);
    // vendor binary 完全没有 WIFI_PS_MAX_MODEM 字符串（穷举多变体均 0 命中）。
    //
    // 我们的 wifi_station.cc 默认映射：
    //   LOW_POWER     → WIFI_PS_MAX_MODEM   ← 我们以前默认走这条
    //   BALANCED      → WIFI_PS_MIN_MODEM   ← vendor 启用 PS 时用
    //   PERFORMANCE   → WIFI_PS_NONE        ← vendor 关 PS 时用
    //
    // application.cc 默认 SetPowerSaveLevel(LOW_POWER)，所以这里强制重映射。
    // MAX_MODEM 让 WiFi modem 周期性 DTIM 唤醒（电流尖峰），是历史诊断中
    // 一度怀疑的"松手即掉电"真因（H10），最终被 H11（GPIO15 PWR_EN latch）
    // 取代。但本 override 保留：vendor parity 总是正确的方向。
    virtual void SetPowerSaveLevel(PowerSaveLevel level) override {
        PowerSaveLevel effective_level = level;
        if (level == PowerSaveLevel::LOW_POWER) {
            effective_level = PowerSaveLevel::BALANCED;
        }
        if (effective_level != PowerSaveLevel::LOW_POWER) {
            power_save_timer_->WakeUp();
        }
        WifiBoard::SetPowerSaveLevel(effective_level);
    }
};

DECLARE_BOARD(BoilonV2Board);
