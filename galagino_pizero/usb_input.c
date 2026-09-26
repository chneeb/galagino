/*
 * usb_input.c - USB gamepads and keyboards on the native USB-C port
 *
 * TinyUSB host on the native USB controller, as frank-snes does. An
 * earlier PIO-USB variant (port labelled "USB PIO", as in tiny_agi) didn't
 * work with the SNES-clone pad and was dropped. No board_init(), which
 * would reset the 252 MHz clock DVI needs.
 *
 * Gamepads are decoded generically from their HID report descriptor (the
 * LUFA-derived parser from msx2pico): hat switch, X/Y axes or d-pad usages
 * give directions, button usages give fire, coin and start. Boot protocol
 * keyboards work too. XInput (Xbox style) pads are not supported.
 *
 * Known pads are decoded from fixed byte positions instead, using the maps
 * measured for frank-snes (drivers/usbhid/hid_app.c, gamepads/*.txt): cheap
 * SNES clones don't describe themselves reliably. 0810:e501 comes from
 * pico-infonesPlus (hid_app.cpp).
 */

#include "usb_input.h"

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "tusb.h"
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

// fixed-layout pads, from frank-snes' measured maps
typedef struct { uint8_t byte, mask; } pad_bit_t;
typedef struct {
  uint16_t vid, pid;
  bool hat;                  // false: axes at dx/dy (0x7f centre), true: hat in low nibble of dx
  uint8_t dx, dy;
  pad_bit_t fire[6];         // A B X Y L R
  pad_bit_t start, select;
} pad_map_t;

#define SNES_CLONE_BUTTONS \
  { {5,0x20}, {5,0x40}, {5,0x10}, {5,0x80}, {6,0x01}, {6,0x02} }, {6,0x20}, {6,0x10}

static const pad_map_t pad_maps[] = {
  { 0x0079, 0x0006, false, 0, 1, SNES_CLONE_BUTTONS },   // DragonRise "USB Gamepad"
  // SNES clone, report 01 7f 7f XX YY 0f 00 00: frank-snes' fallback layout
  // (verified from the on-screen raw report, 2026-09-26)
  { 0x0079, 0x0011, false, 3, 4, SNES_CLONE_BUTTONS },
  { 0x081f, 0xe401, false, 0, 1, SNES_CLONE_BUTTONS },   // SNES clone
  { 0x0810, 0xe501, false, 3, 4, SNES_CLONE_BUTTONS },   // SNES clone variant (infones)
  { 0x046d, 0xc219, true,  5, 0, SNES_CLONE_BUTTONS },   // Logitech
  { 0x11ff, 0x3331, false, 0, 1,
    { {5,0x80}, {5,0x40}, {5,0x20}, {5,0x10}, {6,0x04}, {6,0x08} }, {6,0x20}, {6,0x10} },
  { 0x2563, 0x0575, true,  2, 0,
    { {0,0x04}, {0,0x02}, {0,0x08}, {0,0x01}, {0,0x10}, {0,0x20} }, {1,0x02}, {1,0x01} },
  { 0xfeed, 0x2320, true,  5, 0,
    { {6,0x01}, {6,0x02}, {6,0x08}, {6,0x04}, {6,0x10}, {6,0x20} }, {7,0x08}, {7,0x04} },
};

static const pad_map_t *itf_map[MAX_DEV][MAX_ITF];
static uint8_t raw_dumps[MAX_DEV][MAX_ITF];   // raw reports printed so far
#define RAW_DUMP_MAX  16

static usb_status_t status;

const usb_status_t *usb_input_status(void) {
  return &status;
}

/* --------------------------------- setup -------------------------------- */

void usb_input_init(void) {
  tuh_init(0);
}

void usb_input_task(void) {
  tuh_task();
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

static inline bool pad_bit(const uint8_t *r, uint16_t len, pad_bit_t b) {
  return b.byte < len && (r[b.byte] & b.mask);
}

// fixed-layout pads, decoded like frank-snes' process_gamepad_report()
static unsigned char parse_mapped(const pad_map_t *m, const uint8_t *r, uint16_t len) {
  unsigned char b = 0;
  if(!m->hat) {
    if(m->dx < len && r[m->dx] < 0x40) b |= BUTTON_LEFT;
    if(m->dx < len && r[m->dx] > 0xc0) b |= BUTTON_RIGHT;
    if(m->dy < len && r[m->dy] < 0x40) b |= BUTTON_UP;
    if(m->dy < len && r[m->dy] > 0xc0) b |= BUTTON_DOWN;
  } else if(m->dx < len) {
    static const unsigned char hat_dirs[8] = {
      BUTTON_UP, BUTTON_UP | BUTTON_RIGHT, BUTTON_RIGHT, BUTTON_DOWN | BUTTON_RIGHT,
      BUTTON_DOWN, BUTTON_DOWN | BUTTON_LEFT, BUTTON_LEFT, BUTTON_UP | BUTTON_LEFT };
    uint8_t h = r[m->dx] & 0x0f;
    if(h < 8) b |= hat_dirs[h];
  }
  for(int i=0;i<6;i++)
    if(pad_bit(r, len, m->fire[i])) b |= BUTTON_FIRE;

  bool select = pad_bit(r, len, m->select), start = pad_bit(r, len, m->start);
  if(select && start) b |= BUTTON_EXTRA;
  else {
    if(select) b |= BUTTON_COIN;
    if(start)  b |= BUTTON_START;
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

// any device, HID or not: tells "nothing enumerates" (power) from "not HID"
void tuh_mount_cb(uint8_t dev_addr) {
  uint16_t vid = 0, pid = 0;
  tuh_vid_pid_get(dev_addr, &vid, &pid);
  printf("usb: device %d enumerated, %04x:%04x\n", dev_addr, vid, pid);
  status.devices++;
  status.vid = vid;
  status.pid = pid;
}

void tuh_umount_cb(uint8_t dev_addr) {
  printf("usb: device %d detached\n", dev_addr);
  if(status.devices) status.devices--;
}

void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance, uint8_t const *desc_report, uint16_t desc_len) {
  uint8_t proto = tuh_hid_interface_protocol(dev_addr, instance);
  uint16_t vid = 0, pid = 0;
  tuh_vid_pid_get(dev_addr, &vid, &pid);
  printf("usb: device %d interface %d mounted, %04x:%04x (%s)\n", dev_addr, instance, vid, pid,
         proto == HID_ITF_PROTOCOL_KEYBOARD ? "keyboard" :
         proto == HID_ITF_PROTOCOL_MOUSE ? "mouse" : "other HID");

  status.mounted++;
  status.vid = vid;
  status.pid = pid;
  status.proto = proto;
  status.decoder = 0;

  if(dev_addr < MAX_DEV && instance < MAX_ITF) {
    itf_buttons[dev_addr][instance] = 0;
    raw_dumps[dev_addr][instance] = 0;
    itf_map[dev_addr][instance] = NULL;
    for(unsigned i=0;i<sizeof(pad_maps)/sizeof(pad_maps[0]);i++)
      if(pad_maps[i].vid == vid && pad_maps[i].pid == pid)
        itf_map[dev_addr][instance] = &pad_maps[i];
    if(itf_map[dev_addr][instance]) {
      printf("usb: known pad, using its measured report layout\n");
      status.decoder = 2;
    }

    if(proto == HID_ITF_PROTOCOL_KEYBOARD) {
      tuh_hid_set_protocol(dev_addr, instance, HID_PROTOCOL_BOOT);
      status.decoder = 3;
    } else if(proto == HID_ITF_PROTOCOL_NONE && !itf_map[dev_addr][instance]) {
      if(pad_info[dev_addr][instance]) USB_FreeReportInfo(pad_info[dev_addr][instance]);
      pad_info[dev_addr][instance] = NULL;
      if(USB_ProcessHIDReport(desc_report, desc_len, &pad_info[dev_addr][instance]) != HID_PARSE_Successful) {
        printf("usb: can't parse the report descriptor\n");
        status.decoder = -1;
        if(pad_info[dev_addr][instance]) USB_FreeReportInfo(pad_info[dev_addr][instance]);
        pad_info[dev_addr][instance] = NULL;
      } else
        status.decoder = 1;
    }
  }

  tuh_hid_receive_report(dev_addr, instance);
}

void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance) {
  printf("usb: device %d interface %d removed\n", dev_addr, instance);
  if(status.mounted) status.mounted--;
  if(dev_addr < MAX_DEV && instance < MAX_ITF) {
    itf_buttons[dev_addr][instance] = 0;
    if(pad_info[dev_addr][instance]) USB_FreeReportInfo(pad_info[dev_addr][instance]);
    pad_info[dev_addr][instance] = NULL;
  }
}

void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance, uint8_t const *report, uint16_t len) {
  status.reports++;
  status.len = len;
  memcpy(status.raw, report, len < sizeof(status.raw) ? len : sizeof(status.raw));
  if(dev_addr < MAX_DEV && instance < MAX_ITF) {
    uint8_t proto = tuh_hid_interface_protocol(dev_addr, instance);
    unsigned char b = itf_buttons[dev_addr][instance];

    const pad_map_t *map = itf_map[dev_addr][instance];
    raw_buttons = 0;
    if(proto == HID_ITF_PROTOCOL_KEYBOARD && len >= sizeof(hid_keyboard_report_t))
      b = parse_keyboard((const hid_keyboard_report_t *)report);
    else if(map)
      b = parse_mapped(map, report, len);
    else if(pad_info[dev_addr][instance])
      b = parse_gamepad(pad_info[dev_addr][instance], report);

    // raw reports from non-keyboards, to diagnose pads that don't decode
    if(proto != HID_ITF_PROTOCOL_KEYBOARD && raw_dumps[dev_addr][instance] < RAW_DUMP_MAX) {
      static uint8_t last[16];
      uint16_t n = len < sizeof(last) ? len : sizeof(last);
      if(memcmp(last, report, n)) {
        memcpy(last, report, n);
        raw_dumps[dev_addr][instance]++;
        printf("usb: raw report (%d bytes):", len);
        for(int i=0;i<n;i++) printf(" %02x", report[i]);
        printf("\n");
      }
    }

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
