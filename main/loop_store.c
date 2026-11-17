// main/loop_store.c —— 裸分区读写实现。
#include "loop_store.h"

#include <string.h>

#include "esp_log.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "loop_store";

#define SB_SLOT_A 0x0000u
#define SB_SLOT_B 0x1000u
#define SB_SLOT_BYTES 0x1000u

static const esp_partition_t *s_part;
static SemaphoreHandle_t s_lock;
static int s_active_slot = -1;
static bool s_ready;

static esp_err_t store_lock(void)
{
    if (s_lock == NULL) return ESP_ERR_INVALID_STATE;
    return xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE ? ESP_OK : ESP_FAIL;
}

static void store_unlock(void)
{
    if (s_lock != NULL) xSemaphoreGive(s_lock);
}

static uint32_t track_offset(uint8_t track)
{
    return LOOP_STORE_DATA_OFFSET + (uint32_t)track * LOOP_STORE_SLOT_BYTES;
}

esp_err_t loop_store_init(void)
{
    if (s_ready) return ESP_OK;
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) return ESP_ERR_NO_MEM;
    }
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x40,
                                      LOOP_STORE_PARTITION_LABEL);
    if (s_part == NULL) {
        ESP_LOGE(TAG, "找不到 %s 分区:请检查 partitions.csv 与烧录地址",
                 LOOP_STORE_PARTITION_LABEL);
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "分区 %s 偏移 0x%08" PRIx32 " 大小 0x%08" PRIx32,
             s_part->label, s_part->address, s_part->size);
    s_ready = true;
    return ESP_OK;
}

bool loop_store_ready(void)
{
    return s_ready;
}

esp_err_t loop_store_load(loop_superblock_t *out, bool *found)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;
    if (found != NULL) *found = false;
    if (!s_ready) return ESP_ERR_INVALID_STATE;

    esp_err_t err = store_lock();
    if (err != ESP_OK) return err;

    loop_superblock_t candidates[2];
    bool valid[2] = {false, false};
    const uint32_t offsets[2] = {SB_SLOT_A, SB_SLOT_B};
    for (int i = 0; i < 2; i++) {
        memset(&candidates[i], 0, sizeof(candidates[i]));
        esp_err_t read_err = esp_partition_read(s_part, offsets[i], &candidates[i],
                                                sizeof(candidates[i]));
        if (read_err == ESP_OK && loop_superblock_valid(&candidates[i])) {
            valid[i] = true;
        }
    }

    int best = -1;
    for (int i = 0; i < 2; i++) {
        if (!valid[i]) continue;
        if (best < 0 || loop_superblock_newer(&candidates[i], &candidates[best])) best = i;
    }
    if (best >= 0) {
        *out = candidates[best];
        s_active_slot = best;
        if (found != NULL) *found = true;
        ESP_LOGI(TAG, "载入超级块 slot=%d seq=%" PRIu32 " bpm=%u bars=%u",
                 best, out->seq, (unsigned)out->bpm, (unsigned)out->bars);
    }
    store_unlock();
    return ESP_OK;
}

esp_err_t loop_store_commit(loop_superblock_t *sb)
{
    if (sb == NULL) return ESP_ERR_INVALID_ARG;
    if (!s_ready) return ESP_ERR_INVALID_STATE;

    esp_err_t err = store_lock();
    if (err != ESP_OK) return err;

    const int target = (s_active_slot == 0) ? 1 : 0;
    const uint32_t offset = (target == 0) ? SB_SLOT_A : SB_SLOT_B;
    sb->seq++;
    loop_superblock_seal(sb);

    err = esp_partition_erase_range(s_part, offset, SB_SLOT_BYTES);
    if (err == ESP_OK) {
        err = esp_partition_write(s_part, offset, sb, sizeof(*sb));
    }
    if (err == ESP_OK) {
        s_active_slot = target;
        ESP_LOGI(TAG, "提交超级块 slot=%d seq=%" PRIu32, target, sb->seq);
    } else {
        ESP_LOGE(TAG, "提交超级块失败: %s", esp_err_to_name(err));
    }
    store_unlock();
    return err;
}

esp_err_t loop_store_erase_track(uint8_t track, uint32_t samples)
{
    if (track >= LOOP_TRACK_COUNT) return ESP_ERR_INVALID_ARG;
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    if (samples > LOOP_MAX_TRACK_SAMPLES) return ESP_ERR_INVALID_ARG;

    const uint32_t bytes = samples * sizeof(int16_t);
    // 扇区对齐:擦除范围必须从 4 KB 边界开始、长度为 4 KB 的整数倍。
    const uint32_t aligned = (bytes + 0xFFFu) & ~0xFFFu;
    esp_err_t err = store_lock();
    if (err == ESP_OK) {
        // 按 64 KB 分块擦除,每块之间让出 CPU:最长一档要擦 1 MB,
        // 一次性擦完会长时间独占 CPU(看门狗风险),分块后界面也能继续刷新。
        const uint32_t chunk = 0x10000u;
        for (uint32_t done = 0; done < aligned; done += chunk) {
            const uint32_t span = (aligned - done < chunk) ? (aligned - done) : chunk;
            err = esp_partition_erase_range(s_part, track_offset(track) + done, span);
            if (err != ESP_OK) break;
            vTaskDelay(1);
        }
        store_unlock();
    }
    if (err != ESP_OK) ESP_LOGE(TAG, "擦除轨道 %u 失败: %s", track, esp_err_to_name(err));
    return err;
}

esp_err_t loop_store_write_track(uint8_t track, uint32_t offset, const int16_t *pcm, size_t frames)
{
    if (track >= LOOP_TRACK_COUNT || pcm == NULL) return ESP_ERR_INVALID_ARG;
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    const uint32_t bytes = frames * sizeof(int16_t);
    if ((uint64_t)(offset + frames) * sizeof(int16_t) > LOOP_STORE_SLOT_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }
    return esp_partition_write(s_part, track_offset(track) + offset * sizeof(int16_t), pcm, bytes);
}

esp_err_t loop_store_read_track(uint8_t track, uint32_t offset, int16_t *pcm, size_t frames)
{
    if (track >= LOOP_TRACK_COUNT || pcm == NULL) return ESP_ERR_INVALID_ARG;
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    const uint32_t bytes = frames * sizeof(int16_t);
    if ((uint64_t)(offset + frames) * sizeof(int16_t) > LOOP_STORE_SLOT_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }
    return esp_partition_read(s_part, track_offset(track) + offset * sizeof(int16_t), pcm, bytes);
}

esp_err_t loop_store_erase_all_tracks(void)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    esp_err_t err = store_lock();
    if (err == ESP_OK) {
        err = esp_partition_erase_range(s_part, LOOP_STORE_DATA_OFFSET,
                                        LOOP_TRACK_COUNT * LOOP_STORE_SLOT_BYTES);
        store_unlock();
    }
    return err;
}
