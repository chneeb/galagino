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

// Starting layout of the 224x288 screen on 640x480 (Right in the game menu
// cycles through them):
//   0 DOUBLE  374x480, correct 4:3 shape, 1 in 6 rows and columns dropped
//   1 WIDE    448x432, all rows and columns, 33% too wide on a 4:3 screen
//   2 ASPECT  336x432, correct shape, all rows, 1 in 4 columns dropped
#define DVI_MODE 0

// Alternate which rows/columns are dropped from frame to frame, so every
// one is shown at least every other frame: thin lines shimmer at 30 Hz
// instead of vanishing. Undefine for a steady picture with gaps.
#define DVI_ALTERNATE_DROP

#endif // _CONFIG_H_
