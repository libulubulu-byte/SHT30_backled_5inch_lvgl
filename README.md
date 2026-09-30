| Supported Targets | ESP32-P4 | ESP32-S3 | ESP32-C3 |
| ----------------- | -------- | -------- | -------- |

# 调试记录：RGB 面板（JC8048W550 / ESP32-S3）

本文档记录本板在 RGB 接口下踩过的两个关键问题及解决方法。**改配置前请先读完本节**，
很多参数之间存在强耦合，单独修改某一项会立刻引入异常。

## 问题一：屏幕花屏 / 闪烁 / 明暗跳（本工程的核心问题）

### 现象

面板显示出现以下一种或多种异常：

- 竖向或横向的**撕裂**（上半屏是旧帧、下半屏是新帧）
- 整屏**明暗跳变**（周期性亮度抖动）
- 画面**闪烁**，尤其动画区域
- 有时表现为**启动即花屏**，条纹固定不动

### 根因

这不是单一原因，而是 **LVGL 刷新率、面板扫描率、缓冲策略** 三者不匹配的组合结果。
本板最终定位到三个独立成因：

**成因 1：绘制缓冲的分配位置被写死为内部 RAM**

`esp_lv_adapter` 在 `TRIPLE_PARTIAL` 模式下走的是一条独立分支
（`managed_components/espressif__esp_lvgl_adapter/src/display/display_manager.c`
的 `display_manager_setup_draw_buffers` 中 `case ESP_LV_ADAPTER_TEAR_AVOID_MODE_TRIPLE_PARTIAL`）：

```c
cfg->draw_buf_pixels = (size_t)profile->hor_res * profile->buffer_height;
void *buf = display_manager_alloc_draw_buffer(cfg->draw_buf_pixels * color_size, false);
//                                                                                  ^^^^^ 硬编码 false
return true;   /* 直接返回，永远不会执行下面读取 profile->use_psram 的代码 */
```

结论：**`TRIPLE_PARTIAL` 模式下 `profile.use_psram` 完全无效**，绘制缓冲只能来自内部 RAM。
默认宏里 `buffer_height = 50`，800×50×2 = **80000 字节**，内部 RAM 拿不出这么大的连续块，
启动日志会直接报：

```
E (1110) esp_lvgl:disp: alloc primary buffer 80000 bytes failed
E (1112) main: Failed to register display
```

此时屏幕**根本没有被 LVGL 驱动**，显示的是面板裸扫描未初始化 fb 的花屏。

**成因 2：LVGL 刷新率与面板扫描率失配**

- 面板扫描率 = `pclk / (总行像素 × 总行数)`。本板 832×496（800+32 水平消隐，480+16 垂直消隐）。
- LVGL 刷新率 = `1000 / CONFIG_LV_DEF_REFR_PERIOD`(ms)。

若 LVGL 刷新率**远高于**面板扫描率，LVGL 会持续提交新帧，而面板还在扫上一帧，
表现为**持续闪烁**；若**远低于**，动画则明显卡顿。

**成因 3：DOUBLE_DIRECT 模式的撕裂陷阱（已弃用该方案）**

曾尝试 `DOUBLE_DIRECT` 以求零拷贝省带宽，实测出现"上下闪"（垂直撕裂）。根因是
该模式让 LVGL 直接渲染进面板帧缓冲，而 bounce buffer 分批从 fb 拷贝输出、
**拷贝时才读取 fb 指针**，导致切换 fb 的瞬间上半屏是旧帧、下半屏是新帧。
去掉 bounce buffer 则退回整屏明暗跳（PSRAM 带宽争抢）。

### 解决方法

**（1）显式下调绘制缓冲高度**，使其能放进内部 RAM。在 `main.c` 的 RGB 分支中：

```c
esp_lv_adapter_display_config_t display_config = ESP_LV_ADAPTER_DISPLAY_CONFIG(
        display_panel, display_io_handle,
        ESP_LV_ADAPTER_DISPLAY_PROFILE_RGB_DEFAULT_CONFIG(HW_LCD_H_RES, HW_LCD_V_RES, rotation),
        tear_avoid_mode, ESP_LV_ADAPTER_TE_SYNC_DISABLED());
display_config.profile.buffer_height = 20;   /* 800*20*2 = 32000 字节，内部 RAM 可分配 */
```

> 注意：不能用 `ESP_LV_ADAPTER_DISPLAY_RGB_DEFAULT_CONFIG`，它内部硬编码的
> `TEAR_AVOID_MODE_DEFAULT_RGB` 与 `hw_lcd_init` 传入的模式可能不一致，
> 会导致面板侧 `num_fbs` 与适配器侧 `required` 不符而触发 `StoreProhibited` 崩溃。
> **必须显式传入与 `hw_lcd_init` 相同的 `tear_avoid_mode`。**

**（2）改用 `TRIPLE_PARTIAL` 撕裂避免模式。** 该模式下 LVGL 渲染进自身缓冲
（只处理脏区），不与面板扫描争抢同一块 fb，配合 bounce buffer 可同时避开撕裂与明暗跳。

**（3）协同调整 pclk 与 LVGL 刷新周期。** 两者必须配套修改：

| 位置 | 宏 / 配置项 | 当前值 | 含义 |
| --- | --- | --- | --- |
| `common/hw_init/lcd/lcd_init_rgb.c` | `HW_LCD_PIXEL_CLOCK_HZ` | `18MHz` | 面板扫描率 ≈ 43.6Hz |
| `sdkconfig.defaults` | `CONFIG_LV_DEF_REFR_PERIOD` | `22` (ms) | LVGL 刷新率 ≈ 45Hz |

**只改其中一个必然重新引入闪烁**，请成对调整。

**（4）正确的色深与刷新配置。** `sdkconfig.defaults` 中：

- `CONFIG_LV_COLOR_FORMAT_RGB565=y` —— 面板是 16bit，必须用 16 位色深。
  32bpp 时绘制缓冲为 800×50×4 = 160KB，内部 RAM 同样分配失败。
  注意只能改 `LV_COLOR_FORMAT_DEFAULT`，`CONFIG_LV_COLOR_DEPTH` 是 Kconfig 派生的 int，
  且 `LV_COLOR_DEPTH_CHOICE` 已标记 DEPRECATED，直接设无效。

### 验证

烧录后串口**不应再出现** `alloc primary buffer ... failed` 或 `Failed to register display`，
屏幕应正常显示 UI 且无可见闪烁。可用 `CONFIG_ESP_LVGL_ADAPTER_ENABLE_FPS_STATS=y`
观察实际帧率。

---

## 问题二：触摸导致 Task WDT 复位

### 现象

操作触摸时偶发重启，日志出现 `Task watchdog got triggered`，指向 IDLE 任务饿死。

### 根因

`esp_lcd_touch` 读 GT911 时若 I2C 总线挂死，底层 `esp_lcd` I2C 传输为**无限等待**，
LVGL 任务被阻塞会导致 IDLE 无法运行而触发 WDT。

### 解决方法

已做以下处理，并保留 `CONFIG_ESP_TASK_WDT_TIMEOUT_S=10` 略增大超时以减少误报：

- 降低 I2C 速率至 100kHz
- 确认 SDA/SCL 上拉电阻正常

根本解决仍依赖硬件侧 I2C 稳定性。

---

## 移植到其他 RGB 屏（客户换屏时怎么做）

### 结论

**若客户用同样 800×480 的 RGB 屏，原则上只改参数即可，无需改架构。**
但参数不是一个，而是分布在 3 个文件、共 6 组，且彼此有联动关系，务必按顺序改。

### 移植改动清单

所有硬件参数已集中在 `common/hw_init/lcd/lcd_init_rgb.c` 顶部的
**「RGB 面板移植参数区」**，按 6 个分组排列，换屏时从上往下逐组对照屏手册修改：

| 分组 | 内容 | 依据 | 是否必改 |
| --- | --- | --- | --- |
| 1 | `HW_LCD_PIXEL_CLOCK_HZ` | 屏手册 DCLK/PCLK 频率 | **必改** |
| 2 | `HW_LCD_HSYNC/HBP/HFP/VSYNC/VBP/VFP` | 屏手册消隐区 | **必改** |
| 3 | 极性宏 + 颜色顺序 | 屏手册时序表 / 试色结果 | **必改** |
| 4 | 背光、DISP 引脚 | PCB 原理图 | 视板子 |
| 5 | 16 根 RGB 数据线 + 同步线引脚 | PCB 原理图 | **必改** |
| 6 | `HW_LCD_DATA_WIDTH` / `BOUNCE_BUFFER_HEIGHT` | 一般沿用 | 视情况 |

除上述文件外，还需同步：

| 文件 | 项 | 说明 |
| --- | --- | --- |
| `common/hw_init/hw_init.h` | `HW_LCD_H_RES` / `HW_LCD_V_RES` | 分辨率不同才改（RGB 分支，约 24 行） |
| `sdkconfig.defaults` | `CONFIG_LV_DEF_REFR_PERIOD` | **必须与分组 1 的 PCLK 配套重算** |
| `main/main.c` | `display_config.profile.buffer_height` | 分辨率变化时按内部 RAM 上限调整 |

### 三条最容易踩的坑

**坑 1：时序不能照抄，必须查屏手册。**
当前这套（front=8, pulse=4, back=8）是从 **JC8048W550 屏厂 Arduino 例程**抠出来的，
换一块屏基本不能直接用。消隐区抄错的表现是花屏、黑屏或画面偏移。

**坑 2：PCLK 与刷新周期必须成对改。**
这是本次调试的核心坑，两者失配会立刻闪烁：

```
面板扫描率 = PCLK / (总行像素 × 总行数)
LVGL 刷新率 = 1000 / CONFIG_LV_DEF_REFR_PERIOD
```

当前配置：PCLK 18MHz → 扫描率 ≈ 43.9Hz，配 `LV_DEF_REFR_PERIOD=22`(≈45Hz)。
**换屏后应先按新手册算出扫描率，再反推 `LV_DEF_REFR_PERIOD`。**
`lcd_init_rgb.c` 分组 1 的注释里写了自检方法。

**坑 3：分辨率提高会撞上内部 RAM 天花板（架构约束）。**
`TRIPLE_PARTIAL` 模式下绘制缓冲**强制走内部 RAM**：

```
缓冲占用 = H_RES × buffer_height × 2   ≤  内部 RAM 可用连续块
```

本板 800 宽 / 20 行 = 32KB 安全。若客户换 **1024×600** 的屏，
同样 20 行就是 40KB（51KB），很可能直接启动失败，报
`alloc primary buffer ... failed`。此时**不是改参数能解决的**，需要：

- 降低 `buffer_height`（如 1024 宽取 12~16 行）先跑起来，代价是刷新性能下降
- 或改用不强制内部 RAM 的撕裂避免模式 / 自行修改适配器走 PSRAM 路径

**换屏前务必先确认分辨率。同样是"5 寸屏"，1024×600 和 800×480 的处理方式完全不同。**

---

## 关键参数速查

| 参数 | 位置 | 值 | 改动影响 |
| --- | --- | --- | --- |
| `HW_LCD_PIXEL_CLOCK_HZ` | `lcd_init_rgb.c` 分组 1 | 18MHz | 面板扫描率；改后须同步改刷新周期 |
| `CONFIG_LV_DEF_REFR_PERIOD` | `sdkconfig.defaults` | 22 | LVGL 刷新率；须与 pclk 配套 |
| `profile.buffer_height` | `main.c` | 20 | 绘制缓冲高度；过大会导致内部 RAM 分配失败 |
| `HW_LCD_BOUNCE_BUFFER_HEIGHT` | `lcd_init_rgb.c` 分组 6 | 40 | DMA 灌数窗口；影响断流闪烁 |
| `tear_avoid_mode` | `main.c` + `hw_lcd_init` | `TRIPLE_PARTIAL` | 必须两处一致 |

---

# LVGL Common Demo (Benchmark)

This example demonstrates how to use the `esp_lvgl_adapter` component with LVGL to run the official LVGL benchmark demo on various LCD interfaces. It serves as a reference implementation for integrating LVGL with different display types in a unified way.

## Overview

The example showcases:
- **Unified LCD interface support**: Works with MIPI DSI, RGB, QSPI, and SPI LCD panels through a single codebase
- **LVGL benchmark demo**: Runs the standard LVGL benchmark to measure rendering performance
- **Multiple input methods**: Supports both touch panels and rotary encoders
- **FPS monitoring**: Optional frame rate statistics for performance analysis
- **Display rotation**: Configurable screen orientation (0°/90°/180°/270°)
- **Tear avoidance**: Built-in support for tearing prevention mechanisms

This example is ideal for:
- Evaluating display performance with different LCD interfaces
- Verifying hardware setup and driver configuration
- Benchmarking LVGL rendering capabilities
- Learning the basic structure of LVGL applications with `esp_lvgl_adapter`

## How to Use the Example

### Hardware Required

* An ESP32-P4, ESP32-S3, or ESP32-C3 development board
* A LCD panel with one of the supported interfaces:
  - **MIPI DSI**: For high-resolution displays (e.g., 1024x600)
  - **RGB**: For parallel RGB interface displays (e.g., 800x480)
  - **QSPI**: For quad-SPI displays (e.g., 360x360, 400x400)
  - **SPI**: For standard SPI displays (e.g., 240x240, 320x240)
* (Optional) Touch panel or rotary encoder for input
* A USB cable for power supply and programming

**Recommended Hardware Combinations:**

| Chip | LCD Interface | Development Board |
|------|---------------|-------------------|
| ESP32-P4 | MIPI DSI | [ESP32-P4-Function-EV-Board](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32p4/esp32-p4-function-ev-board/index.html) |
| ESP32-S3 | RGB | [ESP32-S3-LCD-EV-Board](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32s3/esp32-s3-lcd-ev-board/index.html) |
| ESP32-S3 | QSPI | [EchoEar](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32s3/echoear/index.html) |
| ESP32-S3 | SPI | [ESP32-S3-BOX-3](https://github.com/espressif/esp-box/blob/master/docs/hardware_overview/esp32_s3_box_3/hardware_overview_for_box_3.md) |
| ESP32-C3 | SPI | [ESP32-C3-LCDkit](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32c3/esp32-c3-lcdkit/index.html) |

### Hardware Connection

The LCD and touch panel connections depend on your hardware configuration. This example uses the `hw_init` component which provides hardware abstraction for different board configurations.

**Common interface options:**
- **MIPI DSI**: Typically uses dedicated MIPI lanes (D0+/-, D1+/-, CLK+/-)
- **RGB**: Uses parallel data lines (RGB565: 16 data pins + HSYNC/VSYNC/DE/PCLK)
- **QSPI**: Uses 4 data lines (IO0-IO3 + CLK + CS)
- **SPI**: Uses standard SPI pins (MOSI/MISO/CLK + CS + DC)

**Input devices:**
- Touch panels typically use I2C or SPI interface
- Rotary encoders use 3 GPIO pins (A, B, and button)

Refer to your board's schematic or the `hw_init` component configuration for specific GPIO mappings.

### Configure the Project

Run `idf.py menuconfig` and navigate to `Example Configuration`:

**LCD Interface Selection:**
- `LCD Interface Type`: Choose between MIPI DSI, RGB, QSPI, or SPI

**Display Settings:**
- `Display Rotation`: Select screen orientation (0°/90°/180°/270°)
- LCD resolution and timing parameters (configured in `hw_init`)

**Input Device:**
- Choose between touch panel or encoder input
- Configure I2C/SPI parameters for touch controller

**Performance Options:**
- `Enable FPS Statistics`: Enable to monitor frame rate in logs

### Build and Flash

1. Set the target chip:
```bash
idf.py set-target esp32p4
# or
idf.py set-target esp32s3
# or
idf.py set-target esp32c3
```

2. Build, flash and monitor:
```bash
idf.py -p PORT build flash monitor
```

(To exit the serial monitor, type ``Ctrl-]``.)

The first time you run `idf.py` for the example will take extra time as the build system needs to download components from the registry into the `managed_components` folder.

See the [Getting Started Guide](https://docs.espressif.com/projects/esp-idf/en/latest/get-started/index.html) for full steps to configure and use ESP-IDF to build projects.

### Expected Output

After flashing, the LCD should display the LVGL benchmark demo, which includes various animations and UI elements to stress-test rendering performance. The serial console will show initialization logs:

```
I (xxx) main: Selected LCD interface: MIPI DSI
I (xxx) main: Initializing LCD: 1024x600
I (xxx) main: Starting LVGL benchmark demo
```

If FPS statistics are enabled, you will see periodic frame rate reports:
```
I (xxx) main: Current FPS: 45
```

The benchmark will cycle through different test scenes automatically, displaying shapes, images, text, and animations.

## Code Structure

- **`main.c`**: Main application logic
  - Hardware initialization using `hw_init` component
  - `esp_lvgl_adapter` configuration and display registration
  - Input device setup (touch or encoder)
  - LVGL benchmark demo launch

- **`hw_init` component**: Hardware abstraction layer
  - LCD interface initialization
  - GPIO and peripheral configuration
  - Touch/encoder driver setup

## Key Features Demonstrated

1. **Unified LCD API**: Single codebase works with multiple LCD interface types
2. **LVGL Integration**: Proper initialization and task management for LVGL
3. **Input Handling**: Touch and encoder input device registration
4. **Performance Monitoring**: Optional FPS statistics using adapter features
5. **Thread Safety**: Mutex-based protection for LVGL API calls

## Troubleshooting

**Display shows nothing:**
- Verify LCD power and backlight connections
- Check GPIO pin mappings in menuconfig
- Ensure the correct LCD interface type is selected
- Verify LCD initialization sequence for your panel

**Touch/encoder not responding:**
- Check I2C/GPIO connections
- Enable input device in menuconfig
- Verify touch controller I2C address

**Build errors:**
- Ensure ESP-IDF version is 5.5.0 or later
- Run `idf.py fullclean` and rebuild
- Check that all managed components downloaded correctly

For any technical queries, please open an [issue](https://github.com/espressif/esp-iot-solution/issues) on GitHub. We will get back to you soon.

# SHT30_backled_5inch_lvgl
