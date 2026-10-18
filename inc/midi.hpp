#ifndef MIDI_H
#define MIDI_H

#include <cstdint>

namespace midi{
    void init(void);

    // USB → DIN (varhany), blokující odeslání jedné USB-MIDI zprávy
    void send(uint8_t (&packet)[4]);

    // Hlavní smyčka: DIN → USB a vlastní SysEx do TinyUSB (nikdy z přerušení)
    void process();

    // Zařadí vlastní SysEx (USB-MIDI pakety) k odeslání do PC; odešle se celý a neprokládaně
    // s DIN SysEx. Vrací false, když předchozí ještě neodešel.
    bool queueUsbSysex(const uint8_t (*packets)[4], uint8_t count);
}

#endif
