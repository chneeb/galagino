/*
 * main.cpp - Galagino on the Waveshare RP2350-PiZero with DVI/HDMI output
 *
 * Core 1 only scans out: each 60 Hz frame it converts the 224x288 arcade
 * screen to 187x240 (dropping 1 in 6 rows and columns, the layout chosen
 * with pizero_dvi_proto) and pico_lib doubles that to 374x480 on 640x480.
 *
 * Core 0 does everything else, in the hook emulate_frame() calls once per
 * emulated frame: draw the screen into a back buffer, poll the pad, top up
 * the HDMI audio ring, service USB and wait for the next DVI frame. Menu, rendering and
 * sound are shared with the other RP2350 ports (galagino_core.c).
 */

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/vreg.h"
#include "hardware/clocks.h"

#include "dvi/dvi.h"
#include "dvi/timing.h"

#include "galagino_core.h"

extern "C" {
#include "pad.h"
#include "usb_input.h"

// from emulation.c (emulation.h itself isn't valid C++)
void prepare_emulation(void);
void emulate_frame(void);
#ifndef SINGLE_MACHINE
extern signed char machine;
#endif

void galagino_wait_vblank(void);
void galagino_idle(unsigned ms);
}

#define SRC_W   224
#define SRC_H   288
#define LINE_W  320          // pico_lib line buffer, doubled to 640 on screen
#define LINES   240          // unique lines, each shown twice
#define DST_W   187          // 224 * 240 / 288, keeps the arcade's shape
#define DST_X   ((LINE_W - DST_W) / 2)

#define HDMI_AUDIO_RATE  48000
#define AUDIO_VOLUME     40  // core samples are about +/- 512

// Waveshare RP2350-PiZero, as in pico-infonesPlus dvi_configs.h and msx2pico
static const dvi::Config dvi_cfg = {
  .pinTMDS  = {36, 34, 32},
  .pinClock = 38,
  .invert   = false,
};

static dvi::DVI *dvi_inst;

// two screens in Galagino's format (big endian RGB565): core 1 shows one
// while core 0 draws the other
static uint16_t screen[2][SRC_H][SRC_W];
static volatile int front = 0;      // latest finished screen
static volatile int scanning = 0;   // screen core 1 is showing

/* -------------------------------- core 1 -------------------------------- */

static uint8_t xmap[DST_W];         // source column per output column

// big endian RGB565 -> RGB555 as pico_lib's encoder wants it
static inline uint16_t to555(uint16_t be) {
  uint16_t c = (be >> 8) | (be << 8);
  return ((c >> 1) & 0x7fe0) | (c & 0x1f);
}

static void __not_in_flash_func(core1_main)(void) {
  dvi_inst->registerIRQThisCore();
  dvi_inst->start();

  while(true) {
    int b = front;                  // latched once per frame
    scanning = b;
    for(int y=0;y<LINES;y++) {
      dvi::DVI::LineBuffer *lb = dvi_inst->getLineBuffer();
      uint16_t *dst = lb->data();
      const uint16_t *src = screen[b][y * (SRC_H-1) / (LINES-1)];

      memset(dst, 0, DST_X * 2);
      for(int x=0;x<DST_W;x++) dst[DST_X + x] = to555(src[xmap[x]]);
      memset(dst + DST_X + DST_W, 0, (LINE_W - DST_X - DST_W) * 2);

      dvi_inst->setLineBuffer(y, lb);
      dvi_inst->convertScanBuffer15bpp();
    }
  }
}

/* ------------------------------ board glue ------------------------------ */

unsigned char platform_buttons(void) {
  return pad_buttons() | usb_input_buttons();
}

/* --------------------------------- audio -------------------------------- */

// the core renders at 24 kHz (DK: 11765 Hz); HDMI wants a standard rate,
// so resample to 48 kHz with linear interpolation
static uint32_t resample_step = ((uint32_t)CORE_AUDIO_RATE << 16) / HDMI_AUDIO_RATE;
static uint32_t resample_phase = 0;
static int16_t core_buf[64];
static int core_pos = 64;
static int16_t prev_sample = 0, cur_sample = 0;

void platform_audio_set_rate(unsigned rate) {
  resample_step = ((uint32_t)rate << 16) / HDMI_AUDIO_RATE;
}

static inline int16_t next_core_sample(void) {
  if(core_pos == 64) {
    core_audio_render(core_buf, 64);
    core_pos = 0;
  }
  return core_buf[core_pos++];
}

static void audio_fill(void) {
  auto &ring = dvi_inst->getAudioRingBuffer();
  // the writable area may wrap around the ring end, hence two passes
  for(int pass=0;pass<2;pass++) {
    uint32_t n = ring.getWritableSize();
    if(!n) break;
    auto *dst = ring.getWritePointer();
    for(uint32_t i=0;i<n;i++) {
      resample_phase += resample_step;
      while(resample_phase >= 0x10000) {
        resample_phase -= 0x10000;
        prev_sample = cur_sample;
        cur_sample = next_core_sample();
      }
      int v = prev_sample + (((cur_sample - prev_sample) * (int)resample_phase) >> 16);
      v *= AUDIO_VOLUME;
      if(v > 32767) v = 32767;
      if(v < -32768) v = -32768;
      dst[i] = { (int16_t)v, (int16_t)v };
    }
    ring.advanceWritePointer(n);
  }
}

/* -------------------------------- video --------------------------------- */

#ifdef VIDEO_HALF_RATE
static bool half_rate = true;
#else
static bool half_rate = false;
static int late_frames = 0;
#endif

static uint32_t draw_us_sum = 0, draw_us_max = 0, draw_frames = 0;
static uint32_t emu_us_sum = 0, emu_us_max = 0, emu_frames = 0;

static void draw_screen(void) {
  // a finished screen core 1 hasn't picked up yet must not be overwritten
  while(front != scanning) tight_loop_contents();
  int target = scanning ^ 1;

  uint32_t t = time_us_32();
  core_prepare_frame();
  for(int row=0;row<36;row++) {
    frame_buffer = &screen[target][row * 8][0];
    core_render_line(row);
  }
  front = target;
  core_frame_done(half_rate);

  t = time_us_32() - t;
  draw_us_sum += t;
  if(t > draw_us_max) draw_us_max = t;
  draw_frames++;
}

static void print_stats(void) {
  static uint32_t last = 0;
  uint32_t now = time_us_32();
  if(now - last < 2000000) return;
  float secs = (now - last) / 1e6f;
  last = now;

  printf("machine %d: video %s | draw avg %lu max %lu us | emu avg %lu max %lu us | "
         "core1 encode %.0f%% | missed lines %lu\n",
#ifndef SINGLE_MACHINE
         machine,
#else
         1,
#endif
         half_rate ? "30Hz" : "60Hz",
         draw_frames ? draw_us_sum / draw_frames : 0, draw_us_max,
         emu_frames ? emu_us_sum / emu_frames : 0, emu_us_max,
         100.0f * dvi_inst->getEncodeUs() / (secs * 1e6f),
         dvi_inst->getMissedLines());

  draw_us_sum = draw_us_max = draw_frames = 0;
  emu_us_sum = emu_us_max = emu_frames = 0;
  dvi_inst->resetStats();
}

/* ------------------------- emulation frame hooks ------------------------ */

static uint32_t last_dvi_frame = 0;   // DVI frame the current emulated frame belongs to
static uint32_t emu_started = 0;      // 0 = not measuring (booting)
static uint32_t frame_count = 0;

// everything core 0 does between two emulated frames
static void between_frames(void) {
  if(!half_rate || (frame_count & 1))
    draw_screen();
  frame_count++;

  pad_poll();
  usb_input_task();
  audio_fill();
  print_stats();
}

// called by emulate_frame() (via ulTaskNotifyTake in esp32_compat.h) once
// per emulated frame; paced by the DVI frame counter, i.e. 60 Hz
void galagino_wait_vblank(void) {
  uint32_t now = time_us_32();
  if(emu_started) {
    uint32_t us = now - emu_started;
    emu_us_sum += us;
    if(us > emu_us_max) emu_us_max = us;
    emu_frames++;
  }

  between_frames();

  // late if the next DVI frame already started before we got here
  uint32_t f = dvi_inst->getFrameCounter();
#ifndef VIDEO_HALF_RATE
  late_frames = (f - last_dvi_frame > 1) ? late_frames + 1 : 0;
  if(late_frames > 10 && !half_rate
#ifndef SINGLE_MACHINE
     && machine != 0       // MCH_MENU
#endif
    ) {
    printf("60 Hz video can't keep up, switching to 30 Hz\n");
    half_rate = true;
  }
#endif

  while(dvi_inst->getFrameCounter() == last_dvi_frame)
    tight_loop_contents();
  last_dvi_frame = dvi_inst->getFrameCounter();
  emu_started = time_us_32();
}

// called by emulate_frame() instead while the game is still booting
void galagino_idle(unsigned ms) {
  sleep_ms(ms);
  emu_started = 0;

  // keep the screen, pad and audio going during boot
  uint32_t f = dvi_inst->getFrameCounter();
  if(f != last_dvi_frame) {
    last_dvi_frame = f;
    between_frames();
  }
}

/* --------------------------------- main --------------------------------- */

int main(void) {
  // as msx2pico on the same board: 640x480p60 needs exactly 252 MHz
  vreg_set_voltage(VREG_VOLTAGE_1_20);
  sleep_ms(10);
  set_sys_clock_khz(252000, true);
  sleep_ms(10);

  stdio_init_all();
  printf("Galagino RP2350-PiZero: sys %lu Hz\n", clock_get_hz(clk_sys));

  for(int x=0;x<DST_W;x++) xmap[x] = x * (SRC_W-1) / (DST_W-1);

  pad_init();
  prepare_emulation();       // allocates memory[], resets the CPUs

  dvi_inst = new dvi::DVI(pio0, &dvi_cfg, dvi::getTiming640x480p60Hz());
  dvi_inst->setAudioFreq(HDMI_AUDIO_RATE, 0, 6144);
  dvi_inst->allocateAudioBuffer(2048);

#if defined(SINGLE_MACHINE) && defined(ENABLE_DKONG)
  // only dkong installed? Then set up its rate immediately
  platform_audio_set_rate(CORE_AUDIO_RATE_DKONG);
#endif

  multicore_launch_core1(core1_main);
  usb_input_init();          // after DVI, which claims DMA channels 0-5
  audio_fill();
  last_dvi_frame = dvi_inst->getFrameCounter();

  while(true)
    emulate_frame();
}
