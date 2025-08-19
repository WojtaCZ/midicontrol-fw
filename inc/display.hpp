
#ifndef DISPLAY_H
#define DISPLAY_H

#include <cstdint>
#include <cstddef>

namespace display{

    enum ledColor {
        RED = 0x20,
        GREEN = 0x22,
        BLUE = 0x23,
        YELLOW = 0x21,
        OFF = 0xE0
    };
    

    void init();
    void sendState();
    void setSong(uint16_t song, uint8_t visible);
    void setVerse(uint8_t verse, uint8_t visible);
    void setLetter(char letter, uint8_t visible);
    void setLed(ledColor led);

    uint16_t getSong();
    uint8_t getVerse();
    char getLetter();
    ledColor getLed();
    bool getConnected();
    bool wasChanged();
    //uint8_t (&data)[9] getRawState();
    uint8_t getRawState(int index);
    //void setRawState(array<std::uint8_t, 9> state);
    //void setRawState(std::uint8_t data, int index);
    uint8_t getRawSysex(int index);
    void sendToMIDI();

}

extern "C" void display_setRawSysex(uint8_t data, int index);

#endif