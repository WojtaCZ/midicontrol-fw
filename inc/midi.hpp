#ifndef MIDI_H
#define MIDI_H

#include <cstdint>
#include <vector>

using namespace std;

namespace midi{
    void init(void);
    void send(uint8_t (&packet)[4]); 
}

//Wrapper for usb.h to call
extern "C" void midi_send(char * data, int len);

#endif