#ifndef _LCD_H_
#define _LCD_H_

#include <stdint.h>

// set up PIO SPI and initialise the panel; call after the system clock is set
void lcd_init(void);

// fill a rectangle with one colour (big endian RGB565 as stored in memory)
void lcd_fill(int x, int y, int w, int h, uint16_t color);

// Streaming frame write: lcd_begin() opens the window, each lcd_write()
// queues one buffer via DMA (waiting for the previous one first), and
// lcd_end() waits for the last byte to leave. The buffer passed to
// lcd_write() must stay untouched until the next lcd_write() or lcd_end().
void lcd_begin(int x, int y, int w, int h);
void lcd_write(const void *buf, uint32_t len);
void lcd_end(void);

#endif // _LCD_H_
