# DVI geometry prototype (RP2350-PiZero)

Answers the open question from `plans/rp2350-port.md` for a DVI port: can core 1 put a
224×288 arcade frame on 640×480p60 at a usable size, and how busy does that keep it?

No emulation. Core 0 holds a test picture in Galagino's frame format (big-endian RGB565),
redraws the whole frame at 60 Hz with a moving yellow box, and feeds silence into the HDMI
audio ring. Core 1 converts, scales and TMDS-encodes each line, like a real port would.

## Three firmware files

| File | Mode | On screen |
|---|---|---|
| `build/proto_doubled.uf2` | 240 unique lines, each shown twice (msx2pico's mode) | **DOUBLED**: 374×480, correct shape, but 1 in 6 rows and columns dropped |
| `build/proto_480.uf2` | 480 unique lines (patched `pico_lib`) | alternates every 10 s: **WIDE** 448×432, sharp but 33% too wide, and **BLEND** 336×432, correct shape, slightly soft |
| `build/proto_480_direct.uf2` | 480 unique lines, encoder only | **DIRECT**: same picture as WIDE, but core 0 stores the frame ready for the encoder, so core 1 only encodes |

## Build

```sh
cd pizero_dvi_proto
PICO_SDK_PATH=~/Source/pico-sdk cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build
```

## Results so far (2026-09-26)

- `proto_doubled`: picture on a monitor, colours correct. The bottom border was missing because
  of a rounding bug (row 287 never sampled), now fixed. The monitor stretched 640×480 to full
  width. Use its 4:3 / aspect setting to judge shape.
- `proto_480`: **red screen** with one horizontal band of the picture. Red is `pico_lib`'s
  "no data for this line" (`TMDSRedSym_` in `dma.cpp`), so core 1 missed most lines.
- `proto_480_direct` (encoder only): a blueish "shadow" of the picture on the right. Unexplained,
  possibly a bug in the new direct path. No serial output taken.
- **Decision: use the doubled mode** (DOUBLED, 374×480, bottom border fixed). The owner finds the
  picture fine. 480 unique lines is shelved. The untried lever would be encoding only the active
  pixels, with pre-encoded black margins.

## Test (monitor, not the TV)

The TV needs the 5 V pin-18 mod. A PC monitor should work without it.

1. Flash `proto_480.uf2`. Connect the PiZero's native USB-C port to the PC and open the serial
   console (`picocom /dev/ttyACM0`).
2. Check the picture: the **white border must be complete** on all four sides, and the image
   shouldn't roll, flicker or show coloured or black lines.
3. Watch both layouts; it switches every 10 s and serial says which one is active:
   - **Vertical line block** (rows 80–127, alternating 1-px columns): WIDE should look like a
     clean fine stripe pattern. BLEND will look grey-ish (columns merged). That's the softness.
   - **Horizontal line block** (rows 144–191): fine lines. In 1.5× scaling, every other line
     appears doubled, so expect a slightly uneven stripe rhythm.
4. Copy a few serial lines of each layout.
5. Flash `proto_doubled.uf2` and do the same. Expect visible gaps in both line blocks.
6. Flash `proto_480_direct.uf2`. If this shows a picture while `proto_480` doesn't, core 1's
   per-line conversion is the problem and belongs on core 0. If it fails too, the encoder alone
   can't do 480 unique lines.

If a 480 variant fails, note *how*: no signal, black screen with signal, or a picture with
black or flickering lines. Serial still runs on core 0 in every case.

## Serial output

```
BLEND (repeat 1): core1 busy 41.2% (compose 12.3%, encode 28.9%, wait 57.1%), missed lines 0, frames 120 | core0 frame redraw 350 us | sys 252 MHz
```

- `core1 busy`: share of core 1's time spent composing and encoding lines. **The key number.**
  Full-resolution output (640 unique pixels per line, which a sharp and correctly shaped 1.5×
  would need) roughly doubles the encode share.
- `missed lines`: active lines sent without data, because core 1 was late. **Must stay 0.**
- `frames`: should be ~120 per 2 s (60 Hz).
- `core0 frame redraw`: cost of core 0's full-frame copy, a stand-in for bus contention.

## Local changes to pico_lib

Vendored from `~/Source/msx2pico/pico_lib` (fhoedemakers/pico_lib at `4c53bd2`, plus msx2pico's
removed debug printfs). MIT licence, see `pico_lib/LICENSE`. Patched in `dvi/dvi.h` and
`dvi/dvi.cpp`:

- `setLineRepeat(n)`: line doubling (2, upstream) or 480 unique lines (1). Replaces the
  hard-coded `line * 2` and `N_LINE_PER_DATA` in the DMA IRQ.
- Missed-line counter in the IRQ, and wait/encode timing in `convertScanBuffer15bpp()`.
