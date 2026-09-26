#ifndef _PAD_H_
#define _PAD_H_

#include <stdbool.h>

// NES Classic Mini controller (Wii extension protocol) on I2C1, GP2/GP3

void pad_init(void);

// call once per frame: collects the reading requested one frame ago and
// requests the next, so no time is spent waiting on the controller
void pad_poll(void);

// held buttons as BUTTON_* bits from emulation.h. Select+Start together
// give BUTTON_EXTRA (back to the menu) instead of coin and start.
unsigned char pad_buttons(void);

// whether the controller answered the last poll
bool pad_connected(void);

#endif // _PAD_H_
