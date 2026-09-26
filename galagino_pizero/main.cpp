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

#define SRC_W   224
#define SRC_H   288
#define LINE_W  320          // pico_lib line buffer, doubled to 640 on screen
#define LINES   240          // unique lines, each shown twice
#define DST_W_MAX 187        // 224 * 240 / 288: picture width without a border

// from emulation.h, which isn't valid C++
#define BUTTON_LEFT   0x01
#define BUTTON_RIGHT  0x02

#define HDMI_AUDIO_RATE  48000

// on-screen diagnostics in the black margins beside the game: 8 characters
// of 8x8 pixels per margin (shown 16x16), 30 rows
#define OV_COLS   8
#define OV_ROWS   (LINES / 8)
#define OV_LEFT_X   1
#define OV_RIGHT_X  (LINE_W - OV_COLS * 8 - 2)
#define AUDIO_VOLUME     40  // core samples are about +/- 512

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

// two screens in Galagino's format (big endian RGB565): core 1 shows one
// while core 0 draws the other
static uint16_t screen[2][SRC_H][SRC_W];
static volatile int front = 0;      // latest finished screen
static volatile int scanning = 0;   // screen core 1 is showing

/* -------------------------------- core 1 -------------------------------- */

// Set by core 0 (in the menu: Left toggles the diagnostics, Right cycles the
// border), picked up by core 1 once per frame
#ifdef SHOW_OVERLAY
static volatile bool overlay_on = true;
#else
static volatile bool overlay_on = false;
#endif
static volatile int border = DVI_BORDER;   // black lines at top and bottom

// Core 1's current geometry, derived from border. The screen shrinks by
// about 6:5, so each output pixel covers parts of two source rows and two
// source columns: [xa, xb] and [ya, yb] below (equal where it covers one).
static uint8_t xa[DST_W_MAX], xb[DST_W_MAX];
static uint16_t ya[LINES], yb[LINES];
static int geo_border = -1, geo_w, geo_x, geo_lines;

static void __not_in_flash_func(set_geometry)(int b) {
  geo_lines = LINES - 2 * b;
  geo_w = (SRC_W * geo_lines + SRC_H / 2) / SRC_H;   // keeps the arcade's shape
  geo_x = (LINE_W - geo_w) / 2;

  // source span of output pixel i is [i * n / m, (i+1) * n / m)
  for(int x=0;x<geo_w;x++) {
    xa[x] = x * SRC_W / geo_w;
    xb[x] = ((x + 1) * SRC_W - 1) / geo_w;
  }
  for(int y=0;y<geo_lines;y++) {
    ya[y] = y * SRC_H / geo_lines;
    yb[y] = ((y + 1) * SRC_H - 1) / geo_lines;
  }
  geo_border = b;
}

#include "font_8x8.h"              // 95 glyphs, row-major across glyphs, LSB = left
static uint8_t font_ram[95 * 8];   // RAM copy: core 1 must never wait on flash
static_assert(sizeof(font_8x8) == sizeof(font_ram), "font_8x8.h must hold 95 glyphs x 8 rows");
static char ov_text[2][OV_ROWS][OV_COLS];   // [left/right][row][col], space padded
static uint16_t ov_color[2][OV_ROWS];

static void __not_in_flash_func(ov_draw_line)(uint16_t *dst, int y) {
  int row = y >> 3, gy = y & 7;
  for(int side=0;side<2;side++) {
    const char *t = ov_text[side][row];
    uint16_t col = ov_color[side][row];
    uint16_t *p = dst + (side ? OV_RIGHT_X : OV_LEFT_X);
    for(int c=0;c<OV_COLS;c++, p+=8) {
      unsigned ch = (unsigned char)t[c];
      if(ch <= 32 || ch > 126) continue;
      uint8_t bits = font_ram[(ch - 32) + gy * 95];
      for(int b=0;b<8;b++)
        if(bits & (1 << b)) p[b] = col;
    }
  }
}

// newlib's memset lives in flash; core 1 must not touch flash while core 0
// streams through the XIP cache (menu logos), or it misses DVI lines
static inline void __not_in_flash_func(black)(uint16_t *p, int n) {
  while(n-- > 0) *p++ = 0;
}

// big endian RGB565 -> RGB555 as pico_lib's encoder wants it
static inline uint16_t to555(uint16_t be) {
  uint16_t c = (be >> 8) | (be << 8);
  return ((c >> 1) & 0x7fe0) | (c & 0x1f);
}

#ifndef DVI_SCALE_NEAREST
// Shrinking by dropping rows and columns loses Galagino's 1 pixel lines (e.g.
// Pac-Man's maze walls). Instead each output pixel takes the per channel
// maximum of the source pixels it covers: thin bright lines on the black
// background stay fully visible. Pixels are unpacked to one byte per channel
// so the M33's DSP instructions compute the maximum of all three at once.

// big endian RGB565 -> 0x00RRGGBB with 5 bit channels
static inline uint32_t unpack(uint16_t be) {
  uint32_t c = (uint16_t)((be >> 8) | (be << 8));
  return ((c & 0xf800) << 5) | ((c & 0x07c0) << 2) | (c & 0x001f);
}

// 0x00RRGGBB -> RGB555
static inline uint16_t pack555(uint32_t p) {
  return ((p >> 6) & 0x7c00) | ((p >> 3) & 0x03e0) | (p & 0x001f);
}

// byte-wise maximum: usub8 sets a flag per byte where a >= b, sel picks
static inline uint32_t max8(uint32_t a, uint32_t b) {
  uint32_t r;
  __asm__("usub8 %0, %1, %2\n\tsel %0, %1, %2" : "=&r"(r) : "r"(a), "r"(b) : "cc");
  return r;
}

// Unpacked source rows, cached: an output line needs rows r and r+1, which
// differ in parity, and the next line usually reuses one of them. So each
// source row is unpacked once per frame, into slot r & 1.
static uint32_t unpacked[2][SRC_W];
static int unpacked_row[2];
static uint32_t row_max[SRC_W];     // two source rows combined

static inline const uint32_t *__not_in_flash_func(get_row)(const uint16_t (*scr)[SRC_W], int r) {
  uint32_t *u = unpacked[r & 1];
  if(unpacked_row[r & 1] != r) {
    const uint16_t *s = scr[r];
    for(int x=0;x<SRC_W;x++) u[x] = unpack(s[x]);
    unpacked_row[r & 1] = r;
  }
  return u;
}
#endif

static void __not_in_flash_func(core1_main)(void) {
  dvi_inst->registerIRQThisCore();
  dvi_inst->start();

  while(true) {
    int b = front;                  // latched once per frame
    scanning = b;
    if(border != geo_border) set_geometry(border);
    const int first = geo_border, lines = geo_lines;
    const bool ov = overlay_on;
#ifndef DVI_SCALE_NEAREST
    unpacked_row[0] = unpacked_row[1] = -1;   // new frame, maybe another buffer
#endif

    for(int y=0;y<LINES;y++) {
      dvi::DVI::LineBuffer *lb = dvi_inst->getLineBuffer();
      uint16_t *dst = lb->data();

      if(y < first || y >= first + lines)
        black(dst, LINE_W);
      else {
        int ly = y - first;
        black(dst, geo_x);
#ifdef DVI_SCALE_NEAREST
        const uint16_t *src = screen[b][ya[ly]];
        for(int x=0;x<geo_w;x++) dst[geo_x + x] = to555(src[xa[x]]);
#else
        const uint32_t *m = get_row(screen[b], ya[ly]);
        if(yb[ly] != ya[ly]) {
          const uint32_t *m1 = get_row(screen[b], yb[ly]);
          for(int x=0;x<SRC_W;x++) row_max[x] = max8(m[x], m1[x]);
          m = row_max;
        }
        uint16_t *d = dst + geo_x;
        for(int x=0;x<geo_w;x++) d[x] = pack555(max8(m[xa[x]], m[xb[x]]));
#endif
        black(dst + geo_x + geo_w, LINE_W - geo_x - geo_w);
      }
      if(ov) ov_draw_line(dst, y);

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

// last 2 s window, for the overlay
static uint32_t shown_emu_avg, shown_emu_max, shown_draw_avg, shown_draw_max;
static uint32_t shown_core1, shown_missed;

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

static void ov_ms(int side, int row, uint32_t avg, uint32_t max) {
  ov_set(side, row, OV_VALUE, "%lu.%lu %lu.%lu", avg / 1000, (avg / 100) % 10, max / 1000, (max / 100) % 10);
}

static void overlay_update(void) {
  int r = 0;
  // left: this board and timings
  ov_set(0, r++, OV_LABEL, "GALAGINO");
  ov_set(0, r++, OV_LABEL, "PIZERO");
  r++;
  ov_set(0, r++, OV_LABEL, "VIDEO");
  ov_set(0, r++, OV_VALUE, "%s", half_rate ? "30HZ" : "60HZ");
  ov_set(0, r++, OV_LABEL, "EMU MS");
  ov_ms(0, r++, shown_emu_avg, shown_emu_max);
  ov_set(0, r++, OV_LABEL, "DRAW MS");
  ov_ms(0, r++, shown_draw_avg, shown_draw_max);
  ov_set(0, r++, OV_LABEL, "CORE1 %");
  ov_set(0, r++, OV_VALUE, "%lu", shown_core1);
  ov_set(0, r++, OV_LABEL, "MISSED");
  ov_set(0, r++, shown_missed ? OV_WARN : OV_VALUE, "%lu", shown_missed);
  r++;
  ov_set(0, r++, OV_LABEL, "I2C PAD");
  ov_set(0, r++, pad_connected() ? OV_VALUE : OV_WARN, "%s", pad_connected() ? "OK" : "NONE");
  ov_set(0, r++, OV_LABEL, "BUTTONS");
  ov_set(0, r++, OV_VALUE, "%02X", platform_buttons());
  r++;
  ov_set(0, r++, OV_LABEL, "BORDER");
  ov_set(0, r++, OV_VALUE, "%d", border);

  // right: USB
  const usb_status_t *u = usb_input_status();
  static const char *decoders[] = { "NONE", "GENERIC", "KNOWNPAD", "KEYBOARD" };
  r = 0;
  ov_set(1, r++, OV_LABEL, "USB");
  ov_set(1, r++, OV_LABEL, "DEVICES");
  ov_set(1, r++, u->devices ? OV_VALUE : OV_WARN, "%d", u->devices);
  ov_set(1, r++, OV_LABEL, "HID ITF");
  ov_set(1, r++, OV_VALUE, "%d", u->mounted);
  ov_set(1, r++, OV_LABEL, "VID PID");
  ov_set(1, r++, OV_VALUE, "%04X", u->vid);
  ov_set(1, r++, OV_VALUE, "%04X", u->pid);
  ov_set(1, r++, OV_LABEL, "TYPE");
  ov_set(1, r++, OV_VALUE, "%s", u->proto == 1 ? "KEYBOARD" : u->proto == 2 ? "MOUSE" : "OTHER");
  ov_set(1, r++, OV_LABEL, "DECODER");
  ov_set(1, r++, u->decoder < 0 ? OV_WARN : OV_VALUE, "%s", u->decoder < 0 ? "PARSEERR" : decoders[u->decoder]);
  ov_set(1, r++, OV_LABEL, "REPORTS");
  ov_set(1, r++, OV_VALUE, "%lu", u->reports);
  ov_set(1, r++, OV_LABEL, "LEN");
  ov_set(1, r++, OV_VALUE, "%u", u->len);
  ov_set(1, r++, OV_LABEL, "RAW");
  for(int i=0;i<16;i+=4)
    ov_set(1, r++, OV_VALUE, "%02X%02X%02X%02X", u->raw[i], u->raw[i+1], u->raw[i+2], u->raw[i+3]);
  ov_set(1, r++, OV_LABEL, "USB BTN");
  ov_set(1, r++, OV_VALUE, "%02X", usb_input_buttons());
}

// in the menu: Left toggles the diagnostics, Right cycles the top/bottom
// border (0, 4, 8, 12 lines) for screens that crop the picture's edges
static void menu_keys(void) {
#ifndef SINGLE_MACHINE
  static unsigned char last = 0;
  unsigned char k = platform_buttons();
  if(machine == 0) {                  // MCH_MENU
    if((k & BUTTON_LEFT) && !(last & BUTTON_LEFT)) {
      if(!overlay_on) memset(ov_text, ' ', sizeof(ov_text));
      overlay_on = !overlay_on;
    }
    if((k & BUTTON_RIGHT) && !(last & BUTTON_RIGHT))
      border = (border + 4) % 16;
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

  memcpy(font_ram, font_8x8, sizeof(font_ram));
  memset(ov_text, ' ', sizeof(ov_text));

  pad_init();
  prepare_emulation();       // allocates memory[], resets the CPUs

  dvi_timing = *dvi::getTiming640x480p60Hz();
  dvi_inst = new dvi::DVI(pio0, &dvi_cfg, &dvi_timing);
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
