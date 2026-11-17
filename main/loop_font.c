// main/loop_font.c —— 把生成的静态字库包一层 Montserrat 兜底描述符。
#include "loop_font.h"

lv_font_t loop_font_body;
lv_font_t loop_font_title;
lv_font_t loop_font_big;

void loop_font_init(void)
{
    // 浅拷贝:位图数据仍在 Flash 里,这里只换描述符并挂上兜底字体。
    loop_font_body = lv_font_loop_16;
    loop_font_body.fallback = &lv_font_montserrat_14;
    loop_font_title = lv_font_loop_24;
    loop_font_title.fallback = &lv_font_montserrat_14;
    loop_font_big = lv_font_loop_32;
    loop_font_big.fallback = &lv_font_montserrat_14;
}
