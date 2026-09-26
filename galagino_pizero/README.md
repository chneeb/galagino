# Galagino for the Waveshare RP2350-PiZero (DVI/HDMI)

All six Galagino machines on the RP2350-PiZero's mini-HDMI port. It builds the unchanged
emulation code from `../galagino` and the board-independent menu, rendering and sound from
`../galagino_pico_common` (shared with `../galagino_picocalc`), plus:

| File | Purpose |
|---|---|
| `main.cpp` | Core 1: DVI scanout. Core 0: emulation, drawing, pacing, HDMI audio, stats |
| `pad.c` | NES Classic Mini controller over I2C1 (SDA GP2, SCL GP3), 3.3 V |
| `pizero_config.h` | Force-included build config. Defines `_CONFIG_H_` so the CYD `config.h` is skipped |
| `pico_lib/` | fhoedemakers/pico_lib DVI driver (MIT), vendored and patched, see below |

## How it works

- **Video:** 640×480p60 at a fixed 252 MHz. The 224×288 arcade screen is shrunk to 187×240
  (1 in 6 rows and columns dropped), and `pico_lib` doubles that to 374×480. This layout was
  chosen with `../pizero_dvi_proto`; 480 unique lines didn't work. Two screen buffers, so no
  tearing.
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

The pad can be plugged in after power-up; it is retried twice a second.

## Hardware notes

- **TV:** the board doesn't put 5 V on HDMI pin 18, and many TVs stay dark without it (VSYS →
  100 Ω → pin 18 fixes it). PC monitors usually work.
- **USB gamepads** would need 5 V on the USB port too. Not supported; the I2C pad needs only 3.3 V.

## Serial output (USB, every 2 s)

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
