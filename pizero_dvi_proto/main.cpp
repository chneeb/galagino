/*
 * main.cpp - DVI geometry prototype for Galagino on the Waveshare RP2350-PiZero
 *
 * Question: can core 1 scan out a 224x288 arcade frame at a usable size on
 * 640x480p60, and how busy does that keep it?
 *
 * Core 0 plays the emulator: it holds the frame in Galagino's own format
 * (big endian RGB565), redraws all of it every 60 Hz frame with a moving
 * sprite, and feeds silence into the HDMI audio ring. Core 1 converts and
 * scales each line into pico_lib's line buffer and TMDS-encodes it.
 *
 * Layouts (pico_lib always doubles pixels horizontally: 320 in, 640 out):
 *   DOUBLED  lines doubled (240 unique), picture shrunk to 187x240 source
 *            pixels = 374x480 on screen. Correct shape, drops 1 in 6 rows
 *            and columns. Baseline load, same mode as msx2pico.
 *   WIDE     480 unique lines, 1.5x vertical (432 lines), 1x horizontal
 *            = 448x432 on screen. Sharp, but 33% too wide.
 *   BLEND    480 unique lines, 1.5x vertical, 4:3 horizontal blend to 168
 *            source pixels = 336x432 on screen. Correct shape, a bit soft.
 *
 * Build proto_doubled for DOUBLED; proto_480 alternates WIDE and BLEND
 * every 10 seconds. Stats go to USB serial every 2 seconds.
 */

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/vreg.h"
#include "hardware/clocks.h"

#include "dvi/dvi.h"
#include "dvi/timing.h"

// Waveshare RP2350-PiZero, as in pico-infonesPlus dvi_configs.h and msx2pico
static const dvi::Config dvi_cfg = {
  .pinTMDS  = {36, 34, 32},
  .pinClock = 38,
  .invert   = false,
};

#define SRC_W  224
#define SRC_H  288
#define LINE_W 320           // pico_lib line buffer; doubled to 640 on screen

enum layout_t { DOUBLED, WIDE, BLEND };
static const char *layout_name[] = { "DOUBLED", "WIDE", "BLEND" };

static dvi::DVI *dvi_inst;
static volatile layout_t layout;

// Galagino's frame format: big endian RGB565, as the ILI9341 wants it
static uint16_t framebuffer[SRC_H][SRC_W];
static uint16_t pattern[SRC_H][SRC_W];

static inline uint16_t be565(int r, int g, int b) {
  uint16_t c = ((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3);
  return (c >> 8) | (c << 8);
}

// big endian RGB565 -> RGB555 as pico_lib's encoder wants it
static inline uint16_t __not_in_flash_func(to555)(uint16_t be) {
  uint16_t c = (be >> 8) | (be << 8);
  return ((c >> 1) & 0x7fe0) | (c & 0x1f);
}

// average of two RGB555 pixels (per channel, rounding down)
static inline uint16_t avg555(uint16_t a, uint16_t b) {
  return ((a & b) + (((a ^ b) & 0x7bde) >> 1));
}

/* ---------------------------- test pattern ------------------------------ */

static void make_pattern(void) {
  for(int y=0;y<SRC_H;y++)
    for(int x=0;x<SRC_W;x++) {
      uint16_t c = be565(0, 0, 0);

      // 8x8 grid, like tile boundaries
      if((x & 7) == 0 || (y & 7) == 0) c = be565(40, 40, 40);

      // colour bars, rows 16..63
      if(y >= 16 && y < 64) {
        static const uint8_t bars[7][3] = {
          {255,0,0},{0,255,0},{0,0,255},{255,255,255},{0,255,255},{255,0,255},{255,255,0} };
        const uint8_t *b = bars[x / 32];
        c = be565(b[0], b[1], b[2]);
      }

      // 1 pixel wide vertical lines, rows 80..127: dropped or merged columns show
      if(y >= 80 && y < 128 && x >= 16 && x < 208)
        c = (x & 1) ? be565(255, 255, 255) : be565(0, 0, 0);

      // 1 pixel high horizontal lines, rows 144..191: dropped or doubled rows show
      if(y >= 144 && y < 192 && x >= 16 && x < 208)
        c = (y & 1) ? be565(255, 255, 255) : be565(0, 0, 0);

      // grey ramp, rows 208..239
      if(y >= 208 && y < 240)
        c = be565(x * 255 / SRC_W, x * 255 / SRC_W, x * 255 / SRC_W);

      // white border: must be fully visible
      if(x == 0 || y == 0 || x == SRC_W-1 || y == SRC_H-1) c = be565(255, 255, 255);

      pattern[y][x] = c;
    }
}

/* -------------------------------- core 1 -------------------------------- */

static volatile uint32_t compose_us = 0;

// fill one pico_lib line buffer (320 pixels) from source row sy
static void __not_in_flash_func(compose_line)(uint16_t *dst, int sy, layout_t l) {
  const uint16_t *src = framebuffer[sy];

  if(l == BLEND) {
    const int w = SRC_W * 3 / 4, x0 = (LINE_W - w) / 2;   // 168, 76
    memset(dst, 0, x0 * 2);
    uint16_t *d = dst + x0;
    for(int x=0;x<SRC_W;x+=4) {
      uint16_t s0 = to555(src[x]),   s1 = to555(src[x+1]);
      uint16_t s2 = to555(src[x+2]), s3 = to555(src[x+3]);
      uint16_t s01 = avg555(s0, s1), s23 = avg555(s2, s3);
      *d++ = avg555(s0, s01);      // 3/4 s0 + 1/4 s1
      *d++ = avg555(s1, s2);       // 1/2 s1 + 1/2 s2
      *d++ = avg555(s23, s3);      // 1/4 s2 + 3/4 s3
    }
    memset(d, 0, (LINE_W - x0 - w) * 2);
  } else if(l == WIDE) {
    const int x0 = (LINE_W - SRC_W) / 2;                  // 48
    memset(dst, 0, x0 * 2);
    for(int x=0;x<SRC_W;x++) dst[x0 + x] = to555(src[x]);
    memset(dst + x0 + SRC_W, 0, (LINE_W - x0 - SRC_W) * 2);
  } else {  // DOUBLED: 224 -> 187 columns, nearest
    const int w = 187, x0 = (LINE_W - w) / 2;             // 66
    memset(dst, 0, x0 * 2);
    for(int x=0;x<w;x++) dst[x0 + x] = to555(src[x * 6 / 5]);
    memset(dst + x0 + w, 0, (LINE_W - x0 - w) * 2);
  }
}

static void __not_in_flash_func(core1_main)(void) {
  dvi_inst->registerIRQThisCore();
  dvi_inst->start();

  const int repeat = dvi_inst->getLineRepeat();
  const int lines = 480 / repeat;                          // logical lines per frame
  const int top = (repeat == 1) ? (480 - 432) / 2 : 0;     // blank margin, output lines
  const int first = top / repeat, last = lines - first;

  while(true) {
    layout_t l = layout;          // latched per frame
    for(int y=first;y<last;y++) {
      dvi::DVI::LineBuffer *lb = dvi_inst->getLineBuffer();
      uint32_t t = time_us_32();
      int sy = (repeat == 1) ? (y - first) * 2 / 3 : y * 6 / 5;
      compose_line(lb->data(), sy, l);
      compose_us += time_us_32() - t;
      dvi_inst->setLineBuffer(y, lb);
      dvi_inst->convertScanBuffer15bpp();
    }
  }
}

/* -------------------------------- core 0 -------------------------------- */

// redraw the whole frame like Galagino does every frame, plus a moving box
static void draw_frame(int frame) {
  memcpy(framebuffer, pattern, sizeof(framebuffer));

  int bx = 8 + (frame % 192), by = 248 + ((frame / 4) % 24);
  for(int y=by;y<by+16 && y<SRC_H-1;y++)
    for(int x=bx;x<bx+16 && x<SRC_W-1;x++)
      framebuffer[y][x] = be565(255, 200, 0);
}

static void feed_silence(void) {
  auto &ring = dvi_inst->getAudioRingBuffer();
  uint32_t n = ring.getWritableSize();
  auto *dst = ring.getWritePointer();
  for(uint32_t i=0;i<n;i++) dst[i] = { 0, 0 };
  ring.advanceWritePointer(n);
}

int main(void) {
  // as msx2pico on the same board: 640x480p60 needs exactly 252 MHz
  vreg_set_voltage(VREG_VOLTAGE_1_20);
  sleep_ms(10);
  set_sys_clock_khz(252000, true);
  sleep_ms(10);

  stdio_init_all();

  make_pattern();
  draw_frame(0);

#ifdef PROTO_DOUBLED
  layout = DOUBLED;
#else
  layout = WIDE;
#endif

  dvi_inst = new dvi::DVI(pio0, &dvi_cfg, dvi::getTiming640x480p60Hz());
#ifndef PROTO_DOUBLED
  dvi_inst->setLineRepeat(1);
  dvi_inst->getBlankSettings().top = (480 - 432) / 2;
  dvi_inst->getBlankSettings().bottom = (480 - 432) / 2;
#endif
  // HDMI audio packets as a real port would send them (48 kHz is a rate
  // TVs accept); fed with silence below
  dvi_inst->setAudioFreq(48000, 0, 6144);
  dvi_inst->allocateAudioBuffer(1024);

  multicore_launch_core1(core1_main);

  uint32_t frame = 0, last_stats = time_us_32(), last_switch = last_stats;
  uint32_t draw_sum = 0, last_frames = 0;
  sleep_ms(500);
  dvi_inst->resetStats();
  compose_us = 0;

  while(true) {
    uint32_t t0 = time_us_32();
    draw_frame(frame++);
    draw_sum += time_us_32() - t0;
    feed_silence();

    // pace core 0 at ~60 Hz
    uint32_t used = time_us_32() - t0;
    if(used < 16667) sleep_us(16667 - used);

    uint32_t now = time_us_32();
#ifndef PROTO_DOUBLED
    if(now - last_switch >= 10000000) {
      last_switch = now;
      layout = (layout == WIDE) ? BLEND : WIDE;
    }
#endif
    if(now - last_stats >= 2000000) {
      float secs = (now - last_stats) / 1e6f;
      uint32_t enc = dvi_inst->getEncodeUs(), wait = dvi_inst->getWaitUs();
      uint32_t comp = compose_us;
      // core 1 busy share = compose + encode over the elapsed time
      printf("%s (repeat %d): core1 busy %.1f%% (compose %.1f%%, encode %.1f%%, wait %.1f%%), "
             "missed lines %lu, frames %lu | core0 frame redraw %lu us | sys %lu MHz\n",
             layout_name[layout], dvi_inst->getLineRepeat(),
             100.0f * (comp + enc) / (secs * 1e6f),
             100.0f * comp / (secs * 1e6f), 100.0f * enc / (secs * 1e6f),
             100.0f * wait / (secs * 1e6f),
             dvi_inst->getMissedLines(), dvi_inst->getFrameCounter() - last_frames,
             frame ? draw_sum / frame : 0, clock_get_hz(clk_sys) / 1000000);
      dvi_inst->resetStats();
      compose_us = 0;
      last_frames = dvi_inst->getFrameCounter();
      last_stats = now;
    }
  }
}
