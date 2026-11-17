// main/loop_core.h —— LOOP PAD 的纯逻辑层:循环长度、节拍定位、增益、超级块。
//
// 这里刻意不包含 ESP-IDF / LVGL 头文件:所有函数都能在主机上编译并做单元测试。
// 硬件相关的东西(分区读写、I2S)在 loop_store.c / loop_audio.c。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LOOP_SAMPLE_RATE 16000
#define LOOP_TRACK_COUNT 4
#define LOOP_BEATS_PER_BAR 4

#define LOOP_MIN_BPM 60
#define LOOP_MAX_BPM 140
#define LOOP_MIN_BARS 1
#define LOOP_MAX_BARS 8
#define LOOP_BPM_STEP 10

// 音频任务每次处理 240 帧 = 15 ms。循环长度对齐到 240 帧,
// 这样"循环回绕"永远落在块边界上,录音与混音都不需要处理半块。
#define LOOP_CHUNK_FRAMES 240
#define LOOP_MAX_TRACK_SAMPLES 512000u /* 8 小节 @ 60 BPM */

typedef struct {
    uint16_t bpm;
    uint8_t bars;
    uint8_t beats_per_bar;
} loop_timing_t;

// 一首 loop 的完整状态。这个结构会被原样写进 Flash 的超级块,字段布局不可随意改动。
#define LOOP_SB_MAGIC 0x4C504144u /* "LPAD" */
#define LOOP_SB_VERSION 1u

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t bpm;
    uint8_t bars;
    uint8_t beats_per_bar;
    uint16_t reserved;
    uint32_t loop_len; /* 采样数,已对齐到 LOOP_CHUNK_FRAMES */
    struct __attribute__((packed)) {
        uint32_t len;    /* 该轨录制的采样数;0 = 空轨 */
        uint16_t peak;   /* 录音峰值绝对值,播放时用来归一化音量 */
        uint16_t volume; /* 0..256(q8),256 = 100% */
        uint8_t present; /* 0/1 */
        uint8_t mute;    /* 0/1 */
        uint16_t reserved;
    } track[LOOP_TRACK_COUNT];
    uint32_t seq; /* 提交序号,A/B 两份超级块取较大的 */
    uint32_t crc; /* 前面所有字节的 CRC32 */
} loop_superblock_t;

#define LOOP_SB_CRC_BYTES (sizeof(loop_superblock_t) - sizeof(uint32_t))

// ---- 时序 ----
bool loop_timing_valid(uint16_t bpm, uint8_t bars);
uint16_t loop_bpm_next(uint16_t bpm, int direction);
uint8_t loop_bars_next(uint8_t bars, int direction);
uint32_t loop_total_samples(const loop_timing_t *timing);
uint32_t loop_samples_per_bar(const loop_timing_t *timing);
// 把采样位置映射成"第几小节 / 第几拍"(都从 0 开始),给界面用。
void loop_position(const loop_timing_t *timing, uint32_t samples, uint8_t *bar, uint8_t *beat);
uint32_t loop_ms_from_samples(uint32_t samples);

// ---- 增益与混音 ----
int16_t loop_apply_gain(int16_t sample, uint16_t gain_q8);
int16_t loop_clip_i16(int32_t value);
// 归一化增益:把峰值 peak 拉到目标 12000,限制在 1x..8x 之间(q8 即 256..2048)。
uint16_t loop_normalize_gain(uint16_t peak);

// ---- 变速 ----
#define LOOP_RATE_Q8_ONE 256u
#define LOOP_RATE_Q8_MIN 64u  /* 0.25x:磁带停转 */
#define LOOP_RATE_Q8_MAX 512u /* 2.0x:磁带快放   */
// 以 rate_q8 播放 LOOP_CHUNK_FRAMES 帧,需要从 Flash 取多少源采样(含插值余量)。
uint32_t loop_window_samples(uint16_t rate_q8);

// ---- 超级块 ----
uint32_t loop_crc32(const void *data, size_t len);
void loop_superblock_defaults(loop_superblock_t *sb, uint16_t bpm, uint8_t bars);
bool loop_superblock_valid(const loop_superblock_t *sb);
void loop_superblock_seal(loop_superblock_t *sb);
bool loop_superblock_newer(const loop_superblock_t *candidate, const loop_superblock_t *current);
void loop_superblock_clear_tracks(loop_superblock_t *sb);
