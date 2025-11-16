#ifndef DEBUG_HPP
#define DEBUG_HPP

#include <cstdint>
#include <string>

namespace Debug {

    enum class Level : uint8_t {
        NONE = 0,
        ERROR = 1,
        WARNING = 2,
        INFO = 3,
        DEBUG = 4
    };

    Level setLevel(Level level);
    Level getLevel();

    void log(Level level, const char* message, int len);
    void log(Level level, std::string message);

}

#endif