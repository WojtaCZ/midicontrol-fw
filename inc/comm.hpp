
#ifndef COMM_H
#define COMM_H

#include <string>
#include <cstdint>

using namespace std;

namespace communication{

    enum class direction {
        SET,
        GET,
        UNKNOWN
    };

    enum class command {
        BASE_STATUS,
        BASE_UID,
        BASE_CURRENT,
        DISPLAY_STATUS,
        DISPLAY_SONG,
        DISPLAY_VERSE,
        DISPLAY_LETTER,
        DISPLAY_LED,
        UNKNOWN
    };

    bool processBinaryCommand(command cmd, direction dir, uint16_t index, uint16_t value, char * params, uint32_t * length);    
    
}



#endif
