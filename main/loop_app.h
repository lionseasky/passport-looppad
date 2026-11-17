// main/loop_app.h —— 应用控制层:按键分发、页面状态、电量轮询。
//
// 按键回调运行在 esp_timer 任务里,只做"入队";真正的动作在 loop_app 任务里执行,
// 因此 Flash 提交、擦除这类慢操作不会卡住按键中断路径。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "bsp_button.h"
#include "esp_err.h"

typedef enum {
    LOOP_PAGE_PERFORM = 0,
    LOOP_PAGE_SETTINGS,
} loop_page_t;

typedef struct {
    loop_page_t page;
    uint8_t selected;    /* 演奏页:选中轨道;设置页:选中行 */
    uint8_t battery;     /* 0..100,255 = 未知 */
    bool audio_ok;
    bool storage_ok;
    uint32_t toast_id;   /* 递增;界面看到变化就弹提示 */
    const char *toast;
} loop_app_view_t;

// 创建队列与任务;可重复调用(第二次直接返回 ESP_OK)。
esp_err_t loop_app_start(void);
void loop_app_view(loop_app_view_t *out);

// 供 BSP 按键回调调用:只入队,立即返回。
void loop_app_on_button(bsp_btn_t btn, bsp_btn_ev_t ev, void *user);
