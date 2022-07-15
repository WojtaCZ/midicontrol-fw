#ifndef OLED_H
#define OLED_H

#include "scheduler.hpp"
#include "oled_fonts.hpp"
#include <string>
#include <array>
#include <utility>

using namespace std;

#define OLED_MEM_CMD			0x00
#define OLED_MEM_DAT			0x40
#define OLED_ADD                0x3C

#define OLED_WIDTH              130
#define OLED_HEIGHT             64

#define OLED_XOFFSET            2
#define OLED_YOFFSET            0

#define OLED_SLEEP_INTERVAL     5000

//Velikost bufferu
#define OLED_SCREENBUF_SIZE		(OLED_WIDTH*OLED_HEIGHT / 8) + 8

namespace Oled{

    enum class Color{
        BLACK,
        WHITE
    };

    inline Color operator!(const Color & c){
        if(c == Color::BLACK){
            return Color::WHITE;
        }else return Color::BLACK;
    }

    enum class Icon{
        UP_ARROW = 34,
        DOWN_ARROW = 35,
        LEFT_ARROW_UNSEL = 36,
        LEFT_ARROW_SEL = 37,
        CHECKBOX_SEL_UNCH = 40,
        CHECKBOX_SEL_CHCK = 41,
        CHECKBOX_UNSEL_UNCH = 38,
        CHECKBOX_UNSEL_CHCK = 39,
        DOT_SEL = 32,
        DOT_UNSEL = 33,
        SIG_4 = 18
    };

    void setCoordinates(pair<uint16_t, uint16_t> coord);
    void sleep();
    void wakeup();
    bool isSleeping();
    bool isInitialized();
    bool isInverted();
    void setInitialized(bool state);

    void update();
    void fill(Color color);
    void setCursor(pair<uint16_t, uint16_t> coord);
    void drawPixel(pair<uint16_t, uint16_t> coord, Color color);
    void writeSymbol(char c, FontDef font, Color color);
    void writeSymbol(Icon icon, FontDef font, Color color);
    void writeString(string str, FontDef font, Color color);
    void init();

    
    void sleepCallback(void);
    void wakeupCallback(void);
    
}




#endif
