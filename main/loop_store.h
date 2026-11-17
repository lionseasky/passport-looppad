// main/loop_store.h —— "loops" 裸分区上的持久化:超级块 A/B 双备份 + 4 个轨道槽。
//
// 为什么不用文件系统:采样是定长、定偏移的,裸分区省掉 FAT/LittleFS 的 RAM 与代码,
// 写入路径也完全可预测(录音时每 15 ms 一次 480 字节页写)。
//
// 布局(分区大小 0x410000):
//   0x0000  超级块 A(4 KB 扇区)
//   0x1000  超级块 B(4 KB 扇区)
//   0x2000  轨道槽 0..3,每槽 1 MB(0x100000),可放 8 小节 @ 60 BPM
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "loop_core.h"

#define LOOP_STORE_PARTITION_LABEL "loops"
#define LOOP_STORE_SLOT_BYTES 0x100000u
#define LOOP_STORE_DATA_OFFSET 0x2000u

// 找到分区并加载最近一次有效提交;分区缺失时返回 ESP_ERR_NOT_FOUND。
// 成功调用可重复。超级块都无效时回填默认状态(found=false)。
esp_err_t loop_store_init(void);
bool loop_store_ready(void);

esp_err_t loop_store_load(loop_superblock_t *out, bool *found);

// 提交状态:自动递增 seq、重算 CRC,并写入【非当前】那份超级块(先擦后写)。
// 掉电只会损坏正在写的那份,另一份仍然可读。
esp_err_t loop_store_commit(loop_superblock_t *sb);

// 擦除某轨要用到的前 samples 个采样(向上取整到扇区)。录音前调用。
esp_err_t loop_store_erase_track(uint8_t track, uint32_t samples);

// 以采样为单位读写。offset + frames 不得超过该槽容量。
esp_err_t loop_store_write_track(uint8_t track, uint32_t offset, const int16_t *pcm, size_t frames);
esp_err_t loop_store_read_track(uint8_t track, uint32_t offset, int16_t *pcm, size_t frames);

// 整块擦除(仅用于"全部清空"后回收空间,正常流程不需要)。
esp_err_t loop_store_erase_all_tracks(void);
