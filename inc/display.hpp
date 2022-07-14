
#ifndef DISPLAY_H
#define DISPLAY_H

#include <array>
#include <cstdint>

using namespace std;

namespace Display{
    enum LED{
        RED = 0x20,
        GREEN = 0x22,
        BLUE = 0x23,
        YELLOW = 0x21,
        OFF = 0xE0
    };

    void init();
    void send(array<uint8_t, 9> data);
    void setSong(uint16_t song, uint8_t visible);
    void setVerse(uint8_t verse, uint8_t visible);
    void setLetter(char letter, uint8_t visible);
    void setLed(LED led);

    uint16_t getSong();
    uint8_t getVerse();
    char getLetter();
    LED getLed();

}


#endif