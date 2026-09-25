#ifndef _KBD_H_
#define _KBD_H_

// PicoCalc keyboard: STM32 on I2C1 reporting press/hold/release events

void kbd_init(void);

// call once per video frame: collects the event requested one frame ago
// and requests the next. The keyboard MCU needs ~16 ms between the two.
void kbd_poll(void);

// currently held keys as BUTTON_* bits from emulation.h
unsigned char kbd_buttons(void);

#endif // _KBD_H_
