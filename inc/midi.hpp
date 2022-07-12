#ifndef MIDI_H
#define MIDI_H

#include <cstdint>
#include <vector>

using namespace std;

namespace MIDI{
    void init(void);
    void send(vector<byte> data);
}

//Wrapper for usb.h to call
extern "C" void midi_send(char * data, int len);

#endif