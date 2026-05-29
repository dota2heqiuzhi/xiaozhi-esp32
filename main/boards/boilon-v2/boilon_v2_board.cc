/**
 * 博亿朗二代 (Boilon V2) 自定义板级代码
 * 
 * 基于 jiuchuan-s3 修改，关键变更：
 * 1. LCD 驱动：GC9309NA → JD9853 (240x296)
 * 2. 音频：Es8311AudioCodec（与原版 jiuchuan-s3 一致）
 * 3. 按键配置保持与闭源固件一致
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
        // 如果需要微调位置，在这里修改
    }
};

class BoilonV2Board : public WifiBoard {
private:
    i2c_master_bus_handle_t codec_i2c_bus_;
    adc_oneshot_unit_handle_t shared_adc_handle_ = NULL;  // ADC1 共享 handle
    adc_cali_handle_t shared_adc_cali_handle_ = NULL;    // ADC1 校准 handle
    Button boot_button_;
    Button* pwr_button_;  // GPIO Button (GPIO3, INPUT_PULLDOWN, active_high)，在 InitializeButtons 中 new
    Button wifi_button;
    Button cmd_button;
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
        // ⚠️ 关键修复 (2026-05-28)：
        //   之前错误地传 PWR_BATTERY_ADC_GPIO (GPIO4) 当 charging_pin。
        //   PowerManager 构造函数会调 gpio_config(&io_conf) 把传入的 pin 配成
        //   普通 INPUT + 上下拉全 DISABLE，等于把 GPIO4 改成高阻悬空 ——
        //   而 GPIO4 是电池电压检测 ADC 输入，主板 PMIC 自保持环路依赖这条线
        //   的电气特性。结果就是松手即掉电。
        //   正确做法：传 vendor 实测的 DONE 引脚 GPIO16（充电完成下降沿中断），
        //   GPIO4 完全留给 ADC 自己用。
        power_manager_ = new PowerManager(PWR_CHARGING_DONE_GPIO, shared_adc_handle_, shared_adc_cali_handle_);
        power_manager_->OnChargingStatusChanged([this](bool is_charging) {
            if (is_charging) {
                power_save_timer_->SetEnabled(false);
            } else {
                power_save_timer_->SetEnabled(true);
            }
        });
    }

    void InitializePowerSaveTimer() {
        // 5分钟无操作深度睡眠关机
        power_save_timer_ = new PowerSaveTimer(-1, (60*5), -1);
        power_save_timer_->OnEnterSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(true);
            GetBacklight()->SetBrightness(1);
        });
        power_save_timer_->OnExitSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(false);
            GetBacklight()->RestoreBrightness();
        });
        power_save_timer_->OnShutdownRequest([this]() {
            ESP_LOGI(TAG, "Shutting down");
            PowerController::Instance().PowerOff();
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

        // === I2C 总线扫描：列出所有在线设备 ===
        ESP_LOGI(TAG, "========== I2C BUS SCAN (SDA=%d, SCL=%d) ==========",
                 AUDIO_CODEC_I2C_SDA_PIN, AUDIO_CODEC_I2C_SCL_PIN);
        int device_count = 0;
        for (uint8_t addr = 0x08; addr < 0x78; addr++) {
            esp_err_t ret = i2c_master_probe(codec_i2c_bus_, addr, 50);
            if (ret == ESP_OK) {
                const char* name = "unknown";
                if (addr == 0x18) name = "ES8311 (DAC/ADC)";
                else if (addr == 0x30) name = "ES8311 (alt addr)";
                else if (addr == 0x40) name = "ES7210 (4ch ADC)";
                else if (addr == 0x41) name = "ES7210 (alt addr)";
                else if (addr == 0x50) name = "EEPROM";
                else if (addr == 0x68 || addr == 0x69) name = "RTC/IMU";
                else if (addr == 0x3C || addr == 0x3D) name = "OLED SSD1306";
                else if (addr == 0x27) name = "PCF8574 IO expander";
                ESP_LOGI(TAG, "  Found device at 0x%02X (%d) - %s", addr, addr, name);
                device_count++;
            }
        }
        ESP_LOGI(TAG, "  Total: %d device(s) found on I2C bus", device_count);
        ESP_LOGI(TAG, "========== I2C SCAN COMPLETE ==========");
    }

    void InitializeButtons() {
        // ============================================================
        // 行为对齐 jiuchuan-s3 父板（vendor binary 字符串铁证：
        //   `void JiuchuanDevBoard::InitializeButtons()
        //    ./main/boards/jiuchuan-s3/jiuchuan_dev_board.cc`
        //   表明 vendor 在 boilon-v2 上直接复用了父板的这个实现）。
        //
        // 关键设计点（务必保持，曾踩坑数小时）：
        //   1. 先 gpio_get_level(GPIO3) 读电平，若用户按住开机就把
        //      pwrbutton_unreleased 标记为 true，避免"长按开机"被误判为
        //      "长按关机"立即触发关机。
        //   2. GPIO Button + 内部下拉（不是 ADC Button！ADC Button 把
        //      GPIO3 配成高阻输入，会破坏主板电源自保持电路依赖的电气
        //      特性，松手即整板掉电。这条由 vendor boot log 实测确认：
        //      `I (246) gpio: GPIO[3]| InputEn:1 ... Pulldown:1`）。
        //   3. 关机走 OnLongPress + 5 次防抖确认 GPIO3 仍 HIGH，**不**
        //      使用 OnPressUp 回调（vendor 完全没有 OnPressUp，自己加
        //      的 first_release/shutdown_pending 状态机过去几天没起到
        //      正确作用，反而是嫌疑代码）。
        // ============================================================

        // boot 时检测电源键是否被按住（用户按住电源键开机的常见情形）。
        // 若是，标记 pwrbutton_unreleased，等用户第一次松手后由
        // OnPressDown 重新触发时清除（OnPressDown 在 button library 内
        // 是边沿触发：从 LOW->HIGH 才回调，所以"用户已经按着"时不会立
        // 即触发，必须等用户松手再按一次才会清除——刚好就是我们想要的）。
        static bool pwrbutton_unreleased = false;
        if (gpio_get_level(GPIO_NUM_3) == 1) {
            pwrbutton_unreleased = true;
            ESP_LOGI(TAG, "Power button held HIGH at boot - long press shutdown disabled until first release");
        }

        // GPIO3 = 电源键，普通 GPIO Button + 内部下拉，按下=HIGH (active_high)。
        ESP_LOGI(TAG, "Creating GPIO button on GPIO%d (power button, INPUT_PULLDOWN, active_high)", PWR_BUTTON_GPIO);
        pwr_button_ = new Button(PWR_BUTTON_GPIO, true /* active_high */);
        ESP_LOGI(TAG, "Power button initialized");

        // BOOT 按键单击：唤醒省电定时器
        boot_button_.OnClick([this]() {
            ESP_LOGI(TAG, "Boot button clicked");
            power_save_timer_->WakeUp();
        });

        // 电源键按下：清除 pwrbutton_unreleased（第一次完整 down 边沿到来时）。
        // 行为对齐 jiuchuan-s3 父板：父板这里仅设标志位，**不**做其他事。
        pwr_button_->OnPressDown([this]() {
            ESP_LOGI(TAG, "Power button press down");
            pwrbutton_unreleased = false;
            Application::GetInstance().ReportClientEvent(
                "button", "press_down", "{\"btn\":\"power\"}");
        });

        // 电源键长按：行为完全对齐 jiuchuan-s3 父板的 OnLongPress：
        //   - 若 pwrbutton_unreleased=true（用户按住电源键开机还没松手），
        //     则忽略本次长按事件，避免开机即关机。
        //   - 否则做 5 次 100ms 防抖：每次读 GPIO3 电平，一旦读到 LOW 就
        //     说明用户松手了（abort），返回；连续 500ms 都是 HIGH 才真正
        //     调 SetPowerState(SHUTDOWN) 进入关机流程。
        //   - 关机流程内部由 PowerManager 走 esp_deep_sleep_start()。
        pwr_button_->OnLongPress([this]() {
            auto& app = Application::GetInstance();
            ESP_LOGI(TAG, "Power button long press detected");
            if (pwrbutton_unreleased) {
                ESP_LOGI(TAG, "开机后电源键未松开，忽略长按关机");
                app.ReportClientEvent(
                    "button", "long_press_ignored_boot_guard",
                    "{\"btn\":\"power\"}");
                return;
            }
            // 5 次 100ms 防抖确认（对齐 jiuchuan-s3 父板）
            for (int i = 0; i < 5; i++) {
                int level = gpio_get_level(PWR_BUTTON_GPIO);
                ESP_LOGD(TAG, "Debounce check %d: GPIO%d level=%d",
                         i + 1, PWR_BUTTON_GPIO, level);
                if (level == 0) {
                    ESP_LOGW(TAG, "Power button released during confirmation - abort shutdown");
                    app.ReportClientEvent(
                        "button", "long_press_aborted",
                        "{\"btn\":\"power\"}");
                    return;
                }
                vTaskDelay(100 / portTICK_PERIOD_MS);
            }
            ESP_LOGI(TAG, "Confirmed power button long-pressed - initiating shutdown");
            app.ReportClientEvent(
                "button", "long_press_shutdown",
                "{\"btn\":\"power\"}");
            power_manager_->SetPowerState(PowerState::SHUTDOWN);
        });

        // 电源键单击：儿童使用场景下的单轮对话入口
        // 设计目标：点一下开始听一句，AI 回复完自动回 Idle；下次要说话再点一下。
        pwr_button_->OnClick([this]() {
            auto &app = Application::GetInstance();
            auto current_state = app.GetDeviceState();
            ESP_LOGI(TAG, "Power button click, state: %d", current_state);

            // 决定分支前先上报：这条是"按键 → 服务端"现场诊断的命脉。
            // data 里把决策依据全部放进去，服务端日志一行就能定位行为分支。
            {
                const char* branch = "ignored";
                if (current_state == kDeviceStateIdle) branch = "start_single_turn";
                else if (current_state == kDeviceStateListening) branch = "stop_listening";
                else if (current_state == kDeviceStateSpeaking) branch = "abort_to_idle";
                std::string data = std::string("{\"btn\":\"power\",\"state\":")
                                   + std::to_string(static_cast<int>(current_state))
                                   + ",\"branch\":\"" + branch + "\"}";
                app.ReportClientEvent("button", "click", data);
            }

            power_save_timer_->WakeUp();
            if (auto lcd = dynamic_cast<LcdDisplay*>(GetDisplay())) {
                lcd->SetPreviewImage(nullptr);
            }

            if (current_state == kDeviceStateIdle) {
                app.StartSingleTurn();
            } else if (current_state == kDeviceStateListening) {
                app.StopListening();
            } else if (current_state == kDeviceStateSpeaking) {
                app.AbortToIdle();
            }
        });

        // 电源键三击：重置WiFi（对齐 jiuchuan-s3 父板）
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
            ESP_LOGI(TAG, "Volume up button pressed");
            power_save_timer_->WakeUp();
            auto codec = GetAudioCodec();
            int current_vol = codec->output_volume();
            current_vol = (current_vol + 8 > 80) ? 80 : current_vol + 8;
            codec->SetOutputVolume(current_vol);
            int display_volume = MapVolumeForDisplay(current_vol);
            GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(display_volume) + "%");
        });

        // 音量-
        cmd_button.OnPressDown([this]() {
            ESP_LOGI(TAG, "Volume down button pressed");
            power_save_timer_->WakeUp();
            auto codec = GetAudioCodec();
            int current_vol = codec->output_volume();
            current_vol = (current_vol - 8 < 0) ? 0 : current_vol - 8;
            codec->SetOutputVolume(current_vol);
            if (current_vol == 0) {
                GetDisplay()->ShowNotification(Lang::Strings::MUTED);
            } else {
                int display_volume = MapVolumeForDisplay(current_vol);
                GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(display_volume) + "%");
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
        // 使用默认初始化序列（waveshare 的 172x320）
        // TODO: 如果显示不正常，从闭源固件提取博亿朗专用初始化序列
        ESP_ERROR_CHECK(esp_lcd_new_panel_jd9853(panel_io, &panel_config, &panel));

        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
        ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, true));  // 金标准有 INVON
        // 直接写入金标准的 MADCTL=0x4A (MY=0 MX=1 MV=0 ML=0 BGR=1 MH=0)
        // 硬件层控制方向，LVGL 层不再做 mirror
        ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(panel_io, 0x36, (uint8_t[]){0x4A}, 1));

        // LVGL 的 mirror 全部传 false——方向完全由硬件 MADCTL 控制
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

        // 创建共享的 ADC1 handle + 校准 handle（AdcButton 和 PowerManager 共用）
        adc_oneshot_unit_init_cfg_t adc_cfg = {
            .unit_id = ADC_UNIT_1,
            .ulp_mode = ADC_ULP_MODE_DISABLE,
        };
        ESP_ERROR_CHECK(adc_oneshot_new_unit(&adc_cfg, &shared_adc_handle_));
        
        // 配置电池检测通道 (GPIO4 / ADC1_CH3) 与 VBUS 检测通道 (GPIO5 / ADC1_CH4)。
        // 卖家闭源固件字符串明确显示：GPIO5 是 VBUS ADC，不是 PWR_EN 输出脚。
        adc_oneshot_chan_cfg_t chan_cfg = {
            .atten = ADC_ATTEN_DB_12,
            .bitwidth = ADC_BITWIDTH_12,
        };
        ESP_ERROR_CHECK(adc_oneshot_config_channel(shared_adc_handle_, ADC_CHANNEL_3, &chan_cfg));
        ESP_ERROR_CHECK(adc_oneshot_config_channel(shared_adc_handle_, ADC_CHANNEL_4, &chan_cfg));
        
        // 创建校准 handle
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
        adc_cali_curve_fitting_config_t cali_config = {
            .unit_id = ADC_UNIT_1,
            .atten = ADC_ATTEN_DB_12,
            .bitwidth = ADC_BITWIDTH_12,
        };
        ESP_ERROR_CHECK(adc_cali_create_scheme_curve_fitting(&cali_config, &shared_adc_cali_handle_));
#endif

        InitializeI2c();
        InitializeButtons();       // GPIO Button (power) + GPIO Buttons (boot/volume)
        InitializePowerManager();   // PowerManager 复用 shared_adc_handle_
        InitializePowerSaveTimer();
        InitializeDisplay();
        GetBacklight()->RestoreBrightness();

        // 开机表情（SetupUI 时自动显示到 emoji_image_）
        auto lcd = dynamic_cast<LcdDisplay*>(GetDisplay());
        if (lcd) {
            auto* perm_copy = (char*)heap_caps_malloc(boot_emoji_png_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (perm_copy) {
                memcpy(perm_copy, boot_emoji_png, boot_emoji_png_size);
                try {
                    lcd->boot_emoji_dsc_ = std::make_unique<LvglAllocatedImage>(perm_copy, boot_emoji_png_size);
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

    virtual void SetPowerSaveLevel(PowerSaveLevel level) override {
        if (level != PowerSaveLevel::LOW_POWER) {
            power_save_timer_->WakeUp();
        }
        WifiBoard::SetPowerSaveLevel(level);
    }
};

DECLARE_BOARD(BoilonV2Board);
