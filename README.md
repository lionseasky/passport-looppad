<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

<h1 align="center">LOOP PAD</h1>

<p align="center">
  <strong>Four-track lo-fi beatbox sampler for FoloToy AI Passport</strong><br>
  Beatbox into the microphone. The device loops it. Stack three more layers.
</p>

---

LOOP PAD turns an [FoloToy AI Passport](https://ai-passport.folotoy.cn/) (ESP32-C3,
8 MB Flash, 240×320 screen, microphone, speaker, three buttons) into a pocket
sampler. There is no Wi-Fi, no account, and no cloud: everything is recorded,
looped and stored on the device.

> **v0.1 — first public build.** The firmware builds and passes host tests, but it
> has **not been validated on hardware yet**. See [Status](#status).

## Features

- **Four tracks**, each up to 8 bars (32 s at 60 BPM).
- **One-button recording**: erase the target track, count in one bar, record exactly
  one loop length, then start playing automatically.
- **Visual metronome**: the speaker is muted while recording, because the microphone
  and speaker share one plastic shell and monitoring would howl.
- **Tape stop**: hold UP to slow the whole loop to 0.5× and release to snap back.
- **Power-loss safe**: raw 16 kHz/16-bit PCM in a dedicated partition, with an A/B
  superblock written alternately.
- **Chinese UI**: self-drawn 240×320 screens with generated 16/24/32 px fonts.

## Flash the release

Download `LOOP-PAD-v1.0-esp32c3-full.bin` from the [v0.1 release](https://github.com/lionseasky/passport-looppad/releases/tag/v0.1)
and write it at offset `0x0`:

```bash
esptool.py --chip esp32c3 -p /dev/cu.usbmodemXXXX -b 460800 \
    write_flash 0x0 LOOP-PAD-v1.0-esp32c3-full.bin
```

The merged image rewrites the partition table and adds the `loops` data partition
(`0x310000`, 4.06 MB). Verify with `shasum -a 256 -c SHA256SUMS` before flashing.

A correct boot shows an amber **LOOP PAD** title at the top-left and four empty
track rows; the serial log prints `LOOP PAD v0.1`.

## Controls

| Screen | Input | Action |
| --- | --- | --- |
| Perform | UP / DOWN | Select track |
| Perform | OK | Play / stop |
| Perform | OK long | Record into the selected track |
| Perform | UP long | Tape stop (hold) |
| Perform | DOWN long | Settings |
| Recording | OK | Cancel |
| Settings | UP / DOWN | Move row |
| Settings | OK | Change value |
| Settings | OK long | Back (saves volumes) |

## Build from source

Requires ESP-IDF 5.5.3 and the ESP32-C3 toolchain.

```bash
source ~/esp/esp-idf-v5.5.3/export.sh
idf.py set-target esp32c3
./tools/validate.sh --static     # repository checks + host tests
./tools/validate.sh --firmware   # build + merged 0x0 image verification
```

`./tools/validate.sh` writes the verified merged image to
`build/FoloToy-AI-Passport-full.bin`.

## Repository layout

```text
main/loop_core.c    pure loop timing, gain, CRC and superblock logic (host-tested)
main/loop_store.c   raw partition access, A/B commit, chunked erase
main/loop_audio.c   audio task: count-in, recording, four-track mix, tape rate
main/loop_ui.c      LVGL screens and the visual metronome
main/loop_app.c     button queue, page state, battery polling
assets/fonts/       generated 16/24/32 px CJK fonts
tools/loop_font.py  font generator with pixel-exact self-check
tests/              host tests for loop logic and font coverage
docs/apps/          product manual (English and Chinese)
```

## Status

```text
Build:        PASS
Host tests:   PASS
Device tests: NOT RUN
```

Known limitations: no monitoring while recording (no headphone jack), 16 kHz
top end flattens sampled hi-hats, tape stop bottoms out at 0.5×.

## Credits

Built on the [FoloToy AI Passport](https://gitee.com/FoloToy/ai-passport) open
firmware (MIT). BSP drivers are reused; the application UI is purpose-built for
this sampler.
