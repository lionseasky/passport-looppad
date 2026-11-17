<p align="right">
  <a href="loop-pad.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# LOOP PAD — Pocket Beatbox Sampler

LOOP PAD turns FoloToy AI Passport into a four-track lo-fi sampler: beatbox into
the built-in microphone, the device loops it, and you stack three more layers on
top until one mouth sounds like a band.

This page is the product manual. Implementation notes for developers are at the
end.

The on-device interface is in Simplified Chinese; each message is
described in English below.

## 1. What you get

- Four independent tracks, each up to 8 bars long.
- One-button recording: the device erases the target track, counts in one bar,
  records exactly the loop length, and starts playing again by itself.
- A visual metronome that keeps time while the speaker is muted.
- Tape-stop effect: hold UP to slow the whole loop down like a dying tape.
- Everything is stored on the device. No Wi-Fi, no account, no cloud.

## 2. Requirements

| Item | Value |
| --- | --- |
| Device | FoloToy AI Passport (ESP32-C3, 8 MB Flash) |
| Firmware | `FoloToy-AI-Passport-full.bin`, flashed at offset `0x0` |
| Host tool | ESP-IDF 5.5.3 (only if you build it yourself) |
| Audio | Built-in microphone and speaker; no accessories needed |

Flashing the merged image replaces the firmware and creates the `loops` data
partition on first boot. Existing recordings from another firmware are ignored
and reported as an empty kit.

## 3. First session

1. Power on. The screen shows `LOOP PAD`, the tempo (`90 BPM`), four empty track
   rows, and the ready message.
2. **Long-press OK.** The screen shows the preparing message while the target track
   is erased, then the count-in screen with a large beat number and a click.
3. Beatbox one bar of drums. A bar of click is followed by exactly four bars of
   recording; the big number counts the beat you are on.
4. Recording stops by itself and the loop starts playing. Track 1 now says
   that it is playing.
5. Press DOWN to select track 2, **long-press OK**, and hum a bass line over the
   same four bars. Repeat for tracks 3 and 4.
6. Short-press OK to stop or restart the loop. Hold UP to hear the tape stop.
7. Long-press DOWN to open settings; long-press OK there to go back.

The device stores everything as soon as a recording finishes, so you can power
off and continue later.

## 4. Controls

### Performance screen

| Input | Action |
| --- | --- |
| UP / DOWN short press | Select the track (up/down) |
| OK short press | Play / stop |
| OK long press | Record into the selected track (replaces its content) |
| UP long press (hold) | Tape stop: slow down to 0.5x; release returns to normal |
| DOWN long press | Open settings |

### Recording screen

| Input | Action |
| --- | --- |
| OK short press | Cancel the recording; an existing loop resumes playing |

The speaker is muted while recording, on purpose: the microphone and speaker
share one plastic shell, so monitoring would howl. The screen is the metronome.
A one-bar click plays before recording starts so you can lock the tempo.

### Settings screen

| Row | OK short press |
| --- | --- |
| Tempo | Next step (60 to 140 BPM, step 10) |
| Bars | Cycle 1 → 2 → 4 → 8 |
| Drum / Bass / Chord / Vocal volume | Cycle 0 → 25 → 50 → 75 → 100 % |
| Clear current track | Erase the track selected on the performance screen |
| Clear all | Erase all four tracks and stop playback |
| Back | Return to the performance screen |

Tempo and bar length can only be changed while all tracks are empty; the rows
show a clear-first message otherwise. Volumes are saved when you leave the
settings screen.

## 5. Screens

- **Performance**: tempo and battery on top, four beat dots, one row per track
  with a level bar, and a hint line that alternates between record and settings
  shortcuts.
- **Recording**: full-screen. A large number is the beat; the sub-line shows the
  bar you are recording; two bars show loop progress and input level.
- **Settings**: nine rows, the selected row highlighted in amber.
- **Toast**: a short message at the bottom (stopped, cleared, record-a-track-first, …).

## 6. Loop length and storage

The loop length is fixed by tempo and bars and rounded to a 15 ms block so that
recording and mixing always work on whole blocks.

| Tempo | Bars | Duration |
| --- | --- | --- |
| 120 BPM | 1 | 2.0 s |
| 90 BPM | 4 | 10.7 s |
| 60 BPM | 4 | 16.0 s |
| 60 BPM | 8 | 32.0 s (maximum) |

| Partition | Offset | Size | Purpose |
| --- | --- | --- | --- |
| `nvs` | `0x9000` | 24 KB | Reserved |
| `phy_init` | `0xf000` | 4 KB | RF calibration |
| `factory` | `0x10000` | 3 MB | Application |
| `loops` | `0x310000` | 4.06 MB | Two superblock copies + four 1 MB track slots |

Each track is stored as raw 16-bit PCM at 16 kHz (32 KB per second). The
superblock keeps tempo, bars, track presence, volume and peak level, and is
written alternately to two 4 KB slots so a power loss during save leaves the
previous copy readable.

## 7. Sound design

- 16 kHz, 16-bit mono. This is deliberately lo-fi: it keeps the noise floor and
  the slightly dull top end that make a mouth drum sound like a sampler.
- Each take is normalised at playback using the recorded peak, so a quiet take
  is lifted up to 8x without changing the file.
- The first and last 5 ms of every take are faded to kill the seam click.
- A recording that is too long to fit the slot is impossible by construction:
  tempo and bar limits bound the loop length to 32 seconds.

## 8. Known limitations

- **No monitoring while recording.** You cannot hear the other tracks during a
  take; the visual metronome replaces the click track. Adding a headphone jack
  is the hardware fix.
- 16 kHz has no content above 8 kHz, so sampled hi-hats sound dull. Synthesise
  them instead of recording them.
- The speaker is small; low bass lines lose their fundamental and are heard
  mostly as harmonics.
- Tape stop only goes down to 0.5x, not to a full stop.

## 9. Troubleshooting

| Symptom | Cause and fix |
| --- | --- |
| Screen stays black | Backlight is off; reflash and power-cycle. If it persists, the panel did not initialise — check the serial log. |
| The screen reports storage unavailable | The `loops` partition is missing. Flash the merged image at `0x0` so the partition table is written too. |
| The screen reports audio unavailable | The ES8311 codec did not initialise; check the serial log for I2C errors. |
| Recording is silent | Speak within 20 cm of the microphone; the input level bar on the recording screen should move. |
| Loop sounds out of time | Each track starts at the same sample zero, so tempo cannot drift. If it sounds wrong, the take itself was recorded off the click. |
| The tempo row refuses to change | Tracks still hold audio; clear them before changing tempo or bars. |

## 10. Implementation notes

```text
main/loop_core.c    pure logic: loop length, beat position, gain, CRC, superblock
main/loop_store.c   raw partition access, A/B superblock commit
main/loop_audio.c   audio task: erase, count-in, record, four-track mix, tape
main/loop_ui.c      LVGL screens and the visual metronome
main/loop_app.c     button queue, page state, battery polling
assets/fonts/       generated 16/24/32 px Chinese fonts (285 code points)
tools/loop_font.py  font generator; verifies every glyph against FreeType
tests/              host tests for loop logic and font coverage
```

- Audio runs in one task with 240-frame chunks. Recording writes one 480-byte
  page per chunk and never erases while audio is playing.
- `loop_core.c` has no ESP-IDF dependency and is covered by
  `tests/test_loop_core.c`; `tests/test_lofi_font_coverage.py` fails when UI
  strings are added without regenerating the fonts.
- Bluetooth is disabled in `sdkconfig.defaults`; the sampler is fully offline.
