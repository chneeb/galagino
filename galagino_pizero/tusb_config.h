/*
 * tusb_config.h - TinyUSB configuration for galagino_pizero
 *
 * The native USB-C port (rhport 0) is a host for USB gamepads and
 * keyboards, as in frank-snes. No device stack, so no USB serial console.
 */
#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#ifdef __cplusplus
extern "C" {
#endif

#ifndef CFG_TUSB_MCU
#error CFG_TUSB_MCU must be defined
#endif

#ifndef CFG_TUSB_OS
#define CFG_TUSB_OS  OPT_OS_NONE
#endif

#ifndef CFG_TUSB_DEBUG
#define CFG_TUSB_DEBUG  0
#endif

#ifndef CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_SECTION
#endif

#ifndef CFG_TUSB_MEM_ALIGN
#define CFG_TUSB_MEM_ALIGN  __attribute__((aligned(4)))
#endif

#define CFG_TUSB_RHPORT0_MODE  (OPT_MODE_HOST | OPT_MODE_FULL_SPEED)
#define CFG_TUD_ENABLED        0
#define CFG_TUH_ENABLED        1
#define BOARD_TUH_RHPORT       0
#define CFG_TUH_MAX_SPEED      OPT_MODE_FULL_SPEED

#define CFG_TUH_ENUMERATION_BUFSIZE  256
#define CFG_TUH_HUB     1
#define CFG_TUH_HID     4
#define CFG_TUH_CDC     0
#define CFG_TUH_MSC     0
#define CFG_TUH_VENDOR  0
#define CFG_TUH_DEVICE_MAX  (CFG_TUH_HUB ? 4 : 1)
#define CFG_TUH_HID_EPIN_BUFSIZE   64
#define CFG_TUH_HID_EPOUT_BUFSIZE  64

#ifdef __cplusplus
}
#endif

#endif /* _TUSB_CONFIG_H_ */
