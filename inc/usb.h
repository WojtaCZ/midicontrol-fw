#ifndef USB_H
#define USB_H

#include <stdint.h>

//GPIO config for USB
#define PORT_USB_DP GPIOA
#define GPIO_USB_DP GPIO12
#define PORT_USB_DM GPIOA
#define GPIO_USB_DM GPIO11

uint32_t usb_cdc_tx(void *buf, int len);
uint32_t usb_midi_tx(void *buf, int len);
void usb_init();

#endif
