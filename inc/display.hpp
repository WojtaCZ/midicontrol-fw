
#ifndef DISPLAY_H
#define DISPLAY_H

#include "global.hpp"

#define DISPLAY_LED_R       0x20
#define DISPLAY_LED_G       0x22
#define DISPLAY_LED_B       0x23
#define DISPLAY_LED_Y       0x21
#define DISPLAY_LED_OFF     0xE0

void display_set_song(uint16_t song, uint8_t visible);
void display_set_verse(uint8_t verse, uint8_t visible);
void display_set_letter(char letter, uint8_t visible);
void display_set_led(uint8_t led, uint8_t visible);

uint16_t display_get_song();
uint8_t display_get_verse();
char display_get_letter();
uint8_t display_get_led();

#endif