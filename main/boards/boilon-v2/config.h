#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>

#define AUDIO_INPUT_SAMPLE_RATE  24000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000

#define AUDIO_INPUT_REFERENCE    true

#define AUDIO_I2S_GPIO_MCLK GPIO_NUM_38
#define AUDIO_I2S_GPIO_WS GPIO_NUM_13
#define AUDIO_I2S_GPIO_BCLK GPIO_NUM_14
#define AUDIO_I2S_GPIO_DIN  GPIO_NUM_11
#define AUDIO_I2S_GPIO_DOUT GPIO_NUM_12

#define AUDIO_CODEC_PA_PIN       GPIO_NUM_42
#define AUDIO_CODEC_I2C_SDA_PIN  GPIO_NUM_1
#define AUDIO_CODEC_I2C_SCL_PIN  GPIO_NUM_2
#define AUDIO_CODEC_ES8311_ADDR  ES8311_CODEC_DEFAULT_ADDR
#define AUDIO_CODEC_ES7210_ADDR 0x82  // 博亿朗二代 ES7210: 7-bit=0x41, 8-bit=0x82

#define BUILTIN_LED_GPIO        GPIO_NUM_10
#define BOOT_BUTTON_GPIO        GPIO_NUM_0
#define PWR_BUTTON_GPIO         GPIO_NUM_3
#define PWR_BATTERY_ADC_GPIO    GPIO_NUM_4
#define PWR_VBUS_ADC_GPIO       GPIO_NUM_5
// 充电完成 DONE 检测引脚 (来自 vendor 闭源固件 boot log 实测：
//   "电池充满检测引脚 GPIO16 初始化完成 (当前状态: 未充满, 下降沿中断)")
// 之前我们错误地把 PWR_BATTERY_ADC_GPIO (GPIO4) 当 charging_pin 传给 PowerManager
// 构造函数，PowerManager 内部 gpio_config(&io_conf) 把 GPIO4 重新配成普通 INPUT +
// 上下拉全 DISABLE 高阻悬空 -> 破坏了主板 PMIC 通过电池 ADC 线维持自保持的环路
// -> 松手即掉电。修复就是改用真正的 DONE 引脚 GPIO16。
#define PWR_CHARGING_DONE_GPIO  GPIO_NUM_16
#define PWR_BUTTON_TIME         3000000U

#define WIFI_BUTTON_GPIO        GPIO_NUM_6
#define CMD_BUTTON_GPIO         GPIO_NUM_7


#define DISPLAY_SPI_SCK_PIN     GPIO_NUM_41
#define DISPLAY_SPI_MOSI_PIN    GPIO_NUM_40
#define DISPLAY_DC_PIN          GPIO_NUM_39
#define DISPLAY_SPI_CS_PIN      GPIO_NUM_9

#define DISPLAY_WIDTH   240
#define DISPLAY_HEIGHT  296  // 博亿朗二代 JD9853 配置为 240x296（与闭源固件一致）
#define DISPLAY_MIRROR_X true   // 闭源固件 MADCTL=0x4A: MX=1
#define DISPLAY_MIRROR_Y false  // 闭源固件 MADCTL=0x4A: MY=0
#define DISPLAY_SWAP_XY false

#define DISPLAY_OFFSET_X  0
#define DISPLAY_OFFSET_Y  0

#define DISPLAY_BACKLIGHT_PIN GPIO_NUM_46
#define DISPLAY_BACKLIGHT_OUTPUT_INVERT false


#endif // _BOARD_CONFIG_H_
