#ifndef USB_H
#define USB_H

#include <stdint.h>


uint32_t usb_cdc_tx(void *buf, int len);
uint32_t usb_midi_tx(void *buf, int len);
void usb_init();

#endif
