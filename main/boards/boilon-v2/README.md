# 博亿朗二代 (Boilon V2)

博亿朗二代小智 AI 音箱，九川科技方案（ESP32-S3）。这是一份**基于 `jiuchuan-s3` 修改的自定义板**，用于适配博亿朗二代真实硬件（屏幕、音频、按键都和 `jiuchuan-s3` 不同）。

## 硬件规格

- **芯片**：ESP32-S3，16MB Flash，8MB Octal PSRAM
- **屏幕**：JD9853，240×320（RASET 截到 240×296），SPI 接口
- **音频**：ES8311 (DAC/ADC) + ES7210 (4ch ADC)，I2S TDM 模式
- **按键**：BOOT (GPIO0) + 播放/电源（GPIO3，**ADC 按键**不是数字按键）+ 音量+ (GPIO6) + 音量- (GPIO7)
- **背光**：GPIO46 PWM
- **LED**：GPIO10
- **电源**：GPIO5 (PWR_EN) / GPIO4 (ADC 电池检测)

## 编译

用上游标准方式：

```bash
cd xiaozhi-esp32
python scripts/release.py boilon-v2
```

产物在 `build/merged-binary.bin` + `releases/v<ver>_boilon-v2.zip`。烧录用 `idf.py -p <COM_PORT> flash monitor`。

## 文档

本板的完整技术文档在独立目录 `chatbot/firmware/`：

- **总览** → `chatbot/firmware/README.md`
- **硬件参数事实表** → `chatbot/firmware/docs/hardware.md`
- **我们改了上游什么 + 踩坑教训** → `chatbot/firmware/docs/customizations.md`
- **逆向方法论（怎么从卖家 bin 挖参数）** → `chatbot/firmware/docs/reverse-methodology.md`
- **编译/烧录 详细流程** → `chatbot/firmware/docs/build.md`
