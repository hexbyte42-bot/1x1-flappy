# 1x1-Flappy V2 触摸修复记录

> Waveshare ESP32-S3-Touch-AMOLED-1.8 (V2: CO5300 + CST820) 触摸问题的根因与修复。

## 1. 硬件背景

V2 板:
- 显示: CO5300 (GRAM 480×480, 可见窗口 368×448, 列偏移 16)
- 触摸: CST820 (I2C 0x15, 芯片 ID 0xB7) — 与 V1 的 FT3168 (0x38) 不同
- I2C 总线设备: 0x15 (触摸), 0x20 (TCA9554 扩展器), 0x34 (AXP2101), 0x51, 0x6B (QMI8658)

## 2. 上游 bug: make_touch() 从未被调用

`hw_panel.cpp` 提供了 `make_touch()` 做 V1/V2 自动检测,但 `app_1x1_flap.cpp`
**硬编码了 V1 触摸**: `static TouchDrvFT6X36 s_touch;` + `s_touch.begin(Wire, 0x38, ...)`。
V2 板上没有任何设备在 0x38 → 触摸完全失效。显示部分 (make_display) 上游已改为自动检测,
触摸部分漏改。

## 3. 坐标缩放问题 (CST820 固件行为)

换用 SensorLib `TouchDrvCST816` (原生支持 CST816S/T/D/CST820) 后触摸可读,但
**Y 轴坐标被放大**: 触摸 IC 固件把 448px 高的屏映射到约 512 的 Y 坐标范围。

实测标定数据 (屏幕坐标 → 芯片原始值):

| 屏幕位置 (y) | 原始值 | 偏差 |
|---|---|---|
| 175 (第1行键) | 182/187 | +7~12 |
| 271 (第2行键) | 285 | +14 |
| 341 (第3行键) | 376/380 | +35~39 |
| 411 (第4行键) | 447 | +36 |

X 轴实测 1:1 (键宽 104px, 无需修正)。

反推线性映射: `rawY ≈ 1.14 × displayY − 15` →
**修正公式: `displayY = (rawY + 15) × 7 / 8`** (≈ 0.875·rawY + 13.1)

注意: 这是 CST820 (0xB7) 的固件行为。CST816S/T/D 的板子通常报 1:1,
所以修正必须**按芯片 ID 条件启用**, 否则会把 1:1 的板子修歪。

## 4. 修复内容

`hw_panel.cpp`:
- `make_touch()` 检查 `begin()` 返回值, 失败返回 nullptr
- V2 分支调用 `disableAutoSleep()` (防休眠后轮询无响应; 无 RST 引脚时仅为寄存器写, 安全)

`app_1x1_flap.cpp`:
- 触摸对象改为 `TouchDrvInterface*`, setup 中调用 `make_touch()` 自动检测
- V2 (CST816/CST820) 直接轮询 I2C, 不依赖 TP_INT 门控 (V1 保持原 INT 门控)
- CST820 (chip==0xB7) 应用 Y 修正 `ty = (ty + 15) * 7 / 8`; 其他芯片原样
- 触摸初始化失败不崩溃, 串口打印 `touch: init failed`

## 5. 标定工具

`tools/touch_test/` — 独立测试固件:
- 绘制与 app 相同的键盘网格, 串口打印 `RAW/CORR` 坐标
- 红点 = 原始坐标, 绿点 = 修正后坐标 (实测绿点准确)
- 开机 `while(!USBSerial)` 等待串口, 保证不丢启动日志
- `ENABLE_OFFICIAL_INIT` 开关可对比官方例程的 TCA9554 复位脉冲 + 0xFA 写入

## 6. 其他注意事项

- 三个触摸驱动 (SensorLib / Arduino_DriveBus / IDF esp_lcd_touch) 读取坐标的
  寄存器运算完全相同, 均无缩放 — 缩放来自芯片固件, 只能在主机端软件修正
- Waveshare 官方画板例程"看着准"是因为画线容忍 0~40px 偏差; 键盘命中检测在
  误差最大的下半屏, 才会暴露问题
- 构建使用 arduino-cli 1.5.2 + esp32 core 3.3.11 (项目需要 v3 核心: USBCDC/periman),
  PlatformIO 官方 espressif32 平台最高只有 arduino-esp32 v2.0.17, 无法构建本项目

## 7. 补充: furble 移植开机循环 (TG0WDT_SYS_RST)

PlatformIO 的 arduino-esp32 v2.0.17 (IDF 4.4) 构建出的 **bootloader 在这块板子的
flash 芯片上无法启动**——ROM 阶段反复复位:

```
ESP-ROM:esp32s3-20210327
rst:0x7 (TG0WDT_SYS_RST),boot:0x2b (SPI_FAST_FLASH_BOOT)
load:0x3fce3808,len:0x4bc
ets_loader.c 78   ← 卡在这里循环
```

而 arduino-cli 3.3.11 (IDF 5.5) 的 bootloader 在这块板上完全正常(1x1-flappy 一直用它)。

**解决 (两件事都要)**:
1. 用 IDF 5.5.5 的 bootloader + boot_app0 (见 furble/tools/)
2. **esptool merge_bin 必须用 `--flash_mode dio`** —— 用 `qio` 会把 bootloader
   头的 flash 模式从 0x02(DIO) 改写成 0x00(QIO), 这块板子的 flash 在
   ROM/bootloader 阶段跑 QIO 会同样挂死 (能启动的 1x1 固件头就是 0x02)。
