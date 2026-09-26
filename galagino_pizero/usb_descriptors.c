/* USB CDC device descriptors for the DVI target.
 *
 * pico_stdio_usb provides these when CFG_TUD_ENABLED=1 but its descriptor
 * source is wrapped in "#ifndef LIB_TINYUSB_HOST", so it is excluded when
 * tinyusb_host is also linked.  We provide a minimal replacement here.
 *
 * One CDC interface only — no Pico reset interface, no Microsoft OS 2.0.
 */

#include "tusb.h"
#include "pico/unique_id.h"

#define USBD_VID          0x2E8A  /* Raspberry Pi */
#define USBD_PID          0x000A  /* Pico SDK CDC */
#define USBD_CDC_ITF      0       /* CDC control interface (uses 2 interfaces) */
#define USBD_ITF_MAX      2
#define USBD_CDC_EP_CMD   0x81
#define USBD_CDC_EP_OUT   0x02
#define USBD_CDC_EP_IN    0x82

#define USBD_STR_LANG     0
#define USBD_STR_MANUF    1
#define USBD_STR_PRODUCT  2
#define USBD_STR_SERIAL   3
#define USBD_STR_CDC      4

#define USBD_DESC_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN)

static const tusb_desc_device_t usb_device_desc = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = USBD_VID,
    .idProduct          = USBD_PID,
    .bcdDevice          = 0x0100,
    .iManufacturer      = USBD_STR_MANUF,
    .iProduct           = USBD_STR_PRODUCT,
    .iSerialNumber      = USBD_STR_SERIAL,
    .bNumConfigurations = 1,
};

static const uint8_t usb_config_desc[USBD_DESC_LEN] = {
    TUD_CONFIG_DESCRIPTOR(1, USBD_ITF_MAX, USBD_STR_LANG, USBD_DESC_LEN,
        0, 250),
    TUD_CDC_DESCRIPTOR(USBD_CDC_ITF, USBD_STR_CDC,
        USBD_CDC_EP_CMD, 8,
        USBD_CDC_EP_OUT, USBD_CDC_EP_IN, 64),
};

static char usb_serial_str[PICO_UNIQUE_BOARD_ID_SIZE_BYTES * 2 + 1];

static const char *const usb_string_desc[] = {
    [USBD_STR_MANUF]   = "Raspberry Pi",
    [USBD_STR_PRODUCT] = "Galagino PiZero",
    [USBD_STR_SERIAL]  = usb_serial_str,
    [USBD_STR_CDC]     = "Galagino console",
};

const uint8_t *tud_descriptor_device_cb(void) {
    return (const uint8_t *)&usb_device_desc;
}

const uint8_t *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return usb_config_desc;
}

const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
    static uint16_t desc[32];

    if (!usb_serial_str[0])
        pico_get_unique_board_id_string(usb_serial_str, sizeof(usb_serial_str));

    uint8_t len;
    if (index == 0) {
        desc[1] = 0x0409;
        len = 1;
    } else {
        if (index >= sizeof(usb_string_desc) / sizeof(usb_string_desc[0]))
            return NULL;
        const char *s = usb_string_desc[index];
        for (len = 0; len < 30 && s[len]; len++)
            desc[1 + len] = s[len];
    }

    desc[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * len + 2));
    return desc;
}
