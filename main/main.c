/**
 * @file main.c
 * @brief Unified LVGL demo supporting multiple LCD interface types
 *
 * This example demonstrates how to use LVGL with different LCD interfaces
 * (MIPI DSI, QSPI, RGB, SPI) in a single unified codebase.
 * The interface type and hardware configuration can be selected via menuconfig.
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "sdkconfig.h"
#include "hw_init.h"
#include "esp_lv_adapter.h"
#include "ui/generated/gui_guider.h"
#include "app_features.h"
#if APP_ENABLE_COLOR_TEST
#include "driver/gpio.h"
#include "iot_button.h"
#include "button_gpio.h"
#include "color_test.h"
#endif

static const char *TAG = "main";

lv_ui guider_ui;

#if APP_ENABLE_COLOR_TEST
#define GPIO0_KEY_GPIO       GPIO_NUM_0
#endif

/* FPS monitor task configuration */
#define FPS_MONITOR_TASK_STACK_SIZE    4096
#define FPS_MONITOR_TASK_PRIORITY      3
#define FPS_MONITOR_INTERVAL_MS        1000

/**
 * @brief Get the configured display rotation from Kconfig
 *
 * @return esp_lv_adapter_rotation_t Rotation angle
 */
static esp_lv_adapter_rotation_t get_configured_rotation(void)
{
#if CONFIG_EXAMPLE_DISPLAY_ROTATION_0
    return ESP_LV_ADAPTER_ROTATE_0;
#elif CONFIG_EXAMPLE_DISPLAY_ROTATION_90
    return ESP_LV_ADAPTER_ROTATE_90;
#elif CONFIG_EXAMPLE_DISPLAY_ROTATION_180
    return ESP_LV_ADAPTER_ROTATE_180;
#elif CONFIG_EXAMPLE_DISPLAY_ROTATION_270
    return ESP_LV_ADAPTER_ROTATE_270;
#else
    return ESP_LV_ADAPTER_ROTATE_0;
#endif
}

#if CONFIG_ESP_LVGL_ADAPTER_ENABLE_FPS_STATS
/**
 * @brief Task to monitor and log FPS statistics
 *
 * @param arg Pointer to lv_display_t
 */
static void fps_monitor_task(void *arg)
{
    lv_display_t *disp = (lv_display_t *)arg;
    uint32_t fps;

    while (1) {
        if (esp_lv_adapter_get_fps(disp, &fps) == ESP_OK) {
            ESP_LOGI(TAG, "Current FPS: %lu", fps);
        }
        vTaskDelay(pdMS_TO_TICKS(FPS_MONITOR_INTERVAL_MS));
    }
}
#endif

#if APP_ENABLE_COLOR_TEST
/** espressif/button：单击切换 Widgets / 纯色测试（回调在非 LVGL 任务中执行，需加锁） */
static void gpio0_single_click_cb(void *button_handle, void *usr_data)
{
    (void)button_handle;
    (void)usr_data;
    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        color_test_toggle();
        esp_lv_adapter_unlock();
    }
}
#endif

void app_main()
{
    esp_lcd_panel_handle_t display_panel = NULL;
    esp_lcd_panel_io_handle_t display_io_handle = NULL;
    esp_lv_adapter_rotation_t rotation = get_configured_rotation();

    /* Select tear effect mode based on LCD interface type */
#if CONFIG_EXAMPLE_LCD_INTERFACE_MIPI_DSI
    esp_lv_adapter_tear_avoid_mode_t tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_DEFAULT_MIPI_DSI;
    ESP_LOGI(TAG, "Selected LCD interface: MIPI DSI");
#elif CONFIG_EXAMPLE_LCD_INTERFACE_RGB
    /* RGB 面板使用 TRIPLE_PARTIAL。
     *
     * 历程：曾改为 DOUBLE_DIRECT（期望零拷贝省带宽），实测出现「上下闪」（垂直撕裂）。
     * 根因：DOUBLE_DIRECT 让 LVGL 直接渲染进面板帧缓冲，而 bounce buffer 的 DMA 是
     * 分批从 fb 拷贝输出、拷贝时才读 fb 指针，导致切换 fb 的瞬间上半屏是旧帧、
     * 下半屏是新帧 -> 垂直撕裂。去掉 bounce buffer 则退回整屏明暗跳（PSRAM 争抢）。
     *
     * TRIPLE_PARTIAL 下 LVGL 渲染进自身缓冲（只处理脏区），不存在与面板扫描争抢
     * 同一块 fb 的问题，配合 bounce buffer 可同时避开撕裂与明暗跳。
     * 注意：不能用 ESP_LV_ADAPTER_DISPLAY_RGB_DEFAULT_CONFIG，它内部硬编码的
     * TEAR_AVOID_MODE_DEFAULT_RGB 与 hw_lcd_init 传入的模式可能不一致，
     * 会导致面板侧 num_fbs 与适配器侧 required 不符（StoreProhibited 崩溃）。
     * 必须显式传入与 hw_lcd_init 相同的 tear_avoid_mode。 */
    esp_lv_adapter_tear_avoid_mode_t tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_TRIPLE_PARTIAL;
    ESP_LOGI(TAG, "Selected LCD interface: RGB (TRIPLE_PARTIAL)");
#else
    esp_lv_adapter_tear_avoid_mode_t tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_DEFAULT;
#if CONFIG_EXAMPLE_LCD_INTERFACE_QSPI
    ESP_LOGI(TAG, "Selected LCD interface: QSPI");
#elif CONFIG_EXAMPLE_LCD_INTERFACE_SPI_WITH_PSRAM
    ESP_LOGI(TAG, "Selected LCD interface: SPI (with PSRAM)");
#elif CONFIG_EXAMPLE_LCD_INTERFACE_SPI_WITHOUT_PSRAM
    ESP_LOGI(TAG, "Selected LCD interface: SPI (without PSRAM)");
#endif
#endif

    /* Initialize the LCD hardware panel */
    ESP_LOGI(TAG, "Initializing LCD: %dx%d", HW_LCD_H_RES, HW_LCD_V_RES);
    ESP_ERROR_CHECK(hw_lcd_init(&display_panel, &display_io_handle, tear_avoid_mode, rotation));

    /* Initialize the LVGL adapter（SquareLine 复杂 UI + 32bpp 点击刷新时默认 8KB 栈易溢出，见 lvgl_worker 内 ulTaskNotifyTake/uxListRemove 崩溃） */
    esp_lv_adapter_config_t adapter_config = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    adapter_config.task_stack_size = 18 * 1024;
    ESP_ERROR_CHECK(esp_lv_adapter_init(&adapter_config));

    /* Register the display to the LVGL adapter with appropriate configuration */
#if CONFIG_EXAMPLE_LCD_INTERFACE_MIPI_DSI
    esp_lv_adapter_display_config_t display_config = ESP_LV_ADAPTER_DISPLAY_MIPI_DEFAULT_CONFIG(
                                                         display_panel, display_io_handle, HW_LCD_H_RES, HW_LCD_V_RES, rotation);
#elif CONFIG_EXAMPLE_LCD_INTERFACE_RGB
    /* 不能用 ESP_LV_ADAPTER_DISPLAY_RGB_DEFAULT_CONFIG：
     * 它内部硬编码 TEAR_AVOID_MODE_DEFAULT_RGB，可能与 hw_lcd_init 传入的模式
     * 不一致，导致面板侧 num_fbs 与适配器侧 required 不符（StoreProhibited 崩溃）。
     * 因此这里显式构造 profile，并传入与 hw_lcd_init 相同的 tear_avoid_mode。
     *
     * buffer_height 必须显式调小：默认宏里是 50 行。TRIPLE_PARTIAL 分支在
     * display_manager.c:868 硬编码 use_psram=false，绘制缓冲只能从内部 RAM 分配，
     * 50 行 = 800*50*2 = 80000 字节，内部 RAM 拿不出该连续块，
     * 会报 "alloc primary buffer 80000 bytes failed" 导致注册显示失败。
     * 20 行 = 32000 字节，可正常分配。（use_psram 在该分支无效，无需设置。）
     *
     * 【移植时注意】占用字节数 = HW_LCD_H_RES × buffer_height × 2，必须能放进
     * 内部 RAM 的连续块。本板 800 宽取 20 行(32KB) 安全；换更宽的屏要按比例调小，
     * 例如 1024 宽时建议 12~16 行(24~32KB)。分辨率大幅提高（如 1280 宽）时
     * 该模式可能无法满足，需要改用其他撕裂避免模式或改走 PSRAM 缓冲路径。 */
    esp_lv_adapter_display_config_t display_config = ESP_LV_ADAPTER_DISPLAY_CONFIG(
                                                         display_panel, display_io_handle,
                                                         ESP_LV_ADAPTER_DISPLAY_PROFILE_RGB_DEFAULT_CONFIG(HW_LCD_H_RES, HW_LCD_V_RES, rotation),
                                                         tear_avoid_mode,
                                                         ESP_LV_ADAPTER_TE_SYNC_DISABLED());
    display_config.profile.buffer_height = 20;
#elif CONFIG_EXAMPLE_LCD_INTERFACE_SPI_WITHOUT_PSRAM
    esp_lv_adapter_display_config_t display_config = ESP_LV_ADAPTER_DISPLAY_SPI_WITHOUT_PSRAM_DEFAULT_CONFIG(
                                                         display_panel, display_io_handle, HW_LCD_H_RES, HW_LCD_V_RES, rotation);
#else  /* QSPI or SPI with PSRAM */
    esp_lv_adapter_display_config_t display_config = ESP_LV_ADAPTER_DISPLAY_SPI_WITH_PSRAM_DEFAULT_CONFIG(
                                                         display_panel, display_io_handle, HW_LCD_H_RES, HW_LCD_V_RES, rotation);
#endif

    lv_display_t *disp = esp_lv_adapter_register_display(&display_config);
    if (disp == NULL) {
        ESP_LOGE(TAG, "Failed to register display");
        return;
    }

    /* Initialize input device based on interface type */
#if HW_USE_TOUCH
    ESP_LOGI(TAG, "Initializing touch panel");
    esp_lcd_touch_handle_t touch_handle = NULL;
    /* 触摸失败不 abort：否则屏幕还没点亮就重启，无法判断是显示还是触摸的问题 */
    esp_err_t touch_ret = hw_touch_init(&touch_handle, rotation);
    if (touch_ret != ESP_OK) {
        ESP_LOGW(TAG, "Touch init failed (0x%x), continue without touch input", touch_ret);
    } else {
        /* Use the default config macro for quick setup with 1:1 coordinate scaling */
        esp_lv_adapter_touch_config_t touch_config = ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(disp, touch_handle);
        lv_indev_t *touch = esp_lv_adapter_register_touch(&touch_config);
        if (touch == NULL) {
            ESP_LOGE(TAG, "Failed to register touch");
        }
    }

#elif HW_USE_ENCODER && CONFIG_ESP_LVGL_ADAPTER_ENABLE_KNOB
    ESP_LOGI(TAG, "Initializing encoder/knob");
    esp_lv_adapter_encoder_config_t encoder_config = {
        .disp = disp,
        .encoder_a_b = hw_knob_get_config(),
        .encoder_enter = hw_knob_get_button(),
    };
    lv_indev_t *encoder = esp_lv_adapter_register_encoder(&encoder_config);
    if (encoder == NULL) {
        ESP_LOGE(TAG, "Failed to register encoder");
        return;
    }
#endif

    /* Start the LVGL adapter */
    ESP_ERROR_CHECK(esp_lv_adapter_start());

    /* Optional: Enable FPS statistics for performance monitoring */
// #if CONFIG_ESP_LVGL_ADAPTER_ENABLE_FPS_STATS
//     ESP_ERROR_CHECK(esp_lv_adapter_fps_stats_enable(disp, true));
//     xTaskCreate(fps_monitor_task, "fps_monitor", FPS_MONITOR_TASK_STACK_SIZE, disp, FPS_MONITOR_TASK_PRIORITY, NULL);
// #endif

    ESP_LOGI(TAG, "Starting SquareLine UI (ui_init)");
    /* Lock the mutex due to the LVGL APIs are not thread-safe */
    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        setup_ui(&guider_ui);
#if APP_ENABLE_COLOR_TEST
        color_test_init(lv_screen_active());
#endif
        esp_lv_adapter_unlock();
    }

#if APP_ENABLE_COLOR_TEST
    /* GPIO0：乐鑫 espressif/button，单击在 Widgets 与纯色测试界面之间切换 */
    const button_config_t btn_cfg = {0};
    const button_gpio_config_t btn_gpio_cfg = {
        .gpio_num = GPIO0_KEY_GPIO,
        .active_level = 0,
        .enable_power_save = false,
        .disable_pull = false,
    };
    button_handle_t gpio0_btn = NULL;
    ESP_ERROR_CHECK(iot_button_new_gpio_device(&btn_cfg, &btn_gpio_cfg, &gpio0_btn));
    ESP_ERROR_CHECK(iot_button_register_cb(gpio0_btn, BUTTON_SINGLE_CLICK, NULL, gpio0_single_click_cb, NULL));
#endif
}
