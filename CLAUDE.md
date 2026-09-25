# Galagino — Claude Notes

## Project Overview

ESP32 arcade emulator running six machines on a 320×240 SPI TFT: Pac-Man, Galaga,
Donkey Kong, Frogger, Digdug, 1942. Upstream is an Arduino sketch, not a CMake project —
`galagino/galagino.ino` is the entry point and the Arduino IDE / arduino-cli builds it.

The arcade screen is **224×288 portrait**, drawn into a 240×320 TFT with an 8/16 px offset
(`TFT_X_OFFSET` / `TFT_Y_OFFSET`).

## Architecture

**Core split (FreeRTOS, ESP32):**
- Core 0 — `emulation_task()` → `emulate_frame()`, the Z80 / i8048 CPUs
- Core 1 — `loop()` → `update_screen()`, rendering + SPI + audio
- Synchronised by `xTaskNotifyGive(emulationtask)` once (or twice, at half rate) per screen update

**Render pipeline** — there is no full framebuffer. `frame_buffer` is a single 224×8 tile row
(`galagino.ino:804`). Each frame: `*_prepare_frame()` walks the sprite table, then
`render_line(0..35)` fills that one row buffer and `tft.write()` DMAs it out, 36 times.

**Frame rate.** At the default 40 MHz SPI clock, `VIDEO_HALF_RATE` kicks in
(`galagino.ino:714`): video runs at 30 Hz, emulation still at 60 Hz. Only displays that
tolerate an 80 MHz SPI clock get full 60 Hz video. **The ESP32 build has no performance
slack** — this matters for any port.

**Audio.** 24 kHz mono via the ESP32's built-in I2S DAC; Donkey Kong switches to 11,765 Hz
(`audio_dkong_bitrate()`). `snd_transmit()` is called 6× per screen update.
`WORKAROUND_I2S_APLL_PROBLEM` exists solely for an ESP32 I2S PLL bug — it is not
functionality, and any port should drop it along with the `dkong_obuf_toggle` half-buffer
dance it forces.

**Per-game code** lives in headers, not .c files: `pacman.h`, `galaga.h`, `dkong.h`,
`frogger.h`, `digdug.h`, `1942.h`, each defining `*_prepare_frame()` / `*_render_row()`.
Dispatch goes through the `MACHINE_IS_*` / `*_BEGIN` / `*_END` macros in `emulation.h`.

**Z80 core** is Marat Fayzullin's emulator (`Z80.c`, `Tables.h`, `Codes*.h`) — untouched
upstream code, portable C. Same for `i8048.c` (Donkey Kong's audio sub-CPU).

## ROM data

ROMs are **not** in the repo as binaries you can load at runtime — the python scripts in
`romconv/` convert MAME ROM sets into C arrays in `*_rom*.h`, `*_tilemap.h`, `*_spritemap.h`,
`*_cmap*.h`. Run `romconv/conv.sh` after dropping ROM files into `roms/`. Regenerate rather
than hand-editing any `*_rom*.h`.

`roms/` currently holds an untracked Pac-Man set (`pm1_*`).

## Local hardware setup

The working tree carries **uncommitted local config** — do not "clean these up":

- `config.h` — `CHEAP_YELLOW_DISPLAY_CONF` enabled (CYD board: ILI9341, SPI on 12/13/14,
  CS 15, DC 2, backlight 21), `TFT_VFLIP` on, audio on GPIO 26 (`SND_LEFT_CHANNEL`)
- `Nunchuck.h` — swapped from `Nunchuk` to **`NESMiniController`** over I2C (SDA 22, SCL 27),
  d-pad + A/B replacing the analog stick + Z/C. Analog-stick code is commented out, kept
  in place rather than deleted.

Input is nunchuck-style I2C only in this config; `BTN_START_PIN 0` is the boot button and
there is no separate coin button (start doubles as coin).

## Ports under consideration (`plans/`)

Two plans exist. **`galagino_picocalc/`** holds the start of the PicoCalc port: a Pac-Man-only
pico-sdk build that compiles the unchanged `../galagino` emulation sources. See its README for
build, flash and keys. **Pac-Man works on the PicoCalc hardware (2026-09-25):** 60.6 Hz, video 14.06 ms/frame, emulation
1.7 ms/frame. The **six-game build** (menu, all sound paths, auto 30 Hz fallback, per-row timing)
runs on hardware. **Galaga, Donkey Kong and Frogger verified at 60 Hz** (emulation ~7 / ~5 / ~3 ms, row render max ~180 / ~250 / ~315 us; the DMA takes ~382 us per row). Digdug and 1942 are still untested. `main.c` is a C port of `galagino.ino`. Fixes over upstream:
the Namco wavetable pick is a real if/else chain, and the DK audio read pointer only advances
when a buffer is queued.

- `plans/rp2350-port.md` — **ClockworkPi PicoCalc** (Pico 2 / RP2350, built-in 320×320 SPI panel).
  This is the preferred target.
- `plans/rp2040-pizero-dvi-port.md` — Waveshare RP2040-PiZero with DVI/HDMI, reusing
  `pico_lib` from `~/Source/pico-infonesPlus`

**Findings worth not re-deriving:**

- On **RP2040 + libdvi, sysclk is pinned at 252 MHz** — `pico_lib/dvi/timing.cpp` derives the
  TMDS bit clock from `clk_sys` and 640×480p60 needs exactly 10× the 25.2 MHz pixel clock.
  No overclock headroom is available for emulation.
- **Core 1 is fully consumed by TMDS encoding** on RP2040 (no HSTX), so core 0 must do both
  emulation and rendering — the work the ESP32 splits across two cores, on a slower-IPC M0+.
  Conclusion: **Pac-Man is plausible, the other five games are not.** This is DVI-specific:
  with an SPI TFT both RP2040 cores are free (see "Why not other boards" in `plans/rp2350-port.md`).
- **The RP2350's M33 has no MVE/Helium** (DSP extension only) and **no I-cache** (16 KB XIP cache).
  The SPI panel clock, not the MCU, caps video at 30 Hz on a 40 MHz panel.
- **PicoCalc display:** drive it the shapones way (`~/Source/shapones/samples/v3/picocalc.cpp`):
  PIO0 SPI at 75 MHz (300 MHz sysclk / 4), `COLMOD 0x65` RGB565. A 224×288 frame takes ~13.8 ms,
  so 60 Hz fits. Not tiny_agi's `lcdspi` path, which uses 18-bit colour at 50 MHz (~31 ms/frame).
- **Forks worth porting instead of upstream** (details in `plans/rp2350-port.md` §6):
  `VirtualClaudioBoy/GalaginoPlus` (48 games; 6502/6809/6803 cores; `machineBase` class per game;
  platform layer split into `emulation/{video,audio,input,nunchuck}.cpp`) and `speckhoiler/galagino`
  (30 games). `Beaumotplage/galapico` already runs upstream on a Pico 2 at the **stock 150 MHz**,
  with only Digdug halved to keep up.
- **RP2350 flash divider:** `PICO_FLASH_SPI_CLKDIV` does nothing on RP2350 unless
  `PICO_EMBED_XIP_SETUP=1` is also set. Without it `.boot2` is empty and flash keeps the bootrom's
  timing. Both must be set before `pico_sdk_init()`. Verify with the `.boot2` section size
  (256 bytes) and the M0_TIMING literal (`0x40000204` = divider 4, RX delay 2).
- **PicoCalc audio** is GP26 = left, GP27 = right, both on PWM slice 5 (ClockworkPi's MicroPython
  `boot.py` and PicoMite). The panel is an ST7365P (spec PDF in `~/Source/PicoCalc`).
- **Force-include trick:** `galagino_picocalc/picocalc_config.h` defines `_CONFIG_H_`, so the CYD
  `galagino/config.h` becomes a no-op even though upstream includes it with quotes.
- RP2040's **16 KB XIP cache** thrashes against random Z80 opcode fetches from flash;
  copying the active ROM into SRAM is the main mitigation.
- **USB gamepads do not work alongside DVI on RP2040-PiZero** — libdvi owns PIO0 and PIO-USB
  needs a whole PIO block (`pico_shared/BoardConfigs.cmake:183`).
- RP2350-PiZero **cannot use HSTX**: its TMDS pins are GPIO 32–38 (`pico_shared/dvi_configs.h:32`),
  outside HSTX's GPIO 12–19. It runs the same PIO libdvi path as the RP2040 (252 MHz pinned,
  core 1 busy with TMDS). Its gains are only the M33 core 0, 520 KB SRAM and a third PIO
  (so PIO-USB works alongside DVI).
- **RP2350-PiZero on a TV is SETTLED as a hardware problem, not a software one.** The board
  doesn't route 5 V to HDMI pin 18 (source-present), and many TVs stay dark without it. It also
  doesn't power USB VBUS, so there's no USB keyboard or gamepad without injecting 5 V. Signalling
  mode was ruled out: msx2pico runs full HDMI mode with InfoFrames on this board and the TV still
  stays dark. Don't re-investigate DVI/HDMI toggles. Fix: VSYS → 100 Ω → HDMI pin 18 (hard to
  solder on mini-HDMI). Source: `~/Source/tiny_agi/tinyagi-rp2350` notes and `~/Source/msx2pico/CLAUDE.md`.
- **The RP2040-PiZero works on regular TVs.** It routes 5 V to pin 18; tiny-agi's
  `tinyagi_dvi_rp2040` target runs on RP2040 hardware. That answers the open question the
  shelving was waiting on.

**Status: shelved.** For DVI on a TV, the RP2040-PiZero is the only board that works without a
hardware mod, which limits a DVI port to roughly Pac-Man. For all six games, the PicoCalc plan
(`plans/rp2350-port.md`) is the way forward.

## Conventions

- Upstream style: 2-space indent, `snake_case`, per-game logic in headers
- `Z80.c` / `Codes*.h` / `Tables.h` / `i8048.c` are vendored — leave them alone
- Machines are compiled in/out via `ENABLE_*` in `config.h`; disabling all but one sets
  `SINGLE_MACHINE`, which drops the boot menu
