#ifndef _USB_INPUT_H_
#define _USB_INPUT_H_

// USB gamepads and keyboards on the PIO-USB port (D+ GP28), and the serial
// console on the native USB-C port. The PiZero doesn't supply 5 V on its
// USB ports, so the gamepad needs a powered hub or an OTG power adapter.

// after the DVI driver is set up: it claims DMA channels 0-5, PIO-USB uses 7
void usb_input_init(void);

// service both USB stacks; call often (once per frame at least)
void usb_input_task(void);

// held buttons as BUTTON_* bits from emulation.h
unsigned char usb_input_buttons(void);

#endif // _USB_INPUT_H_
