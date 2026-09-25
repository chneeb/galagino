/*
 * main.c - Galagino on the ClockworkPi PicoCalc (Pac-Man only for now)
 *
 * Core 1 runs the Z80 emulation (emulation.c, unchanged). Core 0 renders
 * the screen in 36 tile rows and streams them to the LCD, polls the
 * keyboard and paces the emulation at the arcade's vblank rate. Audio is
 * synthesised from the Namco WSG registers in a DMA IRQ on core 0.
 *
 * Render and sound code follows galagino.ino (c) 2023 Till Harbaum, GPLv3.
 */

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/sem.h"
#include "hardware/vreg.h"
#include "hardware/clocks.h"

#include "Z80.h"      // also pulls in config.h (ours) and emulation.h

#include "lcd.h"
#include "kbd.h"
#include "audio.h"

#define IO_EMULATION

// shared with the per-game render code; defined in leds.h upstream, which
// can't be included here because it pulls in FastLED
struct sprite_S {
  unsigned char code, color, flags;
  short x, y;
};

unsigned char active_sprites = 0;
struct sprite_S sprite[128];
unsigned short *frame_buffer;

#include "tileaddr.h"
#include "pacman.h"

// two row buffers: one is rendered while the other is sent by DMA
static unsigned short row_buffer[2][224*8];

/* ------------------------------ timing stats ---------------------------- */

static volatile uint32_t emu_us_max = 0, emu_us_sum = 0, emu_frames = 0;
static uint32_t video_us_max = 0, video_us_sum = 0, video_frames = 0;

/* --------------------------- core 0 <-> core 1 -------------------------- */

static semaphore_t vblank_sem;

// called by emulate_frame() on core 1 (via ulTaskNotifyTake in esp32_compat.h)
void galagino_wait_vblank(void) {
  static uint32_t started = 0;
  uint32_t now = time_us_32();
  if(started) {
    uint32_t us = now - started;
    emu_us_sum += us;
    if(us > emu_us_max) emu_us_max = us;
    emu_frames++;
  }
  sem_acquire_blocking(&vblank_sem);
  started = time_us_32();
}

static void core1_main(void) {
  while(1)
    emulate_frame();
}

unsigned char buttons_get(void) {
  return kbd_buttons();
}

/* --------------------------------- video -------------------------------- */

// render one of 36 tile rows (8 x 224 pixel lines) into frame_buffer
static void render_line(short row) {
  memset(frame_buffer, 0, 2*224*8);
  pacman_render_row(row);
}

#ifdef SHOW_TEST_PATTERN
// colour bars in the game window: red, green, blue, white, top to bottom.
// Wrong colours here mean the RGB565 byte order is off.
static void show_test_pattern(void) {
  static const unsigned short bars[] = { 0xf800, 0x07e0, 0x001f, 0xffff };
  for(int b=0;b<4;b++) {
    unsigned short c = bars[b];
    lcd_fill(TFT_X_OFFSET, TFT_Y_OFFSET + b*72, 224, 72, (c >> 8) | (c << 8));
  }
}
#endif

// send rows [first, last) of the screen
static void send_rows(int first, int last) {
  for(int row=first;row<last;row++) {
    frame_buffer = row_buffer[row & 1];
    render_line(row);
    lcd_write(frame_buffer, sizeof(row_buffer[0]));
  }
}

static void update_screen(void) {
  uint32_t t0 = time_us_32();

  pacman_prepare_frame();

  lcd_begin(TFT_X_OFFSET, TFT_Y_OFFSET, 224, 288);
#ifdef VIDEO_HALF_RATE
  // one screen per two emulated frames, split in halves so the emulation
  // still gets its vblank every FRAME_US
  for(int half=0;half<2;half++) {
    send_rows(18*half, 18*(half+1));
    if(half) lcd_end();
    kbd_poll();

    uint32_t elapsed = time_us_32() - t0;
    uint32_t target = (half+1) * FRAME_US;
    if(elapsed < target) sleep_us(target - elapsed);
    sem_release(&vblank_sem);
  }
#else
  send_rows(0, 36);
  lcd_end();
  kbd_poll();
#endif

  uint32_t us = time_us_32() - t0;
  video_us_sum += us;
  if(us > video_us_max) video_us_max = us;
  video_frames++;

#ifndef VIDEO_HALF_RATE
  // deadline is re-based on this frame's start, so lateness never carries over
  if(us < FRAME_US) sleep_us(FRAME_US - us);
  sem_release(&vblank_sem);
#endif
}

static void print_stats(void) {
  static uint32_t last = 0;
  uint32_t now = time_us_32();
  if(now - last < 2000000) return;
  last = now;

  uint32_t ef = emu_frames;
  printf("video: avg %lu max %lu us (%lu frames) | emu: avg %lu max %lu us | budget %u us\n",
         video_frames ? video_us_sum / video_frames : 0, video_us_max, video_frames,
         ef ? emu_us_sum / ef : 0, emu_us_max, FRAME_US);

  video_us_sum = video_us_max = video_frames = 0;
  emu_us_sum = emu_us_max = emu_frames = 0;
}

/* --------------------------------- audio -------------------------------- */

// Namco WSG, as in galagino.ino: three channels, 32-step wavetables
static uint32_t snd_cnt[3], snd_freq[3];
static const signed char *snd_wave[3] = { pacman_wavetable[0], pacman_wavetable[0], pacman_wavetable[0] };
static unsigned char snd_volume[3];

static void audio_namco_waveregs_parse(void) {
  for(int ch=0;ch<3;ch++) {
    snd_volume[ch] = soundregs[ch * 5 + 0x15];

    if(snd_volume[ch]) {
      snd_freq[ch]  = (ch == 0) ? soundregs[0x10] : 0;
      snd_freq[ch] += soundregs[ch * 5 + 0x11] << 4;
      snd_freq[ch] += soundregs[ch * 5 + 0x12] << 8;
      snd_freq[ch] += soundregs[ch * 5 + 0x13] << 12;
      snd_freq[ch] += soundregs[ch * 5 + 0x14] << 16;

      snd_wave[ch] = pacman_wavetable[soundregs[ch * 5 + 0x05] & 0x0f];
    }
  }
}

static void audio_fill(uint16_t *dst, int n) {
  audio_namco_waveregs_parse();

  for(int i=0;i<n;i++) {
    int v = 0;   // at most 3 * 15 * 8 = 360
    for(int ch=0;ch<3;ch++) {
      if(snd_volume[ch]) v += snd_volume[ch] * snd_wave[ch][(snd_cnt[ch] >> 13) & 0x1f];
      snd_cnt[ch] += snd_freq[ch];
    }

    v += AUDIO_RANGE/2;
    if(v < 0) v = 0;
    if(v > AUDIO_RANGE-1) v = AUDIO_RANGE-1;
    dst[i] = v;
  }
}

/* --------------------------------- main --------------------------------- */

int main(void) {
  // voltage first, then clock (as shapones does on the same board)
  vreg_set_voltage(VREG_VOLTAGE_1_30);
  sleep_ms(10);
  set_sys_clock_khz(GALAGINO_SYS_CLK_KHZ, true);

  stdio_init_all();

  lcd_init();
#ifdef SHOW_TEST_PATTERN
  show_test_pattern();
#endif

  kbd_init();

  printf("Galagino PicoCalc: sys %lu Hz\n", clock_get_hz(clk_sys));

#ifdef SHOW_TEST_PATTERN
  sleep_ms(1500);
  lcd_fill(TFT_X_OFFSET, TFT_Y_OFFSET, 224, 288, 0x0000);
#endif

  sem_init(&vblank_sem, 0, 1);
  prepare_emulation();       // allocates memory[], resets the Z80
  audio_init(audio_fill);
  multicore_launch_core1(core1_main);

  while(1) {
    update_screen();
    print_stats();
  }
}
