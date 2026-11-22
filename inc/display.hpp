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

    // Main loop: frame transmission (at most one at a time, a frame in progress is never interrupted) and SysEx echo
    void process();

    // Schedules sending the current state (display + SysEx echo to the PC)
    void sendState();

    // NOW_PLAYING_INFO payload (see lib/protocol/PROTOCOL.md): BCD per digit, 0xF = blank digit,
    // 0xFF = unchanged, 0xFE = blank the field, empty payload = clear everything unconditionally, [0x01] = clear only if the base's number is still on the bus. Always a single frame.
    void applyNowPlaying(const uint8_t *payload, uint8_t len);
    void clear();
    // Clears only if the number on the bus is still the one the base wrote (A3). A number another master put on the
    // bus after ours stays. NOW_PLAYING_INFO [NPI_CLEAR_IF_OWN] and the PC-loss path use this.
    void clearIfOwn();

    void setSong(uint16_t song, bool visible);
    void setVerse(uint8_t verse, bool visible);
    void setLetter(char letter, bool visible);
    void setLed(ledColor led);

    // OLED text per digit, space = blanked digit (" 401", "1 ")
    void formatSong(char (&out)[5]);
    void formatVerse(char (&out)[3]);
    char getLetter(); // 'A'-'D' or ' '
    ledColor getLed();
    bool isConnected();
    bool sendToMIDI();

}

#endif
