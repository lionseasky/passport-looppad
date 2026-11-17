<p align="right">
  <a href="README.md">English</a> · <strong>简体中文</strong>
</p>

<h1 align="center">LOOP PAD</h1>

<p align="center">
  <strong>FoloToy AI Passport 上的四轨 lo-fi 嘴鼓循环采样器</strong><br>
  对着麦克风打嘴鼓,设备把它录成循环,你再往上叠三层。
</p>

---

LOOP PAD 把一块 [FoloToy AI Passport](https://ai-passport.folotoy.cn/)(ESP32-C3,
8 MB Flash、240×320 屏幕、麦克风、喇叭、三个按键)变成口袋采样器。不需要 Wi-Fi、
不需要账号、不联云:录音、循环、存储全部在设备上完成。

> **v0.1 —— 首个公开版本。** 固件可以构建、主机测试通过,但**尚未在真机上验证**。
> 见[状态](#状态)。

## 功能

- **四条轨道**,每轨最长 8 小节(60 BPM 下 32 秒)。
- **一键录音**:擦除目标轨 → 倒计时一小节 → 严格录满一个循环 → 自动开始播放。
- **视觉节拍器**:录音时喇叭静音——麦克风和喇叭在同一个塑料腔体里,监听必然啸叫。
- **磁带效果**:按住 UP,整段循环降到 0.5 倍速,松手回速。
- **掉电不丢**:16 kHz/16-bit 裸 PCM 存在独立分区,超级块 A/B 双备份交替写入。
- **全中文界面**:自绘 240×320 界面,16/24/32 px 字库由脚本生成并逐像素自检。

## 刷写发布版

从 [v0.1 Release](https://github.com/lionseasky/passport-looppad/releases/tag/v0.1) 下载 `LOOP-PAD-v1.0-esp32c3-full.bin`,
从 `0x0` 整片写入:

```bash
esptool.py --chip esp32c3 -p /dev/cu.usbmodemXXXX -b 460800 \
    write_flash 0x0 LOOP-PAD-v1.0-esp32c3-full.bin
```

合并镜像会重写分区表,新增 `loops` 数据分区(`0x310000`,4.06 MB)。刷前请先
`shasum -a 256 -c SHA256SUMS` 校验。

刷对的样子:开机左上角是琥珀色 **LOOP PAD**,下面是四条空轨道;串口日志会打印
`LOOP PAD v0.1`。

## 按键

| 界面 | 操作 | 作用 |
| --- | --- | --- |
| 演奏页 | UP / DOWN | 选择轨道 |
| 演奏页 | OK | 播放 / 停止 |
| 演奏页 | OK 长按 | 录制选中轨道 |
| 演奏页 | UP 长按 | 磁带降速(按住生效) |
| 演奏页 | DOWN 长按 | 进入设置 |
| 录音页 | OK | 取消 |
| 设置页 | UP / DOWN | 移动光标 |
| 设置页 | OK | 修改数值 |
| 设置页 | OK 长按 | 返回(保存音量) |

## 从源码构建

需要 ESP-IDF 5.5.3 与 ESP32-C3 工具链。

```bash
source ~/esp/esp-idf-v5.5.3/export.sh
idf.py set-target esp32c3
./tools/validate.sh --static     # 仓库检查 + 主机测试
./tools/validate.sh --firmware   # 构建 + 合并镜像校验
```

`./tools/validate.sh` 会把校验通过的合并镜像写到 `build/FoloToy-AI-Passport-full.bin`。

## 目录结构

```text
main/loop_core.c    纯逻辑:循环长度、增益、CRC、超级块(有主机测试)
main/loop_store.c   裸分区读写、A/B 提交、分块擦除
main/loop_audio.c   音频任务:倒计时、录音、四轨混音、变速
main/loop_ui.c      LVGL 界面与视觉节拍器
main/loop_app.c     按键队列、页面状态、电量轮询
assets/fonts/       生成的 16/24/32 px 中文字库
tools/loop_font.py  字库生成器(逐像素自检)
tests/              循环逻辑与字库覆盖的主机测试
docs/apps/          产品说明书(中英双语)
```

## 状态

```text
Build:        PASS
Host tests:   PASS
Device tests: NOT RUN
```

已知限制:录音时没有监听(没有耳机口)、16 kHz 上限 8 kHz 让采样的踩镲发闷、
磁带效果最低只到 0.5 倍速。

## 致谢

基于 [FoloToy AI Passport](https://gitee.com/FoloToy/ai-passport) 开源固件(MIT)。
BSP 驱动直接复用;应用界面是为这台采样器重新设计的。
