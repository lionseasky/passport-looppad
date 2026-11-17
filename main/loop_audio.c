// main/loop_audio.c —— 音频引擎实现。
#include "loop_audio.h"

#include <inttypes.h>
#include <string.h>

#include "bsp_audio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "loop_store.h"

static const char *TAG = "loop_audio";

#define AUDIO_TASK_STACK 6144
#define AUDIO_TASK_PRIO 9
#define WINDOW_MAX 512 /* loop_window_samples(512) = 482 */
#define FADE_FRAMES 80 /* 5 ms 淡入淡出,压住循环接缝的咔哒声 */

// 1 kHz、幅度 3000 的正弦表,64 点 = 4 个周期 @16 kHz。只给倒计时咔哒用。
static const int16_t kSine[64] = {
    0,    294,  585,  871,  1148, 1414, 1667, 1903,
    2121, 2319, 2494, 2646, 2772, 2871, 2942, 2986,
    3000, 2986, 2942, 2871, 2772, 2646, 2494, 2319,
    2121, 1903, 1667, 1414, 1148, 871,  585,  294,
    0,    -294, -585, -871, -1148, -1414, -1667, -1903,
    -2121, -2319, -2494, -2646, -2772, -2871, -2942, -2986,
    -3000, -2986, -2942, -2871, -2772, -2646, -2494, -2319,
    -2121, -1903, -1667, -1414, -1148, -871, -585, -294,
};

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static loop_superblock_t s_sb;
static volatile loop_mode_t s_mode;
static volatile uint8_t s_record_track;
static volatile uint32_t s_pos;
static volatile uint32_t s_frac;
static volatile uint8_t s_level;
static volatile uint32_t s_revision;
static volatile uint16_t s_rate_q8 = LOOP_RATE_Q8_ONE;

// 以下变量只在音频任务里读写。
static uint16_t s_rate_target = LOOP_RATE_Q8_ONE;
static uint32_t s_countin_pos;
static uint32_t s_countin_frames;
static uint32_t s_bar_len;
static uint32_t s_rec_samples;
static uint16_t s_rec_peak;
static bool s_ready;

static int16_t s_out[LOOP_CHUNK_FRAMES];
static int16_t s_in[LOOP_CHUNK_FRAMES];
static int16_t s_win[WINDOW_MAX];
static int32_t s_acc[LOOP_CHUNK_FRAMES];

static bool model_has_tracks(void)
{
    bool any = false;
    taskENTER_CRITICAL(&s_mux);
    for (size_t i = 0; i < LOOP_TRACK_COUNT; i++) {
        if (s_sb.track[i].present) any = true;
    }
    taskEXIT_CRITICAL(&s_mux);
    return any;
}

static void start_recording_now(void)
{
    s_rec_samples = 0;
    s_rec_peak = 0;
    s_pos = 0;
    s_frac = 0;
    s_rate_q8 = LOOP_RATE_Q8_ONE;
    s_rate_target = LOOP_RATE_Q8_ONE;
    s_mode = LOOP_MODE_RECORD;
    ESP_LOGI(TAG, "开始录音:轨 %u,长度 %" PRIu32 " 采样", s_record_track, s_sb.loop_len);
}

static void read_wrapped(uint8_t track, uint32_t start, int16_t *dst, uint32_t frames,
                         uint32_t loop_len)
{
    start %= loop_len;
    uint32_t first = loop_len - start;
    if (first > frames) first = frames;
    if (loop_store_read_track(track, start, dst, first) != ESP_OK) {
        memset(dst, 0, first * sizeof(int16_t));
    }
    if (frames > first) {
        uint32_t rest = frames - first;
        if (loop_store_read_track(track, 0, dst + first, rest) != ESP_OK) {
            memset(dst + first, 0, rest * sizeof(int16_t));
        }
    }
}

static void mix_chunk(void)
{
    loop_superblock_t sb;
    taskENTER_CRITICAL(&s_mux);
    sb = s_sb;
    taskEXIT_CRITICAL(&s_mux);

    if (sb.loop_len == 0) {
        memset(s_out, 0, sizeof(s_out));
        return;
    }

    memset(s_acc, 0, sizeof(s_acc));
    const uint16_t rate = s_rate_q8;
    const uint32_t window = loop_window_samples(rate);
    const uint32_t step = (uint32_t)rate << 8; /* Q16.16 步长 */
    const uint32_t start = s_pos;
    const uint32_t frac = s_frac;

    for (uint8_t t = 0; t < LOOP_TRACK_COUNT; t++) {
        if (!sb.track[t].present || sb.track[t].mute) continue;
        if (sb.track[t].volume == 0) continue;
        const uint32_t gain = ((uint32_t)sb.track[t].volume *
                               loop_normalize_gain(sb.track[t].peak)) >> 8;
        read_wrapped(t, start, s_win, window, sb.loop_len);
        uint32_t cursor = frac;
        for (uint32_t i = 0; i < LOOP_CHUNK_FRAMES; i++) {
            const uint32_t off = cursor >> 16;
            const int32_t a = s_win[off];
            const int32_t b = s_win[off + 1];
            const int32_t v = a + (((b - a) * (int32_t)(cursor & 0xFFFF)) >> 16);
            s_acc[i] += (v * (int32_t)gain) >> 8;
            cursor += step;
        }
    }

    for (uint32_t i = 0; i < LOOP_CHUNK_FRAMES; i++) {
        s_out[i] = loop_clip_i16(s_acc[i]);
    }

    const uint32_t advanced = frac + step * LOOP_CHUNK_FRAMES;
    s_pos = (start + (advanced >> 16)) % sb.loop_len;
    s_frac = advanced & 0xFFFF;
}

static void countin_chunk(void)
{
    const uint32_t beat_len = (s_bar_len / LOOP_BEATS_PER_BAR) > 0
                                  ? (s_bar_len / LOOP_BEATS_PER_BAR)
                                  : 1;
    const uint32_t click_len = (LOOP_SAMPLE_RATE * 30) / 1000;

    for (uint32_t i = 0; i < LOOP_CHUNK_FRAMES; i++) {
        const uint32_t abs = s_countin_pos + i;
        const uint32_t index = abs / beat_len;
        const uint32_t into = abs - index * beat_len;
        int32_t sample = 0;
        if (into < click_len) {
            const int32_t amp = (index % LOOP_BEATS_PER_BAR == 0) ? 5200 : 3400;
            const int32_t env = amp * (int32_t)(click_len - into) / (int32_t)click_len;
            sample = (kSine[(into * 4) & 63] * env) / 3000;
        }
        s_out[i] = loop_clip_i16(sample);
    }

    if (bsp_audio_write(s_out, sizeof(s_out)) != ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    s_countin_pos += LOOP_CHUNK_FRAMES;

    if (s_countin_pos >= s_countin_frames) {
        start_recording_now();
    }
}

static void record_chunk(void)
{
    if (bsp_audio_read(s_in, sizeof(s_in)) != ESP_OK) {
        memset(s_in, 0, sizeof(s_in));
    }

    uint32_t peak = 0;
    for (uint32_t i = 0; i < LOOP_CHUNK_FRAMES; i++) {
        const int32_t v = s_in[i] < 0 ? -(int32_t)s_in[i] : s_in[i];
        if (v > (int32_t)peak) peak = (uint32_t)v;
    }
    if (peak > s_rec_peak) s_rec_peak = (uint16_t)(peak > 32767 ? 32767 : peak);

    // 首尾各 5 ms 淡入淡出,避免循环接缝处的爆音。
    if (s_rec_samples == 0) {
        for (uint32_t i = 0; i < FADE_FRAMES; i++) {
            s_in[i] = (int16_t)(((int32_t)s_in[i] * (int32_t)i) / FADE_FRAMES);
        }
    }
    const uint32_t remain = s_sb.loop_len - s_rec_samples;
    if (remain == LOOP_CHUNK_FRAMES) {
        for (uint32_t i = 0; i < FADE_FRAMES; i++) {
            const uint32_t index = LOOP_CHUNK_FRAMES - FADE_FRAMES + i;
            s_in[index] = (int16_t)(((int32_t)s_in[index] * (int32_t)(FADE_FRAMES - i)) /
                                    FADE_FRAMES);
        }
    }

    esp_err_t err = loop_store_write_track(s_record_track, s_rec_samples, s_in,
                                           LOOP_CHUNK_FRAMES);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "写 Flash 失败: %s", esp_err_to_name(err));
        s_mode = LOOP_MODE_OFF;
        return;
    }
    s_rec_samples += LOOP_CHUNK_FRAMES;
    s_pos = s_rec_samples < s_sb.loop_len ? s_rec_samples : 0;

    const uint32_t level = (peak * 100u) / 20000u;
    s_level = (uint8_t)(level > 100 ? 100 : level);

    // 录音期间喇叭保持静音,但仍要喂 I2S,否则 TX 欠载会出杂音。
    memset(s_out, 0, sizeof(s_out));
    if (bsp_audio_write(s_out, sizeof(s_out)) != ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (s_rec_samples >= s_sb.loop_len) {
        loop_superblock_t tmp;
        taskENTER_CRITICAL(&s_mux);
        tmp = s_sb;
        taskEXIT_CRITICAL(&s_mux);
        tmp.track[s_record_track].len = tmp.loop_len;
        tmp.track[s_record_track].peak = s_rec_peak;
        tmp.track[s_record_track].present = 1;
        esp_err_t commit_err = loop_store_commit(&tmp);
        if (commit_err == ESP_OK) {
            taskENTER_CRITICAL(&s_mux);
            s_sb = tmp;
            s_mode = LOOP_MODE_PLAY;
            s_pos = 0;
            s_frac = 0;
            s_level = 0;
            taskEXIT_CRITICAL(&s_mux);
            s_rate_q8 = LOOP_RATE_Q8_ONE;
            s_rate_target = LOOP_RATE_Q8_ONE;
            ESP_LOGI(TAG, "录音完成:轨 %u,峰值 %u", s_record_track, s_rec_peak);
        } else {
            ESP_LOGE(TAG, "提交录音失败: %s", esp_err_to_name(commit_err));
            s_mode = LOOP_MODE_OFF;
        }
    }
}

static void prep_chunk(void)
{
    // 先停输出:擦除期间 Flash 缓存会被关掉,音频代码和数据都可能停顿。
    memset(s_out, 0, sizeof(s_out));
    bsp_audio_write(s_out, sizeof(s_out));

    ESP_LOGI(TAG, "擦除轨道 %u …", s_record_track);
    esp_err_t err = loop_store_erase_track(s_record_track, s_sb.loop_len);
    if (err != ESP_OK) {
        s_mode = LOOP_MODE_OFF;
        return;
    }

    taskENTER_CRITICAL(&s_mux);
    s_sb.track[s_record_track].present = 0;
    s_sb.track[s_record_track].len = 0;
    s_sb.track[s_record_track].peak = 0;
    s_revision++;
    taskEXIT_CRITICAL(&s_mux);

    s_bar_len = s_sb.loop_len / s_sb.bars;
    s_countin_frames = ((s_bar_len + LOOP_CHUNK_FRAMES - 1) / LOOP_CHUNK_FRAMES) *
                       LOOP_CHUNK_FRAMES;
    s_countin_pos = 0;
    s_mode = LOOP_MODE_COUNTIN;
}

static void audio_task(void *arg)
{
    (void)arg;
    // 变速斜坡:每块(15 ms)靠近目标 16/256,0.5x ↔ 1.0x 大约 0.5 秒。
    for (;;) {
        if (s_rate_q8 < s_rate_target) {
            s_rate_q8 = (uint16_t)((s_rate_target - s_rate_q8 > 16)
                                       ? s_rate_q8 + 16
                                       : s_rate_target);
        } else if (s_rate_q8 > s_rate_target) {
            s_rate_q8 = (uint16_t)((s_rate_q8 - s_rate_target > 16)
                                       ? s_rate_q8 - 16
                                       : s_rate_target);
        }

        switch (s_mode) {
        case LOOP_MODE_OFF:
            s_level = (uint8_t)((s_level * 3) / 4);
            vTaskDelay(pdMS_TO_TICKS(20));
            break;
        case LOOP_MODE_PREP:
            prep_chunk();
            break;
        case LOOP_MODE_COUNTIN:
            countin_chunk();
            break;
        case LOOP_MODE_RECORD:
            record_chunk();
            break;
        case LOOP_MODE_PLAY:
            s_level = (uint8_t)((s_level * 7) / 8);
            mix_chunk();
            if (bsp_audio_write(s_out, sizeof(s_out)) != ESP_OK) {
                vTaskDelay(pdMS_TO_TICKS(10));
            }
            break;
        default:
            s_mode = LOOP_MODE_OFF;
            break;
        }
    }
}

esp_err_t loop_audio_init(void)
{
    esp_err_t err = bsp_audio_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "音频初始化失败: %s", esp_err_to_name(err));
        return err;
    }
    err = bsp_audio_set_format(LOOP_SAMPLE_RATE, 16, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "设置音频格式失败: %s", esp_err_to_name(err));
        return err;
    }
    bsp_audio_set_volume(85);

    bool found = false;
    esp_err_t store_err = loop_store_init();
    if (store_err == ESP_OK) {
        store_err = loop_store_load(&s_sb, &found);
    }
    if (store_err != ESP_OK || !found) {
        loop_superblock_defaults(&s_sb, 90, 4);
        if (store_err == ESP_OK) loop_store_commit(&s_sb);
        ESP_LOGI(TAG, "首次启动:默认 90 BPM / 4 小节");
    }

    if (xTaskCreate(audio_task, "loop_audio", AUDIO_TASK_STACK, NULL, AUDIO_TASK_PRIO, NULL) !=
        pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    s_ready = true;
    if (model_has_tracks()) {
        s_pos = 0;
        s_frac = 0;
        s_mode = LOOP_MODE_PLAY;
        ESP_LOGI(TAG, "检测到已有 loop,开机自动播放");
    }
    return ESP_OK;
}

bool loop_audio_ready(void)
{
    return s_ready;
}

void loop_audio_get_status(loop_audio_status_t *out)
{
    if (out == NULL) return;
    memset(out, 0, sizeof(*out));

    taskENTER_CRITICAL(&s_mux);
    out->mode = s_mode;
    out->storage_ok = loop_store_ready();
    out->record_track = s_record_track;
    out->bpm = s_sb.bpm;
    out->bars = s_sb.bars;
    out->loop_len = s_sb.loop_len;
    out->revision = s_revision;
    for (size_t i = 0; i < LOOP_TRACK_COUNT; i++) {
        out->tracks[i].present = s_sb.track[i].present;
        out->tracks[i].mute = s_sb.track[i].mute;
        out->tracks[i].volume = (uint8_t)((s_sb.track[i].volume * 100u) / 256u);
    }
    const uint32_t pos = s_pos;
    const uint32_t countin = s_countin_pos;
    const uint32_t countin_frames = s_countin_frames;
    const uint32_t rec = s_rec_samples;
    const uint32_t bar_len = s_bar_len;
    const uint16_t rate = s_rate_q8;
    taskEXIT_CRITICAL(&s_mux);

    if (out->bpm < LOOP_MIN_BPM || out->bpm > LOOP_MAX_BPM) out->bpm = 90;
    if (out->bars < LOOP_MIN_BARS || out->bars > LOOP_MAX_BARS) out->bars = 4;
    loop_timing_t timing = {out->bpm, out->bars, LOOP_BEATS_PER_BAR};
    if (out->mode == LOOP_MODE_PLAY || out->mode == LOOP_MODE_RECORD) {
        loop_position(&timing, pos, &out->bar, &out->beat);
    }
    if (bar_len > 0) {
        out->rec_bar = (uint8_t)(rec / bar_len);
        const uint32_t beat_len = bar_len / LOOP_BEATS_PER_BAR;
        uint32_t left = 0;
        if (countin_frames > countin && beat_len > 0) {
            left = (countin_frames - countin + beat_len - 1) / beat_len;
        }
        out->countin_left = (uint8_t)(left > LOOP_BEATS_PER_BAR ? LOOP_BEATS_PER_BAR : left);
    }
    out->level = s_level;
    out->rate_percent = (uint8_t)(((uint32_t)rate * 100u) / LOOP_RATE_Q8_ONE);
}

esp_err_t loop_audio_start_record(uint8_t track)
{
    if (track >= LOOP_TRACK_COUNT) return ESP_ERR_INVALID_ARG;
    if (!loop_store_ready()) return ESP_ERR_INVALID_STATE;
    loop_mode_t mode = s_mode;
    if (mode == LOOP_MODE_PREP || mode == LOOP_MODE_COUNTIN || mode == LOOP_MODE_RECORD) {
        return ESP_ERR_INVALID_STATE;
    }
    s_record_track = track;
    s_level = 0;
    s_mode = LOOP_MODE_PREP;
    return ESP_OK;
}

void loop_audio_cancel(void)
{
    loop_mode_t mode = s_mode;
    if (mode != LOOP_MODE_PREP && mode != LOOP_MODE_COUNTIN && mode != LOOP_MODE_RECORD) return;
    s_level = 0;
    if (model_has_tracks()) {
        // 取消录音不该把已有的 loop 也停掉:回到播放更符合直觉。
        s_pos = 0;
        s_frac = 0;
        s_mode = LOOP_MODE_PLAY;
    } else {
        s_mode = LOOP_MODE_OFF;
    }
    ESP_LOGI(TAG, "录音已取消");
}

void loop_audio_play(void)
{
    if (!model_has_tracks()) return;
    s_pos = 0;
    s_frac = 0;
    s_level = 0;
    s_mode = LOOP_MODE_PLAY;
}

void loop_audio_stop(void)
{
    s_mode = LOOP_MODE_OFF;
    s_level = 0;
    s_pos = 0;
    s_frac = 0;
}

esp_err_t loop_audio_set_track_volume(uint8_t track, uint8_t percent)
{
    if (track >= LOOP_TRACK_COUNT) return ESP_ERR_INVALID_ARG;
    if (percent > 100) percent = 100;
    taskENTER_CRITICAL(&s_mux);
    s_sb.track[track].volume = (uint16_t)((percent * 256u) / 100u);
    taskEXIT_CRITICAL(&s_mux);
    return ESP_OK;
}

esp_err_t loop_audio_set_track_mute(uint8_t track, bool mute)
{
    if (track >= LOOP_TRACK_COUNT) return ESP_ERR_INVALID_ARG;
    taskENTER_CRITICAL(&s_mux);
    s_sb.track[track].mute = mute ? 1 : 0;
    taskEXIT_CRITICAL(&s_mux);
    return ESP_OK;
}

esp_err_t loop_audio_persist(void)
{
    if (!loop_store_ready()) return ESP_ERR_INVALID_STATE;
    loop_superblock_t tmp;
    taskENTER_CRITICAL(&s_mux);
    tmp = s_sb;
    taskEXIT_CRITICAL(&s_mux);
    esp_err_t err = loop_store_commit(&tmp);
    if (err == ESP_OK) {
        taskENTER_CRITICAL(&s_mux);
        s_sb = tmp;
        taskEXIT_CRITICAL(&s_mux);
    }
    return err;
}

esp_err_t loop_audio_set_timing(uint16_t bpm, uint8_t bars)
{
    if (!loop_timing_valid(bpm, bars)) return ESP_ERR_INVALID_ARG;
    if (model_has_tracks()) return ESP_ERR_INVALID_STATE;
    loop_superblock_t tmp;
    taskENTER_CRITICAL(&s_mux);
    tmp = s_sb;
    taskEXIT_CRITICAL(&s_mux);
    tmp.bpm = bpm;
    tmp.bars = bars;
    tmp.loop_len = loop_total_samples(&(loop_timing_t){bpm, bars, LOOP_BEATS_PER_BAR});
    esp_err_t err = loop_store_commit(&tmp);
    if (err == ESP_OK) {
        taskENTER_CRITICAL(&s_mux);
        s_sb = tmp;
        s_revision++;
        taskEXIT_CRITICAL(&s_mux);
    }
    return err;
}

esp_err_t loop_audio_clear_track(uint8_t track)
{
    if (track >= LOOP_TRACK_COUNT) return ESP_ERR_INVALID_ARG;
    if (s_mode == LOOP_MODE_RECORD && s_record_track == track) s_mode = LOOP_MODE_OFF;

    loop_superblock_t tmp;
    taskENTER_CRITICAL(&s_mux);
    tmp = s_sb;
    taskEXIT_CRITICAL(&s_mux);
    tmp.track[track].present = 0;
    tmp.track[track].len = 0;
    tmp.track[track].peak = 0;
    esp_err_t err = loop_store_commit(&tmp);
    if (err == ESP_OK) {
        taskENTER_CRITICAL(&s_mux);
        s_sb = tmp;
        s_revision++;
        taskEXIT_CRITICAL(&s_mux);
    }
    if (!model_has_tracks()) loop_audio_stop();
    return err;
}

esp_err_t loop_audio_clear_all(void)
{
    loop_superblock_t tmp;
    taskENTER_CRITICAL(&s_mux);
    tmp = s_sb;
    taskEXIT_CRITICAL(&s_mux);
    loop_superblock_clear_tracks(&tmp);
    esp_err_t err = loop_store_commit(&tmp);
    if (err == ESP_OK) {
        taskENTER_CRITICAL(&s_mux);
        s_sb = tmp;
        s_revision++;
        taskEXIT_CRITICAL(&s_mux);
    }
    loop_audio_stop();
    return err;
}

void loop_audio_set_tape(bool engaging)
{
    s_rate_target = engaging ? (LOOP_RATE_Q8_ONE / 2) : LOOP_RATE_Q8_ONE;
}
