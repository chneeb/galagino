#ifndef _AUDIO_H_
#define _AUDIO_H_

#include <stdint.h>

#define AUDIO_RATE     24000
#define AUDIO_SAMPLES  128     // per buffer, ~5.3 ms at 24 kHz
#define AUDIO_RANGE    1024    // PWM steps; silence is AUDIO_RANGE/2

// Fills n PWM duty values (0 .. AUDIO_RANGE-1) into dst. Runs in IRQ
// context on the core that called audio_init().
typedef void (*audio_fill_fn)(uint16_t *dst, int n);

void audio_init(audio_fill_fn fill);

#endif // _AUDIO_H_
