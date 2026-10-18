#include "tusb.h"
#include <stm32g431xx.h>
#include <stdio.h>
#include <string.h>

// Bootloader PID = application PID + 1 (app: ABCD:1234, both placeholders until a
// real VID/PID is assigned), so the PC can tell "bootloader waiting for an image"
// from the running application.
#define USB_PID   0x1235
#define USB_VID   0xABCD
#define USB_BCD   0x0200

//--------------------------------------------------------------------+
// Device Descriptor
//--------------------------------------------------------------------+
tusb_desc_device_t const desc_device =
{
	.bLength            = sizeof(tusb_desc_device_t),
	.bDescriptorType    = TUSB_DESC_DEVICE,
	.bcdUSB             = USB_BCD,
	.bDeviceClass       = TUSB_CLASS_MISC,
	.bDeviceSubClass    = MISC_SUBCLASS_COMMON,
	.bDeviceProtocol    = MISC_PROTOCOL_IAD,
	.bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
	.idVendor           = USB_VID,
	.idProduct          = USB_PID,
	.bcdDevice          = 0x0100,
	.iManufacturer      = 0x01,
	.iProduct           = 0x02,
	.iSerialNumber      = 0x03,
	.bNumConfigurations = 0x01
};

uint8_t const *tud_descriptor_device_cb(void) {
	return (uint8_t const *) &desc_device;
}

//--------------------------------------------------------------------+
// Configuration Descriptor (CDC only)
//--------------------------------------------------------------------+
enum {
	ITF_NUM_CDC = 0,
	ITF_NUM_CDC_DATA,
	ITF_NUM_TOTAL
};

#define CONFIG_TOTAL_LEN    (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN)

#define EPNUM_CDC_NOTIF   0x81
#define EPNUM_CDC_OUT     0x02
#define EPNUM_CDC_IN      0x82

uint8_t const desc_fs_configuration[] =
{
	TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 100),
	TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, 4, EPNUM_CDC_NOTIF, 8, EPNUM_CDC_OUT, EPNUM_CDC_IN, 64),
};

uint8_t const *tud_descriptor_configuration_cb(uint8_t index) {
	(void) index;
	return desc_fs_configuration;
}

//--------------------------------------------------------------------+
// String Descriptors
//--------------------------------------------------------------------+
enum {
	STRID_LANGID = 0,
	STRID_MANUFACTURER,
	STRID_PRODUCT,
	STRID_SERIAL,
};

char const *string_desc_arr[] =
{
	(const char[]) { 0x09, 0x04 },     // 0: English (0x0409)
	"Vojtech Vosahlo",                 // 1: Manufacturer
	"MIDIControl base (bootloader)",   // 2: Product
	NULL,                              // 3: serial (from UID)
	"Serial Communication",            // 4: CDC interface
};

static uint16_t _desc_str[32 + 1];
static char strUID[25];

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
	(void) langid;
	size_t chr_count;

	switch (index) {
		case STRID_LANGID:
			memcpy(&_desc_str[1], string_desc_arr[0], 2);
			chr_count = 1;
			break;

		case STRID_SERIAL:
			snprintf(strUID, sizeof(strUID), "%08lX%08lX%08lX",
					 (unsigned long) *(uint32_t *)(UID_BASE),
					 (unsigned long) *(uint32_t *)(UID_BASE + 4),
					 (unsigned long) *(uint32_t *)(UID_BASE + 8));
			chr_count = strlen(strUID);
			for (size_t i = 0; i < chr_count; i++) _desc_str[1 + i] = strUID[i];
			break;

		default:
			if (!(index < sizeof(string_desc_arr) / sizeof(string_desc_arr[0]))) return NULL;
			const char *str = string_desc_arr[index];
			chr_count = strlen(str);
			size_t const max_count = sizeof(_desc_str) / sizeof(_desc_str[0]) - 1;
			if (chr_count > max_count) chr_count = max_count;
			for (size_t i = 0; i < chr_count; i++) _desc_str[1 + i] = str[i];
			break;
	}

	_desc_str[0] = (uint16_t) ((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
	return _desc_str;
}
