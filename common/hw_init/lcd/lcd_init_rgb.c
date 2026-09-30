#include "hw_init.h"

#if CONFIG_EXAMPLE_LCD_INTERFACE_RGB

#include "esp_lcd_panel_rgb.h"
#include "driver/gpio.h"

static const char *TAG = "hw_lcd_init";

/*==============================================================================
 *                        RGB 面板移植参数区（换屏只改这里）
 *==============================================================================
 *
 * 换一块 RGB 屏时，请按下面 6 个分组逐项对照新屏的数据手册修改。
 * 除本区块外，还需同步修改：
 *   - hw_init.h        :  HW_LCD_H_RES / HW_LCD_V_RES（若分辨率不同）
 *   - sdkconfig.defaults: CONFIG_LV_DEF_REFR_PERIOD（必须与 PCLK 配套，见下）
 *   - main.c           :  display_config.profile.buffer_height（受分辨率影响）
 *
 * ┌─── 分组 1：刷新率（最重要的联动项）───────────────────────────────────────┐
 * │ 面板扫描率 = PCLK / (总行像素 × 总行数)                                  │
 * │            = PCLK / ((H_RES+HBP+HFP+HSYNC) × (V_RES+VBP+VFP+VSYNC))      │
 * │ LVGL 刷新率 = 1000 / CONFIG_LV_DEF_REFR_PERIOD                           │
 * │                                                                          │
 * │ 两者必须【接近】：                                                        │
 * │   LVGL 远快于面板 -> 持续提交新帧而面板还在扫旧帧 -> 画面闪烁             │
 * │   LVGL 远慢于面板 -> 动画明显卡顿                                        │
 * │                                                                          │
 * │ 改 PCLK 后务必按同样比例调整 CONFIG_LV_DEF_REFR_PERIOD，                  │
 * │ 例：PCLK 提高 1.5 倍，刷新周期应缩短到原来的 1/1.5。                      │
 * └──────────────────────────────────────────────────────────────────────────┘
 *
 * 当前值来源：屏厂 JC8048W550 的 Arduino 例程 7_1_lvgl_music_gt911_5.0
 *            （Arduino_RPi_DPI_RGBPanel 参数）。
 *            例程原始 PCLK 为 12MHz，此处提高到 18MHz 以提升扫描率。
 *            800x480, hsync/vsync_polarity=0, pclk_active_neg=1
 */

/*--- 分组 1：像素时钟（单位 Hz，按屏手册 "DCLK/PCLK frequency" 填标称值）---*/
#define HW_LCD_PIXEL_CLOCK_HZ                   (18 * 1000 * 1000)
/* 当前扫描率自检：
 *   总行像素 = 800 + 8(HBP) + 8(HFP) + 4(HSYNC) = 820
 *   总行数   = 480 + 8(VBP) + 8(VFP) + 4(VSYNC) = 500
 *   扫描率   = 18MHz / (820 × 500) ≈ 43.9Hz
 * 对应 CONFIG_LV_DEF_REFR_PERIOD = 22ms (≈45Hz)，见 sdkconfig.defaults。 */

/*--- 分组 2：水平/垂直时序（按屏手册消隐区填，单位：像素/行）---*/
#define HW_LCD_HSYNC                            (4)     /* hsync pulse width */
#define HW_LCD_HBP                              (8)     /* hsync back porch  */
#define HW_LCD_HFP                              (8)     /* hsync front porch */
#define HW_LCD_VSYNC                            (4)     /* vsync pulse width */
#define HW_LCD_VBP                              (8)     /* vsync back porch  */
#define HW_LCD_VFP                              (8)     /* vsync front porch */

/*--- 分组 3：数据与时序极性（若出现红蓝颠倒/画面偏移，优先改这里）---*/
/* 对齐屏厂例程：hsync_polarity=0 / vsync_polarity=0，即同步信号空闲时为低电平；
 * pclk_active_neg=1 表示下降沿采样。
 * 不同屏厂可能相反，改错表现为：画面整体偏移、抖动、或颜色错乱。 */
#define HW_LCD_HSYNC_IDLE_LOW                   (true)
#define HW_LCD_VSYNC_IDLE_LOW                   (true)
#define HW_LCD_PCLK_ACTIVE_NEG                  (true)
/* 数据线颜色顺序：RGB565 下若红蓝互换，把 LCD_RGB_ELEMENT_ORDER_RGB
 * 改为 LCD_RGB_ELEMENT_ORDER_BGR（该宏在 hw_init.h 中定义）。 */

/*--- 分组 4：背光与面板控制引脚 ---*/
#define HW_LCD_RGB_BL                           (GPIO_NUM_2)
#define HW_LCD_RGB_DISP                         (GPIO_NUM_NC)

/*--- 分组 5：RGB 数据与同步信号引脚（必须对照 PCB 原理图逐根核对）---*/
#define HW_LCD_RGB_VSYNC                        (GPIO_NUM_41)
#define HW_LCD_RGB_HSYNC                        (GPIO_NUM_39)
#define HW_LCD_RGB_DE                           (GPIO_NUM_40)
#define HW_LCD_RGB_PCLK                         (GPIO_NUM_42)
#define HW_LCD_RGB_DATA0                        (GPIO_NUM_8)
#define HW_LCD_RGB_DATA1                        (GPIO_NUM_3)
#define HW_LCD_RGB_DATA2                        (GPIO_NUM_46)
#define HW_LCD_RGB_DATA3                        (GPIO_NUM_9)
#define HW_LCD_RGB_DATA4                        (GPIO_NUM_1)
#define HW_LCD_RGB_DATA5                        (GPIO_NUM_5)
#define HW_LCD_RGB_DATA6                        (GPIO_NUM_6)
#define HW_LCD_RGB_DATA7                        (GPIO_NUM_7)
#define HW_LCD_RGB_DATA8                        (GPIO_NUM_15)
#define HW_LCD_RGB_DATA9                        (GPIO_NUM_16)
#define HW_LCD_RGB_DATA10                       (GPIO_NUM_4)
#define HW_LCD_RGB_DATA11                       (GPIO_NUM_45)
#define HW_LCD_RGB_DATA12                       (GPIO_NUM_48)
#define HW_LCD_RGB_DATA13                       (GPIO_NUM_47)
#define HW_LCD_RGB_DATA14                       (GPIO_NUM_21)
#define HW_LCD_RGB_DATA15                       (GPIO_NUM_14)

/*--- 分组 6：总线位宽与 DMA bounce buffer ---*/
#define HW_LCD_DATA_WIDTH                       (16)    /* RGB565 并行数据线数 */
#define HW_LCD_BIT_PER_PIXEL                    (16)    /* 每像素位数，与色深一致 */
/* bounce buffer 高度（行）。DMA 分批从 fb 搬运到面板，块越大灌数窗口越长，
 * 越不容易因总线争抢而断流；但占用内部 DMA 内存也越多。
 * 屏厂例程绘制缓冲为 800*480/8 = 48000px（40 行），此处取同量级。
 * 注意：PCLK 提高后单行时间缩短，若出现断流闪烁可适当加大该值。 */
#define HW_LCD_BOUNCE_BUFFER_HEIGHT             (40)

static esp_lcd_panel_handle_t s_panel_handle;

esp_err_t hw_lcd_init(esp_lcd_panel_handle_t *panel_handle, esp_lcd_panel_io_handle_t *io_handle, esp_lv_adapter_tear_avoid_mode_t tear_avoid_mode, esp_lv_adapter_rotation_t rotation)
{
    ESP_LOGI(TAG, "Initialize RGB panel");
    gpio_config_t bk_gpio_config = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << HW_LCD_RGB_BL};
    ESP_ERROR_CHECK(gpio_config(&bk_gpio_config));

    esp_lcd_rgb_panel_config_t panel_conf = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .dma_burst_size = 64,
        .data_width = HW_LCD_DATA_WIDTH,
        .bits_per_pixel = HW_LCD_BIT_PER_PIXEL,
        .de_gpio_num = HW_LCD_RGB_DE,
        .pclk_gpio_num = HW_LCD_RGB_PCLK,
        .vsync_gpio_num = HW_LCD_RGB_VSYNC,
        .hsync_gpio_num = HW_LCD_RGB_HSYNC,
        .disp_gpio_num = HW_LCD_RGB_DISP,
        .data_gpio_nums = {
            HW_LCD_RGB_DATA0,
            HW_LCD_RGB_DATA1,
            HW_LCD_RGB_DATA2,
            HW_LCD_RGB_DATA3,
            HW_LCD_RGB_DATA4,
            HW_LCD_RGB_DATA5,
            HW_LCD_RGB_DATA6,
            HW_LCD_RGB_DATA7,
            HW_LCD_RGB_DATA8,
            HW_LCD_RGB_DATA9,
            HW_LCD_RGB_DATA10,
            HW_LCD_RGB_DATA11,
            HW_LCD_RGB_DATA12,
            HW_LCD_RGB_DATA13,
            HW_LCD_RGB_DATA14,
            HW_LCD_RGB_DATA15,
        },
        .timings = {
            .pclk_hz = HW_LCD_PIXEL_CLOCK_HZ,
            .h_res = HW_LCD_H_RES,
            .v_res = HW_LCD_V_RES,
            .hsync_back_porch = HW_LCD_HBP,
            .hsync_front_porch = HW_LCD_HFP,
            .hsync_pulse_width = HW_LCD_HSYNC,
            .vsync_back_porch = HW_LCD_VBP,
            .vsync_front_porch = HW_LCD_VFP,
            .vsync_pulse_width = HW_LCD_VSYNC,
            .flags = {
                /* 极性宏见本文件顶部「分组 3」 */
                .hsync_idle_low = HW_LCD_HSYNC_IDLE_LOW,
                .vsync_idle_low = HW_LCD_VSYNC_IDLE_LOW,
                .pclk_active_neg = HW_LCD_PCLK_ACTIVE_NEG,
            },
        },
        .flags.fb_in_psram = 1,
        .num_fbs = esp_lv_adapter_get_required_frame_buffer_count(tear_avoid_mode, rotation),
        .bounce_buffer_size_px = HW_LCD_H_RES * HW_LCD_BOUNCE_BUFFER_HEIGHT,
    };
    ESP_ERROR_CHECK(esp_lcd_new_rgb_panel(&panel_conf, &s_panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel_handle));

    *panel_handle = s_panel_handle;
    if (io_handle) {
        *io_handle = NULL;
    }
    gpio_set_level(HW_LCD_RGB_BL, 1);

    return ESP_OK;
}

#endif
