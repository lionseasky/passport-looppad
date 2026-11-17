// tests/test_loop_core.c —— loop_core 的主机侧单元测试(不依赖 ESP-IDF)。
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "loop_core.h"

static int s_failures;

#define CHECK(expr)                                                          \
    do {                                                                     \
        if (!(expr)) {                                                       \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);           \
            s_failures++;                                                    \
        }                                                                    \
    } while (0)

static void test_timing_math(void)
{
    loop_timing_t t90x4 = {90, 4, LOOP_BEATS_PER_BAR};
    loop_timing_t t60x8 = {60, 8, LOOP_BEATS_PER_BAR};
    loop_timing_t t120x1 = {120, 1, LOOP_BEATS_PER_BAR};

    // 4 小节 @ 90 BPM = 10.667 s,对齐到 240 帧的整数倍。
    CHECK(loop_total_samples(&t90x4) == 170640u);
    CHECK(loop_total_samples(&t90x4) % LOOP_CHUNK_FRAMES == 0);
    CHECK(loop_samples_per_bar(&t90x4) == 42660u);

    // 8 小节 @ 60 BPM 是最长的一档,必须塞得进单个轨道槽(512000 采样)。
    CHECK(loop_total_samples(&t60x8) == 511920u);
    CHECK(loop_total_samples(&t60x8) <= LOOP_MAX_TRACK_SAMPLES);

    CHECK(loop_total_samples(&t120x1) == 31920u);

    CHECK(loop_timing_valid(60, 1));
    CHECK(loop_timing_valid(90, 4));
    CHECK(loop_timing_valid(60, 8));  // 最长一档,刚好放得下
    CHECK(loop_timing_valid(140, 8)); // 快歌反而更短
    CHECK(loop_timing_valid(59, 4) == false);
    CHECK(loop_timing_valid(141, 4) == false);
    CHECK(loop_timing_valid(90, 0) == false);
    CHECK(loop_timing_valid(90, 9) == false);
}

static void test_position(void)
{
    loop_timing_t timing = {90, 4, LOOP_BEATS_PER_BAR};
    uint8_t bar = 0xFF;
    uint8_t beat = 0xFF;

    loop_position(&timing, 0, &bar, &beat);
    CHECK(bar == 0 && beat == 0);

    loop_position(&timing, 42660, &bar, &beat);
    CHECK(bar == 1 && beat == 0);

    loop_position(&timing, 42660 + 10665, &bar, &beat);
    CHECK(bar == 1 && beat == 1);

    // 超出长度的位置按回绕处理。
    loop_position(&timing, 170640 + 42660, &bar, &beat);
    CHECK(bar == 1 && beat == 0);

    loop_position(&timing, 170639, &bar, &beat);
    CHECK(bar == 3 && beat == 3);
}

static void test_gain(void)
{
    CHECK(loop_apply_gain(1000, 256) == 1000);
    CHECK(loop_apply_gain(1000, 128) == 500);
    CHECK(loop_apply_gain(0, 2048) == 0);
    CHECK(loop_apply_gain(30000, 512) == 32767); // 饱和而不是回绕
    CHECK(loop_apply_gain(-30000, 512) == -32768);
    CHECK(loop_clip_i16(40000) == 32767);
    CHECK(loop_clip_i16(-40000) == -32768);

    CHECK(loop_normalize_gain(0) == 256);
    CHECK(loop_normalize_gain(12000) == 256);
    CHECK(loop_normalize_gain(3000) == 1024);
    CHECK(loop_normalize_gain(100) == 2048); // 8x 上限
}

static void test_window(void)
{
    CHECK(loop_window_samples(LOOP_RATE_Q8_ONE) == 242);
    CHECK(loop_window_samples(LOOP_RATE_Q8_MAX) == 482);
    CHECK(loop_window_samples(LOOP_RATE_Q8_MIN) == 62);
    // 超出范围时按边界钳制,不能返回 0 让调用方读到空缓冲。
    CHECK(loop_window_samples(0) == 62);
    CHECK(loop_window_samples(9999) == 482);
}

static void test_crc_and_superblock(void)
{
    CHECK(loop_crc32("123456789", 9) == 0xCBF43926u);

    loop_superblock_t sb;
    loop_superblock_defaults(&sb, 90, 4);
    CHECK(sb.magic == LOOP_SB_MAGIC);
    CHECK(sb.loop_len == 170640u);
    CHECK(loop_superblock_valid(&sb));

    loop_superblock_t broken = sb;
    broken.bpm = 91;
    CHECK(loop_superblock_valid(&broken) == false); // CRC 不再匹配

    loop_superblock_t rec = sb;
    rec.track[2].present = 1;
    rec.track[2].len = rec.loop_len;
    loop_superblock_seal(&rec);
    CHECK(loop_superblock_valid(&rec));

    // 有内容但长度不等于 loop_len 的超级块必须被判为损坏。
    loop_superblock_t bad = rec;
    bad.track[2].len = 100;
    loop_superblock_seal(&bad);
    CHECK(loop_superblock_valid(&bad) == false);

    loop_superblock_t wrapped = sb;
    wrapped.seq = 0xFFFFFFFFu;
    rec.seq = 1; // 序号回绕后仍然"更新"
    CHECK(loop_superblock_newer(&rec, &wrapped));

    loop_superblock_clear_tracks(&rec);
    CHECK(rec.track[2].present == 0);
    CHECK(rec.track[2].len == 0);
    CHECK(loop_superblock_valid(&sb));
}

static void test_steppers(void)
{
    CHECK(loop_bpm_next(90, 1) == 100);
    CHECK(loop_bpm_next(140, 1) == 60);
    CHECK(loop_bpm_next(60, -1) == 140);
    CHECK(loop_bars_next(4, 1) == 8);
    CHECK(loop_bars_next(8, 1) == 1);
    CHECK(loop_bars_next(1, -1) == 8);
    CHECK(loop_ms_from_samples(16000) == 1000);
}

int main(void)
{
    test_timing_math();
    test_position();
    test_gain();
    test_window();
    test_crc_and_superblock();
    test_steppers();

    if (s_failures != 0) {
        printf("loop_core: %d failure(s)\n", s_failures);
        return 1;
    }
    printf("loop_core: PASS\n");
    return 0;
}
