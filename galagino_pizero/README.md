# Galagino for the Waveshare RP2350-PiZero (DVI/HDMI)

All six Galagino machines on the RP2350-PiZero's mini-HDMI port. It builds the unchanged
emulation code from `../galagino` and the board-independent menu, rendering and sound from
`../galagino_pico_common` (shared with `../galagino_picocalc`), plus:

| File | Purpose |
|---|---|
| `main.cpp` | Core 1: DVI scanout. Core 0: emulation, drawing, pacing, HDMI audio, stats |
| `pad.c` | NES Classic Mini controller over I2C1 (SDA GP2, SCL GP3), 3.3 V |
| `usb_input.c` | USB gamepads/keyboards via PIO-USB (D+ GP28), and the USB-C serial console |
| `tusb_config.h`, `usb_descriptors.c` | TinyUSB: CDC device on native USB, host on PIO-USB (as in tiny_agi) |
| `hidparser/` | LUFA-derived HID report descriptor parser, from msx2pico |
| `font_8x8.h` | 8×8 font for the on-screen diagnostics (pico-infonesPlus, via msx2pico) |
| `pizero_config.h` | Force-included build config. Defines `_CONFIG_H_` so the CYD `config.h` is skipped |
| `pico_lib/` | fhoedemakers/pico_lib DVI driver (MIT), vendored and patched, see below |

## Hardware results (2026-09-26)

- **DVI output works** on a monitor. The monitor stretches 640×480 to 16:9 although the HDMI
  AVI InfoFrame already says 4:3 (`pico_lib` sends picture aspect 4:3, VIC 1). Use the monitor's
  4:3/aspect setting. TVs usually honour the InfoFrame or have a 4:3 mode. A software pre-squeeze
  (224 → 140 columns, blended) would be possible as a build option, but looks soft.
- **USB keyboard works** on the PIO-USB port, even without 5 V (this keyboard runs on 3.3 V).
- **USB gamepad (cheap SNES clone) does nothing**, also through a USB-C OTG cable, and through
  a powered hub that makes the same pad work with frank-snes. frank-snes uses the native USB
  controller rather than PIO-USB, hence `galagino_pizero_native.uf2` (untested). The same pad works in `~/Source/circle-libretro`
  on a Pi, which supplies 5 V. Most likely the pad doesn't run on 3.3 V. To confirm, check the
  console for `usb: device … mounted` when plugging it in; no line means power/enumeration.
  The mapping should already fit: circle-libretro's notes give this pad's buttons as
  X/A/B/Y/L/R = 1–6, Select = 9, Start = 10, which matches the `PAD_*` defaults.
- **NES Mini pad over I2C:** not yet tested.

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

## Two firmware variants

| File | Gamepad port | Serial console |
|---|---|---|
| `build/galagino_pizero.uf2` | PIO-USB port (D+ GP28) | native USB-C (CDC) + UART0 |
| `build/galagino_pizero_native.uf2` | **native USB-C port**, via OTG cable/hub, as frank-snes does | UART0 only (TX GP0) |

The native variant exists because the SNES-clone pad + hub works in frank-snes, which uses the
native USB controller, but not over PIO-USB here. With it, power the board through its other
USB-C port or the 5 V header pin, since the native port is busy as host. The on-screen
diagnostics show `USBNATIV` or `USB PIO` at the top left.

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

## USB gamepad or keyboard

Plug it into the PiZero's **PIO-USB port** (D+ on GP28, the USB-C port that isn't the native
one). **It needs 5 V from elsewhere:** the board doesn't power its USB ports. Use a powered hub,
or an OTG adapter with a power input. Some devices run on the 3.3 V that's there (a keyboard did);
the SNES-clone gamepad tested so far didn't.

- **Generic HID gamepads** (DirectInput style, most cheap/retro USB pads): d-pad, hat or stick
  moves. Buttons 1–6 fire, 7 or 9 coin (select), 8 or 10 start. Coin + start held 1 s returns to
  the menu. Pads differ: the console prints `usb: galagino buttons 0x.., pad buttons 3 9` on
  every change, so a wrong mapping can be fixed in the `PAD_*` defines in `usb_input.c`.
- **Known pads** are decoded from fixed byte positions measured for frank-snes
  (`~/Source/frank-snes/gamepads`): `0079:0006`, `081f:e401` (SNES clones), `11ff:3331`,
  `046d:c219`, `2563:0575`, `feed:2320`, plus `0810:e501` from pico-infonesPlus. Cheap SNES
  clones don't describe themselves reliably, so descriptor parsing isn't used for these.
- **XInput (Xbox style) pads** are not supported.

**If a pad does nothing,** watch the console while plugging it in:
- No `usb: device … mounted, vvvv:pppp` line: power or enumeration. A plain OTG cable adds no
  power; it needs a power input or a powered hub.
- Mounted, but pressing buttons prints no `galagino buttons` lines: the decoding is wrong. The
  console shows the pad's USB ID and its first raw reports (`usb: raw report …`), which is what
  a new quirk needs.
- **USB keyboards:** arrows move, Space/Ctrl/Z/X fire, 5 or C coin, 1 or Enter start, Esc (hold)
  menu.

The USB pad, keyboard and I2C pad can be used together.

## Hardware notes

- **TV:** the board doesn't put 5 V on HDMI pin 18, and many TVs stay dark without it (VSYS →
  100 Ω → pin 18 fixes it). PC monitors usually work.
- **USB gamepads** would need 5 V on the USB port too. Not supported; the I2C pad needs only 3.3 V.

## On-screen diagnostics

With `SHOW_OVERLAY` (on by default in `pizero_config.h`), the black margins beside the game show
live diagnostics, so no serial console is needed:

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

On the native USB-C port (a small CDC driver in `usb_input.c`, since the SDK disables its USB
stdio when the TinyUSB host is linked), and on UART0 TX (GP0, 115200) as a backup.


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
