/*
 * pizero_config.h - build configuration for the RP2350-PiZero (DVI) target
 *
 * Force-included into every translation unit. Defining _CONFIG_H_ here
 * turns ../galagino/config.h (the CYD configuration) into a no-op.
 */
#ifndef _CONFIG_H_
#define _CONFIG_H_

// machine selection, shared with the other RP2350 ports
#include "machines_config.h"

// Define to always use 30 Hz video (emulation stays at 60 Hz). Otherwise
// a game session switches to 30 Hz once 60 Hz can't keep up (Digdug).
// #define VIDEO_HALF_RATE

// Diagnostics in the black margins beside the game: timings, I2C pad and
// USB device state (IDs, raw reports), so no serial console is needed.
#define SHOW_OVERLAY

#endif // _CONFIG_H_
