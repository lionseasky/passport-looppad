// main/loop_ui.c —— LOOP PAD 界面实现。
//
// 这是一套为采样器重新设计的界面,不复用基线的测试菜单/卡片皮肤:
//   * 深色底 + 琥珀色强调,四行"轨道条"是主角;
//   * 录音时整屏切换成"视觉节拍器"(喇叭此时静音,靠屏幕给节奏);
//   * 所有 LVGL 操作都在 LVGL 任务里(lv_timer 回调),不需要额外加锁。
#include "loop_ui.h"

#include <stdio.h>
#include <string.h>

#include "bsp_display.h"
#include "esp_log.h"
#include "loop_app.h"
#include "loop_audio.h"
#include "loop_font.h"

static const char *TAG = "loop_ui";

#define SCR_W 240
#define SCR_H 320

#define C_BG 0x0B0C10u
#define C_PANEL 0x171A21u
#define C_PANEL_HI 0x232A36u
#define C_ACCENT 0xF5A524u
#define C_ACCENT2 0x3DDC97u
#define C_TEXT 0xEDEFF2u
#define C_DIM 0x78808Fu
#define C_RED 0xFF5A5Au

#define ROW_COUNT 4
#define SET_ROW_COUNT 9

static lv_obj_t *s_screen;
static lv_obj_t *s_perf;
static lv_obj_t *s_settings;
static lv_obj_t *s_rec;
static lv_obj_t *s_toast;

static lv_obj_t *s_bpm;
static lv_obj_t *s_bar_pos;
static lv_obj_t *s_batt;
static lv_obj_t *s_status;
static lv_obj_t *s_hint;
static lv_obj_t *s_dot[ROW_COUNT];
static lv_obj_t *s_row[ROW_COUNT];
static lv_obj_t *s_row_name[ROW_COUNT];
static lv_obj_t *s_row_state[ROW_COUNT];
static lv_obj_t *s_row_bar[ROW_COUNT];

static lv_obj_t *s_set_row[SET_ROW_COUNT];
static lv_obj_t *s_set_name[SET_ROW_COUNT];
static lv_obj_t *s_set_value[SET_ROW_COUNT];

static lv_obj_t *s_rec_title;
static lv_obj_t *s_rec_big;
static lv_obj_t *s_rec_sub;
static lv_obj_t *s_rec_hint;
static lv_obj_t *s_rec_prog;
static lv_obj_t *s_rec_level;

static lv_obj_t *s_toast_label;

static int s_settings_shown = -1;
static int s_toast_shown = -1;
static uint32_t s_toast_seen;
static uint32_t s_toast_until;
static uint32_t s_rec_shown = 0xFFFFFFFFu;
static int s_last_selected = -1;
static int s_last_setting_row = -1;
static int s_last_beat = -1;
static int s_state_kind[ROW_COUNT] = {-1, -1, -1, -1};
static bool s_last_recording;

static const char *const TRACK_NAME[ROW_COUNT] = {"鼓", "贝斯", "和声", "人声"};
static const char *const SET_NAME[SET_ROW_COUNT] = {
    "速度", "小节", "鼓音量", "贝斯音量", "和声音量", "人声音量",
    "清空当前轨", "清空全部", "返回",
};

static lv_obj_t *make_panel(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color,
                            int radius)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, int x, int y,
                            const char *text)
{
    lv_obj_t *obj = lv_label_create(parent);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_style_text_font(obj, font, 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(color), 0);
    lv_label_set_text(obj, text);
    return obj;
}

static lv_obj_t *make_bar(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *bar = lv_bar_create(parent);
    lv_obj_set_pos(bar, x, y);
    lv_obj_set_size(bar, w, h);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_radius(bar, h / 2, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(C_PANEL_HI), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_set_style_radius(bar, h / 2, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(bar, lv_color_hex(C_ACCENT2), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    return bar;
}

static void set_text(lv_obj_t *obj, const char *text)
{
    const char *current = lv_label_get_text(obj);
    if (current != NULL && strcmp(current, text) == 0) return;
    lv_label_set_text(obj, text);
}

static void set_bar(lv_obj_t *bar, int value)
{
    if (lv_bar_get_value(bar) != value) lv_bar_set_value(bar, value, LV_ANIM_OFF);
}

static void build_perform(void)
{
    s_perf = make_panel(s_screen, 0, 0, SCR_W, SCR_H, C_BG, 0);

    make_label(s_perf, &loop_font_title, C_ACCENT, 12, 4, "LOOP PAD");
    s_batt = make_label(s_perf, &loop_font_body, C_DIM, 176, 10, "--");
    lv_obj_set_width(s_batt, 52);
    lv_obj_set_style_text_align(s_batt, LV_TEXT_ALIGN_RIGHT, 0);

    s_bpm = make_label(s_perf, &loop_font_body, C_TEXT, 120, 38, "90 BPM");
    s_bar_pos = make_label(s_perf, &loop_font_body, C_DIM, 190, 38, "1/4");
    lv_obj_set_width(s_bar_pos, 40);
    lv_obj_set_style_text_align(s_bar_pos, LV_TEXT_ALIGN_RIGHT, 0);

    for (int i = 0; i < ROW_COUNT; i++) {
        s_dot[i] = make_panel(s_perf, 12 + i * 18, 46, 12, 12, C_PANEL_HI, 6);
    }

    for (int i = 0; i < ROW_COUNT; i++) {
        const int y = 70 + i * 52;
        s_row[i] = make_panel(s_perf, 8, y, 224, 46, C_PANEL, 10);
        lv_obj_set_style_border_width(s_row[i], 2, 0);
        lv_obj_set_style_border_color(s_row[i], lv_color_hex(C_ACCENT), 0);
        lv_obj_set_style_border_opa(s_row[i], LV_OPA_TRANSP, 0);

        s_row_name[i] = make_label(s_row[i], &loop_font_body, C_TEXT, 10, 4, TRACK_NAME[i]);
        s_row_state[i] = make_label(s_row[i], &loop_font_body, C_DIM, 140, 4, "空");
        lv_obj_set_width(s_row_state[i], 70);
        lv_obj_set_style_text_align(s_row_state[i], LV_TEXT_ALIGN_RIGHT, 0);
        s_row_bar[i] = make_bar(s_row[i], 10, 30, 204, 6);
    }

    s_status = make_label(s_perf, &loop_font_body, C_DIM, 12, 280, "已停止");
    s_hint = make_label(s_perf, &loop_font_body, C_DIM, 12, 300, "长按OK 录音");
    lv_obj_set_width(s_hint, 216);
}

static void build_settings(void)
{
    s_settings = make_panel(s_screen, 0, 0, SCR_W, SCR_H, C_BG, 0);
    make_label(s_settings, &loop_font_title, C_ACCENT, 12, 4, "设置");
    make_label(s_settings, &loop_font_body, C_DIM, 126, 10, "长按OK 返回");

    for (int i = 0; i < SET_ROW_COUNT; i++) {
        const int y = 40 + i * 28;
        s_set_row[i] = make_panel(s_settings, 8, y, 224, 26, C_BG, 6);
        s_set_name[i] = make_label(s_set_row[i], &loop_font_body, C_TEXT, 10, 3, SET_NAME[i]);
        s_set_value[i] = make_label(s_set_row[i], &loop_font_body, C_DIM, 120, 3, "");
        lv_obj_set_width(s_set_value[i], 94);
        lv_obj_set_style_text_align(s_set_value[i], LV_TEXT_ALIGN_RIGHT, 0);
    }
    lv_obj_add_flag(s_settings, LV_OBJ_FLAG_HIDDEN);
}

static void build_recorder(void)
{
    s_rec = make_panel(s_screen, 0, 0, SCR_W, SCR_H, C_BG, 0);
    s_rec_title = make_label(s_rec, &loop_font_title, C_ACCENT, 12, 18, "录音中");
    s_rec_big = make_label(s_rec, &loop_font_big, C_TEXT, 0, 64, "1");
    lv_obj_set_width(s_rec_big, SCR_W);
    lv_obj_set_style_text_align(s_rec_big, LV_TEXT_ALIGN_CENTER, 0);
    s_rec_sub = make_label(s_rec, &loop_font_body, C_DIM, 0, 132, "");
    lv_obj_set_width(s_rec_sub, SCR_W);
    lv_obj_set_style_text_align(s_rec_sub, LV_TEXT_ALIGN_CENTER, 0);

    make_label(s_rec, &loop_font_body, C_DIM, 30, 174, "进度");
    s_rec_prog = make_bar(s_rec, 30, 196, 180, 10);
    make_label(s_rec, &loop_font_body, C_DIM, 30, 214, "电平");
    s_rec_level = make_bar(s_rec, 30, 236, 180, 10);
    s_rec_hint = make_label(s_rec, &loop_font_body, C_DIM, 12, 296, "长按OK 取消");
    lv_obj_add_flag(s_rec, LV_OBJ_FLAG_HIDDEN);
}

static void build_toast(void)
{
    s_toast = make_panel(s_screen, 30, 252, 180, 32, C_PANEL_HI, 8);
    s_toast_label = make_label(s_toast, &loop_font_body, C_TEXT, 0, 6, "");
    lv_obj_set_width(s_toast_label, 180);
    lv_obj_set_style_text_align(s_toast_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
}

static void update_perform(const loop_audio_status_t *st, const loop_app_view_t *view)
{
    char buffer[48];

    snprintf(buffer, sizeof(buffer), "%u BPM", (unsigned)st->bpm);
    set_text(s_bpm, buffer);
    snprintf(buffer, sizeof(buffer), "%u/%u", (unsigned)(st->bar + 1), (unsigned)st->bars);
    set_text(s_bar_pos, buffer);

    if (view->battery <= 100) {
        snprintf(buffer, sizeof(buffer), "%u%%", (unsigned)view->battery);
    } else {
        snprintf(buffer, sizeof(buffer), "--");
    }
    set_text(s_batt, buffer);

    const bool playing = st->mode == LOOP_MODE_PLAY;
    const int beat = playing ? (int)st->beat : -1;
    if (beat != s_last_beat) {
        for (int i = 0; i < ROW_COUNT; i++) {
            lv_obj_set_style_bg_color(s_dot[i],
                                      lv_color_hex(i == beat ? C_ACCENT : C_PANEL_HI), 0);
        }
        s_last_beat = beat;
    }

    int present_count = 0;
    for (int i = 0; i < ROW_COUNT; i++) {
        const loop_track_view_t *track = &st->tracks[i];
        if (track->present) present_count++;
        const char *state = NULL;
        uint32_t color = C_DIM;
        if (!track->present) {
            state = "空";
        } else if (track->mute || track->volume == 0) {
            state = "静音";
            color = C_RED;
        } else {
            state = "播放";
            color = C_ACCENT2;
        }
        set_text(s_row_state[i], state);
        const int kind = !track->present ? 0 : ((track->mute || track->volume == 0) ? 2 : 1);
        if (kind != s_state_kind[i]) {
            lv_obj_set_style_text_color(s_row_state[i], lv_color_hex(color), 0);
            s_state_kind[i] = kind;
        }
        set_bar(s_row_bar[i], track->present ? track->volume : 0);
        set_text(s_row_name[i], TRACK_NAME[i]);
    }

    if (s_last_selected != (int)view->selected) {
        for (int i = 0; i < ROW_COUNT; i++) {
            const bool chosen = (int)view->selected == i;
            lv_obj_set_style_border_opa(s_row[i], chosen ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
            lv_obj_set_style_bg_color(s_row[i], lv_color_hex(chosen ? C_PANEL_HI : C_PANEL), 0);
        }
        s_last_selected = (int)view->selected;
    }

    if (playing) {
        snprintf(buffer, sizeof(buffer), "播放中 · %d 轨", present_count);
    } else if (!view->audio_ok) {
        snprintf(buffer, sizeof(buffer), "音频不可用");
    } else if (!view->storage_ok) {
        snprintf(buffer, sizeof(buffer), "存储不可用");
    } else if (present_count == 0) {
        snprintf(buffer, sizeof(buffer), "就绪 · 录第一轨");
    } else {
        snprintf(buffer, sizeof(buffer), "已停止");
    }
    set_text(s_status, buffer);

    // 两行提示轮流出现,避免一行塞不下。
    const bool second = ((lv_tick_get() / 4000u) & 1u) != 0u;
    if (present_count == 0) {
        set_text(s_hint, second ? "长按DOWN 设置 · 长按UP 变速"
                                : "长按OK 开始录音");
    } else {
        set_text(s_hint, second ? "长按UP 变速 · 长按DOWN 设置"
                                : "长按OK 录音 · 短按OK 播放");
    }
}

static void update_settings(const loop_audio_status_t *st, const loop_app_view_t *view)
{
    const int selected = view->selected;
    char buffer[32];
    const bool has_tracks = st->tracks[0].present || st->tracks[1].present ||
                            st->tracks[2].present || st->tracks[3].present;

    if (selected != s_last_setting_row) {
        for (int i = 0; i < SET_ROW_COUNT; i++) {
            const bool is_selected = (i == selected);
            lv_obj_set_style_bg_color(s_set_row[i], lv_color_hex(is_selected ? C_PANEL_HI : C_BG), 0);
            lv_obj_set_style_text_color(s_set_name[i], lv_color_hex(is_selected ? C_ACCENT : C_TEXT), 0);
        }
        s_last_setting_row = selected;
    }

    for (int i = 0; i < SET_ROW_COUNT; i++) {
        buffer[0] = '\0';
        if (i == 0) {
            if (has_tracks) {
                snprintf(buffer, sizeof(buffer), "需先清空");
            } else {
                snprintf(buffer, sizeof(buffer), "%u BPM", (unsigned)st->bpm);
            }
        } else if (i == 1) {
            if (has_tracks) {
                snprintf(buffer, sizeof(buffer), "需先清空");
            } else {
                snprintf(buffer, sizeof(buffer), "%u 小节", (unsigned)st->bars);
            }
        } else if (i >= 2 && i <= 5) {
            const loop_track_view_t *track = &st->tracks[i - 2];
            if (!track->present) {
                snprintf(buffer, sizeof(buffer), "空");
            } else if (track->mute) {
                snprintf(buffer, sizeof(buffer), "静音");
            } else {
                snprintf(buffer, sizeof(buffer), "%u%%", (unsigned)track->volume);
            }
        }
        set_text(s_set_value[i], buffer);
        lv_obj_set_style_text_color(s_set_value[i], lv_color_hex(C_DIM), 0);
    }
}

static void update_recorder(const loop_audio_status_t *st)
{
    char buffer[40];
    switch (st->mode) {
    case LOOP_MODE_PREP:
        set_text(s_rec_title, "准备中");
        set_text(s_rec_big, "...");
        set_text(s_rec_sub, "擦除旧录音");
        set_bar(s_rec_prog, 0);
        set_bar(s_rec_level, 0);
        lv_obj_add_flag(s_rec_prog, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_rec_level, LV_OBJ_FLAG_HIDDEN);
        break;
    case LOOP_MODE_COUNTIN:
        set_text(s_rec_title, "倒计时");
        snprintf(buffer, sizeof(buffer), "%u", (unsigned)(st->countin_left == 0 ? 1 : st->countin_left));
        set_text(s_rec_big, buffer);
        set_text(s_rec_sub, "准备录音");
        lv_obj_add_flag(s_rec_prog, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_rec_level, LV_OBJ_FLAG_HIDDEN);
        break;
    case LOOP_MODE_RECORD:
        set_text(s_rec_title, "录音中");
        snprintf(buffer, sizeof(buffer), "%u", (unsigned)(st->beat + 1));
        set_text(s_rec_big, buffer);
        snprintf(buffer, sizeof(buffer), "第 %u/%u 小节", (unsigned)(st->rec_bar + 1),
                 (unsigned)st->bars);
        set_text(s_rec_sub, buffer);
        lv_obj_remove_flag(s_rec_prog, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_rec_level, LV_OBJ_FLAG_HIDDEN);
        set_bar(s_rec_prog,
                st->bars == 0 ? 0 : (int)(((uint32_t)(st->rec_bar + 1) * 100u) / st->bars));
        set_bar(s_rec_level, st->level);
        break;
    default:
        break;
    }
}

static void ui_tick(lv_timer_t *timer)
{
    (void)timer;
    loop_audio_status_t st;
    loop_app_view_t view;
    loop_audio_get_status(&st);
    loop_app_view(&view);

    const bool recording = st.mode == LOOP_MODE_PREP || st.mode == LOOP_MODE_COUNTIN ||
                           st.mode == LOOP_MODE_RECORD;

    if (s_last_recording && !recording) {
        // 录音结束:给一个即时反馈,免得分不清"录完了"还是"出错了"。
        s_toast_until = lv_tick_get() + 1500;
        set_text(s_toast_label, "录制完成");
        lv_obj_remove_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
        s_toast_shown = 1;
    }
    s_last_recording = recording;

    if (recording) {
        if (s_rec_shown != 1) {
            lv_obj_remove_flag(s_rec, LV_OBJ_FLAG_HIDDEN);
            s_rec_shown = 1;
        }
        update_recorder(&st);
    } else if (s_rec_shown != 0) {
        lv_obj_add_flag(s_rec, LV_OBJ_FLAG_HIDDEN);
        s_rec_shown = 0;
    }

    const int show_settings = view.page == LOOP_PAGE_SETTINGS ? 1 : 0;
    if (s_settings_shown != show_settings) {
        if (show_settings) {
            lv_obj_remove_flag(s_settings, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_perf, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_settings, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(s_perf, LV_OBJ_FLAG_HIDDEN);
        }
        s_settings_shown = show_settings;
    }
    if (show_settings) {
        update_settings(&st, &view);
    } else {
        update_perform(&st, &view);
    }

    if (view.toast_id != s_toast_seen && view.toast != NULL) {
        s_toast_seen = view.toast_id;
        s_toast_until = lv_tick_get() + 1800;
        set_text(s_toast_label, view.toast);
        lv_obj_remove_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
        s_toast_shown = 1;
    }
    if (s_toast_shown == 1 && (int32_t)(lv_tick_get() - s_toast_until) > 0) {
        lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
        s_toast_shown = 0;
    }
}

esp_err_t loop_ui_init(void)
{
    if (!bsp_lvgl_lock(3000)) return ESP_ERR_TIMEOUT;

    loop_font_init();
    s_screen = lv_obj_create(NULL);
    lv_obj_remove_style_all(s_screen);
    lv_obj_set_size(s_screen, SCR_W, SCR_H);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_screen, 0, 0);
    lv_screen_load(s_screen);

    build_perform();
    build_settings();
    build_recorder();
    build_toast();

    lv_timer_create(ui_tick, 30, NULL);
    bsp_lvgl_unlock();
    ESP_LOGI(TAG, "界面初始化完成");
    return ESP_OK;
}
