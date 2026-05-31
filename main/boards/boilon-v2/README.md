# 博亿朗二代 (Boilon V2)

博亿朗二代小智 AI 音箱，九川科技方案（ESP32-S3）。这是一份**基于 `jiuchuan-s3` 修改的自定义板**，用于适配博亿朗二代真实硬件（屏幕、音频、按键、电源管理都和 `jiuchuan-s3` 有差异）。

## 硬件规格

- **芯片**：ESP32-S3，16MB Flash，8MB Octal PSRAM
- **屏幕**：JD9853，240×320（RASET 截到 240×296），SPI 接口
- **音频**：ES8311 (DAC/ADC) + ES7210 (4ch ADC)，I2S TDM 模式
- **按键**：BOOT (GPIO0) + 电源 (GPIO3，GPIO 数字按键，按下=HIGH) + 音量+ (GPIO6) + 音量- (GPIO7)
- **背光**：GPIO46 PWM
- **LED**：GPIO10
- **电源管理**：
  - **PWR_EN**：GPIO15（boot 时必须主动驱动 HIGH 才能维持 PMIC EN，详见 `power_controller.h::LatchPowerOn()`）
  - **VBUS ADC**：GPIO5（CH4，用于检测 USB 接入）
  - **电池 ADC**：GPIO4（CH3，串联 200kΩ + 100kΩ 分压）
  - **充电完成 DONE**：GPIO16（pull-up + NEGEDGE，"未充满"=HIGH）

## 编译

用上游标准方式：

```bash
cd firmware/src
python scripts/release.py boilon-v2
```

或直接使用 `boilon-firmware-dev` skill 的 `build_firmware.ps1`。产物在 `build/merged-binary.bin` + `releases/v<ver>_boilon-v2.zip`。

## 关键设计点（务必保持）

1. **GPIO15 主动驱动 HIGH**（`power_controller.h::LatchPowerOn()`）：boot 时必须做这件事，否则用户松开电源键后整板 < 50ms 掉电。这是 vendor parity，反编译铁证。
2. **GPIO Button + enable_power_save=true**（`boilon_v2_board.cc::InitializeButtons()`）：5 参数 `Button(GPIO3, true, 0, 0, true)` 对齐 vendor binary 的 `Button::Button(gpio_num_t, bool, uint16_t, uint16_t, bool)` 签名。
3. **不注册 OnPressUp 回调**：vendor binary 完全没有 BUTTON_PRESS_UP 字符串，对齐 vendor parity。
4. **VBUS ADC 充电检测**（`power_manager.h::CheckBatteryStatus()`）：用 GPIO5 ADC 电压判断"正在充电"，不能用 GPIO16 电平（GPIO16 是"未充满"信号，不是充电信号）。
5. **`SetPowerSaveLevel(LOW_POWER)` → `BALANCED`**：`boilon_v2_board.cc::SetPowerSaveLevel()` 强制重映射到 BALANCED（即 `WIFI_PS_MIN_MODEM`），对齐 vendor 永不使用 `WIFI_PS_MAX_MODEM`。

## 文档

- **整体技术文档** → `firmware/README.md`
- **硬件参数事实表** → `firmware/docs/hardware.md`
- **「松手即掉电」BUG 完整跟踪** → `firmware/docs/power-loss-bug.md`
- **逆向方法论** → `firmware/docs/reverse-methodology.md`
- **vendor 反编译产出** → `firmware/reverse/disasm/`
