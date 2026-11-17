// main/loop_app.c —— 应用控制层实现。
#include "loop_app.h"

#include <string.h>

#include "bsp_battery.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "loop_audio.h"
#include "loop_core.h"
#include "loop_store.h"

static const char *TAG = "loop_app";

#define APP_QUEUE_DEPTH 8
#define APP_TASK_STACK 4096
#define APP_TASK_PRIO 5

typedef struct {
    bsp_btn_t btn;
    bsp_btn_ev_t ev;
} app_event_t;

static QueueHandle_t s_queue;
static TaskHandle_t s_task;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static loop_app_view_t s_view = {
    .page = LOOP_PAGE_PERFORM,
    .selected = 0,
    .battery = 255,
    .audio_ok = false,
    .storage_ok = false,
    .toast_id = 0,
    .toast = NULL,
};
static uint8_t s_track_selected;
static bool s_tape;

static void set_toast(const char *text)
{
    taskENTER_CRITICAL(&s_mux);
    s_view.toast = text;
    s_view.toast_id++;
    taskEXIT_CRITICAL(&s_mux);
}

static void set_page(loop_page_t page, uint8_t selected)
{
    taskENTER_CRITICAL(&s_mux);
    s_view.page = page;
    s_view.selected = selected;
    taskEXIT_CRITICAL(&s_mux);
}

void loop_app_view(loop_app_view_t *out)
{
    if (out == NULL) return;
    taskENTER_CRITICAL(&s_mux);
    *out = s_view;
    taskEXIT_CRITICAL(&s_mux);
}

static bool any_track(const loop_audio_status_t *st)
{
    for (int i = 0; i < LOOP_TRACK_COUNT; i++) {
        if (st->tracks[i].present) return true;
    }
    return false;
}

static void handle_perform(const app_event_t *ev)
{
    loop_audio_status_t st;
    loop_audio_get_status(&st);

    switch (ev->btn) {
    case BSP_BTN_UP:
        if (ev->ev == BSP_BTN_CLICK) {
            s_track_selected = (uint8_t)((s_track_selected + LOOP_TRACK_COUNT - 1) % LOOP_TRACK_COUNT);
            set_page(LOOP_PAGE_PERFORM, s_track_selected);
        } else if (ev->ev == BSP_BTN_LONG) {
            if (st.mode == LOOP_MODE_PLAY) {
                s_tape = true;
                loop_audio_set_tape(true);
                set_toast("磁带慢放");
            }
        } else if (ev->ev == BSP_BTN_RELEASE && s_tape) {
            s_tape = false;
            loop_audio_set_tape(false);
        }
        break;
    case BSP_BTN_DOWN:
        if (ev->ev == BSP_BTN_CLICK) {
            s_track_selected = (uint8_t)((s_track_selected + 1) % LOOP_TRACK_COUNT);
            set_page(LOOP_PAGE_PERFORM, s_track_selected);
        } else if (ev->ev == BSP_BTN_LONG) {
            set_page(LOOP_PAGE_SETTINGS, 0);
            set_toast("设置");
        }
        break;
    case BSP_BTN_OK:
        if (ev->ev == BSP_BTN_CLICK) {
            if (st.mode == LOOP_MODE_PLAY) {
                loop_audio_stop();
                set_toast("已停止");
            } else if (any_track(&st)) {
                loop_audio_play();
                set_toast("播放");
            } else {
                set_toast("先录一轨");
            }
        } else if (ev->ev == BSP_BTN_LONG) {
            if (!st.storage_ok) {
                set_toast("存储不可用");
            } else if (loop_audio_start_record(s_track_selected) != ESP_OK) {
                set_toast("无法录音");
            }
        }
        break;
    default:
        break;
    }
}

static void leave_settings(void)
{
    loop_audio_persist();
    set_page(LOOP_PAGE_PERFORM, s_track_selected);
}

static void handle_settings(const app_event_t *ev)
{
    loop_audio_status_t st;
    loop_audio_get_status(&st);
    loop_app_view_t view;
    loop_app_view(&view);
    uint8_t row = view.selected;

    switch (ev->btn) {
    case BSP_BTN_UP:
        if (ev->ev == BSP_BTN_CLICK) {
            row = (uint8_t)((row + 8) % 9);
            set_page(LOOP_PAGE_SETTINGS, row);
        }
        break;
    case BSP_BTN_DOWN:
        if (ev->ev == BSP_BTN_CLICK) {
            row = (uint8_t)((row + 1) % 9);
            set_page(LOOP_PAGE_SETTINGS, row);
        }
        break;
    case BSP_BTN_OK:
        if (ev->ev == BSP_BTN_LONG) {
            leave_settings();
            break;
        }
        if (ev->ev != BSP_BTN_CLICK) break;
        if (row == 0) {
            if (any_track(&st)) {
                set_toast("需先清空");
            } else {
                loop_audio_set_timing(loop_bpm_next(st.bpm, 1), st.bars);
            }
        } else if (row == 1) {
            if (any_track(&st)) {
                set_toast("需先清空");
            } else {
                loop_audio_set_timing(st.bpm, loop_bars_next(st.bars, 1));
            }
        } else if (row >= 2 && row <= 5) {
            const uint8_t track = (uint8_t)(row - 2);
            if (!st.tracks[track].present) {
                set_toast("空轨");
            } else {
                const uint8_t current = st.tracks[track].volume / 25;
                const uint8_t next = (uint8_t)(((current + 1) % 5) * 25);
                loop_audio_set_track_volume(track, next);
            }
        } else if (row == 6) {
            loop_audio_clear_track(s_track_selected);
            set_toast("已清空");
        } else if (row == 7) {
            loop_audio_clear_all();
            set_toast("已清空");
        } else if (row == 8) {
            leave_settings();
        }
        break;
    default:
        break;
    }
}

static void app_task(void *arg)
{
    (void)arg;
    TickType_t last_battery = 0;
    for (;;) {
        app_event_t ev;
        if (xQueueReceive(s_queue, &ev, pdMS_TO_TICKS(250)) == pdTRUE) {
            loop_audio_status_t st;
            loop_audio_get_status(&st);
            const bool recording = st.mode == LOOP_MODE_PREP || st.mode == LOOP_MODE_COUNTIN ||
                                   st.mode == LOOP_MODE_RECORD;
            if (recording) {
                // 录音期间只认"短按 OK = 取消",避免误触改设置。
                if (ev.btn == BSP_BTN_OK && ev.ev == BSP_BTN_CLICK) {
                    loop_audio_cancel();
                    set_toast("已取消");
                }
                if (ev.ev == BSP_BTN_RELEASE && s_tape) {
                    s_tape = false;
                    loop_audio_set_tape(false);
                }
                continue;
            }
            loop_app_view_t view;
            loop_app_view(&view);
            if (view.page == LOOP_PAGE_SETTINGS) {
                handle_settings(&ev);
            } else {
                handle_perform(&ev);
            }
        }

        const TickType_t now = xTaskGetTickCount();
        if (now - last_battery >= pdMS_TO_TICKS(2000)) {
            last_battery = now;
            const int soc = bsp_battery_soc();
            taskENTER_CRITICAL(&s_mux);
            s_view.battery = (uint8_t)((soc < 0 || soc > 100) ? 255 : soc);
            taskEXIT_CRITICAL(&s_mux);
        }
    }
}

esp_err_t loop_app_start(void)
{
    if (s_task != NULL) return ESP_OK;
    if (s_queue == NULL) {
        s_queue = xQueueCreate(APP_QUEUE_DEPTH, sizeof(app_event_t));
        if (s_queue == NULL) return ESP_ERR_NO_MEM;
    }

    esp_err_t battery_err = bsp_battery_init();
    if (battery_err != ESP_OK) {
        ESP_LOGW(TAG, "电量计不可用: %s", esp_err_to_name(battery_err));
    }

    loop_audio_status_t st;
    loop_audio_get_status(&st);
    taskENTER_CRITICAL(&s_mux);
    s_view.audio_ok = loop_audio_ready();
    s_view.storage_ok = loop_store_ready();
    taskEXIT_CRITICAL(&s_mux);

    if (xTaskCreate(app_task, "loop_app", APP_TASK_STACK, NULL, APP_TASK_PRIO, &s_task) != pdPASS) {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void loop_app_on_button(bsp_btn_t btn, bsp_btn_ev_t ev, void *user)
{
    (void)user;
    if (s_queue == NULL) return;
    if (ev == BSP_BTN_PRESS || ev == BSP_BTN_DOUBLE) return; /* 双击未使用 */
    app_event_t event = {.btn = btn, .ev = ev};
    (void)xQueueSend(s_queue, &event, 0);
}
