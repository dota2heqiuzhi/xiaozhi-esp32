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
    Button* pwr_button_;  // ADC 按键，在 InitializeButtons 中创建
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
        power_manager_ = new PowerManager(PWR_ADC_GPIO, shared_adc_handle_, shared_adc_cali_handle_);
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
            rtc_gpio_set_level(PWR_EN_GPIO, 0);
            rtc_gpio_hold_dis(PWR_EN_GPIO);
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
        // 开机保护：如果用户按住电源键开机，在第一次松开前长按事件一律忽略，
        // 防止"长按开机"被误判为"长按关机"导致刚开机就关机。
        // 开机时默认 true，等第一次 OnPressUp 置 false。
        static bool pwrbutton_unreleased = true;

        // 播放/电源键是 ADC 按键 (ADC1_CH2 / GPIO3)
        // 实测: 不按=~229 (raw), 按下=~3975 (raw)
        // 对应电压约: 不按=~185mV, 按下=~3200mV
        // 设置检测阈值: min=2000mV, max=3500mV
        ESP_LOGI(TAG, "Creating ADC button on ADC1_CH2 (play/power button)");
        button_adc_config_t adc_btn_cfg = {
            .adc_handle = &shared_adc_handle_,
            .unit_id = ADC_UNIT_1,
            .adc_channel = 2,  // ADC1_CH2 = GPIO3
            .button_index = 0,
            .min = 2000,  // 按下时电压 > 2000mV
            .max = 3500,  // 按下时电压 < 3500mV
        };
        pwr_button_ = new AdcButton(adc_btn_cfg);
        ESP_LOGI(TAG, "ADC power button created");

        boot_button_.OnClick([this]() {
            ESP_LOGI(TAG, "Boot button clicked");
            power_save_timer_->WakeUp();
        });

        // 电源键按下：单纯打日志（保护标志由 OnPressUp 维护）
        pwr_button_->OnPressDown([this]() {
            ESP_LOGI(TAG, "Power button press down");
        });

        // 电源键松开：第一次松开后，开机保护标志归零，后续长按关机才有效
        pwr_button_->OnPressUp([this]() {
            if (pwrbutton_unreleased) {
                pwrbutton_unreleased = false;
                ESP_LOGI(TAG, "Power button first release - long press shutdown now armed");
            }
        });

        // 电源键长按关机
        pwr_button_->OnLongPress([this]() {
            if (pwrbutton_unreleased) {
                ESP_LOGI(TAG, "开机后电源键未松开，忽略长按关机");
                return;
            }
            ESP_LOGI(TAG, "Power button long press - shutting down");
            power_manager_->SetPowerState(PowerState::SHUTDOWN);
        });

        // 电源键单击：切换对话状态
        pwr_button_->OnClick([this]() {
            auto &app = Application::GetInstance();
            auto current_state = app.GetDeviceState();
            ESP_LOGI(TAG, "Power button click, state: %d", current_state);
            
            if (current_state == kDeviceStateIdle) {
                // 模拟唤醒词触发，走和语音唤醒完全一致的路径
                app.WakeWordInvoke("你好小智");
            } else if (current_state == kDeviceStateListening) {
                app.ToggleChatState();
            } else if (current_state == kDeviceStateSpeaking) {
                app.ToggleChatState();
            } else {
                power_save_timer_->WakeUp();
            }
        });

        // 电源键三击：重置WiFi
        pwr_button_->OnMultipleClick([this]() {
            ESP_LOGI(TAG, "Power button triple click: reset WiFi");
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
        
        // 配置电池检测通道 (CH3)
        adc_oneshot_chan_cfg_t chan_cfg = {
            .atten = ADC_ATTEN_DB_12,
            .bitwidth = ADC_BITWIDTH_12,
        };
        ESP_ERROR_CHECK(adc_oneshot_config_channel(shared_adc_handle_, ADC_CHANNEL_3, &chan_cfg));
        
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
        InitializeButtons();       // AdcButton 复用 shared_adc_handle_
        InitializePowerManager();   // PowerManager 也复用 shared_adc_handle_
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
