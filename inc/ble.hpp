
#ifndef BLE_H
#define BLE_H

#include <string>

namespace BLE{
    void init(void);
    void send(std::string data);
    bool isConnected();
}


#endif