/*
 * main.c - Galagino on the ClockworkPi PicoCalc
 *
 * Core 1 runs the CPU emulation (emulation.c, unchanged). Core 0 renders
 * the screen in 36 tile rows and streams them to the LCD, polls the
 * keyboard and paces the emulation at 60 Hz. Audio is synthesised in a
 * DMA IRQ on core 0.
 *
 * Menu, rendering and sound are shared with the other RP2350 ports in
 * ../galagino_pico_common/galagino_core.c.
 */

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/sem.h"
#include "hardware/vreg.h"
#include "hardware/clocks.h"

#include "Z80.h"      // also pulls in config.h (ours) and emulation.h
#include "galagino_core.h"

#include "lcd.h"
#include "kbd.h"
#include "audio.h"

// two row buffers: one is rendered while the other is sent by DMA
static unsigned short row_buffer[2][224*8];

/* ------------------------------ timing stats ---------------------------- */

static volatile uint32_t emu_us_max = 0, emu_us_sum = 0, emu_frames = 0;
static uint32_t video_us_max = 0, video_us_sum = 0, video_frames = 0;
static uint32_t row_us_max = 0;
// CPU time spent drawing a frame (sprite prep + all 36 rows), without
// waiting for the LCD. It hides behind the transfer here, but a DVI port
// would have to spend it on the same core as the emulation.
static uint32_t draw_us_frame = 0, draw_us_max = 0, draw_us_sum = 0;

/* --------------------------- core 0 <-> core 1 -------------------------- */

static semaphore_t vblank_sem;

// start of the current emulated frame on core 1, 0 = not measuring
static uint32_t emu_started = 0;

// called by emulate_frame() on core 1 (via ulTaskNotifyTake in esp32_compat.h)
void galagino_wait_vblank(void) {
  uint32_t started = emu_started;
  uint32_t now = time_us_32();
  if(started) {
    uint32_t us = now - started;
    emu_us_sum += us;
    if(us > emu_us_max) emu_us_max = us;
    emu_frames++;
  }
  sem_acquire_blocking(&vblank_sem);
  emu_started = time_us_32();
}

// called by emulate_frame() instead while the game is still booting
void galagino_idle(unsigned ms) {
  sleep_ms(ms);
  emu_started = 0;
}

static void core1_main(void) {
  while(1)
    emulate_frame();
}


/* ----------------------------- board glue ------------------------------- */

unsigned char platform_buttons(void) {
  return kbd_buttons();
}

void platform_audio_set_rate(unsigned rate) {
  audio_set_rate(rate);
}

// PWM duty values from the core's signed samples
static void audio_fill(uint16_t *dst, int n) {
  int16_t tmp[AUDIO_SAMPLES];
  core_audio_render(tmp, n);
  for(int i=0;i<n;i++) {
    int v = tmp[i] + AUDIO_RANGE/2;
    if(v < 0) v = 0;
    if(v > AUDIO_RANGE-1) v = AUDIO_RANGE-1;
    dst[i] = v;
  }
}

/* --------------------------------- video -------------------------------- */

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
    uint32_t t = time_us_32();
    frame_buffer = row_buffer[row & 1];
    core_render_line(row);
    t = time_us_32() - t;
    if(t > row_us_max) row_us_max = t;
    draw_us_frame += t;
    lcd_write(frame_buffer, sizeof(row_buffer[0]));
  }
}

// 30 Hz video while still emulating at 60 Hz, like the CYD. Switched on
// for the rest of a game session when 60 Hz video can't keep up.
#ifdef VIDEO_HALF_RATE
static bool half_rate = true;
#else
static bool half_rate = false;
static int late_frames = 0;
#endif

static void update_screen(void) {
  uint32_t t0 = time_us_32();

  core_prepare_frame();
  draw_us_frame = time_us_32() - t0;

  lcd_begin(TFT_X_OFFSET, TFT_Y_OFFSET, 224, 288);
  if(half_rate) {
    // split in halves so the emulation still gets its vblank every FRAME_US
    for(int half=0;half<2;half++) {
      send_rows(18*half, 18*(half+1));
      if(half) lcd_end();
      kbd_poll();

      uint32_t elapsed = time_us_32() - t0;
      uint32_t target = (half+1) * FRAME_US;
      if(elapsed < target) sleep_us(target - elapsed);
      sem_release(&vblank_sem);
    }
  } else {
    send_rows(0, 36);
    lcd_end();
    kbd_poll();
  }

  uint32_t us = time_us_32() - t0;
  video_us_sum += us;
  if(us > video_us_max) video_us_max = us;
  video_frames++;

  draw_us_sum += draw_us_frame;
  if(draw_us_frame > draw_us_max) draw_us_max = draw_us_frame;

  if(!half_rate) {
    // deadline is re-based on this frame's start, so lateness never carries over
    if(us < FRAME_US) sleep_us(FRAME_US - us);
    sem_release(&vblank_sem);

#ifndef VIDEO_HALF_RATE
    // more than 10 late frames in a row: drop to 30 Hz video for this game
    late_frames = (us > FRAME_US) ? late_frames + 1 : 0;
    if(late_frames > 10
#ifndef SINGLE_MACHINE
       && machine != MCH_MENU
#endif
      ) {
      printf("video can't keep up (%lu us), switching to 30 Hz\n", us);
      half_rate = true;
    }
#endif
  }

  core_frame_done(half_rate);
}

static void print_stats(void) {
  static uint32_t last = 0;
  uint32_t now = time_us_32();
  if(now - last < 2000000) return;
  last = now;

  uint32_t ef = emu_frames;
  printf("machine %d: video %s avg %lu max %lu us, row max %lu us | draw avg %lu max %lu us | emu avg %lu max %lu us | budget %u us\n",
#ifndef SINGLE_MACHINE
         machine,
#else
         1,
#endif
         half_rate ? "30Hz" : "60Hz",
         video_frames ? video_us_sum / video_frames : 0, video_us_max, row_us_max,
         video_frames ? draw_us_sum / video_frames : 0, draw_us_max,
         ef ? emu_us_sum / ef : 0, emu_us_max, FRAME_US);

  video_us_sum = video_us_max = video_frames = row_us_max = 0;
  draw_us_sum = draw_us_max = 0;
  emu_us_sum = emu_us_max = emu_frames = 0;
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
  prepare_emulation();       // allocates memory[], resets the CPUs

  audio_init(audio_fill);
#if defined(SINGLE_MACHINE) && defined(ENABLE_DKONG)
  // only dkong installed? Then set up its rate immediately
  audio_dkong_bitrate(true);
#endif

  multicore_launch_core1(core1_main);

  while(1) {
    update_screen();
    print_stats();
  }
}
