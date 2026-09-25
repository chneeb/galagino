/*
 * esp32_compat.h - Arduino/FreeRTOS calls used by ../galagino/emulation.c
 *
 * Force-included into emulation.c only.
 */
#ifndef _ESP32_COMPAT_H_
#define _ESP32_COMPAT_H_

#include <stdlib.h>
#include "pico/time.h"
#include "pico/rand.h"

static inline unsigned long micros(void) { return time_us_32(); }
static inline unsigned long millis(void) { return to_ms_since_boot(get_absolute_time()); }
#define esp_random()  get_rand_32()

// emulate_frame() blocks on the video task's notification once per frame.
// While a game boots it sleeps 1 ms instead, which galagino_idle() also
// uses to restart the emulation timing stats.
void galagino_wait_vblank(void);
void galagino_idle(unsigned ms);
#define ulTaskNotifyTake(clear, timeout)  galagino_wait_vblank()
#define vTaskDelay(ms)                    galagino_idle(ms)

#endif // _ESP32_COMPAT_H_
