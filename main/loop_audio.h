// main/loop_audio.h —— LOOP PAD 音频引擎:倒计时、录音、四轨循环混音、磁带变速。
//
// 设计约束(来自本机硬件):
//   * 16 kHz / 16-bit / 单声道,ES8311 全双工,同一时刻只有一个 240 帧的块;
//   * 录音时喇叭静音:麦克风和喇叭在同一个塑料腔体里,边放边录必然啸叫;
//   * 因此录音阶段用"屏幕视觉节拍器"给节奏,倒计时才出声。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "loop_core.h"

typedef enum {
    LOOP_MODE_OFF = 0,  // 停止
    LOOP_MODE_PREP,     // 擦除目标轨道(界面显示"准备中")
    LOOP_MODE_COUNTIN,  // 一小节倒计时,有咔哒声
    LOOP_MODE_RECORD,   // 录音中,静音,靠屏幕节拍
    LOOP_MODE_PLAY,     // 循环播放
} loop_mode_t;

typedef struct {
    uint8_t present;
    uint8_t mute;
    uint8_t volume; /* 0..100 */
} loop_track_view_t;

typedef struct {
    loop_mode_t mode;
    bool storage_ok;
    uint8_t record_track;
    uint16_t bpm;
    uint8_t bars;
    uint32_t loop_len;
    uint8_t bar;
    uint8_t beat;
    uint8_t countin_left; /* 倒计时剩余拍数 1..4 */
    uint8_t rec_bar;      /* 录音已完成的第几小节 */
    uint8_t level;        /* 输入电平 0..100,界面用 */
    uint8_t rate_percent; /* 100 = 原速 */
    uint32_t revision;    /* 轨道结构变化时递增 */
    loop_track_view_t tracks[LOOP_TRACK_COUNT];
} loop_audio_status_t;

esp_err_t loop_audio_init(void);
// 音频引擎是否已成功启动(编解码器 + 任务)。
bool loop_audio_ready(void);
void loop_audio_get_status(loop_audio_status_t *out);

// 开始录音(异步):擦除 → 1 小节倒计时 → 录满 loop_len 采样 → 自动提交并播放。
esp_err_t loop_audio_start_record(uint8_t track);
void loop_audio_cancel(void);

void loop_audio_play(void);
void loop_audio_stop(void);

esp_err_t loop_audio_set_track_volume(uint8_t track, uint8_t percent);
esp_err_t loop_audio_set_track_mute(uint8_t track, bool mute);
esp_err_t loop_audio_persist(void);

// 只有空轨时才能改速度/小节数。
esp_err_t loop_audio_set_timing(uint16_t bpm, uint8_t bars);
esp_err_t loop_audio_clear_track(uint8_t track);
esp_err_t loop_audio_clear_all(void);

// 磁带效果:按住时降到 0.5 倍速,松开回原速(内部做斜坡,避免爆音)。
void loop_audio_set_tape(bool engaging);
