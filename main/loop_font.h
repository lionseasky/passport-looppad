// main/loop_font.h —— 应用自带的中文字库(16/24/32 px,4bpp,由 tools/loop_font.py 生成)。
#pragma once

#include "lvgl.h"

LV_FONT_DECLARE(lv_font_loop_16);
LV_FONT_DECLARE(lv_font_loop_24);
LV_FONT_DECLARE(lv_font_loop_32);

// 带 Montserrat 兜底的描述符副本:中文走自带字库,个别符号(如 LV_SYMBOL_*)走兜底。
extern lv_font_t loop_font_body;
extern lv_font_t loop_font_title;
extern lv_font_t loop_font_big;

void loop_font_init(void);
