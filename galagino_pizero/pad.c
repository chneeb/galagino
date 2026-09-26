/*
 * pad.c - NES Classic Mini controller over I2C
 *
 * Protocol as in RP2040-GNUBoy's sys/pico/input.cpp: unencrypted init,
 * high resolution data mode, 8 byte reports with the buttons active low
 * in bytes 6 and 7. The controller runs on 3.3 V, so unlike a USB pad it
 * works without 5 V on the board's connectors.
 */

#include "pad.h"

#include "pico/stdlib.h"
#include "hardware/i2c.h"

#include "Z80.h"          // pulls in emulation.h for the BUTTON_* bits

#define PAD_I2C      i2c1
#define PAD_SDA      2
#define PAD_SCL      3
#define PAD_ADDR     0x52
#define PAD_SPEED    100000
#define PAD_TIMEOUT  2000   // us

static volatile unsigned char buttons = 0;
static bool connected = false, request_pending = false;
static uint32_t retry_at = 0;

static bool write2(uint8_t reg, uint8_t val) {
  uint8_t buf[2] = { reg, val };
  return i2c_write_timeout_us(PAD_I2C, PAD_ADDR, buf, 2, false, PAD_TIMEOUT) == 2;
}

static bool pad_connect(void) {
  // unencrypted init, then high resolution mode (8 byte reports)
  if(!write2(0xf0, 0x55)) return false;
  sleep_us(500);
  if(!write2(0xfb, 0x00)) return false;
  sleep_us(500);
  return write2(0xfe, 0x03);
}

void pad_init(void) {
  i2c_init(PAD_I2C, PAD_SPEED);
  gpio_set_function(PAD_SDA, GPIO_FUNC_I2C);
  gpio_set_function(PAD_SCL, GPIO_FUNC_I2C);
  gpio_pull_up(PAD_SDA);
  gpio_pull_up(PAD_SCL);
  connected = pad_connect();
  request_pending = false;
}

static void decode(const uint8_t *d) {
  // active low: byte 6 = right down . select . start . ., byte 7 = . B . A . . left up
  unsigned char b = 0;
  if(!(d[7] & 0x01)) b |= BUTTON_UP;
  if(!(d[6] & 0x40)) b |= BUTTON_DOWN;
  if(!(d[7] & 0x02)) b |= BUTTON_LEFT;
  if(!(d[6] & 0x80)) b |= BUTTON_RIGHT;
  if(!(d[7] & 0x10)) b |= BUTTON_FIRE;     // A
  if(!(d[7] & 0x40)) b |= BUTTON_FIRE;     // B

  bool select = !(d[6] & 0x10), start = !(d[6] & 0x04);
  if(select && start) b |= BUTTON_EXTRA;   // hold both: back to the menu
  else {
    if(select) b |= BUTTON_COIN;
    if(start)  b |= BUTTON_START;
  }
  buttons = b;
}

void pad_poll(void) {
  uint32_t now = to_ms_since_boot(get_absolute_time());

  if(!connected) {
    // retry about twice a second, so the pad can be plugged in later
    if((int32_t)(now - retry_at) < 0) return;
    retry_at = now + 500;
    connected = pad_connect();
    request_pending = false;
    if(!connected) { buttons = 0; return; }
  }

  if(request_pending) {
    uint8_t d[8];
    request_pending = false;
    if(i2c_read_timeout_us(PAD_I2C, PAD_ADDR, d, 8, false, PAD_TIMEOUT) != 8) {
      connected = false;
      buttons = 0;
      return;
    }
    decode(d);
  }

  uint8_t reg = 0x00;
  if(i2c_write_timeout_us(PAD_I2C, PAD_ADDR, &reg, 1, false, PAD_TIMEOUT) == 1)
    request_pending = true;
  else {
    connected = false;
    buttons = 0;
  }
}

unsigned char pad_buttons(void) {
  return buttons;
}
