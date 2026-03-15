#ifndef OLED_H
#define OLED_H

#include <stmcpp/scheduler.hpp>
#include <stmcpp/units.hpp>
#include <gfx/framebuffer.hpp>
#include <string>
#include <array>
#include <utility>

using namespace std;
using namespace stmcpp::units;

#define OLED_MEM_CMD			0x00
#define OLED_MEM_DAT			0x40
#define OLED_ADD                0x3C

#define OLED_WIDTH              130
#define OLED_HEIGHT             64

#define OLED_XOFFSET            2
#define OLED_YOFFSET            0

#define OLED_SLEEP_INTERVAL     5000_ms

//Velikost bufferu
#define OLED_SCREENBUF_SIZE		(OLED_WIDTH*OLED_HEIGHT / 8) + 8

namespace Oled{

    class OledBuffer : public gfx::IFrameBuffer<uint8_t> {
        public:
            OledBuffer();
            virtual ~OledBuffer() = default;

            // --- Pixel Logic ---
            uint8_t getPixel(int x, int y) const override;
            void setPixel(int x, int y, uint8_t color) override;

            // --- Metadata & Hardware ---
            int getWidth() const override;
            int getHeight() const override;
            uint8_t * getBufferAddress(int offset);
            int getBufferSize() const;

        private:
            // Dimensions
            static constexpr int WIDTH = 130;
            static constexpr int HEIGHT = 64;
            static constexpr int PAGES = 8; // 64 pixels / 8 bits

            // Memory Layout
            // 0x40 is the "Data" command byte for SSD1306/SH1106
            static constexpr int CONTROL_BYTES = 1;
            static constexpr int STRIDE = CONTROL_BYTES + WIDTH; // 131 bytes
            static constexpr int BUFFER_SIZE = STRIDE * PAGES;   // 1048 bytes

            // The actual container
            uint8_t buffer[BUFFER_SIZE];
    };

    enum class Color{
        BLACK,
        WHITE
    };

    inline Color operator!(const Color & c){
        if(c == Color::BLACK){
            return Color::WHITE;
        }else return Color::BLACK;
    }

    void setCoordinates(pair<uint16_t, uint16_t> coord);
    void sleep();
    void wakeup();
    bool isSleeping();
    bool isInitialized();
    bool isInverted();
    void setInitialized(bool state);

    void update();
    void init();

    void sleepCallback(void);
    void wakeupCallback(void);

}

#endif
