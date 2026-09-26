/*
 * usb_input.c - USB gamepads and keyboards via PIO-USB, serial via USB-C
 *
 * Setup follows tiny_agi's RP2350-PiZero DVI target: TinyUSB host on
 * PIO-USB (pio1, D+ on GP28, DMA channel 7, since DVI claims 0-5) and a CDC
 * device on the native controller for the console. The SDK's own USB stdio
 * is disabled when the TinyUSB host is linked, so a small stdio driver
 * sends printf() output to the CDC port. No board_init(), which would
 * reset the 252 MHz clock DVI needs.
 *
 * Gamepads are decoded generically from their HID report descriptor (the
 * LUFA-derived parser from msx2pico): hat switch, X/Y axes or d-pad usages
 * give directions, button usages give fire, coin and start. Boot protocol
 * keyboards work too. XInput (Xbox style) pads are not supported.
 */

#include "usb_input.h"

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/stdio/driver.h"
#include "tusb.h"
#include "pio_usb.h"
#include "hidparser/hidparser.h"

#include "Z80.h"          // pulls in emulation.h for the BUTTON_* bits

// Gamepad button usages (1-based, as in the HID report descriptor). Generic
// pads differ; pressed buttons are printed on the console to help remap.
#define PAD_FIRE_FIRST   1      // buttons 1..6: fire
#define PAD_FIRE_LAST    6
#define PAD_COIN_A       7      // select/back on most pads is 7 or 9
#define PAD_COIN_B       9
#define PAD_START_A      8      // start is 8 or 10
#define PAD_START_B     10

#define MAX_DEV   (CFG_TUH_DEVICE_MAX + 2)   // device addresses start at 1, plus hub
#define MAX_ITF   CFG_TUH_HID

static HID_ReportInfo_t *pad_info[MAX_DEV][MAX_ITF];
static unsigned char itf_buttons[MAX_DEV][MAX_ITF];
static uint32_t raw_buttons;   // pressed gamepad button usages, bit n = button n+1

/* ---------------------------- serial console ---------------------------- */

static void cdc_out_chars(const char *buf, int len) {
  if(!tud_cdc_connected()) return;
  // never wait for the host: drop what doesn't fit (printf may run in IRQs)
  uint32_t avail = tud_cdc_write_available();
  uint32_t n = (uint32_t)len < avail ? (uint32_t)len : avail;
  if(n) tud_cdc_write(buf, n);
  tud_cdc_write_flush();
}

static stdio_driver_t cdc_stdio_drv = {
  .out_chars = cdc_out_chars,
#if PICO_STDIO_ENABLE_CRLF_SUPPORT
  .crlf_enabled = PICO_STDIO_DEFAULT_CRLF,
#endif
};

/* --------------------------------- setup -------------------------------- */

void usb_input_init(void) {
  pio_usb_configuration_t pio_cfg = PIO_USB_DEFAULT_CONFIG;
  pio_cfg.pin_dp     = PICO_DEFAULT_PIO_USB_DP_PIN;   // GP28
  pio_cfg.pio_tx_num = 1;
  pio_cfg.pio_rx_num = 1;
  pio_cfg.tx_ch      = 7;
  tuh_configure(1, TUH_CFGID_RPI_PIO_USB_CONFIGURATION, &pio_cfg);
  tuh_init(1);

  tud_init(0);
  stdio_set_driver_enabled(&cdc_stdio_drv, true);
}

void usb_input_task(void) {
  tuh_task();
  tud_task();
}

unsigned char usb_input_buttons(void) {
  unsigned char b = 0;
  for(int d=0;d<MAX_DEV;d++)
    for(int i=0;i<MAX_ITF;i++)
      b |= itf_buttons[d][i];
  return b;
}

/* ------------------------------- gamepads ------------------------------- */

// called by the parser: keep only what a gamepad needs
bool CALLBACK_HIDParser_FilterHIDReportItem(HID_ReportItem_t *const item) {
  if(item->ItemType != HID_REPORT_ITEM_In) return false;

  switch(item->Attributes.Usage.Page) {
    case HID_USAGE_PAGE_DESKTOP:
      switch(item->Attributes.Usage.Usage) {
        case HID_USAGE_DESKTOP_X:
        case HID_USAGE_DESKTOP_Y:
        case HID_USAGE_DESKTOP_HAT_SWITCH:
        case HID_USAGE_DESKTOP_DPAD_UP:
        case HID_USAGE_DESKTOP_DPAD_DOWN:
        case HID_USAGE_DESKTOP_DPAD_LEFT:
        case HID_USAGE_DESKTOP_DPAD_RIGHT:
          return true;
      }
      return false;
    case HID_USAGE_PAGE_BUTTON:
      return true;
  }
  return false;
}

// item value as a signed number if its logical range is signed
static int32_t item_value(const HID_ReportItem_t *item) {
  int32_t v = item->Value;
  int32_t min = (int32_t)item->Attributes.Logical.Minimum;
  int bits = item->Attributes.BitSize;
  if(min < 0 && bits > 0 && bits < 32 && (v & (1 << (bits - 1))))
    v -= 1 << bits;
  return v;
}

// -1, 0 or 1 for an axis, with a dead zone of a quarter of each half
static int axis_dir(const HID_ReportItem_t *item) {
  int32_t min = (int32_t)item->Attributes.Logical.Minimum;
  int32_t max = (int32_t)item->Attributes.Logical.Maximum;
  if(max <= min) return 0;
  int32_t center = min + (max - min) / 2;
  int32_t dead = (max - min) / 8;
  int32_t v = item_value(item);
  if(v < center - dead) return -1;
  if(v > center + dead) return 1;
  return 0;
}

static unsigned char parse_gamepad(HID_ReportInfo_t *info, const uint8_t *report) {
  // hat: 0 = N, clockwise to 7 = NW, anything else = released
  static const unsigned char hat_dirs[8] = {
    BUTTON_UP, BUTTON_UP | BUTTON_RIGHT, BUTTON_RIGHT, BUTTON_DOWN | BUTTON_RIGHT,
    BUTTON_DOWN, BUTTON_DOWN | BUTTON_LEFT, BUTTON_LEFT, BUTTON_UP | BUTTON_LEFT };

  unsigned char b = 0;
  bool coin = false, start = false;
  raw_buttons = 0;

  for(HID_ReportItem_t *item = info->FirstReportItem; item; item = item->Next) {
    // skip items belonging to another report id
    const uint8_t *data = report;
    if(item->ReportID) {
      if(item->ReportID != report[0]) continue;
      data++;
    }
    if(!USB_GetHIDReportItemInfo(item->ReportID, data, item)) continue;

    if(item->Attributes.Usage.Page == HID_USAGE_PAGE_DESKTOP) {
      switch(item->Attributes.Usage.Usage) {
        case HID_USAGE_DESKTOP_X: {
          int d = axis_dir(item);
          if(d < 0) b |= BUTTON_LEFT;
          if(d > 0) b |= BUTTON_RIGHT;
          break;
        }
        case HID_USAGE_DESKTOP_Y: {
          int d = axis_dir(item);
          if(d < 0) b |= BUTTON_UP;
          if(d > 0) b |= BUTTON_DOWN;
          break;
        }
        case HID_USAGE_DESKTOP_HAT_SWITCH: {
          uint32_t h = item->Value - item->Attributes.Logical.Minimum;
          if(h < 8) b |= hat_dirs[h];
          break;
        }
        case HID_USAGE_DESKTOP_DPAD_UP:    if(item->Value) b |= BUTTON_UP;    break;
        case HID_USAGE_DESKTOP_DPAD_DOWN:  if(item->Value) b |= BUTTON_DOWN;  break;
        case HID_USAGE_DESKTOP_DPAD_LEFT:  if(item->Value) b |= BUTTON_LEFT;  break;
        case HID_USAGE_DESKTOP_DPAD_RIGHT: if(item->Value) b |= BUTTON_RIGHT; break;
      }
    } else if(item->Attributes.Usage.Page == HID_USAGE_PAGE_BUTTON && item->Value) {
      int u = item->Attributes.Usage.Usage;
      if(u >= 1 && u <= 32) raw_buttons |= 1u << (u - 1);
      if(u >= PAD_FIRE_FIRST && u <= PAD_FIRE_LAST) b |= BUTTON_FIRE;
      if(u == PAD_COIN_A || u == PAD_COIN_B)        coin = true;
      if(u == PAD_START_A || u == PAD_START_B)      start = true;
    }
  }

  if(coin && start) b |= BUTTON_EXTRA;     // hold both: back to the menu
  else {
    if(coin)  b |= BUTTON_COIN;
    if(start) b |= BUTTON_START;
  }
  return b;
}

/* ------------------------------- keyboards ------------------------------ */

static unsigned char parse_keyboard(const hid_keyboard_report_t *r) {
  unsigned char b = 0;
  if(r->modifier & (KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_RIGHTCTRL))
    b |= BUTTON_FIRE;

  for(int i=0;i<6;i++) {
    switch(r->keycode[i]) {
      case HID_KEY_ARROW_UP:    b |= BUTTON_UP;    break;
      case HID_KEY_ARROW_DOWN:  b |= BUTTON_DOWN;  break;
      case HID_KEY_ARROW_LEFT:  b |= BUTTON_LEFT;  break;
      case HID_KEY_ARROW_RIGHT: b |= BUTTON_RIGHT; break;
      case HID_KEY_SPACE:
      case HID_KEY_Z:
      case HID_KEY_X:           b |= BUTTON_FIRE;  break;
      case HID_KEY_5:
      case HID_KEY_C:           b |= BUTTON_COIN;  break;
      case HID_KEY_1:
      case HID_KEY_ENTER:       b |= BUTTON_START; break;
      case HID_KEY_ESCAPE:      b |= BUTTON_EXTRA; break;
    }
  }
  return b;
}

/* --------------------------- TinyUSB callbacks -------------------------- */

void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance, uint8_t const *desc_report, uint16_t desc_len) {
  uint8_t proto = tuh_hid_interface_protocol(dev_addr, instance);
  printf("usb: device %d interface %d mounted (%s)\n", dev_addr, instance,
         proto == HID_ITF_PROTOCOL_KEYBOARD ? "keyboard" :
         proto == HID_ITF_PROTOCOL_MOUSE ? "mouse" : "other HID");

  if(dev_addr < MAX_DEV && instance < MAX_ITF) {
    itf_buttons[dev_addr][instance] = 0;
    if(proto == HID_ITF_PROTOCOL_KEYBOARD)
      tuh_hid_set_protocol(dev_addr, instance, HID_PROTOCOL_BOOT);
    else if(proto == HID_ITF_PROTOCOL_NONE) {
      if(pad_info[dev_addr][instance]) USB_FreeReportInfo(pad_info[dev_addr][instance]);
      pad_info[dev_addr][instance] = NULL;
      if(USB_ProcessHIDReport(desc_report, desc_len, &pad_info[dev_addr][instance]) != HID_PARSE_Successful) {
        printf("usb: can't parse the report descriptor\n");
        if(pad_info[dev_addr][instance]) USB_FreeReportInfo(pad_info[dev_addr][instance]);
        pad_info[dev_addr][instance] = NULL;
      }
    }
  }

  tuh_hid_receive_report(dev_addr, instance);
}

void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance) {
  printf("usb: device %d interface %d removed\n", dev_addr, instance);
  if(dev_addr < MAX_DEV && instance < MAX_ITF) {
    itf_buttons[dev_addr][instance] = 0;
    if(pad_info[dev_addr][instance]) USB_FreeReportInfo(pad_info[dev_addr][instance]);
    pad_info[dev_addr][instance] = NULL;
  }
}

void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance, uint8_t const *report, uint16_t len) {
  if(dev_addr < MAX_DEV && instance < MAX_ITF) {
    uint8_t proto = tuh_hid_interface_protocol(dev_addr, instance);
    unsigned char b = itf_buttons[dev_addr][instance];

    raw_buttons = 0;
    if(proto == HID_ITF_PROTOCOL_KEYBOARD && len >= sizeof(hid_keyboard_report_t))
      b = parse_keyboard((const hid_keyboard_report_t *)report);
    else if(pad_info[dev_addr][instance])
      b = parse_gamepad(pad_info[dev_addr][instance], report);

    static uint32_t last_raw;
    if(b != itf_buttons[dev_addr][instance] || raw_buttons != last_raw) {
      // pad button numbers, to check or change the PAD_* mapping above
      printf("usb: galagino buttons 0x%02x, pad buttons", b);
      for(int i=0;i<32;i++) if(raw_buttons & (1u << i)) printf(" %d", i + 1);
      printf("\n");
      itf_buttons[dev_addr][instance] = b;
      last_raw = raw_buttons;
    }
  }

  tuh_hid_receive_report(dev_addr, instance);
}
