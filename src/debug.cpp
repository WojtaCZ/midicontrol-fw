
#include "debug.hpp"

#include <stdio.h>
#include <string.h>
#include <string>

#include <stm32g431xx.h>
#include <core_cm4.h>
#include <cmsis_compiler.h>
#include <tusb.h>


using namespace std;

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
            std::string levelString;

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
            
            sprintf( "[%s]: %.*s\n", levelString.c_str(), len, message);

            tud_cdc_write_str(_logBuffer);
        }
    }

    void log(Level level, std::string message) {
        log(level, message.c_str(), message.length());
    }
} 
