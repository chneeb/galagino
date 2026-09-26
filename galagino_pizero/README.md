# Galagino for the Waveshare RP2350-PiZero (DVI/HDMI)

All six Galagino machines on the RP2350-PiZero's mini-HDMI port. It builds the unchanged
emulation code from `../galagino` and the board-independent menu, rendering and sound from
`../galagino_pico_common` (shared with `../galagino_picocalc`), plus:

| File | Purpose |
|---|---|
| `main.cpp` | Core 1: DVI scanout. Core 0: emulation, drawing, pacing, HDMI audio, stats |
| `pad.c` | NES Classic Mini controller over I2C1 (SDA GP2, SCL GP3), 3.3 V |
| `usb_input.c` | USB gamepads/keyboards on the native USB-C port (TinyUSB host) |
| `tusb_config.h` | TinyUSB host configuration |
| `hidparser/` | LUFA-derived HID report descriptor parser, from msx2pico |
| `ram_wrappers.c` | RAM copies of `memset`, `memcpy`, `interp_save/restore`, linked in via `--wrap` so core 1 never runs flash code |
| `font_8x8.h` | 8×8 font for the on-screen diagnostics (pico-infonesPlus, via msx2pico) |
| `pizero_config.h` | Force-included build config. Defines `_CONFIG_H_` so the CYD `config.h` is skipped |
| `pico_lib/` | fhoedemakers/pico_lib DVI driver (MIT), vendored and patched, see below |

## Hardware results (2026-09-26)

- **DVI output works** on a monitor. The monitor stretches 640×480 to 16:9 although the HDMI
  AVI InfoFrame already says 4:3 (`pico_lib` sends picture aspect 4:3, VIC 1). Use the monitor's
  4:3/aspect setting. TVs usually honour the InfoFrame or have a 4:3 mode. A software pre-squeeze
  (224 → 140 columns, blended) would be possible as a build option, but looks soft.
- **USB keyboard works** on the PIO-USB port, even without 5 V (this keyboard runs on 3.3 V).
- **USB gamepad (SNES clone `0079:0011`):** over PIO-USB nothing happened (not diagnosed further;
  that variant has since been removed). On the **native USB-C port** it enumerates and sends
  reports (`01 7f 7f XX YY 0f 00 00`). The generic parser misread byte 0 as the stick (Left held
  constantly), so the pad now has a fixed map: frank-snes' fallback layout, d-pad in bytes 3/4.
  **Confirmed working (2026-09-26)**, and on the native USB-C port it also works **without the OTG
  cable**. On the port labelled "USB PIO" the pad doesn't work.
- **Red flicker in the menu** (fixed, untested): core 1 missed DVI lines while core 0 streamed
  the menu logos through the 16 KB XIP cache, because core 1's per-line path still ran code from
  flash: `memset`, `memcpy`, the SDK's `interp_save/restore`, and a lambda in `pico_lib`'s DVI IRQ
  (lambdas don't inherit `__not_in_flash_func`). All now run from RAM (`ram_wrappers.c`,
  `--wrap`, `DVI::prepareDataPacket`), and the 640×480 timing table is copied to RAM.
  Verified by disassembly: no call from core 1's per-line functions reaches flash.
- **Red flicker gone** after the flash fix (confirmed).
- **Bottom line / thin maze walls missing:** not overscan (the border made no difference). The
  6:5 shrink drops every 6th row and column, which removes 1-pixel lines. The max-combining scaler
  fixed that in principle but caused **lots of red flicker** (core 1 too slow) and was removed
  (see commit `9679606`). Alternating the dropped rows/columns per frame
  (`DVI_ALTERNATE_DROP`) is being tried instead (untested).
- **Switchable 480-line layouts** (DOUBLE/WIDE/ASPECT, direct path) work (2026-09-26); WIDE
  looked best and is the default. This
  replaces the border setting (didn't help) and the parked max-combining scaler (can't work on
  the direct path; in git history before this change).
- **SIO TMDS encoder** enabled: core 1 35% → 24% in the old doubled mode (confirmed). Before: `CORE1 %` 35 with Pac-Man (interpolator
  encoder). If it drops a lot, 480 unique lines become worth another try. Note: the direct 480
  prototype's "blue shadow" was a bug, not a limit: it passed 320 as the line buffer size, where
  pico_lib's encoder expects the full 640 (it encodes the first half, doubled).
- **Pac-Man timing (on screen):** emulation 2.7 ms (max 2.8), draw 3.2 ms, 60 Hz,
  core 1 35%, 0 missed lines. Emulation is a bit above the ~2.0 ms scaled from the PicoCalc;
  `GALAGINO_FAST_FLASH` may close that. The same pad works in `~/Source/circle-libretro`
  on a Pi, which supplies 5 V. Most likely the pad doesn't run on 3.3 V. To confirm, check the
  console for `usb: device … mounted` when plugging it in; no line means power/enumeration.
  The mapping should already fit: circle-libretro's notes give this pad's buttons as
  X/A/B/Y/L/R = 1–6, Select = 9, Start = 10, which matches the `PAD_*` defaults.
- **NES Mini pad over I2C:** not yet tested.

## How it works

- **Video:** 640×480p60 at a fixed 252 MHz, **480 unique lines** (pico_lib line doubling off).
  Three layouts, cycled with **Right in the game menu** (the name shows for 2 s):

  | Layout | On screen | Rows | Columns |
  |---|---|---|---|
  | DOUBLE | 374×480, correct 4:3 shape | 240 of 288, each shown twice | 187 of 224 |
  | **WIDE** (default) | 448×432 | all, 1.5× | all, sharp, 33% too wide on a 4:3 screen |
  | ASPECT | 336×432, correct shape | all, 1.5× | 168 of 224 |

  With `DVI_ALTERNATE_DROP` (default), dropped rows/columns alternate per frame, so every one
  shows at least at 30 Hz. `DVI_MODE` in `pizero_config.h` sets the starting layout.
- **Direct path:** core 0 draws each 8-line tile strip and converts it into encoder-ready RGB555
  rows (horizontal layout, margins, diagnostics) in one of two frame buffers. Core 1 only picks
  the row for each of the 480 output lines (vertical layout) and encodes it with the RP2350's
  SIO TMDS encoder, as proven by `pizero_dvi_proto` (43% core 1 at 480 lines). Converting on
  core 1 through pico_lib's line queue missed most lines at 480.
- **Core 0** emulates, then draws the whole screen into the back buffer, polls the pad and tops
  up audio, then waits for the next DVI frame. Five games should fit at 60 Hz. Digdug is expected
  to drop to 30 Hz video, with emulation still at 60 Hz. This happens automatically after 10
  late frames and is printed on serial.
- **Audio:** HDMI audio at 48 kHz, linearly resampled from the core's 24 kHz (DK: 11,765 Hz).
  TVs reject non-standard rates, see msx2pico.

## Build

```sh
cd galagino_pizero
PICO_SDK_PATH=~/Source/pico-sdk cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build
```

Flash `build/galagino_pizero.uf2` via BOOTSEL. `-DGALAGINO_FAST_FLASH=ON` embeds a 63 MHz QSPI
flash setup, like the PicoCalc build. Try it if emulation times are much higher than the PicoCalc
numbers in `plans/rp2350-port.md`.

## Controls (NES Classic Mini controller)

| Button | Action |
|---|---|
| D-pad | Joystick; up/down picks a game in the menu |
| A or B | Fire; selects a game in the menu |
| Select | Coin |
| Start | Start |
| Select + Start (hold 1 s) | Back to the menu (reboots) |
| Left (in the menu) | Diagnostics on/off |
| Right (in the menu) | Layout: DOUBLE → WIDE → ASPECT |

The pad can be plugged in after power-up; it is retried twice a second.

## USB gamepad or keyboard

Plug it into the **native USB-C port** (not the one labelled "USB PIO"); an OTG cable isn't
needed for the SNES-clone pad. Power the board through its other USB-C port or the 5 V pin.

- **Known pads** are decoded from fixed byte positions measured for frank-snes
  (`~/Source/frank-snes/gamepads`): `0079:0006`, `0079:0011`, `081f:e401` (SNES clones),
  `11ff:3331`, `046d:c219`, `2563:0575`, `feed:2320`, plus `0810:e501` from pico-infonesPlus.
- **Other HID gamepads** are decoded from their report descriptor (hat, stick or d-pad; buttons
  1–6 fire, 7/9 coin, 8/10 start). If one misbehaves, the diagnostics show its USB ID and raw
  reports, which is what a fixed map needs.
- **XInput (Xbox style) pads** are not supported.
- **USB keyboards:** arrows move, Space/Ctrl/Z/X fire, 5 or C coin, 1 or Enter start, Esc (hold)
  menu.

The USB pad, keyboard and I2C pad can be used together.

## Hardware notes

- **TV:** the board doesn't put 5 V on HDMI pin 18, and many TVs stay dark without it (VSYS →
  100 Ω → pin 18 fixes it). PC monitors usually work.
- **USB gamepads** would need 5 V on the USB port too. Not supported; the I2C pad needs only 3.3 V.

## On-screen diagnostics

The black margins beside the game can show live diagnostics, so no serial console is needed.
They're **off by default**; press Left in the game menu to show or hide them. `SHOW_OVERLAY` in
`pizero_config.h` makes them start on. Core 0 draws them into the margins (6–8 characters wide,
depending on the layout).

| Left margin | Right margin (USB) |
|---|---|
| video rate (60/30 Hz) | `DEVICES`: USB devices enumerated (any class). **0 with a pad plugged in = no power or it doesn't enumerate** |
| `EMU MS` / `DRAW MS`: avg and max per frame | `HID ITF`: HID interfaces mounted. Devices > 0 but 0 here = not a HID pad (e.g. XInput) |
| `CORE1 %`: core 1 encode load | `VID PID`: USB ID of the last device |
| `MISSED`: DVI lines core 1 missed (should be 0) | `TYPE` / `DECODER`: keyboard, generic HID parser, SNES clone quirk, or parse error |
| `I2C PAD`: OK or NONE | `REPORTS` / `LEN` / `RAW`: report count, length and the latest bytes in hex |
| `BUTTONS`: combined Galagino button bits | `USB BTN`: what the USB input decoded to |

A photo of the screen while pressing pad buttons is enough to diagnose a pad. The font is the 8×8
one from pico-infonesPlus (via msx2pico), kept in RAM so core 1 never waits on flash.

## Serial output (every 2 s)

On UART0 TX (GP0, 115200); the native USB port is busy as the gamepad host.


```
machine 2: video 60Hz | draw avg … max … us | emu avg … max … us | core1 encode …% | missed lines 0
```

`draw` + `emu` must stay under ~16,700 us for 60 Hz. `missed lines` must be 0; non-zero means
core 1 fell behind (red lines).

## Local changes to pico_lib

Vendored from `~/Source/msx2pico/pico_lib` (fhoedemakers/pico_lib at `4c53bd2`, plus msx2pico's
removed debug printfs). MIT, see `pico_lib/LICENSE`. Patched in `dvi/dvi.h` and `dvi/dvi.cpp`:

- `setLineRepeat(n)`: line doubling (2, the default) or 480 unique lines (1)
- missed-line counter in the DMA IRQ; wait/encode timing in `convertScanBuffer15bpp()`
- `convertScanBuffer15bpp(line, buffer, size)` encoding straight from a caller's buffer
- `frameCounter_` is `volatile`, so core 0 can poll it for pacing
- the data-island lambda in `updateDataPacket()` is now `prepareDataPacket()`, marked
  `__not_in_flash_func` (lambdas don't inherit it, so it ran from flash inside the DVI IRQ)
- `encodeTMDSChannel16bpp()` uses the RP2350's **SIO TMDS encoder**
  (`tmds_encode_sio_loop_poppop_ratio2`, set up as in PicoDVI's `tmds_encode.c`). pico_lib shipped
  the assembly but always used the RP2040 interpolator path. `DVI_TMDS_INTERP` restores that.
