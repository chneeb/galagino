/*
 * kbd.c - PicoCalc keyboard input
 *
 * Protocol as in ClockworkPi's picocalc_helloworld i2ckbd.c: write
 * register 0x09 (key FIFO), wait, then read two bytes. The low byte is
 * the state (1 = pressed, 2 = held, 3 = released), the high byte the key.
 */

#include "kbd.h"

#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/i2c.h"

#include "Z80.h"          // pulls in emulation.h for the BUTTON_* bits

#define KBD_I2C       i2c1
#define KBD_SDA       6
#define KBD_SCL       7
#define KBD_ADDR      0x1f
#define KBD_SPEED     400000
#define KBD_REG_FIFO  0x09
#define KBD_TIMEOUT   2000   // us

#define KEY_LEFT      0xb4
#define KEY_UP        0xb5
#define KEY_DOWN      0xb6
#define KEY_RIGHT     0xb7
#define KEY_ENTER     0x0a
#define KEY_ESC       0xb1

static volatile unsigned char buttons = 0;
static bool request_pending = false;
static int errors = 0;

void kbd_init(void) {
  i2c_init(KBD_I2C, KBD_SPEED);
  gpio_set_function(KBD_SDA, GPIO_FUNC_I2C);
  gpio_set_function(KBD_SCL, GPIO_FUNC_I2C);
  gpio_pull_up(KBD_SDA);
  gpio_pull_up(KBD_SCL);
  request_pending = false;
}

static unsigned char key_to_button(int key) {
  switch(key) {
    case KEY_LEFT:  return BUTTON_LEFT;
    case KEY_RIGHT: return BUTTON_RIGHT;
    case KEY_UP:    return BUTTON_UP;
    case KEY_DOWN:  return BUTTON_DOWN;
    case ' ':       return BUTTON_FIRE;
    case '1':
    case KEY_ENTER: return BUTTON_START;
    case '5':
    case 'c':       return BUTTON_COIN;
    case KEY_ESC:   return BUTTON_EXTRA;   // hold to return to the menu
  }
  return 0;
}

static void handle_event(uint16_t ev) {
  unsigned char state = ev & 0xff;
  unsigned char bit = key_to_button(ev >> 8);
  if(!bit) return;

  if(state == 1 || state == 2)  buttons |= bit;
  else if(state == 3)           buttons &= ~bit;
}

void kbd_poll(void) {
  if(request_pending) {
    uint16_t ev = 0;
    int r = i2c_read_timeout_us(KBD_I2C, KBD_ADDR, (uint8_t *)&ev, 2, false, KBD_TIMEOUT);
    request_pending = false;
    if(r == 2) {
      if(ev) handle_event(ev);
    } else
      errors++;
  }

  uint8_t reg = KBD_REG_FIFO;
  if(i2c_write_timeout_us(KBD_I2C, KBD_ADDR, &reg, 1, false, KBD_TIMEOUT) == 1)
    request_pending = true;
  else
    errors++;

  // recover from a wedged bus, as shapones does
  if(errors > 10) {
    printf("kbd: i2c errors, re-init\n");
    i2c_deinit(KBD_I2C);
    kbd_init();
    errors = 0;
  }
}

unsigned char kbd_buttons(void) {
  return buttons;
}
