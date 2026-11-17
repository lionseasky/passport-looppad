// main/loop_core.c —— 纯逻辑实现;不含任何 ESP-IDF 依赖,可被主机测试直接链接。
#include "loop_core.h"

#include <string.h>

_Static_assert(sizeof(loop_superblock_t) == 72, "超级块布局变了就要升版本号");
_Static_assert(offsetof(loop_superblock_t, track) == 16, "超级块轨道表偏移");
_Static_assert(offsetof(loop_superblock_t, crc) == 68, "超级块 CRC 必须是最后一个字段");

bool loop_timing_valid(uint16_t bpm, uint8_t bars)
{
    if (bpm < LOOP_MIN_BPM || bpm > LOOP_MAX_BPM) return false;
    if (bars < LOOP_MIN_BARS || bars > LOOP_MAX_BARS) return false;
    // 8 小节 @ 60 BPM 正好是 512000 采样,再长就超出单个轨道槽。
    return loop_total_samples(&(loop_timing_t){bpm, bars, LOOP_BEATS_PER_BAR}) <=
           LOOP_MAX_TRACK_SAMPLES;
}

uint16_t loop_bpm_next(uint16_t bpm, int direction)
{
    int value = (int)bpm + (direction >= 0 ? LOOP_BPM_STEP : -LOOP_BPM_STEP);
    if (value > LOOP_MAX_BPM) value = LOOP_MIN_BPM;
    if (value < LOOP_MIN_BPM) value = LOOP_MAX_BPM;
    return (uint16_t)value;
}

uint8_t loop_bars_next(uint8_t bars, int direction)
{
    static const uint8_t kChoices[] = {1, 2, 4, 8};
    const size_t count = sizeof(kChoices) / sizeof(kChoices[0]);
    size_t index = 0;
    for (size_t i = 0; i < count; i++) {
        if (kChoices[i] == bars) {
            index = i;
            break;
        }
    }
    if (direction >= 0) {
        index = (index + 1) % count;
    } else {
        index = (index + count - 1) % count;
    }
    return kChoices[index];
}

uint32_t loop_total_samples(const loop_timing_t *timing)
{
    if (timing == NULL || timing->bpm == 0 || timing->bars == 0) return 0;
    const uint64_t beats = (uint64_t)timing->bars * timing->beats_per_bar;
    const uint64_t raw = beats * 60u * LOOP_SAMPLE_RATE / timing->bpm;
    // 就近对齐到块边界:最多偏 7.5 ms,听感上是零,换来录音/混音永远整块处理。
    const uint64_t chunks = (raw + LOOP_CHUNK_FRAMES / 2) / LOOP_CHUNK_FRAMES;
    return (uint32_t)(chunks * LOOP_CHUNK_FRAMES);
}

uint32_t loop_samples_per_bar(const loop_timing_t *timing)
{
    if (timing == NULL || timing->bars == 0) return 0;
    return loop_total_samples(timing) / timing->bars;
}

void loop_position(const loop_timing_t *timing, uint32_t samples, uint8_t *bar, uint8_t *beat)
{
    uint8_t bar_out = 0;
    uint8_t beat_out = 0;
    const uint32_t total = loop_total_samples(timing);
    if (timing != NULL && total > 0) {
        if (samples >= total) samples %= total;
        const uint32_t beats_total = (uint32_t)timing->bars * timing->beats_per_bar;
        const uint32_t beat_index = (uint32_t)(((uint64_t)samples * beats_total) / total);
        bar_out = (uint8_t)(beat_index / timing->beats_per_bar);
        beat_out = (uint8_t)(beat_index % timing->beats_per_bar);
    }
    if (bar != NULL) *bar = bar_out;
    if (beat != NULL) *beat = beat_out;
}

uint32_t loop_ms_from_samples(uint32_t samples)
{
    return (uint32_t)(((uint64_t)samples * 1000u) / LOOP_SAMPLE_RATE);
}

int16_t loop_apply_gain(int16_t sample, uint16_t gain_q8)
{
    return loop_clip_i16(((int32_t)sample * (int32_t)gain_q8) >> 8);
}

int16_t loop_clip_i16(int32_t value)
{
    if (value > 32767) return 32767;
    if (value < -32768) return -32768;
    return (int16_t)value;
}

uint16_t loop_normalize_gain(uint16_t peak)
{
    if (peak == 0) return 256;
    const uint32_t gain = (12000u * 256u) / peak;
    if (gain < 256u) return 256;
    if (gain > 2048u) return 2048;
    return (uint16_t)gain;
}

uint32_t loop_window_samples(uint16_t rate_q8)
{
    if (rate_q8 < LOOP_RATE_Q8_MIN) rate_q8 = LOOP_RATE_Q8_MIN;
    if (rate_q8 > LOOP_RATE_Q8_MAX) rate_q8 = LOOP_RATE_Q8_MAX;
    const uint32_t span = ((uint32_t)LOOP_CHUNK_FRAMES * rate_q8 + LOOP_RATE_Q8_ONE - 1) /
                          LOOP_RATE_Q8_ONE;
    // +2:线性插值要取右邻点,回绕读取也要留一个安全余量。
    return span + 2;
}

uint32_t loop_crc32(const void *data, size_t len)
{
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= bytes[i];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

void loop_superblock_defaults(loop_superblock_t *sb, uint16_t bpm, uint8_t bars)
{
    memset(sb, 0, sizeof(*sb));
    sb->magic = LOOP_SB_MAGIC;
    sb->version = LOOP_SB_VERSION;
    sb->bpm = bpm;
    sb->bars = bars;
    sb->beats_per_bar = LOOP_BEATS_PER_BAR;
    sb->loop_len = loop_total_samples(&(loop_timing_t){bpm, bars, LOOP_BEATS_PER_BAR});
    for (size_t i = 0; i < LOOP_TRACK_COUNT; i++) {
        sb->track[i].volume = 256;
    }
    loop_superblock_seal(sb);
}

bool loop_superblock_valid(const loop_superblock_t *sb)
{
    if (sb == NULL) return false;
    if (sb->magic != LOOP_SB_MAGIC || sb->version != LOOP_SB_VERSION) return false;
    if (sb->crc != loop_crc32(sb, LOOP_SB_CRC_BYTES)) return false;
    if (!loop_timing_valid(sb->bpm, sb->bars)) return false;
    if (sb->beats_per_bar != LOOP_BEATS_PER_BAR) return false;
    if (sb->loop_len != loop_total_samples(&(loop_timing_t){sb->bpm, sb->bars, sb->beats_per_bar})) {
        return false;
    }
    for (size_t i = 0; i < LOOP_TRACK_COUNT; i++) {
        const uint32_t len = sb->track[i].len;
        if (len > sb->loop_len) return false;
        if (sb->track[i].present && len != sb->loop_len) return false;
        if (sb->track[i].volume > 256) return false;
    }
    return true;
}

void loop_superblock_seal(loop_superblock_t *sb)
{
    sb->crc = loop_crc32(sb, LOOP_SB_CRC_BYTES);
}

bool loop_superblock_newer(const loop_superblock_t *candidate, const loop_superblock_t *current)
{
    if (candidate == NULL) return false;
    if (current == NULL) return true;
    // 用有符号差值比较,序号回绕时仍然正确。
    return (int32_t)(candidate->seq - current->seq) > 0;
}

void loop_superblock_clear_tracks(loop_superblock_t *sb)
{
    for (size_t i = 0; i < LOOP_TRACK_COUNT; i++) {
        sb->track[i].len = 0;
        sb->track[i].peak = 0;
        sb->track[i].present = 0;
        sb->track[i].mute = 0;
    }
}
