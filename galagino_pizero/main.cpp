/*
 * main.cpp - Galagino on the Waveshare RP2350-PiZero with DVI/HDMI output
 *
 * pico_lib runs 640x480p60 with 480 unique lines. Three layouts of the
 * 224x288 arcade screen, cycled with Right in the game menu:
 *
 *   DOUBLE  374x480: 187x240 source pixels, each shown 2x2 (1 in 6 rows and
 *           columns dropped). Correct 4:3 shape.
 *   WIDE    448x432: every row 1.5x, every column 2x. Sharp and complete,
 *           but 33% too wide on a screen that keeps 4:3.
 *   ASPECT  336x432: every row 1.5x, 1 in 4 columns dropped. Correct shape.
 *
 * With DVI_ALTERNATE_DROP the dropped rows/columns change every frame, so
 * every one is shown at least at 30 Hz.
 *
 * Core 0 emulates, then draws the screen strip by strip and converts each
 * strip into encoder-ready RGB555 rows (horizontal layout, margins,
 * diagnostics). Core 1 only picks the row for each of the 480 output lines
 * (vertical layout) and TMDS-encodes it with the SIO encoder: the "direct"
 * path proven by pizero_dvi_proto (converting on core 1 through pico_lib's
 * line queue missed most lines at 480).
 *
 * Menu, rendering and sound are shared with the other RP2350 ports
 * (galagino_core.c).
 */

#include <stdio.h>
#include <stdarg.h>
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

#define SRC_W      224
#define SRC_H      288
#define LINE_W     320       // encoder input per line, doubled to 640 on screen
#define OUT_LINES  480

// from emulation.h, which isn't valid C++
#define BUTTON_LEFT   0x01
#define BUTTON_RIGHT  0x02

#define HDMI_AUDIO_RATE  48000
#define AUDIO_VOLUME     40  // core samples are about +/- 512

// on-screen diagnostics in the margins beside the game: up to 8 characters
// of 8x8 source pixels per margin, one text row per 8 source rows
#define OV_COLS   8
#define OV_ROWS   (SRC_H / 8)

// Waveshare RP2350-PiZero, as in pico-infonesPlus dvi_configs.h and msx2pico
static const dvi::Config dvi_cfg = {
  .pinTMDS  = {36, 34, 32},
  .pinClock = 38,
  .invert   = false,
};

static dvi::DVI *dvi_inst;

// RAM copy of the 640x480p60 timing: pico_lib's DVI IRQ on core 1 reads it
// every line, and the original is const data in flash
static dvi::Timing dvi_timing;

/* ------------------------------- layouts -------------------------------- */

enum { MODE_DOUBLE, MODE_WIDE, MODE_ASPECT, MODES };
static const char *const mode_names[MODES] = { "DOUBLE", "WIDE", "ASPECT" };
static const int mode_w[MODES] = { 187, 224, 168 };   // source pixels per row after scaling
#define MODE_X0(m)  ((LINE_W - mode_w[m]) / 2)

// horizontal: source column per output column, per phase (core 0)
static uint8_t hmap[MODES][2][SRC_W];
// vertical: source row per output line, per phase, 0xffff = black (core 1)
static uint16_t vmap[MODES][2][OUT_LINES];
#define VMAP_BLACK 0xffff

static void init_maps(void) {
  for(int m=0;m<MODES;m++)
    for(int p=0;p<2;p++) {
#ifdef DVI_ALTERNATE_DROP
      int ph = p;
#else
      int ph = 0;
#endif
      // phase 1 samples half a step later, hitting what phase 0 skips
      int w = mode_w[m];
      for(int x=0;x<w;x++)
        hmap[m][p][x] = (x * SRC_W + (ph ? SRC_W / 2 : 0)) / w;

      for(int y=0;y<OUT_LINES;y++) {
        if(m == MODE_DOUBLE) {
          int l = y / 2;                        // 240 unique lines
          vmap[m][p][y] = (l * SRC_H + (ph ? SRC_H / 2 : 0)) / (OUT_LINES / 2);
        } else if(y < 24 || y >= 24 + 432)
          vmap[m][p][y] = VMAP_BLACK;           // 432 lines, centred
        else
          vmap[m][p][y] = (y - 24) * 2 / 3;
      }
    }
}

/* ------------------------------ frame data ------------------------------ */

// Two frames of encoder-ready rows (RGB555, 320 wide incl. margins): core 1
// shows one while core 0 prepares the other.
static uint16_t disp[2][SRC_H][LINE_W] __attribute__((aligned(4)));
static volatile uint8_t disp_mode[2];     // layout each frame was prepared for
static uint16_t black_row[LINE_W] __attribute__((aligned(4)));
static volatile int front = 0;            // latest finished frame
static volatile int scanning = 0;         // frame core 1 is showing

// set in the game menu: Left toggles the diagnostics, Right cycles the layout
#ifdef SHOW_OVERLAY
static volatile bool overlay_on = true;
#else
static volatile bool overlay_on = false;
#endif
static volatile int video_mode = DVI_MODE;

/* -------------------------------- core 1 -------------------------------- */

static void __not_in_flash_func(core1_main)(void) {
  dvi_inst->registerIRQThisCore();
  dvi_inst->start();

  int phase = 0;
  while(true) {
    int b = front;                  // latched once per frame
    scanning = b;
    phase ^= 1;
    const uint16_t *vm = vmap[disp_mode[b]][phase];

    // pico_lib wants the full 640 line size and encodes its first half doubled
    for(int y=0;y<OUT_LINES;y++) {
      const uint16_t *row = (vm[y] == VMAP_BLACK) ? black_row : disp[b][vm[y]];
      dvi_inst->convertScanBuffer15bpp(y, row, LINE_W * 2);
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

// last 2 s window, for the overlay
static uint32_t shown_emu_avg, shown_emu_max, shown_draw_avg, shown_draw_max;
static uint32_t shown_core1, shown_missed;

// big endian RGB565 -> RGB555 as pico_lib's encoder wants it
static inline uint16_t to555(uint16_t be) {
  uint16_t c = (be >> 8) | (be << 8);
  return ((c >> 1) & 0x7fe0) | (c & 0x1f);
}

// diagnostics text, filled by overlay_update(), drawn into the margins
#include "font_8x8.h"              // 95 glyphs, row-major across glyphs, LSB = left
static char ov_text[2][OV_ROWS][OV_COLS];   // [left/right][row][col], space padded
static uint16_t ov_color[2][OV_ROWS];
static uint32_t banner_until = 0;           // layout name shown after a change

static void draw_margin_text(uint16_t (*f)[LINE_W], int side, int row, const char *t, uint16_t color, int m) {
  int margin = side ? LINE_W - MODE_X0(m) - mode_w[m] : MODE_X0(m);
  int cols = (margin - 2) / 8;
  if(cols > OV_COLS) cols = OV_COLS;
  int x0 = side ? LINE_W - cols * 8 - 1 : 1;
  for(int c=0;c<cols;c++) {
    unsigned ch = (unsigned char)t[c];
    if(ch <= 32 || ch > 126) continue;
    for(int gy=0;gy<8;gy++) {
      uint8_t bits = font_8x8[(ch - 32) + gy * 95];
      uint16_t *p = &f[row * 8 + gy][x0 + c * 8];
      for(int b=0;b<8;b++)
        if(bits & (1 << b)) p[b] = color;
    }
  }
}

// margins of each frame buffer: clean for which layout, and whether text was drawn
static int margin_mode[2] = { -1, -1 };
static bool margin_text[2] = { false, false };

static void prepare_margins(int t, int m) {
  bool text = overlay_on || (int32_t)(banner_until - time_us_32()) > 0;
  if(margin_mode[t] != m || margin_text[t] || text) {
    int x0 = MODE_X0(m), w = mode_w[m];
    for(int y=0;y<SRC_H;y++) {
      memset(disp[t][y], 0, x0 * 2);
      memset(disp[t][y] + x0 + w, 0, (LINE_W - x0 - w) * 2);
    }
    margin_mode[t] = m;
  }
  margin_text[t] = text;
  if(!text) return;

  if(overlay_on) {
    for(int side=0;side<2;side++)
      for(int r=0;r<OV_ROWS;r++)
        draw_margin_text(disp[t], side, r, ov_text[side][r], ov_color[side][r], m);
  } else {
    char name[OV_COLS + 1];
    snprintf(name, sizeof(name), "%-8s", mode_names[m]);
    draw_margin_text(disp[t], 0, 0, "LAYOUT  ", 0x03ff, m);
    draw_margin_text(disp[t], 0, 1, name, 0x7fff, m);
  }
}

static uint16_t strip[SRC_W * 8] __attribute__((aligned(4)));   // one tile row

static void draw_screen(void) {
  // a finished frame core 1 hasn't picked up yet must not be overwritten
  while(front != scanning) tight_loop_contents();
  int t = scanning ^ 1;
  int m = video_mode;
#ifdef DVI_ALTERNATE_DROP
  static int phase = 0;
  phase ^= 1;
#else
  const int phase = 0;
#endif
  const uint8_t *hm = hmap[m][phase];
  const int x0 = MODE_X0(m), w = mode_w[m];

  uint32_t us = time_us_32();
  core_prepare_frame();
  frame_buffer = strip;
  for(int row=0;row<36;row++) {
    core_render_line(row);
    for(int r=0;r<8;r++) {
      const uint16_t *s = strip + r * SRC_W;
      uint16_t *d = disp[t][row * 8 + r] + x0;
      for(int x=0;x<w;x++) d[x] = to555(s[hm[x]]);
    }
  }
  prepare_margins(t, m);
  disp_mode[t] = m;
  front = t;
  core_frame_done(half_rate);

  us = time_us_32() - us;
  draw_us_sum += us;
  if(us > draw_us_max) draw_us_max = us;
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

  shown_emu_avg  = emu_frames ? emu_us_sum / emu_frames : 0;
  shown_emu_max  = emu_us_max;
  shown_draw_avg = draw_frames ? draw_us_sum / draw_frames : 0;
  shown_draw_max = draw_us_max;
  shown_core1    = (uint32_t)(100.0f * dvi_inst->getEncodeUs() / (secs * 1e6f));
  shown_missed   = dvi_inst->getMissedLines();

  draw_us_sum = draw_us_max = draw_frames = 0;
  emu_us_sum = emu_us_max = emu_frames = 0;
  dvi_inst->resetStats();
}

/* ------------------------------- overlay -------------------------------- */

#define OV_LABEL  0x03ff   // cyan (RGB555)
#define OV_VALUE  0x7fff   // white
#define OV_WARN   0x7c1f   // magenta

static void ov_set(int side, int row, uint16_t color, const char *fmt, ...) {
  char buf[16];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  char *t = ov_text[side][row];
  int i = 0;
  for(;i<OV_COLS && buf[i];i++) t[i] = buf[i];
  for(;i<OV_COLS;i++) t[i] = ' ';
  ov_color[side][row] = color;
}

// average and maximum on two rows, to fit 6 character margins
static void ov_ms(int side, int row, uint32_t avg, uint32_t max) {
  ov_set(side, row,     OV_VALUE, "A%lu.%lu", avg / 1000, (avg / 100) % 10);
  ov_set(side, row + 1, OV_VALUE, "M%lu.%lu", max / 1000, (max / 100) % 10);
}

static void overlay_update(void) {
  int r = 0;
  // left: this board and timings
  ov_set(0, r++, OV_LABEL, "GALA-");
  ov_set(0, r++, OV_LABEL, "GINO");
  ov_set(0, r++, OV_LABEL, "PIZERO");
  r++;
  ov_set(0, r++, OV_LABEL, "LAYOUT");
  ov_set(0, r++, OV_VALUE, "%s", mode_names[video_mode]);
  r++;
  ov_set(0, r++, OV_LABEL, "VIDEO");
  ov_set(0, r++, OV_VALUE, "%s", half_rate ? "30HZ" : "60HZ");
  ov_set(0, r++, OV_LABEL, "EMU MS");
  ov_ms(0, r, shown_emu_avg, shown_emu_max); r += 2;
  ov_set(0, r++, OV_LABEL, "DRAWMS");
  ov_ms(0, r, shown_draw_avg, shown_draw_max); r += 2;
  ov_set(0, r++, OV_LABEL, "CORE1%");
  ov_set(0, r++, OV_VALUE, "%lu", shown_core1);
  ov_set(0, r++, OV_LABEL, "MISSED");
  ov_set(0, r++, shown_missed ? OV_WARN : OV_VALUE, "%lu", shown_missed);
  r++;
  ov_set(0, r++, OV_LABEL, "I2CPAD");
  ov_set(0, r++, pad_connected() ? OV_VALUE : OV_WARN, "%s", pad_connected() ? "OK" : "NONE");
  ov_set(0, r++, OV_LABEL, "BUTTON");
  ov_set(0, r++, OV_VALUE, "%02X", platform_buttons());

  // right: USB
  const usb_status_t *u = usb_input_status();
  static const char *decoders[] = { "NONE", "HIDPAR", "MAPPED", "KEYBRD" };
  r = 0;
  ov_set(1, r++, OV_LABEL, "USB");
  ov_set(1, r++, OV_LABEL, "DEVICE");
  ov_set(1, r++, u->devices ? OV_VALUE : OV_WARN, "%d", u->devices);
  ov_set(1, r++, OV_LABEL, "HIDITF");
  ov_set(1, r++, OV_VALUE, "%d", u->mounted);
  ov_set(1, r++, OV_LABEL, "VIDPID");
  ov_set(1, r++, OV_VALUE, "%04X", u->vid);
  ov_set(1, r++, OV_VALUE, "%04X", u->pid);
  ov_set(1, r++, OV_LABEL, "TYPE");
  ov_set(1, r++, OV_VALUE, "%s", u->proto == 1 ? "KEYBRD" : u->proto == 2 ? "MOUSE" : "OTHER");
  ov_set(1, r++, OV_LABEL, "DECODE");
  ov_set(1, r++, u->decoder < 0 ? OV_WARN : OV_VALUE, "%s", u->decoder < 0 ? "PARERR" : decoders[u->decoder]);
  ov_set(1, r++, OV_LABEL, "REPORT");
  ov_set(1, r++, OV_VALUE, "%lu", u->reports);
  ov_set(1, r++, OV_LABEL, "LEN");
  ov_set(1, r++, OV_VALUE, "%u", u->len);
  ov_set(1, r++, OV_LABEL, "RAW");
  for(int i=0;i<15;i+=3)
    ov_set(1, r++, OV_VALUE, "%02X%02X%02X", u->raw[i], u->raw[i+1], u->raw[i+2]);
  ov_set(1, r++, OV_LABEL, "USBBTN");
  ov_set(1, r++, OV_VALUE, "%02X", usb_input_buttons());
}

// in the menu: Left toggles the diagnostics, Right cycles the layout
static void menu_keys(void) {
#ifndef SINGLE_MACHINE
  static unsigned char last = 0;
  unsigned char k = platform_buttons();
  if(machine == 0) {                  // MCH_MENU
    if((k & BUTTON_LEFT) && !(last & BUTTON_LEFT)) {
      if(!overlay_on) memset(ov_text, ' ', sizeof(ov_text));
      overlay_on = !overlay_on;
    }
    if((k & BUTTON_RIGHT) && !(last & BUTTON_RIGHT)) {
      video_mode = (video_mode + 1) % MODES;
      banner_until = time_us_32() + 2000000;
    }
  }
  last = k;
#endif
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
  menu_keys();
  if(overlay_on && (frame_count & 7) == 0) overlay_update();
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

  init_maps();
  memset(ov_text, ' ', sizeof(ov_text));
  disp_mode[0] = disp_mode[1] = video_mode;

  pad_init();
  prepare_emulation();       // allocates memory[], resets the CPUs

  dvi_timing = *dvi::getTiming640x480p60Hz();
  dvi_inst = new dvi::DVI(pio0, &dvi_cfg, &dvi_timing);
  dvi_inst->setLineRepeat(1);          // 480 unique lines, layouts do the rest
  dvi_inst->setAudioFreq(HDMI_AUDIO_RATE, 0, 6144);
  dvi_inst->allocateAudioBuffer(2048);

#if defined(SINGLE_MACHINE) && defined(ENABLE_DKONG)
  // only dkong installed? Then set up its rate immediately
  platform_audio_set_rate(CORE_AUDIO_RATE_DKONG);
#endif

  multicore_launch_core1(core1_main);
  usb_input_init();
  audio_fill();
  last_dvi_frame = dvi_inst->getFrameCounter();

  while(true)
    emulate_frame();
}
