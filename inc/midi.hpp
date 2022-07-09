#ifndef MIDI_H
#define MIDI_H

#include <stdint.h>
#include <vector>

using namespace std;

namespace MIDI{
    void init(void);
    void send(vector<byte> data);
}

#endif