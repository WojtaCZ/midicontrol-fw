
#include "debug.hpp"

#include <stdio.h>
#include <string.h>

#include <stm32g431xx.h>
#include <core_cm4.h>
#include <cmsis_compiler.h>
#include <tusb.h>


namespace Debug {
    static Level _currentLevel = Level::NONE;
    char _logBuffer[1024];

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

            snprintf(_logBuffer, sizeof(_logBuffer), "[%s]: %.*s\n", levelString, len, message);

            tud_cdc_write_str(_logBuffer);
        }
    }

    void log(Level level, const char* message) {
        log(level, message, strlen(message));
    }
}
