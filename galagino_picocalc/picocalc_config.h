/*
 * picocalc_config.h - build configuration for the PicoCalc target
 *
 * Force-included into every translation unit. Defining _CONFIG_H_ here
 * turns ../galagino/config.h (the CYD configuration) into a no-op.
 */
#ifndef _CONFIG_H_
#define _CONFIG_H_

// Pac-Man only for now
#define ENABLE_PACMAN
#define SINGLE_MACHINE

#include "dip_switches.h"

// Clocks: 300 MHz at 1.30 V is proven on the PicoCalc by shapones
#define GALAGINO_SYS_CLK_KHZ   300000

// 224x288 arcade screen centred in the 320x320 panel
#define LCD_WIDTH     320
#define LCD_HEIGHT    320
#define TFT_X_OFFSET  ((LCD_WIDTH - 224) / 2)
#define TFT_Y_OFFSET  ((LCD_HEIGHT - 288) / 2)

// LCD SPI clock. The PIO divider must come out as an exact integer:
// 300 MHz / 2 / 75 MHz = 2.0
#define LCD_SPI_HZ    75000000

// Pac-Man's vblank runs at 60.61 Hz
#define FRAME_US      16500

// Define to update the LCD at 30 Hz while emulating at 60 Hz, like the CYD.
// #define VIDEO_HALF_RATE

// Show colour bars for a moment at boot to check LCD byte order
#define SHOW_TEST_PATTERN

#endif // _CONFIG_H_
