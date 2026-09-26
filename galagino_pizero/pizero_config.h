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

// Diagnostics in the black margins beside the game (timings, pad and USB
// state). Left in the game menu toggles them; define this to start with
// them on.
// #define SHOW_OVERLAY

// Black border at the top and bottom, in source lines (0, 4, 8 or 12), for
// screens that crop the picture's edges. The picture shrinks to keep its
// shape. Right in the game menu cycles through the values.
#define DVI_BORDER 0

// The 224x288 screen is shrunk to 187x240 by dropping every 6th row and
// column, so 1 pixel lines can vanish. DVI_SCALE_MAX combines them instead
// (per channel max of the pixels each output pixel covers), but is parked:
// too slow for core 1, it causes red flicker (missed lines).
// #define DVI_SCALE_MAX

// Alternate which rows and columns are dropped from frame to frame, so
// every one is shown at least every other frame: thin lines shimmer at
// 30 Hz instead of vanishing. Costs core 1 nothing. Undefine for a steady
// picture where some 1 pixel lines are missing.
#define DVI_ALTERNATE_DROP

#endif // _CONFIG_H_
