// main/loop_ui.h —— LOOP PAD 的界面层(演奏页 / 录音页 / 设置页)。
#pragma once

#include "esp_err.h"

// 必须在 bsp_lvgl_init() 之后调用;内部会获取 LVGL 锁。
esp_err_t loop_ui_init(void);
