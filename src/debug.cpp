
#include "debug.hpp"

#include <stdio.h>
#include <string.h>

#include <stm32g431xx.h>
#include <core_cm4.h>
#include <cmsis_compiler.h>


namespace Debug {
    static Level _currentLevel = Level::NONE;
    // Debug text se do USB-CDC neposílá: CDC nese výhradně protokolové rámce pro PC.
    // Log se ukládá do kruhového bufferu v RAM, čitelného debuggerem (Debug::_logRing).
    static constexpr size_t LOG_RING_SIZE = 512;
    char _logRing[LOG_RING_SIZE];
    size_t _logHead = 0;

    Level setLevel(Level level) {
        _currentLevel = level;
        return _currentLevel;
    }

    Level getLevel() {
        return _currentLevel;
    }

    void log(Level level, const char* message, int len) {
        if (level <= _currentLevel) {
            const char* levelString;

            switch (level) {
                case Level::ERROR:
                    levelString = "ERROR";
                    break;

                case Level::WARNING:
                    levelString = "WARNING";
                    break;

                case Level::INFO:
                    levelString = "INFO";
                    break;

                case Level::DEBUG:
                default:
                    levelString = "DEBUG";
                    break;
            }

            char line[96];
            int n = snprintf(line, sizeof(line), "[%s]: %.*s\n", levelString, len, message);
            if (n <= 0) return;
            if (n >= static_cast<int>(sizeof(line))) n = sizeof(line) - 1;

            for (int i = 0; i < n; ++i) {
                _logRing[_logHead] = line[i];
                _logHead = (_logHead + 1) % LOG_RING_SIZE;
            }
        }
    }

    void log(Level level, const char* message) {
        log(level, message, strlen(message));
    }
}
