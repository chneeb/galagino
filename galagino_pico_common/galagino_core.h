/*
 * galagino_core.h - board-independent Galagino code for the RP2350 ports
 */
#ifndef _GALAGINO_CORE_H_
#define _GALAGINO_CORE_H_

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// sample rate core_audio_render() produces; Donkey Kong switches it
#define CORE_AUDIO_RATE        24000
#define CORE_AUDIO_RATE_DKONG  11765

// where core_render_line() draws: 8 lines of 224 pixels, big endian RGB565
extern unsigned short *frame_buffer;

// once per screen update: sprite preparation for the running machine
void core_prepare_frame(void);
// render one of the 36 tile rows of the 224x288 screen into frame_buffer
void core_render_line(short row);
// after each screen update; half_rate = video runs at 30 Hz
void core_frame_done(bool half_rate);
// render n (a multiple of 64) signed samples, roughly +/- 512
void core_audio_render(int16_t *dst, int n);

// provided by the board
unsigned char platform_buttons(void);         // BUTTON_* bits, EXTRA = back to menu
void platform_audio_set_rate(unsigned rate);  // called when Donkey Kong starts

#ifdef __cplusplus
}
#endif

#endif // _GALAGINO_CORE_H_
