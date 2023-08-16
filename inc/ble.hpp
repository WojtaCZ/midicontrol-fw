
#ifndef BLE_H
#define BLE_H

#include <string>
#include <stdint.h>

namespace BLE{
    void init(void);
    void send(std::string data);
    bool isConnected();
}

// Wrappers to allow sending data from C files
extern "C" void ble_send(char * data, int len);
extern "C" bool ble_isConnected();
extern "C" void ble_loadBuffer(char * data, int len);

#endif