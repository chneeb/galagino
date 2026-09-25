/*
 * lcd.c - PicoCalc 320x320 LCD over PIO SPI + DMA
 *
 * Pinout, panel init sequence and the PIO approach follow the PicoCalc
 * driver in shapones (MIT), which runs this panel at 75 MHz in RGB565.
 */

#include "lcd.h"

#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/clocks.h"

#include "lcd.pio.h"

#define PIN_SCK   10
#define PIN_MOSI  11
#define PIN_CS    13
#define PIN_DC    14
#define PIN_RST   15

static PIO pio = pio0;
static uint sm;
static int dma_ch;
static bool dma_active;

static void pio_put(uint8_t b) {
  while(pio_sm_is_tx_fifo_full(pio, sm)) ;
  // narrow writes are replicated across the word, so the byte lands in
  // the MSBs the left-shifting SM sends first
  *(volatile uint8_t *)&pio->txf[sm] = b;
}

// wait until the FIFO is empty and the last bit has been shifted out
static void pio_wait_idle(void) {
  uint32_t mask = 1u << (PIO_FDEBUG_TXSTALL_LSB + sm);
  pio->fdebug = mask;
  while(!(pio->fdebug & mask)) ;
}

static void command(uint8_t cmd, const uint8_t *data, int len) {
  pio_wait_idle();
  gpio_put(PIN_DC, 0);
  gpio_put(PIN_CS, 0);
  pio_put(cmd);
  pio_wait_idle();
  gpio_put(PIN_DC, 1);
  for(int i=0;i<len;i++)
    pio_put(data[i]);
  pio_wait_idle();
  gpio_put(PIN_CS, 1);
}

#define CMD(c, ...) do { \
    static const uint8_t d[] = { __VA_ARGS__ }; \
    command(c, d, sizeof(d)); \
  } while(0)

static void set_window(int x, int y, int w, int h) {
  int x1 = x + w - 1, y1 = y + h - 1;
  uint8_t xa[] = { x >> 8, x & 0xff, x1 >> 8, x1 & 0xff };
  uint8_t ya[] = { y >> 8, y & 0xff, y1 >> 8, y1 & 0xff };
  command(0x2a, xa, 4);
  command(0x2b, ya, 4);
}

void lcd_init(void) {
  gpio_init(PIN_CS);  gpio_set_dir(PIN_CS, GPIO_OUT);  gpio_put(PIN_CS, 1);
  gpio_init(PIN_DC);  gpio_set_dir(PIN_DC, GPIO_OUT);  gpio_put(PIN_DC, 1);
  gpio_init(PIN_RST); gpio_set_dir(PIN_RST, GPIO_OUT); gpio_put(PIN_RST, 1);

  sm = pio_claim_unused_sm(pio, true);
  uint offset = pio_add_program(pio, &lcd_spi_program);
  pio_gpio_init(pio, PIN_SCK);
  pio_gpio_init(pio, PIN_MOSI);
  pio_sm_set_consecutive_pindirs(pio, sm, PIN_SCK, 1, true);
  pio_sm_set_consecutive_pindirs(pio, sm, PIN_MOSI, 1, true);

  pio_sm_config c = lcd_spi_program_get_default_config(offset);
  sm_config_set_out_pins(&c, PIN_MOSI, 1);
  sm_config_set_sideset_pins(&c, PIN_SCK);
  sm_config_set_out_shift(&c, false, true, 8);
  sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);
  sm_config_set_clkdiv(&c, (float)clock_get_hz(clk_sys) / (2.0f * LCD_SPI_HZ));
  pio_sm_init(pio, sm, offset, &c);
  pio_sm_set_enabled(pio, sm, true);

  dma_ch = dma_claim_unused_channel(true);
  dma_channel_config dc = dma_channel_get_default_config(dma_ch);
  channel_config_set_transfer_data_size(&dc, DMA_SIZE_8);
  channel_config_set_read_increment(&dc, true);
  channel_config_set_write_increment(&dc, false);
  channel_config_set_dreq(&dc, pio_get_dreq(pio, sm, true));
  dma_channel_configure(dma_ch, &dc, &pio->txf[sm], NULL, 0, false);

  // hardware reset
  sleep_ms(10);
  gpio_put(PIN_RST, 0);
  sleep_ms(10);
  gpio_put(PIN_RST, 1);
  sleep_ms(120);

  // init sequence from shapones samples/v3/picocalc.cpp
  CMD(0xf0, 0xc3);
  CMD(0xf0, 0x96);
  CMD(0x36, 0x48);                // MADCTL: MX, BGR
  CMD(0x3a, 0x65);                // 16 bit RGB565
  CMD(0xb1, 0xa0);                // frame rate control
  CMD(0xb4, 0x00);
  CMD(0xb7, 0xc6);
  CMD(0xb9, 0x02, 0xe0);
  CMD(0xc0, 0x80, 0x06);
  CMD(0xc1, 0x15);
  CMD(0xc2, 0xa7);
  CMD(0xc5, 0x04);
  CMD(0xe8, 0x40, 0x8a, 0x00, 0x00, 0x29, 0x19, 0xaa, 0x33);
  CMD(0xe0, 0xf0, 0x06, 0x0f, 0x05, 0x04, 0x20, 0x37, 0x33, 0x4c, 0x37, 0x13, 0x14, 0x2b, 0x31);
  CMD(0xe1, 0xf0, 0x11, 0x1b, 0x11, 0x0f, 0x0a, 0x37, 0x43, 0x4c, 0x37, 0x13, 0x13, 0x2c, 0x32);
  CMD(0xf0, 0x3c);
  CMD(0xf0, 0x69);
  CMD(0x35, 0x00);
  command(0x11, NULL, 0);         // sleep out
  sleep_ms(120);
  command(0x21, NULL, 0);         // inversion on, needed for this IPS panel

  lcd_fill(0, 0, LCD_WIDTH, LCD_HEIGHT, 0x0000);

  command(0x29, NULL, 0);         // display on
  sleep_ms(120);
}

void lcd_begin(int x, int y, int w, int h) {
  set_window(x, y, w, h);
  pio_wait_idle();
  gpio_put(PIN_DC, 0);
  gpio_put(PIN_CS, 0);
  pio_put(0x2c);                  // memory write
  pio_wait_idle();
  gpio_put(PIN_DC, 1);
  dma_active = false;
}

void lcd_write(const void *buf, uint32_t len) {
  if(dma_active)
    dma_channel_wait_for_finish_blocking(dma_ch);
  dma_channel_transfer_from_buffer_now(dma_ch, buf, len);
  dma_active = true;
}

void lcd_end(void) {
  if(dma_active)
    dma_channel_wait_for_finish_blocking(dma_ch);
  dma_active = false;
  pio_wait_idle();
  gpio_put(PIN_CS, 1);
}

void lcd_fill(int x, int y, int w, int h, uint16_t color) {
  static uint16_t line[LCD_WIDTH];
  for(int i=0;i<w;i++) line[i] = color;

  lcd_begin(x, y, w, h);
  for(int r=0;r<h;r++)
    lcd_write(line, 2*w);         // same buffer every time, never modified
  lcd_end();
}
