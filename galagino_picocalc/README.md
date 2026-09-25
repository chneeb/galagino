# Galagino for the ClockworkPi PicoCalc

Pac-Man on the PicoCalc's Pico 2 (RP2350). It builds the unchanged emulation
code from `../galagino` (`Z80.c`, `emulation.c`, `pacman.h` and the generated
ROM headers) and adds a PicoCalc platform layer:

| File | Purpose |
|---|---|
| `main.c` | Core split, frame loop, Namco WSG sound, timing stats |
| `lcd.c`, `lcd.pio` | 320×320 panel over PIO SPI at 75 MHz, RGB565, DMA (after shapones) |
| `kbd.c` | I2C keyboard (STM32 at 0x1F on GP6/GP7) |
| `audio.c` | PWM audio on GP26/GP27 at 24 kHz, DMA ping-pong |
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
| Arrow keys | Joystick |
| `5` or `c` | Coin |
| `1` or Enter | Start |
| Space | Fire (unused in Pac-Man) |

## First boot checklist

1. **Colour bars** for 1.5 s, top to bottom: red, green, blue, white. Other colours mean the
   RGB565 byte order is wrong.
2. **Pac-Man attract mode**, centred with a black border.
3. **Coin, then start**, and play. Check that holding an arrow key keeps Pac-Man moving.
4. **Sound**, including the intro tune.
5. **USB serial** (`picocom /dev/ttyACM0` or similar) prints every 2 s:
   `video: avg … max … us | emu: avg … max … us | budget 16500 us`.
   If the video time goes over budget, enable `VIDEO_HALF_RATE` in `picocalc_config.h`.

System clock is 300 MHz at 1.30 V. Flash runs at 75 MHz via the embedded
boot2 (`PICO_EMBED_XIP_SETUP`, `PICO_FLASH_SPI_CLKDIV=4`).
