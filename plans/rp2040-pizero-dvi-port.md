# Galagino → Waveshare RP2040-PiZero (DVI) Port Plan

**Target board:** Waveshare RP2040-PiZero (RP2040, 264 KB SRAM, 16 MB flash, HDMI/DVI connector)
**Video/audio library:** `pico_lib/dvi` (libdvi fork) from `~/Source/pico-infonesPlus`
**SDK:** `~/Source/pico-sdk`
**Status:** Planned — read the feasibility verdict before committing to hardware

---

## 0. Verdict first

**Pac-Man: yes. The other five games: probably not at full speed on RP2040.**

This is a hard-constraints conclusion, not pessimism, and it comes from three facts that stack:

1. **Sysclk is locked to 252 MHz.** In `pico_lib/dvi`, `getTiming640x480p60Hz()` sets
   `bitClockKHz = clock_get_hz(clk_sys)/1000`, and TMDS needs 10× the pixel clock. 640×480p60
   ⇒ 25.2 MHz pixel ⇒ **252 MHz system clock, non-negotiable**. There is no overclocking
   headroom to spend on emulation — turning the clock up changes the video timing.
   (You can push ~264 MHz and let the monitor tolerate a 26.4 MHz pixel clock, but that's ~5%.)

2. **Core 1 is fully consumed by TMDS encoding.** On RP2040 there is no HSTX; `core1_main()`
   in `pico_shared/FrensHelpers.cpp:1030` spins in `convertScanBuffer12bpp()` forever. Core 1
   is not available for emulation.

3. **So core 0 must do everything ESP32 currently splits across two cores** — Z80 emulation
   *and* tile/sprite rendering *and* audio generation — on a Cortex-M0+, which is roughly
   0.7× the ESP32 LX6's per-clock throughput on interpreter-style code (table dispatch, byte
   loads, dense branching — M0+'s weakest profile).

Net: core 0 has about **0.7× of one ESP32 core** to do the work that today needs **two** ESP32
cores. Call it a ~2.5–3× shortfall. And note the ESP32 build is *already* running the heavy
games at 30 Hz video / 60 Hz emulation (`VIDEO_HALF_RATE` in `galagino.ino:714`) — there is no
slack left to give back.

Pac-Man (one Z80, no wavetable-heavy sound CPU) has enough margin to survive this. Galaga and
Digdug (three Z80s each), Frogger (two Z80s + AY), 1942 (two Z80s + bank switching) and Donkey
Kong (Z80 + i8048) do not, by a wide margin.

### The recommendation

**Build this for the Waveshare RP2350-PiZero instead.** It is the same form factor and the same
HDMI connector, `pico_shared/BoardConfigs.cmake:267` already has a `dviConfig_WaveShareRp2350`
entry with pins, and it changes all three constraints at once:

- **HSTX drives DVI in hardware** — core 1 is free for emulation, restoring galagino's original
  two-core split almost exactly.
- **Cortex-M33**, dual-issue, ~2× the M0+'s IPC, and the clock is decoupled from video timing
  so 300+ MHz overclocking is on the table.
- **520 KB SRAM** instead of 264 KB — the framebuffer-vs-ROM-caching tension in §4 disappears.
- USB host works alongside video (see §6), so real gamepads become possible.

You already have `plans/rp2350-port.md`. This plan and that one converge if you take this route:
keep that plan's phases, swap its SPI/ILI9341 video layer for the HSTX DVI path documented here.
That is the version I'd actually build.

The rest of this document is the RP2040 plan as asked, written so that everything except the
video/audio backend is shared with an RP2350 build.

---

## 1. What the DVI library gives you

From `~/Source/pico-infonesPlus`, you need `pico_lib/dvi` and `pico_lib/util` (both are
`INTERFACE` CMake libraries, C++, pulling in `hardware_dma/pio/interp/gpio/pwm`).

Board pin config (`pico_shared/dvi_configs.h`):

```cpp
constexpr dvi::Config dviConfig_WaveShareRp2040 = {
    .pinTMDS = {26, 24, 22},
    .pinClock = 28,
    .invert = false,
};
```

The API surface you actually use:

| Call | Purpose |
|---|---|
| `dvi::DVI(pio0, &cfg, dvi::getTiming640x480p60Hz())` | construct |
| `registerIRQThisCore()` / `start()` | called **on core 1** |
| `convertScanBuffer12bpp(line, buffer, 640)` | core 1 pulls one scanline, TMDS-encodes it |
| `getAudioRingBuffer()` | HDMI audio, `int16_t[2]` stereo samples |
| `setAudioFreq(48000, 0, 6144)` | CTS=0 auto-computes for the actual pixel clock |
| `getBlankSettings()` | top/bottom letterbox margins |

Two drive models exist in `FrensHelpers.cpp` — the scanline-lockstep one (`core1_main`) and the
framebuffer one (`coreFB_main`, reads `framebuffer[line * SCREENWIDTH]`). **Use the framebuffer
model.** Galagino renders 8 scanlines at a time in `render_line()` and does a full
`*_prepare_frame()` sprite pass up front; forcing that into per-scanline lockstep with a 31.5 kHz
DVI IRQ would be a rewrite of the render path for no benefit.

---

## 2. Geometry: 224×288 portrait into 640×480 landscape

Galagino's `render_line(c)` already produces scanlines `8c … 8c+7` of the **upright portrait
image** — `video.cpp:174` sets a 224×288 address window and each `tft.write()` is 224×8 pixels
in reading order. So source rows map straight onto destination scanlines. No rotation needed.

Scale factor options:

| Scale | Output | Aspect | Notes |
|---|---|---|---|
| 1× | 224×288 | correct | Tiny island in the middle of the screen. Fine for phase 1 bring-up. |
| **1.5×** | **336×432** | **correct** | Recommended. Nearly fills the 480 lines, uniform on both axes so pixels stay square. |
| 2× | 448×576 | correct | Doesn't fit — 576 > 480. |

**Go with 1.5×.** Vertical: emit 3 output scanlines per 2 source rows (pattern `A A B B` →
`A A B` is wrong; use `A, A/B blend or A, B, B` — simplest is nearest: source row
`(y*2)/3`). Horizontal: same 2:3 nearest map, precomputed once into a 336-entry `uint16_t`
index LUT so the inner loop is a plain gather. Centre with `x0 = (640-336)/2 = 152`,
`y0 = (480-432)/2 = 24`, and set `getBlankSettings().top/bottom = 24`.

The scaling happens **on core 1**, in the `coreFB_main`-equivalent loop, reading the 224×288
RGB565 framebuffer core 0 wrote. Core 0 pays nothing for it.

---

## 3. Core split and timing

```
Core 0                                  Core 1
------                                  ------
prepare_emulation()                     dvi.registerIRQThisCore()
loop {                                  dvi.start()
  emulate_frame()        ← Z80s         loop {
  *_prepare_frame()      ← sprites        for line in 0..479:
  for row in 0..35:                         scale+convert from framebuffer
    render_row(row) → framebuffer           dvi.convertScanBuffer12bpp(...)
    snd_transmit()       ← audio        }
}
```

A significant simplification falls out: on ESP32, emulation and video are separate FreeRTOS
tasks synchronised by `xTaskNotifyGive(emulationtask)` / `ulTaskNotifyTake`. Here they are both
just sequential code on core 0. **All the task-notification vblank plumbing disappears** —
`emulate_frame()` then render, in a loop. Frame pacing comes from a `time_us_32()` deadline at
the bottom of the loop instead of `vTaskDelay`.

The framebuffer needs no double-buffering *if* you accept tearing, which at these frame rates is
barely visible on a scaled-up arcade image. Double-buffering costs another 129 KB and you do not
have it (§4). Start single-buffered.

---

## 4. RAM budget — this is tight

RP2040 has **264 KB**.

| Item | Bytes |
|---|---|
| Framebuffer 224×288 RGB565 | 129,024 |
| DVI TMDS buffers (5 × 320 words × 3 lanes × 4 B) | 19,200 |
| DVI line buffers (5 × 640 × 2 B) | 6,400 |
| HDMI audio ring | ~4,000 |
| Z80 RAM (`RAMSIZE`, `emulation.c:550`) | 9,344 |
| Sprite table (128 × `sizeof(sprite_S)`) | ~512 |
| Scaling LUT (336 × 2 B) | 672 |
| Stacks, C++ runtime, misc | ~12,000 |
| **Subtotal** | **~181 KB** |
| **Free** | **~83 KB** |

Looks comfortable — but there is a trap:

**XIP cache pressure.** The RP2040 has a **16 KB** XIP cache. Galagino's ROM/tile/sprite data
lives in `.rodata` in flash (~250–400 KB across all six games), and the Z80 interpreter fetches
opcodes from it on *every instruction*, with essentially random access across a 16–48 KB ROM. A
16 KB cache against a 48 KB working set thrashes, and each miss is a QSPI round-trip that stalls
the M0+ hard. This alone could cost more than the IPC difference does.

**Mitigation:** copy the active game's Z80 program ROM into SRAM at game-select time and point
`RdZ80`/`OpZ80` at the SRAM copy. Pac-Man needs 16 KB; Galaga's three CPUs need ~28 KB.
That fits in the 83 KB free — but it competes with any other use of that space, and it's the
reason not to double-buffer. Tile/sprite/colour-map data can stay in flash: it's read in
sequential bursts during rendering, which the cache handles well.

Mark the hot code paths `__not_in_flash_func()` too — `Z80.c`'s `ExecZ80`, `RdZ80`, `WrZ80`,
and the per-game `*_render_row()` functions. That's a few KB of SRAM for a large win.

---

## 5. Audio

Galagino generates **24 kHz mono** (11,765 Hz when Donkey Kong is running — `audio_dkong_bitrate()`,
`galagino.ino:613`) into `snd_buffer` as `0x8000 + v` unsigned samples for the ESP32's I2S DAC.

Target is **48 kHz stereo `int16_t`** into `dvi_->getAudioRingBuffer()`, carried in HDMI data
islands. The Waveshare RP2040-PiZero has no audio jack, so HDMI audio is the only output.

Changes needed in a new `audio_pico.cpp`:
- Keep `snd_render_buffer()` and `audio_namco_waveregs_parse()` **unmodified** — pure integer
  math, no ESP-IDF dependency.
- Replace `snd_transmit()`'s `i2s_write()` with a ring-buffer push: convert unsigned-offset mono
  → signed, duplicate to both channels, and 2× upsample (24 k → 48 k is a straight sample repeat).
- **Donkey Kong's 11,765 Hz is the awkward case** — 48000/11765 is not an integer. Either
  resample properly (linear interpolation, cheap enough) or retune DKong's audio generation to
  12 kHz and accept a 2% pitch error, which is inaudible. Recommend the latter.
- The `WORKAROUND_I2S_APLL_PROBLEM` block exists purely for an ESP32 I2S PLL bug — **delete it**,
  along with the `dkong_obuf_toggle` half-buffer dance it forces.

Feed the ring from the same six-times-per-frame cadence the render loop already uses.

---

## 6. Input — and a real limitation

The board has a USB-A host port, but **PIO USB does not work on RP2040-PiZero alongside DVI**.
`BoardConfigs.cmake:183` says so explicitly, and the reason is structural: libdvi occupies PIO0
(three TMDS state machines plus clock), and PIO-USB needs a full PIO block of its own — RP2040
has only two, and the config sets `PIO_USB_USE_PIO 2`, which doesn't exist on RP2040. So
**USB gamepads are off the table on this board**. (On RP2350 they are not — another point in
its favour.)

Workable input paths, in order of preference:

1. **Wii Nunchuck over I2C** — galagino already supports this (`Nunchuck.h`, `NUNCHUCK_INPUT`
   in `config.h`, and you have local modifications in the working tree). Port the Arduino `Wire`
   calls to `hardware_i2c`; the protocol code is unchanged. Use `i2c1` on GPIO 2/3 to match the
   existing board config convention.
2. **GPIO buttons** — replace `pinMode(INPUT_PULLUP)`/`digitalRead()` with `gpio_init` +
   `gpio_pull_up` + `gpio_get`. Pick from GPIOs not taken by DVI (22, 24, 26, 28) or the SD card
   (18–21). GPIO 0–17 are largely free.
3. **NES pad via PIO1** — `pico_shared/nespad.cpp` + `nespad.pio` drop in if you want it.

---

## 7. Build system

New tree, side by side with the Arduino build (which keeps working):

```
galagino/
├── galagino/              ← existing ESP32/Arduino sources, untouched
└── pico/
    ├── CMakeLists.txt
    ├── pico_sdk_import.cmake
    ├── src/
    │   ├── main.cpp           ← from galagino.ino: setup/loop/render_line/buttons_get
    │   ├── video_dvi.cpp      ← framebuffer + core1 scale/convert loop (replaces video.cpp)
    │   ├── audio_pico.cpp     ← HDMI audio ring feed (replaces the I2S block)
    │   ├── input_pico.cpp     ← GPIO / nunchuck
    │   ├── nunchuck_pico.cpp  ← Wire → hardware_i2c
    │   └── config_pico.h      ← pin map, replaces config.h's TFT/BTN section
    └── lib/dvi, lib/util      ← git submodule of fhoedemakers/pico_lib
```

Notes:
- `pico_lib/dvi` is **C++** (`std::vector`, `std::array`, namespaces). The top-level target must
  be C++; galagino's `Z80.c`, `i8048.c`, `emulation.c` stay C and get `extern "C"` wrappers.
- `galagino.ino` must become `main.cpp` with explicit `#include`s and forward declarations —
  the Arduino preprocessor's implicit prototype generation is gone.
- Add `pico_lib` as a submodule rather than copying, so upstream fixes flow in.
- Per-game builds via `-DENABLE_PACMAN` etc. — `config.h`'s `SINGLE_MACHINE` logic works as-is
  and matters more here, since a Pac-Man-only build is the realistic deliverable.

---

## 8. Phases

Each phase should end in something observable on a real monitor.

**Phase 1 — Scaffold.** CMake project, pico_lib submodule, boots at 252 MHz, core 1 runs DVI,
core 0 fills the framebuffer with a test pattern. *Verify: colour bars at 640×480 on the HDMI
output, stable, correct colours (`invert = false` for this board).*

**Phase 2 — Geometry.** 224×288 framebuffer, 1.5× nearest-neighbour scaler on core 1, centred
with letterbox margins. *Verify: a 224×288 checkerboard lands at 336×432 centred, square pixels,
no shimmer at the scale boundaries.*

**Phase 3 — Emulator core, headless.** `Z80.c`, `i8048.c`, `emulation.c` compile and link
unmodified except for `millis()`/`micros()` → `to_ms_since_boot()`/`time_us_32()`,
`esp_random()` → `get_rand_32()`, `ESP.restart()` → `watchdog_reboot()`. Run Pac-Man's
`emulate_frame()` in a loop with no video. *Verify: no faults for 60 s; `printf` a checksum of
video RAM each frame and confirm it changes plausibly.*

**Phase 4 — Pac-Man on screen.** Wire `render_line()` into the framebuffer, sequential core-0
loop, `time_us_32()` frame pacing. *Verify: attract mode runs and is recognisable; measure and
print actual achieved frame rate — this number decides whether phases 7–9 are worth starting.*

**Phase 5 — Input.** Buttons and/or nunchuck. *Verify: coin + start, Pac-Man is playable.*

**Phase 6 — Audio.** Ring-buffer feed at 48 kHz stereo. *Verify: Pac-Man's intro melody and
waka-waka at correct pitch through the monitor's speakers; no underrun crackle over 5 minutes.*

**Phase 7 — Optimisation.** This is where the RP2040 build lives or dies. In order of expected
payoff: copy the active Z80 ROM to SRAM; `__not_in_flash_func` the interpreter and render path;
`-O3` on `Z80.c`; consider a computed-goto dispatch in place of the `switch` in `ExecZ80`.
*Verify: re-measure frame rate against the phase 4 baseline after each change individually.*

**Phase 8 — Remaining games, one at a time, gated on phase 7's numbers.** Donkey Kong first
(2 CPUs, lightest of the rest), then Frogger, 1942, then Galaga and Digdug. *Verify each: attract
mode at a measured frame rate. Expect to stop early.*

**Phase 9 — Menu and polish.** `render_logo()`, game selection, master attract timeout.
Skip entirely if only Pac-Man survives — build with `SINGLE_MACHINE`.

**Phase 10 — WS2812 marquee (optional).** PIO1 state machine, `pico_shared/ws2812.pio` drops in.

---

## 9. What ports unchanged vs. what needs work

**Unchanged:** `Z80.c/h`, `Tables.h`, `i8048.c/h`, all `*_rom*.h` / `*_tilemap.h` /
`*_spritemap.h` / `*_cmap*.h` / `*_logo.h` / `*_sample_*.h` data headers, all six per-game
`*.h` render/emulation headers, `emulation.h`'s `MACHINE_IS_*` macros, `dip_switches.h`,
`tileaddr.h`, `romconv/*.py`.

**Light adaptation:** `emulation.c` (timing/RNG/reset calls only), `snd_render_buffer()` and
`audio_namco_waveregs_parse()` (output stage only).

**Rewritten:** `video.cpp` → `video_dvi.cpp`; the I2S block in `galagino.ino` → `audio_pico.cpp`;
`leds.cpp` (FastLED → PIO); `Nunchuck.h` (Wire → `hardware_i2c`); `galagino.ino`'s
setup/loop/task structure → `main.cpp`.

---

## 10. Risk summary

| Risk | Severity | Mitigation |
|---|---|---|
| Core 0 can't sustain the heavy games | **High — expected to materialise** | Ship Pac-Man; move to RP2350-PiZero for the rest |
| 16 KB XIP cache thrashing on Z80 fetches | **High** | Copy active ROM to SRAM; `__not_in_flash_func` hot paths |
| Sysclk locked at 252 MHz, no OC headroom | **High, unfixable on RP2040** | None — it's why RP2350 is the recommendation |
| No USB gamepad support alongside DVI | Medium | Nunchuck (already supported) or GPIO buttons |
| RAM too tight for double-buffering + ROM caching | Medium | Single-buffer, accept tearing |
| DKong 11,765 Hz → 48 kHz resampling | Low | Retune to 12 kHz, 2% pitch error is inaudible |
| C/C++ linkage across the emulator core | Low | `extern "C"` wrappers |

---

## 11. Suggested decision point

Build **phases 1–4 for RP2040** — that's maybe a weekend, and phase 4 ends with a measured
Pac-Man frame rate on real hardware. That number is worth more than any further estimation in
this document.

If Pac-Man lands comfortably above 60 Hz with headroom, continue. If it lands near or below,
order an RP2350-PiZero: phases 1–6 port across almost verbatim (same library, same board
pinout family, same geometry work), and the HSTX path frees core 1 for the emulation that the
other five games need.
