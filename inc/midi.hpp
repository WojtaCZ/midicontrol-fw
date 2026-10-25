#ifndef MIDI_H
#define MIDI_H

#include <cstdint>

namespace midi{
    void init(void);

    // USB → DIN (organ), blocking send of one USB-MIDI message
    void send(uint8_t (&packet)[4]);

    // Main loop: DIN → USB and our own SysEx into TinyUSB (never from an interrupt)
    void process();

    // Queues our own SysEx (USB-MIDI packets) for sending to the PC; it is sent whole and not interleaved
    // with DIN SysEx. Returns false when the previous one has not been sent yet.
    bool queueUsbSysex(const uint8_t (*packets)[4], uint8_t count);
}

#endif
