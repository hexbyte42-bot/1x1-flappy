# Waveshare ESP32-S3-Touch-AMOLED-1.8 开发技术参考

> 面向后续开发的硬件/软件技术手册。基于官方 BSP、`pin_config.h` 以及
> 1x1-flappy / furble 两个真实移植项目的实测经验。
> 适用于 **V1(FT3168 + SH8601)** 与 **V2(CO5300 + CST816/CST820)** 两个硬件版本,
> 默认讨论 V2。

---

## 1. 硬件总览

| 类别 | 规格 |
|---|---|
| SoC | ESP32-S3,双核 Xtensa LX7 @ **240 MHz**,WiFi + BLE 5,USB-OTG |
| Flash | **16 MB** QSPI(80 MHz) |
| PSRAM | **8 MB OPI(八线)PSRAM**(80 MHz,3V) |
| 显示 | **CO5300** AMOLED,**368×448 竖屏**;GRAM 480×480;QSPI 接口(4 数据线,80 MHz);四角圆角 |
| 触摸 | V2: **CST816/CST820** @ I2C 0x15;V1: **FT3168** @ 0x38(自动探测) |
| 电源 | **AXP2101** PMU:PEK 电源键、库仑计、充电检测、关机 |
| 音频 | **ES8311** 编解码(I2S);功放 PA=46 |
| 传感器 | **QMI8658** IMU @ 0x6B;**PCF85063** RTC @ 0x51 |
| IO 扩展 | **TCA9554** @ 0x20(复位/复用输出) |
| SD 卡 | SDMMC 1.8V(CLK=2,CMD=1,DATA=3) |
| 按键 | BOOT=GPIO0(下拉);PWR=AXP2101 PEK |

### I2C 总线设备地址(实测扫描)

| 地址 | 设备 |
|---|---|
| 0x15 | CST816/CST820 触摸(V2) |
| 0x20 | TCA9554 IO 扩展 |
| 0x34 | AXP2101 PMU |
| 0x51 | PCF85063 RTC |
| 0x6B | QMI8658 IMU |

---

## 2. 引脚映射(`pin_config.h`)

```
LCD  QSPI : CS=12  SCLK=11  SDIO0=4 SDIO1=5 SDIO2=6 SDIO3=7
I2C       : SDA=15  SCL=14   TP_INT=21
ES8311    : MCLK=16  BCK=9  WS=45  DO=10  DI=8  PA=46
SD        : SDMMC_CLK=2  SDMMC_CMD=1  SDMMC_DATA=3
BOOT      : GPIO0
```

> 注意:QSPI 显示占用 SPI2 的 4 根数据线;SD 用专用 SDMMC 接口(引脚 1/2/3),
> 与 SPI 不冲突。空闲 GPIO(如 17/18 用于 GPS UART)可直接使用。

---

## 3. V2 板关键特性(CO5300 + CST816/CST820)

### 3.1 显示:CO5300

- 分辨率 368×448(竖屏),GRAM 为 480×480 → **可见窗口偏移 16px**(列)
- QSPI 协议要点(裸驱动实测):
  - 命令:opcode `0x02` + 24 位地址(`reg<<8`),`SPI_TRANS_MULTILINE_CMD|ADDR`
  - 像素:opcode `0x32` + addr `0x003C00`,`SPI_TRANS_MODE_QIO`,`length = 字节数×8`
  - **CS 必须整段保持低电平**(中途抬起会中断 RAMWR 连续写 → 屏幕绿线)
  - 像素字节序:大端 RGB565(LVGL 渲染后 `lv_draw_sw_rgb565_swap` 再送出)
- 初始化表(官方 BSP):`0xFE:00, 0xC4:80, 0x3A:55, 0x35:00, 0x53:20, 0x51:FF, 0x63:FF, 0x2A:0000016F, 0x2B:000001BF, 0x11(+100ms), 0x29`
- 亮度:寄存器 `0x51`(0–255)
- 四角圆角:物理玻璃圆角,安全区设计见 §6

### 3.2 触摸:CST816 / CST820

| 项 | 值 |
|---|---|
| I2C 地址 | 0x15(两代相同) |
| 芯片 ID 寄存器 | 0xA7:CST816S=0xB4 / CST816T=0xB5 / CST816D=0xB6 / **CST820=0xB7** |
| 坐标寄存器 | 0x03/0x04=X,0x05/0x06=Y(12bit) |
| 固件 | 不同芯片 OEM 固件坐标映射**可能不同** |

**⚠️ CST820 Y 轴放大(必须修正)**:
实测 CST820 固件把 448px 屏映射到约 512 的 Y 范围:
```
rawY ≈ (屏幕Y + 15) × 8/7     →  修正: displayY = (rawY + 15) × 7/8
```
X 轴 1:1。**只对 chip==0xB7 应用**(CST816S/T/D 报 1:1,修了反而错)。
另需 `disableAutoSleep()` 保持轮询响应。标定数据与推导见
`1x1-flappy/TOUCH_FIX_NOTES.md`。

### 3.3 与 V1 的差异

| | V1 | V2 |
|---|---|---|
| 显示驱动 | SH8601 | CO5300 |
| 触摸 | FT3168 @0x38 | CST816/CST820 @0x15 |
| 识别方式 | 默认 | I2C 探测 0x15(有应答=V2) |

`hw_panel.cpp` 已实现 V1/V2 自动探测(V1 用 INT 门控,V2 直接轮询)。

---

## 4. 电源 / 按键(AXP2101)

| 功能 | 实现 |
|---|---|
| PMU 初始化 | `power.begin(i2c, 0x34)`;`disableIRQ(ALL)`;`enableIRQ(PKEY_SHORT_IRQ)` |
| 电池检测 | `enableBattDetection()` + `enableBattVoltageMeasure()` |
| 电量 | `getBatteryPercent()`(未接电池返回 -1,需先判 `isBatteryConnect()`) |
| 充电 | `isCharging()`(STATUS2 的 [6:5]==01);**满电时不充电,属正常** |
| PEK 单击/双击 | IRQ 计数 + 400ms 窗口解码(单=功能键,双=锁屏),避免双重消费竞态 |
| 关机 | `power.shutdown()`(硬件长按 PEK 也默认关机) |

> XPowersLib IDF 新接口需 `-DCONFIG_XPOWERS_ESP_IDF_NEW_API`(走
> `i2c_master_bus_handle_t`);Arduino 侧用 `TwoWire&`。

---

## 5. 软件栈与构建

### 5.1 两套已验证的移植参考

| 项目 | 框架 | 依赖 | 状态 |
|---|---|---|---|
| `1x1-flappy` | arduino-cli + esp32 core **3.3.11** | GFX 1.6.7 / SensorLib / XPowersLib | 触摸修复完成,纯游戏版 |
| `furble` | PlatformIO + **ESP-IDF 5.4** | LVGL 9.4 / NimBLE-cpp 2.5 / SensorLib / XPowersLib | 相机快门/对焦实测通过 |

### 5.2 构建环境

- PlatformIO 核心(平台/工具链/缓存)已迁移到 **`/mnt/ftp/esp/.pio-core`**,
  使用前 `export PLATFORMIO_CORE_DIR=/mnt/ftp/esp/.pio-core`
- furble IDF env:`pio run -e waveshare-esp32s3-amoled18`
- 合并固件:`tools/make_merged.sh`(自动定位 esptool.py 与 python)

### 5.3 关键 sdkconfig(仅 furble IDF)

```
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=240     # 双核 240MHz(继承的 80 会覆盖 board 配置)
CONFIG_ESP32S3_DEFAULT_CPU_FREQ_MHZ=240
# CONFIG_FREERTOS_UNICORE is not set     # 双核
CONFIG_SPIRAM=y / MODE_OCT / USE_CAPS_ALLOC   # 8MB OPI PSRAM
CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y    # 日志走 USB-C(默认 UART0 看不到)
CONFIG_ESPTOOLPY_FLASHMODE="dio"        # 镜像 DIO(见 §7 烧录)
CONFIG_XPOWERS_ESP_IDF_NEW_API=y
```

---

## 6. UI / 安全区设计规范

> 四角圆角 → 需要安全区(参考 1x1-flappy 与 furble 的实际布局)。

| 区域 | 预留 | 说明 |
|---|---|---|
| 四角圆角区 | 单边 **20px** | 不放核心功能按钮或重要文字 |
| 左右直线边 | **10px** | 避开物理外壳边框的视觉压迫 |
| 上/下直线边 | **10px** | 顶部适合弧形/居中状态栏,底部放指示条 |

控件最小尺寸:**≥ 104×62 px**(= 1x1 数字键尺寸)。
全屏渲染(见 §7)时用 PSRAM 帧缓冲,避免部分渲染的绿线带/残影。

---

## 7. 必踩坑清单(经验教训)

1. **烧录合并必须 `--flash_mode dio`**
   `esptool merge_bin --flash_mode qio` 会把 bootloader 头从 DIO(0x02)改写为
   QIO(0x00),该 flash 在 ROM/bootloader 阶段跑 QIO 会 TG0WDT 开机循环
   (`ets_loader.c` 反复复位)。
2. **IDF 4.4 的 bootloader 不兼容此 flash**
   arduino-esp32 2.0.17(PlatformIO Arduino 框架)自带 bootloader 开机循环;
   用 ESP-IDF 5.4+ 的 bootloader 正常。合并时可直接用 IDF 构建产出的
   `build/waveshare-esp32s3-amoled18/bootloader.bin`。
3. **LVGL 显示绿线/残影**
   小缓冲 + 部分渲染会在漏区显示未初始化 GRAM(绿)与旧帧残留。
   解决:**全屏渲染模式 + 单 PSRAM 帧缓冲**(368×448×2 ≈ 330KB)。
4. **CO5300 像素突发必须保持 CS 低** — 见 §3.1。
5. **UI 任务栈**:BLE + LVGL 初始化需要大栈,放到独立 32KB 任务,
   不要在 Arduino loopTask(默认 8KB)上跑。
6. **SPI 传输 ≤ max_transfer_sz**(默认 4096 字节),大块需分包。
7. **日志控制台**:IDF 默认 UART0(GPIO43/44)未接 USB,必须
   `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` 才能在 USB-C 上看日志。
8. **CPU 频率**:sdkconfig 里的 `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=80`
   (继承自 M5StickS3 配置)会覆盖 `board_build.f_cpu`;需显式设 240。
9. **CST820 触摸 Y 轴**修正只对 0xB7 生效 — 见 §3.2。
10. **分辨率/字节序**:LVGL 大端 RGB565;驱动原样送;不要二次交换。

---

## 8. 官方参考资源

- 硬件页:https://www.waveshare.com/esp32-s3-touch-amoled-1.8.htm
- 官方仓库(含 BSP 与 arduino-v2 例程):
  https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.8
- ESP-IDF BSP 组件:`waveshare/esp32_s3_touch_amoled_1_8`(显示 CO5300 +
  触摸 CST816S/FT5x06 + esp_lvgl_port)
- 本文档配套项目:
  - `slgray/furble`(BLE 相机遥控,IDF 5.4 移植)
  - `hexbyte42-bot/1x1-flappy`(纯 flappy 游戏,触摸修复参考)
