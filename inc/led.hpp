#ifndef LED_H
#define LED_H

#include <cstdint>
#include <array>
#include <cstddef>

namespace LED{

    // Indexy pixelů na zadním LED pásku
    constexpr uint8_t PIXEL_USB       = 0;
    constexpr uint8_t PIXEL_DISPLAY   = 1;
    constexpr uint8_t PIXEL_CURRENT   = 2;
    constexpr uint8_t PIXEL_MIDIA     = 3;
    constexpr uint8_t PIXEL_MIDIB     = 4;
    constexpr uint8_t PIXEL_BLUETOOTH = 5;

    void init(void);
    void update();
    void notifyActivity(uint8_t pixelIndex);
    void updateStatus();


    class Color{
        private:
            uint8_t r_, g_, b_;   
            float intensity_;
            void rgb2hsv(uint8_t r, uint8_t g, uint8_t b, double & h, double & s, double & v);
            void hsv2rgb(double h, double s, double v, uint8_t & r, uint8_t & g, uint8_t & b);
        public:
            Color(uint8_t r, uint8_t g, uint8_t b, float intensity = 1.0f) : r_(r), g_(g), b_(b), intensity_(intensity) { };
            uint32_t raw(){ 
                return (((uint8_t)((float)r_*intensity_) << 8) | 
                        ((uint8_t)((float)g_*intensity_) << 16) | 
                        (uint8_t)((float)b_*intensity_)); 
            };
            void shiftHue(int16_t degrees);
            void setIntensity(float intensity) { intensity_ = intensity; };
            float getIntensity() const { return intensity_; };
    };

    class Pixel{
        private:
            Color color_;
            bool turnedOn_;
        public:
            Pixel(Color color, bool turnedOn = true) : color_(color), turnedOn_(turnedOn) {}
            Color & getColor(){ return color_; };
            void setColor(Color color){ color_ = color; update(); };
            void setColorQuiet(Color color){ color_ = color; };
            void setIntensity(float intensity) { color_.setIntensity(intensity);  update();  };
            void shiftHue(int16_t degrees) { if(color_.raw() != 0) color_.shiftHue(degrees); update(); };
            bool isOn() const { return turnedOn_; };
            void toggle() { turnedOn_ = !turnedOn_; update(); };
            void on() { turnedOn_ = true; update(); };
            void off() { turnedOn_ = false; update(); };
    };

    template<size_t N>
    class Strip{
        private:
            std::array<Pixel, N> pixels_;

        public: 
            Strip(std::array<Pixel, N> pixels) : pixels_(pixels) {};
            std::array<Pixel, N> & getPixels() { return pixels_; };
            void setIntensity(float intensity) { for (Pixel &p : pixels_) { p.setIntensity(intensity); } };
            void setColor(Color color) { for (Pixel &p : pixels_) { p.setColor(color); } };
            void shiftHue(int16_t degrees) { for (Pixel &p : pixels_) { p.shiftHue(degrees); } };
    };

    void colorToTiming(Color & color, uint8_t * timingBuffer);
	extern Strip<6> rearStrip;
    extern Strip<4> frontStrip;

}

#endif
