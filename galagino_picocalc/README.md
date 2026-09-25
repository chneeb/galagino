# Galagino for the ClockworkPi PicoCalc

All six Galagino machines (Pac-Man, Galaga, Donkey Kong, Frogger, Digdug, 1942)
on the PicoCalc's Pico 2 (RP2350). It builds the unchanged emulation code from
`../galagino` (`Z80.c`, `i8048.c`, `emulation.c`, the per-game headers and the
generated ROM headers) and adds a PicoCalc platform layer:

| File | Purpose |
|---|---|
| `main.c` | Core split, frame loop, menu, sound for all machines, timing stats (ported from `galagino.ino`) |
| `lcd.c`, `lcd.pio` | 320×320 panel over PIO SPI at 75 MHz, RGB565, DMA (after shapones) |
| `kbd.c` | I2C keyboard (STM32 at 0x1F on GP6/GP7) |
| `audio.c` | PWM audio on GP26/GP27 at 24 kHz (11,765 Hz for DK), DMA ping-pong |
| `picocalc_config.h` | Force-included build config. Defines `_CONFIG_H_` so the CYD `config.h` is skipped |
| `esp32_compat.h` | `micros`/`millis`/FreeRTOS stand-ins for `emulation.c` |

## Build

```sh
cd galagino_picocalc
PICO_SDK_PATH=~/Source/pico-sdk cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build
```

Flash `build/galagino_picocalc.uf2` with BOOTSEL, or with `picotool load -f build/galagino_picocalc.uf2`.

## Keys

| Key | Action |
|---|---|
| Arrow keys | Joystick; up/down picks a game in the menu |
| `5` or `c` | Coin |
| `1` or Enter | Start |
| Space | Fire; also selects a game in the menu |
| Esc (hold 1 s) | Back to the menu (reboots, like upstream) |

After 20 s idle in the menu, a random game starts (upstream's master attract
mode). Pressing any key in the menu stops that.

## Timing output

USB serial (`picocom /dev/ttyACM0` or similar) prints every 2 s:

```
machine 2: video 60Hz avg … max … us, row max … us | draw avg … max … us | emu avg … max … us | budget 16500 us
```

- `machine`: 0 = menu, then games in menu order (1 = Pac-Man … 6 = 1942)
- `video`: time per screen update, excluding the pacing sleep. The transfer alone takes ~13.8 ms.
- `row max`: slowest tile-row render. Above ~380 us it no longer hides behind the DMA.
- `draw`: CPU time to draw a whole frame (sprite prep + 36 rows), without the LCD waits. Here it
  overlaps the transfer; a DVI port would spend it on the emulation core.
- `emu`: CPU emulation time per 60 Hz frame on core 1

If 60 Hz video misses its budget for more than 10 frames in a row, that game
session drops to 30 Hz video (emulation stays at 60 Hz) and says so on serial.
`VIDEO_HALF_RATE` in `picocalc_config.h` forces 30 Hz for everything.
`SHOW_TEST_PATTERN` brings back the boot colour bars.

System clock is 300 MHz at 1.30 V. Flash runs at 75 MHz via the embedded
boot2 (`PICO_EMBED_XIP_SETUP`, `PICO_FLASH_SPI_CLKDIV=4`).
