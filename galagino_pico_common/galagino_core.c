/*
 * galagino_core.c - board-independent Galagino code for the RP2350 ports
 *
 * Game menu, per-machine rendering, input handling and sound synthesis,
 * shared by galagino_picocalc and galagino_pizero. Each board supplies the
 * platform_* functions declared in galagino_core.h.
 *
 * Ported from galagino.ino (c) 2023 Till Harbaum, GPLv3. ESP32-only parts
 * (I2S, the APLL workaround, FreeRTOS) are replaced.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "pico/stdlib.h"
#include "hardware/watchdog.h"

#include "Z80.h"      // also pulls in config.h (the board's) and emulation.h
#include "leds.h"     // struct sprite_S and no-op LED stubs, as LED_PIN is not defined

#include "galagino_core.h"

#define IO_EMULATION

// the hardware supports 64 sprites
unsigned char active_sprites = 0;
struct sprite_S sprite[128];

// the row currently being rendered: 8 lines of 224 pixels
unsigned short *frame_buffer;

#include "tileaddr.h"

#ifdef ENABLE_PACMAN
#include "pacman.h"
#endif
#ifdef ENABLE_GALAGA
#include "galaga.h"
#endif
#ifdef ENABLE_DKONG
#include "dkong.h"
#endif
#ifdef ENABLE_FROGGER
#include "frogger.h"
#endif
#ifdef ENABLE_DIGDUG
#include "digdug.h"
#endif
#ifdef ENABLE_1942
#include "1942.h"
#endif

#ifndef SINGLE_MACHINE
signed char machine = MCH_MENU;   // start with menu
#endif

// one method to return to the main menu is to reset the entire machine
void hw_reset(void) {
  watchdog_reboot(0, 0, 0);
  while(1) ;
}

/* ---------------------------------- menu -------------------------------- */

#ifndef SINGLE_MACHINE
// convert rgb565 big endian color to greyscale
static unsigned short greyscale(unsigned short in) {
  unsigned short r = (in>>3) & 31;
  unsigned short g = ((in<<3) & 0x38) | ((in>>13)&0x07);
  unsigned short b = (in>>8)& 31;
  unsigned short avg = (2*r + g + 2*b)/4;

  return (((avg << 13) & 0xe000) |   // g2-g0
          ((avg <<  7) & 0x1f00) |   // b5-b0
          ((avg <<  2) & 0x00f8) |   // r5-r0
          ((avg >>  3) & 0x0007));   // g5-g3
}

// render one of the menu logos. Only the active one is colorful.
// render logo into current buffer starting with line "row" of the logo
static void render_logo(short row, const unsigned short *logo, char active) {
  unsigned short marker = logo[0];
  const unsigned short *data = logo+1;

  // current pixel to be drawn
  unsigned short ipix = 0;

  // less than 8 rows in image left?
  unsigned short pix2draw = ((row <= 96-8)?(224*8):((96-row)*224));

  if(row >= 0) {
    // skip ahead to row
    unsigned short col = 0;
    unsigned short pix = 0;
    while(pix < 224*row) {
      if(data[0] != marker) {
        pix++;
        data++;
      } else {
        pix += data[1]+1;
        col = data[2];
        data += 3;
      }
    }

    // draw pixels remaining from previous run
    if(!active) col = greyscale(col);
    while(ipix < ((pix - 224*row < pix2draw)?(pix - 224*row):pix2draw))
      frame_buffer[ipix++] = col;
  } else
    // if row is negative, then skip target pixel
    ipix -= row * 224;

  while(ipix < pix2draw) {
    if(data[0] != marker)
      frame_buffer[ipix++] = active?*data++:greyscale(*data++);
    else {
      unsigned short color = data[2];
      if(!active) color = greyscale(color);
      for(unsigned short j=0;j<data[1]+1 && ipix < pix2draw;j++)
        frame_buffer[ipix++] = color;

      data += 3;
    }
  }
}

static const unsigned short *logos[] = {
#ifdef ENABLE_PACMAN
  pacman_logo,
#endif
#ifdef ENABLE_GALAGA
  galaga_logo,
#endif
#ifdef ENABLE_DKONG
  dkong_logo,
#endif
#ifdef ENABLE_FROGGER
  frogger_logo,
#endif
#ifdef ENABLE_DIGDUG
  digdug_logo,
#endif
#ifdef ENABLE_1942
  _1942_logo,
#endif
};

static void render_menu_row(short row) {
  if(MACHINES <= 3) {
    // non-scrolling menu for 2 or 3 machines
    for(int i=0;i<(int)(sizeof(logos)/sizeof(unsigned short*));i++) {
      int offset = i*12;
      if(sizeof(logos)/sizeof(unsigned short*) == 2) offset += 6;

      if(row >= offset && row < offset+12)
        render_logo(8*(row-offset), logos[i], menu_sel == i+1);
    }
  } else {
    // scrolling menu for more than 3 machines

    // valid offset values range from 0 to MACHINE*96-1
    static int offset = 0;

    // check which logo would show up in this row. Actually
    // two may show up in the same character row when scrolling
    int logo_idx = ((row + offset/8) / 12)%MACHINES;
    if(logo_idx < 0) logo_idx += MACHINES;

    int logo_y = (row * 8 + offset)%96;  // logo line in this row

    // check if logo at logo_y shows up in current row
    render_logo(logo_y, logos[logo_idx], (menu_sel-1) == logo_idx);

    // check if a second logo may show up here
    if(logo_y > (96-8)) {
      logo_idx = (logo_idx + 1)%MACHINES;
      logo_y -= 96;
      render_logo(logo_y, logos[logo_idx], (menu_sel-1) == logo_idx);
    }

    if(row == 35) {
      // finally offset is bound to game, something like 96*game:
      int new_offset = 96*((unsigned)(menu_sel-2)%MACHINES);
      if(menu_sel == 1) new_offset = (MACHINES-1)*96;

      // check if we need to scroll
      if(new_offset != offset) {
        int diff = (new_offset - offset) % (MACHINES*96);
        if(diff < 0) diff += MACHINES*96;

        if(diff < MACHINES*96/2) offset = (offset+8)%(MACHINES*96);
        else                     offset = (offset-8)%(MACHINES*96);
        if(offset < 0) offset += MACHINES*96;
      }
    }
  }
}
#endif

/* --------------------------------- input -------------------------------- */

unsigned char buttons_get(void) {
  unsigned char keys = platform_buttons();

#ifndef SINGLE_MACHINE
  static unsigned long reset_timer = 0;

  // return to the menu if Esc is held for more than one second
  if(keys & BUTTON_EXTRA) {
    if(machine != MCH_MENU) {
#ifdef MASTER_ATTRACT_MENU_TIMEOUT
      // a game started by the master attract mode keeps running
      // as long as the user wants
      master_attract_timeout = 0;
#endif
      unsigned long now = to_ms_since_boot(get_absolute_time());
      if(!reset_timer)
        reset_timer = now;

      if(now - reset_timer > 1000)
        emulation_reset();
    }
  } else
    reset_timer = 0;
#endif

  return keys & ~BUTTON_EXTRA;
}

/* --------------------------------- video -------------------------------- */

// render one of 36 tile rows (8 x 224 pixel lines) into frame_buffer
void core_render_line(short row) {
  // the upper screen half of frogger has a blue background
  // using 8 in fact adds a tiny fraction of red as well. But that does not hurt
  memset(frame_buffer,
#ifdef ENABLE_FROGGER
    (MACHINE_IS_FROGGER && row <= 17)?8:
#endif
    0, 2*224*8);

#ifndef SINGLE_MACHINE
  if(machine == MCH_MENU)
    render_menu_row(row);
  else
#endif

#ifdef ENABLE_PACMAN
PACMAN_BEGIN
  pacman_render_row(row);
PACMAN_END
#endif

#ifdef ENABLE_GALAGA
GALAGA_BEGIN
  galaga_render_row(row);
GALAGA_END
#endif

#ifdef ENABLE_DKONG
DKONG_BEGIN
  dkong_render_row(row);
DKONG_END
#endif

#ifdef ENABLE_FROGGER
FROGGER_BEGIN
  frogger_render_row(row);
FROGGER_END
#endif

#ifdef ENABLE_DIGDUG
DIGDUG_BEGIN
  digdug_render_row(row);
DIGDUG_END
#endif

#ifdef ENABLE_1942
_1942_BEGIN
  _1942_render_row(row);
_1942_END
#endif
  ;
}

void core_prepare_frame(void) {
#ifndef SINGLE_MACHINE
  if(machine == MCH_MENU) return;
#endif

#ifdef ENABLE_PACMAN
PACMAN_BEGIN
  pacman_prepare_frame();
PACMAN_END
#endif

#ifdef ENABLE_GALAGA
GALAGA_BEGIN
  galaga_prepare_frame();
GALAGA_END
#endif

#ifdef ENABLE_DKONG
DKONG_BEGIN
  dkong_prepare_frame();
DKONG_END
#endif

#ifdef ENABLE_FROGGER
FROGGER_BEGIN
  frogger_prepare_frame();
FROGGER_END
#endif

#ifdef ENABLE_DIGDUG
DIGDUG_BEGIN
  digdug_prepare_frame();
DIGDUG_END
#endif

#ifdef ENABLE_1942
_1942_BEGIN
  _1942_prepare_frame();
_1942_END
#endif
  ;
}

void core_frame_done(bool half_rate) {
#ifdef ENABLE_GALAGA
  // stars scroll per screen update, so twice as far at 30 Hz
  static const signed char speeds[8] = { -1, -2, -3, 0, 3, 2, 1, 0 };
  stars_scroll_y += (half_rate ? 2 : 1) * speeds[starcontrol & 7];
#endif
}

/* --------------------------------- audio -------------------------------- */

#ifdef ENABLE_GALAGA
// the ship explosion sound is stored as a digi sample.
// All other sounds are generated on the fly via the
// original wave tables
static unsigned short snd_boom_cnt = 0;
static const signed char *snd_boom_ptr = NULL;

void galaga_trigger_sound_explosion(void) {
  if(game_started) {
    snd_boom_ptr = (const signed char*)galaga_sample_boom;
    snd_boom_cnt = 2*sizeof(galaga_sample_boom);
  }
}
#endif

#ifdef ENABLE_DKONG
static unsigned short dkong_sample_cnt[3] = { 0,0,0 };
static const signed char *dkong_sample_ptr[3];

void dkong_trigger_sound(char snd) {
  static const struct {
    const signed char *data;
    const unsigned short length;
  } samples[] = {
    { (const signed char *)dkong_sample_walk0, sizeof(dkong_sample_walk0) },
    { (const signed char *)dkong_sample_walk1, sizeof(dkong_sample_walk1) },
    { (const signed char *)dkong_sample_walk2, sizeof(dkong_sample_walk2) },
    { (const signed char *)dkong_sample_jump,  sizeof(dkong_sample_jump)  },
    { (const signed char *)dkong_sample_stomp, sizeof(dkong_sample_stomp) }
  };

  // samples 0 = walk, 1 = jump, 2 = stomp

  if(!snd) {
    // walk0, walk1 and walk2 are variants
    char rnd = random() % 3;
    dkong_sample_cnt[0] = samples[rnd].length;
    dkong_sample_ptr[0] = samples[rnd].data;
  } else {
    dkong_sample_cnt[snd] = samples[snd+2].length;
    dkong_sample_ptr[snd] = samples[snd+2].data;
  }
}

void audio_dkong_bitrate(char is_dkong) {
  // The audio CPU of donkey kong runs at 6Mhz. A full bus
  // cycle needs 15 clocks which results in 400k cycles
  // per second. The sound CPU typically needs 34 instruction
  // cycles to write an updated audio value to the external
  // DAC connected to port 0.

  // The effective sample rate thus is 6M/15/34 = 11764.7 Hz
  platform_audio_set_rate(is_dkong ? CORE_AUDIO_RATE_DKONG : CORE_AUDIO_RATE);
}
#endif

#if defined(ENABLE_FROGGER) || defined(ENABLE_1942)
static int ay_period[2][4] = {{0,0,0,0}, {0,0,0,0}};
static int ay_volume[2][3] = {{0,0,0}, {0,0,0}};
static int ay_enable[2][3] = {{0,0,0}, {0,0,0}};
static int audio_cnt[2][4], audio_toggle[2][4] = {{1,1,1,1},{1,1,1,1}};
static unsigned long ay_noise_rng[2] = { 1, 1 };
#endif

#if defined(ENABLE_PACMAN) || defined(ENABLE_GALAGA) || defined(ENABLE_DIGDUG)
#define USE_NAMCO_WAVETABLE
static unsigned long snd_cnt[3] = { 0,0,0 };
static unsigned long snd_freq[3];
static const signed char *snd_wave[3];
static unsigned char snd_volume[3];

static void audio_namco_waveregs_parse(void) {
#ifndef SINGLE_MACHINE
  if(
#ifdef ENABLE_PACMAN
    MACHINE_IS_PACMAN ||
#endif
#ifdef ENABLE_GALAGA
    MACHINE_IS_GALAGA ||
#endif
#ifdef ENABLE_DIGDUG
    MACHINE_IS_DIGDUG ||
#endif
  0)
#endif
  {
    // parse all three wsg channels
    for(char ch=0;ch<3;ch++) {
      // channel volume
      snd_volume[ch] = soundregs[ch * 5 + 0x15];

      if(snd_volume[ch]) {
        // frequency
        snd_freq[ch] = (ch == 0) ? soundregs[0x10] : 0;
        snd_freq[ch] += soundregs[ch * 5 + 0x11] << 4;
        snd_freq[ch] += soundregs[ch * 5 + 0x12] << 8;
        snd_freq[ch] += soundregs[ch * 5 + 0x13] << 12;
        snd_freq[ch] += soundregs[ch * 5 + 0x14] << 16;

        // wavetable entry
#ifdef ENABLE_PACMAN
  #if defined(ENABLE_GALAGA) || defined(ENABLE_DIGDUG)  // there's at least a second machine
        if(machine == MCH_PACMAN)
  #endif
          snd_wave[ch] = pacman_wavetable[soundregs[ch * 5 + 0x05] & 0x0f];
  #if defined(ENABLE_GALAGA) || defined(ENABLE_DIGDUG)
        else
  #endif
#endif
#ifdef ENABLE_GALAGA
  #ifdef ENABLE_DIGDUG
        if(machine == MCH_GALAGA)
  #endif
          snd_wave[ch] = galaga_wavetable[soundregs[ch * 5 + 0x05] & 0x07];
  #ifdef ENABLE_DIGDUG
        else
  #endif
#endif
#ifdef ENABLE_DIGDUG
          snd_wave[ch] = digdug_wavetable[soundregs[ch * 5 + 0x05] & 0x0f];
#endif
      }
    }
  }
}
#endif

// render 64 signed samples, following galagino.ino's snd_render_buffer()
static void snd_render_buffer(int16_t *dst) {
#if defined(ENABLE_FROGGER) || defined(ENABLE_1942)
  #ifndef ENABLE_1942        // only frogger
    #define AY        1      // frogger has one AY
    #define AY_INC    9      // and it runs at 1.78 MHz -> 223718/24000 = 9,32
    #define AY_VOL   11      // min/max = -/+ 3*15*11 = -/+ 495
  #else
    #ifndef ENABLE_FROGGER   // only 1942
      #define AY      2      // 1942 has two AYs
      #define AY_INC  8      // and they runs at 1.5 MHz -> 187500/24000 = 7,81
      #define AY_VOL  4      // min/max = -/+ 6*15*4 = -/+ 360 (upstream fix)
    #else
      // both enabled
      #define AY ((machine == MCH_FROGGER)?1:2)
      #define AY_INC ((machine == MCH_FROGGER)?9:8)
      #define AY_VOL ((machine == MCH_FROGGER)?11:4)
    #endif
  #endif

  if(
#ifdef ENABLE_FROGGER
     MACHINE_IS_FROGGER ||
#endif
#ifdef ENABLE_1942
     MACHINE_IS_1942 ||
#endif
     0) {

    // up to two AY's
    for(char ay=0;ay<AY;ay++) {
      int ay_off = 16*ay;

      // three tone channels
      for(char c=0;c<3;c++) {
        ay_period[ay][c] = soundregs[ay_off+2*c] + 256 * (soundregs[ay_off+2*c+1] & 15);
        ay_enable[ay][c] = (((soundregs[ay_off+7] >> c)&1) | ((soundregs[ay_off+7] >> (c+2))&2))^3;
        ay_volume[ay][c] = soundregs[ay_off+8+c] & 0x0f;
      }
      // noise channel
      ay_period[ay][3] = soundregs[ay_off+6] & 0x1f;
    }
  }
#endif

  for(int i=0;i<64;i++) {
    int v = 0;

#ifdef USE_NAMCO_WAVETABLE
  #ifndef SINGLE_MACHINE
    if(0
    #ifdef ENABLE_PACMAN
        || (machine == MCH_PACMAN)
    #endif
    #ifdef ENABLE_GALAGA
        || (machine == MCH_GALAGA)
    #endif
    #ifdef ENABLE_DIGDUG
        || (machine == MCH_DIGDUG)
    #endif
    )
  #endif
    {
      // add up to three wave signals
      if(snd_volume[0]) v += snd_volume[0] * snd_wave[0][(snd_cnt[0]>>13) & 0x1f];
      if(snd_volume[1]) v += snd_volume[1] * snd_wave[1][(snd_cnt[1]>>13) & 0x1f];
      if(snd_volume[2]) v += snd_volume[2] * snd_wave[2][(snd_cnt[2]>>13) & 0x1f];

  #ifdef ENABLE_GALAGA
      if(snd_boom_cnt) {
        v += *snd_boom_ptr;
        if(snd_boom_cnt & 1) snd_boom_ptr++;
        snd_boom_cnt--;
      }
  #endif
    }
#endif

#ifdef ENABLE_DKONG
DKONG_BEGIN
    {
      v = 0;  // silence

      // copy data from dkong buffer if one is available
      // 8048 sounds gets 50% of the available volume range
      if(dkong_audio_rptr != dkong_audio_wptr)
        v = dkong_audio_transfer_buffer[dkong_audio_rptr][i];

      // include sample sounds
      // walk is 6.25% volume, jump is at 12.5% volume and, stomp is at 25%
      for(char j=0;j<3;j++) {
        if(dkong_sample_cnt[j]) {
          v += *dkong_sample_ptr[j]++ >> (2-j);
          dkong_sample_cnt[j]--;
        }
      }
    }
DKONG_END
#endif

#if defined(ENABLE_FROGGER) || defined(ENABLE_1942)
    if(
#ifdef ENABLE_FROGGER
     MACHINE_IS_FROGGER ||
#endif
#ifdef ENABLE_1942
     MACHINE_IS_1942 ||
#endif
     0) {
      v = 0;  // silence

      for(char ay=0;ay<AY;ay++) {

        // frogger can acually skip the noise generator as
        // it doesn't use it
        if(ay_period[ay][3]) {
          // process noise generator
          audio_cnt[ay][3] += AY_INC; // for 24 khz
          if(audio_cnt[ay][3] > ay_period[ay][3]) {
            audio_cnt[ay][3] -=  ay_period[ay][3];
            // progress rng
            ay_noise_rng[ay] ^= (((ay_noise_rng[ay] & 1) ^ ((ay_noise_rng[ay] >> 3) & 1)) << 17);
            ay_noise_rng[ay] >>= 1;
          }
        }

        for(char c=0;c<3;c++) {
          // a channel is on if period != 0, vol != 0 and tone bit == 0
          if(ay_period[ay][c] && ay_volume[ay][c] && ay_enable[ay][c]) {
            short bit = 1;
            if(ay_enable[ay][c] & 1) bit &= (audio_toggle[ay][c]>0)?1:0;  // tone
            if(ay_enable[ay][c] & 2) bit &= (ay_noise_rng[ay]&1)?1:0;     // noise

            if(bit == 0) bit = -1;
            v += AY_VOL * bit * ay_volume[ay][c];

            audio_cnt[ay][c] += AY_INC; // for 24 khz
            if(audio_cnt[ay][c] > ay_period[ay][c]) {
              audio_cnt[ay][c] -= ay_period[ay][c];
              audio_toggle[ay][c] = -audio_toggle[ay][c];
            }
          }
        }
      }
    }
#endif

    // v is now in the range of +/- 512 (upstream scales it to 16 bit here)
    dst[i] = v;

#ifdef USE_NAMCO_WAVETABLE
    snd_cnt[0] += snd_freq[0];
    snd_cnt[1] += snd_freq[1];
    snd_cnt[2] += snd_freq[2];
#endif
  }

#ifdef ENABLE_DKONG
  #ifndef SINGLE_MACHINE
  if(machine == MCH_DKONG)
  #endif
  {
    // advance read pointer. The buffer is a ring
    if(dkong_audio_rptr != dkong_audio_wptr)
      dkong_audio_rptr = (dkong_audio_rptr+1)&DKONG_AUDIO_QUEUE_MASK;
  }
#endif
}

void core_audio_render(int16_t *dst, int n) {
  // in chunks of 64 samples, the size of one dkong transfer buffer
  for(int i=0;i<n;i+=64) {
#ifdef USE_NAMCO_WAVETABLE
    audio_namco_waveregs_parse();
#endif
    snd_render_buffer(dst + i);
  }
}
