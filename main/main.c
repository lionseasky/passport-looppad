// main/main.c —— LOOP PAD:口袋嘴鼓循环采样器。
//
// 与基线 demo 不同,这里不构建测试菜单:上电直接进入重新设计的采样器界面。
// 初始化顺序:显示 → LVGL → 音频/存储 → 界面 → 应用控制 → 按键。
#include "bsp_button.h"
#include "bsp_display.h"
#include "esp_err.h"
#include "esp_log.h"
#include "loop_app.h"
#include "loop_audio.h"
#include "loop_ui.h"
#include "loop_version.h"

static const char *TAG = "main";

void app_main(void)
{
    ESP_LOGI(TAG, "LOOP PAD v%s 启动", LOOP_PAD_VERSION);

    esp_err_t err = bsp_display_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "显示初始化失败: %s", esp_err_to_name(err));
        return;
    }
    if (bsp_lvgl_init() == NULL) {
        ESP_LOGE(TAG, "LVGL 初始化失败");
        return;
    }
    // 背光默认是 0(灭),必须显式点亮,否则界面全黑。
    bsp_display_backlight(85);

    err = loop_audio_init();
    if (err != ESP_OK) {
        // 音频失败仍然显示界面:用户能看到"音频不可用",而不是一块黑屏。
        ESP_LOGE(TAG, "音频引擎初始化失败: %s", esp_err_to_name(err));
    }

    err = loop_ui_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "界面初始化失败: %s", esp_err_to_name(err));
        return;
    }

    err = loop_app_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "应用启动失败: %s", esp_err_to_name(err));
        return;
    }

    // 按键回调只入队,队列已在 loop_app_start() 里建好。
    err = bsp_button_init(loop_app_on_button, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "按键初始化失败: %s", esp_err_to_name(err));
    }
}
