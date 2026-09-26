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

// state for the on-screen diagnostics (latest HID device / report)
typedef struct {
  int devices;              // USB devices enumerated (any class), currently attached
  int mounted;              // HID interfaces currently mounted
  unsigned short vid, pid;  // of the last mounted one
  unsigned char proto;      // 0 other, 1 keyboard, 2 mouse
  signed char decoder;      // 0 none, 1 generic parser, 2 known pad map, 3 keyboard, -1 parse failed
  unsigned long reports;    // reports received so far
  unsigned char len;        // length of the latest report
  unsigned char raw[16];    // its first bytes
} usb_status_t;

const usb_status_t *usb_input_status(void);

#endif // _USB_INPUT_H_
