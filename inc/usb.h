#ifndef USB_H
#define USB_H

#include <stdint.h>

#define PORT_USB_DP GPIOA
#define GPIO_USB_DP GPIO12

#define PORT_USB_DM GPIOA
#define GPIO_USB_DM GPIO11

void usb_init(void);

uint32_t usb_cdc_tx(void *buf, int len);
uint32_t usb_midi_tx(void *buf, int len);

extern void midi_send(uint8_t * buff, int len);
void usb_init();

#endif
