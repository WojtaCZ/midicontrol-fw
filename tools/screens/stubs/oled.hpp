#pragma once
// Host stand-in for inc/oled.hpp: a plain 130x64 framebuffer (SH1106 RAM columns 0..129;
// the visible 128 columns start at RAM column 2, see theme.displayOffsetX)
#include <gfx/framebuffer.hpp>
#include <cstdint>

namespace Oled {
	class OledBuffer : public gfx::IFrameBuffer<uint8_t> {
	  public:
		static constexpr int WIDTH = 130, HEIGHT = 64;
		uint8_t getPixel(int x, int y) const override { return inside(x, y) ? px_[y][x] : 0; }
		void setPixel(int x, int y, uint8_t c) override {
			if (inside(x, y)) px_[y][x] = c ? 1 : 0;
		}
		int getWidth() const override { return WIDTH; }
		int getHeight() const override { return HEIGHT; }

	  private:
		static bool inside(int x, int y) { return x >= 0 && x < WIDTH && y >= 0 && y < HEIGHT; }
		uint8_t px_[HEIGHT][WIDTH] = {};
	};

	void update();
	bool isSleeping();
	void wakeupCallback();
	void flush(uint32_t timeoutMs);
}
