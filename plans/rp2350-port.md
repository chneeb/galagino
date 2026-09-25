# PicoCalc (RP2350) Port Plan — Galagino

**Target:** ClockworkPi PicoCalc with its Pico 2 module (RP2350, 2× Cortex-M33, 520 KB SRAM, 4 MB flash)
**Display:** built-in 320×320 SPI IPS panel (ST7796-family command set), driven over PIO
**Status:** Planned, not started. Retargeted from a generic Pico 2 + ILI9341 to the PicoCalc on
2026-09-25. Display facts come from `~/Source/shapones/samples/v3/`, which runs NES at up to
60 fps on the same PicoCalc.

---

## Overview

Galagino is mostly portable C. Only about 19 lines use ESP32-specific APIs, spread across
`galagino.ino`, `video.cpp` and `emulation.c`. The Z80 and i8048 cores, all six per-game headers
and every ROM/graphics array carry over unchanged. The port replaces the two-core task split, the
display driver, audio output and input.

The PicoCalc fits well. There's no DVI, so both cores stay free for the ESP32-style split. The
320×320 panel takes the 224×288 portrait arcade image at 1:1. Keyboard, speaker and battery are
built in. The owner already has two working PicoCalc code bases to borrow from.

### Hardware facts (verified in shapones / tiny_agi source)

| Item | Detail | Source |
|---|---|---|
| LCD pins | SCK 10, MOSI 11, MISO 12, CS 13, DC 14, RST 15. No backlight or TE pin in the map | `shapones/samples/v3/picocalc.hpp:15` |
| LCD driver | **PIO0 SPI** (2-instruction program with side-set clock) + one DMA channel, 8-bit transfers | `picocalc.cpp`, `picocalc.pio` |
| LCD clock | PIO clkdiv = `sys/2/speed`; at 300 MHz sysclk, `SYS_CLK_FREQ/4` → an exact **75 MHz** | `boot_menu.cpp:242`, ROADMAP §1 |
| Pixel format | **`COLMOD 0x65` = RGB565 works** over this PIO SPI | `picocalc.cpp:85` |
| Panel init | ST7796-style (`F0 C3`/`F0 96` unlock), `MADCTL 0x48` (MX, BGR), `INVON` (0x21) required, `B1 A0` frame rate | `picocalc.cpp:69–156` |
| SD card | hardware `spi0` on GP16–19, separate from the LCD (no shared-bus issues) | `shapones/CLAUDE.md` |
| Keyboard | STM32 over I2C1 (SDA 6, SCL 7, addr 0x1F, 400 kHz); press/hold/release events; arrows 0xB4–0xB7 | `i2ckbd.hpp`, `common.cpp:71` |
| External controller | NES Mini / Nunchuck on I2C0 **GP4/GP5**. It conflicts only with PSRAM, which Galagino doesn't need | `shapones/samples/v3/common.hpp:54` |
| Audio | PWM + DMA on **GP26** (speaker); shapones uses 8-bit at 22,050 Hz | `picocalc_nes.cpp:33` |
| Clock | **300 MHz at 1.30 V**, stable on this board (360 MHz at 1.30 V "worked but not soaked") | `picocalc_nes.cpp:115`, ROADMAP §1 |
| Flash | At 300 MHz with the default divider, flash runs at **150 MHz**: out of spec, though it works. Set `PICO_FLASH_SPI_CLKDIV=4` → 75 MHz | shapones ROADMAP §2 |

**Don't use tiny_agi's PicoCalc display path.** It uses ClockworkPi's `lcdspi` on hardware SPI1 at
50 MHz in 18-bit colour (`COLMOD 0x66`, 3 bytes per pixel). That's roughly 2× slower per frame.
Shapones proved RGB565 at 75 MHz on the same panel.

### Video budget: full 60 Hz looks reachable

One arcade frame is 224×288×2 = **129,024 bytes**.

| Link | Transfer time | Result |
|---|---|---|
| CYD today: 40 MHz, RGB565 | ~25.8 ms | 30 Hz (`VIDEO_HALF_RATE`) |
| PicoCalc via lcdspi: 50 MHz, RGB666 | ~31 ms | 30 Hz, just barely |
| **PicoCalc via PIO: 75 MHz, RGB565** | **~13.8 ms** | **60 Hz fits (16.67 ms), ~2.9 ms margin** |

The margin only holds if **the bus never idles**. Shapones' notes from the InfoNES port
(`NOTES-from-infones.md` §4) found that per-row DMA restarts cost milliseconds per frame. Galagino
is already in a good position: it opens one address window for the whole frame
(`video.cpp:174`), then streams 36 tile rows of 224×8 px (3,584 B, ~382 µs each at 75 MHz). Each
row must be rendered in less than that time, and the next DMA must be armed immediately. Use
**two row buffers (ping-pong)** instead of the ESP32's `memcpy` into `dma_buffer`, or chain DMA
through control blocks.

With no TE pin there's no vsync to the panel, so expect the same mild tearing as on the CYD.

Placement: centre 224×288 in 320×320 → `TFT_X_OFFSET 48`, `TFT_Y_OFFSET 16`. Clear the border
once at startup.

Byte order: the ESP32 streams the RGB565 buffer bytes in memory order, and so does shapones' 8-bit
DMA. Galagino's colour tables should therefore work unchanged. **Verify on the first frame**; if
the colours are wrong, swap once in the tables, never per pixel.

### Emulation feasibility (estimates, not measured)

| Machine | CPUs | Expectation at 300 MHz |
|---|---|---|
| Pac-Man | 1× Z80 | Yes |
| Donkey Kong | Z80 + i8048 | Very likely |
| Frogger | 2× Z80 | Very likely |
| 1942 | 2× Z80, banked ROM (88 KB) | Likely |
| Galaga | 3× Z80 | Unknown; benchmark decides |
| Digdug | 3× Z80 | Unknown; benchmark decides |

The M33 at 300 MHz should out-run the 240 MHz LX6 per core, but nobody has measured the Z80 core
on it. **Phase 3 settles this.**

---

## 1. Toolchain

**Recommendation for the PicoCalc: the pico-sdk (CMake).** This reverses the earlier "arduino-pico
first" advice, for three reasons:
- Every piece of proven PicoCalc code (the shapones LCD driver, keyboard ISR, PWM audio; tiny_agi)
  is pico-sdk C/C++. It drops in unchanged.
- The clock setup this plan depends on — vreg 1.30 V before 300 MHz, `PICO_FLASH_SPI_CLKDIV=4`, a
  PIO clkdiv that is an exact integer — is trivial and explicit in the SDK. In arduino-pico it goes
  through board-menu settings and its own boot code, which we'd have to verify first.
- The owner already builds PicoCalc firmware this way.

Layout: a new `galagino_picocalc/` directory with its own `CMakeLists.txt` that compiles the
**unchanged shared files from `../galagino/`** (Z80, i8048, per-game headers, ROM arrays,
`emulation.c`). Platform code is new: `main.cpp`, `lcd.cpp` (from shapones), `input.cpp`,
`audio.cpp`. Game logic stays in a single source of truth, and the CYD build is untouched.

The cost: the platform-neutral parts of `galagino.ino` (`render_line()`, `update_screen()`, menu,
logo rendering, `snd_render_buffer()` glue) have to be moved or duplicated into the new
`main.cpp`. That's a few hundred lines. If duplicating grates, pull them into a shared
`galagino/core.cpp` later. That would change upstream layout, so it's a separate decision.

---

## 2. Architecture

- **Core 1:** `emulate_frame()`, running the Z80 / i8048 CPUs
- **Core 0:** `update_screen()` → `*_prepare_frame()`, `render_line()` × 36 into ping-pong row
  buffers, PIO DMA to the LCD, keyboard/controller polling
- **Audio:** PWM DMA double-buffer with a completion IRQ that refills from the Galagino sound
  renderer (the shapones `PwmAudio` pattern)
- **Sync:** core 0 signals core 1 once per emulated frame via the SIO FIFO or a semaphore, where
  `xTaskNotifyGive(emulationtask)` sits today. At 60 Hz video, that's once per screen update.
- **Frame pacing:** re-base the deadline from `get_absolute_time()` each frame and never carry
  lateness forward (shapones' lesson, `NOTES-from-infones.md` §5)

### Memory

| Item | Size |
|---|---|
| Active game's CPU ROMs copied to SRAM | Pac-Man 16 KB … 1942 **88 KB** |
| Z80 RAM / video / sprite RAM | < 16 KB |
| Two row buffers (2 × 224×8×2) | 7 KB |
| Audio buffers | ~2 KB |

No full framebuffer is needed. **Copying the active game's CPU ROMs into SRAM at game start**
matters, because random Z80 opcode fetches thrash the 16 KB XIP cache. That's especially true
with flash throttled to 75 MHz. Tile/sprite maps stay in flash.

---

## 3. Phases

### Phase 1: Scaffold + display
- CMake project, `PICO_BOARD=pico2`, `vreg_set_voltage(VREG_VOLTAGE_1_30)` → 300 MHz,
  `PICO_FLASH_SPI_CLKDIV=4`
- Port shapones' `picocalc.cpp`/`.pio` LCD driver. Add `start_write_data()` for the fixed 224×288
  window, plus a streaming "write next rows" call that keeps CS low across all 36 row DMAs.
- Test: a colour test pattern at the centred position; confirm RGB565 byte order.

### Phase 2: Input
- Keyboard: port shapones' `i2ckbd` + `kbd_interrupt` (hardware alarm, press/hold/release).
  Map arrows → joystick, and pick keys for fire / start / coin.
- **Test early: hold a direction and fire together.** Arcade games depend on it.
- Optional: the NES Mini controller on I2C0 GP4/GP5, reusing `Nunchuck.h`'s NES Mini logic.

### Phase 3: Emulation on core 1 + benchmark (go/no-go gate)
- Emulation loop on core 1; FIFO sync; ROM-to-SRAM copy; replace `esp_random` → `get_rand_32()`,
  `ESP.restart` → `watchdog_reboot(0,0,0)`, `micros` → `time_us_32()`
- Pac-Man only (`SINGLE_MACHINE`) first: attract mode visible
- **Instrument `emulate_frame()` and the full `update_screen()` with `time_us_32()`** for every
  machine at 300 MHz (and at 150 / 200 MHz, for comparison). Record the results below.
- Decide per machine: 60 Hz video, 30 Hz video (keep `VIDEO_HALF_RATE`), or drop it.

### Phase 4: Audio
- PWM + DMA on GP26 (optionally GP27 for the second speaker), fed by `snd_render_buffer()`
- 24 kHz; DK at 11,765 Hz (`audio_dkong_bitrate()`), which is just a different PWM wrap/divider
- **Drop `WORKAROUND_I2S_APLL_PROBLEM` and `dkong_obuf_toggle`.** They only work around an ESP32 bug.

### Phase 5: Remaining machines
- In the order suggested by the Phase 3 numbers: DK, Frogger, 1942, Galaga, Digdug
- Per machine: attract mode, sound and input work, and the frame fits its budget

### Phase 6: Optimisation (only if needed)
1. Place the Z80 hot paths (`ExecZ80` dispatch, `RdZ80`/`WrZ80` hooks) in SRAM
   (`__not_in_flash_func`)
2. 360 MHz at 1.30 V, following shapones ROADMAP §1 step 2. **Pin the LCD PIO at 75 MHz
   explicitly** so it doesn't ride up to 90 MHz with sysclk. Raise flash timing first, then
   voltage, then clock.
3. DMA control-block chaining so the LCD bus has zero CPU gaps between rows
4. The SIO interpolator for tile address math in `render_line()`

### Phase 7: Polish
- Menu key to return to game select; volume keys
- Battery / backlight via the keyboard MCU, as shapones does (`I2C_Send_RegData(... 0x05 ...)`)

---

## 4. Files

**Unchanged, compiled from `../galagino/`:** `Z80.c/h`, `Codes*.h`, `Tables.h`, `i8048.c/h`, all
`*_rom*.h`, `*_tilemap.h`, `*_spritemap.h`, `*_cmap*.h`, `*_logo.h`, sample/wavetable headers,
`pacman.h`, `galaga.h`, `dkong.h`, `frogger.h`, `digdug.h`, `1942.h`, `emulation.h`, `romconv/`.

**Adapted:** `emulation.c` (ESP calls, ROM-to-SRAM copy), and `config.h` (a `PICOCALC_CONF` block
with offsets 48/16, or a separate config header for the new target).

**New in `galagino_picocalc/`:**
| File | Origin |
|---|---|
| `CMakeLists.txt` | new; pattern from `shapones/samples/v3/CMakeLists.txt` |
| `lcd.cpp/.hpp`, `lcd.pio` | shapones `picocalc.cpp/.hpp/.pio` + streaming row write |
| `input.cpp` | shapones `i2ckbd.cpp` + `kbd_interrupt`; optional NES Mini on I2C0 |
| `audio.cpp` | shapones `pwm_audio.hpp` pattern |
| `main.cpp` | core split, frame loop, menu; glue from `galagino.ino` |

---

## 5. Risks

- **High: Galaga/Digdug emulation throughput.** Three Z80s; only Phase 3 answers it. Fallback:
  30 Hz video or dropping them.
- **Medium: the render must keep up with the bus.** 60 Hz needs each tile row rendered in less
  than ~382 µs, with no idle gaps. If it can't keep up, keep `VIDEO_HALF_RATE` (30 Hz, same as the CYD).
- **Medium: keyboard feel.** I2C polling latency and simultaneous-key behaviour are untested for
  arcade play. The NES Mini on GP4/GP5 is the fallback.
- **Low: overclock stability.** 300 MHz at 1.30 V is already proven on this board by shapones.
- **Low: memory.** Even 1942's 88 KB ROM copy leaves more than 400 KB free.

---

## 6. Prior art: forks to port instead of upstream (found 2026-09-25)

- **[Beaumotplage/galapico](https://github.com/Beaumotplage/galapico)**: upstream Galagino already
  ported to the **Pico 2** (pico-sdk, 15 kHz CRT output via PIO, PWM audio). It never calls
  `set_sys_clock`, so it runs at the **stock 150 MHz**. Its README says Digdug had to run its Z80s
  at half speed (`INST_PER_FRAME_DIGDUG 150000/60/4`) to avoid overload; it reports no such
  problem for Galaga. That's our first real data point: at 300 MHz, all six look within reach. Last
  pushed 2025-01; self-described rough (sound issues, a Frogger scroll bug). Use it as a reference
  for the core split and audio, not as a base.
- **[speckhoiler/galagino](https://github.com/speckhoiler/galagino)**: PlatformIO, **30 games**,
  GPL-3.0, actively maintained, CYD-targeted.
- **[VirtualClaudioBoy/GalaginoPlus](https://github.com/VirtualClaudioBoy/GalaginoPlus)**: built
  on speckhoiler, **48 games**, high-score saving, GPL-3.0, active. Adds **6502, 6809 and 6803** CPU
  cores next to Z80/i8048. Each game is a `machineBase` subclass in `machines/<game>/` rather than
  a set of `*_BEGIN` macros. The platform layer is split out into `emulation/video.cpp`,
  `audio.cpp`, `input.cpp` and `nunchuck.cpp`, which are exactly the files a PicoCalc port replaces.
  About 30 games fit in the ESP32's flash at once; the Pico 2's 4 MB flash gives a similar limit.

**Recommendation:** base the PicoCalc port on GalaginoPlus (or speckhoiler, if you want fewer
moving parts), not upstream. Its structure is more portable, and each added game comes for free.
Remaining porting work: the Arduino/FastLED includes in `machineBase.h`, plus the four platform
files. The heavier Namco multi-CPU titles (Xevious, Gaplus, Mappy, Tower of Druaga) need the
Phase 3 benchmark like Galaga/Digdug.

---

## 7. Why not other boards

- **RP2350-PiZero (DVI):** no 5 V on HDMI pin 18, so the owner's TV stays dark. That's settled as
  a hardware issue; see `CLAUDE.md`. No HSTX either (TMDS on GPIO 32–38).
- **RP2040-PiZero (DVI):** works on TVs, but core 1 goes to TMDS encoding, sysclk is pinned at
  252 MHz and core 0 is an M0+. Realistically Pac-Man only.
- **RP2040 + SPI TFT:** both cores free, but the M0+ at ~250–270 MHz is clearly below the M33.
  Pac-Man yes, DK/Frogger possible, Galaga/Digdug unlikely.

---

## Benchmark results (fill in during Phase 3)

Measured on hardware at 300 MHz, 75 MHz LCD. `update_screen()` includes the keyboard poll (two
I2C transfers) but not the pacing sleep. Pac-Man (2026-09-25): video is **bus-bound**. 14.06 ms
against a theoretical 13.76 ms transfer means row rendering hides completely behind the DMA.
Emulation of one Z80 takes ~1.7 ms, ~10% of the frame. The ESP32 comment in `emulation.c` puts
Galaga's three Z80s at ~13 ms on ESP32.

Galaga (2026-09-25): three Z80s in **~7 ms** (max 7.7 ms), i.e. ~45% of the frame. That's ~1.8x the
ESP32. Row render max ~180 us, half the ~382 us DMA time per row, so video stays bus-bound at 60 Hz.
The three-CPU question from §5 is answered for Galaga; Digdug is similar but polls the Namco I/O chip more.

Digdug (2026-09-25): ~8 ms emulation (max 8.6 ms), full speed. galapico had to halve it at
150 MHz. First game whose **row render (up to ~520 us) exceeds the ~382 us row DMA**, so drawing no
longer fully hides behind the transfer. The frame still takes only 14.2 ms. The menu is worse (row max
~790 us, frame max 15.2 ms) because `render_logo()` re-decodes the RLE logo from its start for every
row. Both are within budget; optimise only if a future game needs the room.

| Machine | `emulate_frame()` µs @150 | @300 | `update_screen()` µs @300 | Video rate |
|---|---|---|---|---|
| Pac-Man | — | 1,726 (max 1,964) | 14,061 (max 14,187) | **60.6 Hz** (122 frames / 2 s) |
| Donkey Kong | — | ~4,600–5,300 (max 6,690) | ~14,000 (max 14,143); row max 249 | **60 Hz** |
| Frogger | — | ~3,100 (max 3,399) | ~13,990 (max 14,150); row max 315 | **60 Hz** |
| 1942 | | | | |
| Galaga | — | ~6,400–7,100 (max 7,662) | ~14,090 (max 14,184); row max 184 | **60 Hz** |
| Digdug | — | ~7,100–8,200 (max 8,608) | ~14,200 (max 14,314); row max 517 | **60 Hz** |

## Checklist

- [x] Plan authored; retargeted to the PicoCalc 2026-09-25
- [x] Phase 1: Scaffold + display (PIO LCD, 300 MHz, flash clkdiv 4): Pac-Man verified on hardware 2026-09-25
- [x] Phase 2: Input (keyboard): coin/start/arrows work; simultaneous keys not yet tested (Pac-Man has no fire)
- [ ] Phase 3: Emulation on core 1 + **benchmark table**
- [x] Phase 4: Audio (Namco WSG on GP26/27; other sound chips come with their games)
- [ ] Phase 5: Remaining machines
- [ ] Phase 6: Optimisation (if needed)
- [ ] Phase 7: Polish
