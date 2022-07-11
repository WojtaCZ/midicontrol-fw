
#ifndef DISPLAY_H
#define DISPLAY_H
#include <cstdint>

namespace Display{
    enum LED{
        RED = 0x20,
        GREEN = 0x22,
        BLUE = 0x23,
        YELLOW = 0x21,
        OFF = 0xE0
    };

    void set_song(uint16_t song, uint8_t visible);
    void set_verse(uint8_t verse, uint8_t visible);
    void set_letter(char letter, uint8_t visible);
    void set_led(LED led);

    uint16_t get_song();
    uint8_t get_verse();
    char get_letter();
    LED get_led();
}


#endif