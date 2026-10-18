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

    // Hlavní smyčka: odeslání rámce (nejvýše jeden najednou, bez přerušení rozpracovaného) a SysEx ozvěna
    void process();

    // Naplánuje odeslání aktuálního stavu (displej + SysEx ozvěna do PC)
    void sendState();

    // NOW_PLAYING_INFO payload (viz lib/protocol/PROTOCOL.md): BCD po číslicích, 0xF = prázdná číslice,
    // 0xFF = beze změny, 0xFE = zhasnout pole, prázdný payload = smazat vše. Vždy jeden rámec.
    void applyNowPlaying(const uint8_t *payload, uint8_t len);
    void clear();

    void setSong(uint16_t song, bool visible);
    void setVerse(uint8_t verse, bool visible);
    void setLetter(char letter, bool visible);
    void setLed(ledColor led);

    // Text pro OLED po číslicích, mezera = zhasnutá číslice (" 401", "1 ")
    void formatSong(char (&out)[5]);
    void formatVerse(char (&out)[3]);
    char getLetter(); // 'A'-'D' nebo ' '
    ledColor getLed();
    bool isConnected();
    bool wasChanged();
    bool sendToMIDI();

}

#endif
