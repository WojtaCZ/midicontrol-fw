#pragma once
// Saves the visible part of the OLED framebuffer as a PNG and collects an index.html contact
// sheet. Uncompressed PNG (stored deflate blocks): no zlib or image library needed.
// Kept identical in midicontrol-fw and midicontroller-fw (tools/screens/).

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace shot {

	// Pixel colours of the rendered image (lit / unlit OLED pixel)
	constexpr uint8_t ON[3]	 = {0xE8, 0xF4, 0xFF};
	constexpr uint8_t OFF[3] = {0x08, 0x0A, 0x10};

	inline uint32_t crc32(const uint8_t *p, std::size_t n, uint32_t c = 0xFFFFFFFFu) {
		for (std::size_t i = 0; i < n; i++) {
			c ^= p[i];
			for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
		}
		return c;
	}

	inline void put32(std::vector<uint8_t> &v, uint32_t x) {
		v.push_back(uint8_t(x >> 24));
		v.push_back(uint8_t(x >> 16));
		v.push_back(uint8_t(x >> 8));
		v.push_back(uint8_t(x));
	}

	inline void chunk(std::vector<uint8_t> &out, const char *type, const std::vector<uint8_t> &data) {
		put32(out, static_cast<uint32_t>(data.size()));
		std::vector<uint8_t> body(type, type + 4);
		body.insert(body.end(), data.begin(), data.end());
		out.insert(out.end(), body.begin(), body.end());
		put32(out, crc32(body.data(), body.size()) ^ 0xFFFFFFFFu);
	}

	// Writes an RGB image (w*h*3 bytes) as PNG
	inline bool writePng(const std::string &path, int w, int h, const std::vector<uint8_t> &rgb) {
		std::vector<uint8_t> raw;
		raw.reserve(static_cast<std::size_t>(h) * (w * 3 + 1));
		for (int y = 0; y < h; y++) {
			raw.push_back(0); // filter: none
			raw.insert(raw.end(), rgb.begin() + y * w * 3, rgb.begin() + (y + 1) * w * 3);
		}

		// zlib stream of stored blocks
		std::vector<uint8_t> z = {0x78, 0x01};
		std::size_t pos = 0;
		do {
			std::size_t n = raw.size() - pos;
			if (n > 65535) n = 65535;
			bool last = pos + n == raw.size();
			z.push_back(last ? 1 : 0);
			z.push_back(uint8_t(n));
			z.push_back(uint8_t(n >> 8));
			z.push_back(uint8_t(~n));
			z.push_back(uint8_t(~n >> 8));
			z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n);
			pos += n;
		} while (pos < raw.size());
		uint32_t a = 1, b = 0;
		for (uint8_t c : raw) {
			a = (a + c) % 65521;
			b = (b + a) % 65521;
		}
		put32(z, (b << 16) | a);

		std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
		std::vector<uint8_t> ihdr;
		put32(ihdr, static_cast<uint32_t>(w));
		put32(ihdr, static_cast<uint32_t>(h));
		ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0}); // 8 bit, RGB
		chunk(png, "IHDR", ihdr);
		chunk(png, "IDAT", z);
		chunk(png, "IEND", {});

		FILE *f = std::fopen(path.c_str(), "wb");
		if (!f) return false;
		bool ok = std::fwrite(png.data(), 1, png.size(), f) == png.size();
		return std::fclose(f) == 0 && ok;
	}

	// One exported screen, for the contact sheet
	struct Entry {
		std::string file;
		std::string caption;
		int w, h;
		std::vector<uint8_t> bits; // 1 byte per OLED pixel
	};

	class Exporter {
	  public:
		// scale: every OLED pixel becomes scale x scale image pixels
		Exporter(std::string dir, int scale) : dir_(std::move(dir)), scale_(scale) {}

		// getPixel(x, y) for the visible 128x64 area, x = 0 is the leftmost visible column
		template <typename GetPixel>
		void save(const std::string &name, const std::string &caption, int w, int h, GetPixel getPixel) {
			std::vector<uint8_t> bits(static_cast<std::size_t>(w) * h);
			for (int y = 0; y < h; y++)
				for (int x = 0; x < w; x++) bits[static_cast<std::size_t>(y) * w + x] = getPixel(x, y) ? 1 : 0;
			int W = w * scale_, H = h * scale_;
			std::vector<uint8_t> rgb(static_cast<std::size_t>(W) * H * 3);
			for (int y = 0; y < H; y++)
				for (int x = 0; x < W; x++) {
					const uint8_t *c = bits[static_cast<std::size_t>(y / scale_) * w + x / scale_] ? ON : OFF;
					uint8_t *p		 = &rgb[(static_cast<std::size_t>(y) * W + x) * 3];
					p[0] = c[0], p[1] = c[1], p[2] = c[2];
				}
			std::string file = name + ".png";
			if (!writePng(dir_ + "/" + file, W, H, rgb)) std::fprintf(stderr, "cannot write %s/%s\n", dir_.c_str(), file.c_str());
			entries_.push_back({file, caption, w, h, std::move(bits)});
			std::printf("%-36s %s\n", file.c_str(), caption.c_str());
		}

		// index.html: every screen with its file name and caption
		void writeIndex(const std::string &title) const {
			FILE *f = std::fopen((dir_ + "/index.html").c_str(), "w");
			if (!f) return;
			std::fprintf(f,
						 "<!doctype html><meta charset=utf-8><title>%s</title>\n"
						 "<style>body{font:14px sans-serif;background:#222;color:#ddd;margin:16px}"
						 "div{display:inline-block;margin:8px;vertical-align:top;width:%dpx}"
						 "img{display:block;image-rendering:pixelated;border:1px solid #555}"
						 "b{display:block;margin-top:4px;font-family:monospace}</style>\n<h1>%s</h1>\n",
						 title.c_str(), 128 * scale_ + 2, title.c_str());
			for (const auto &e : entries_)
				std::fprintf(f, "<div><img src=\"%s\"><b>%s</b>%s</div>\n", e.file.c_str(), e.file.c_str(), e.caption.c_str());
			std::fclose(f);
		}

		// sheet.png: every screen in a grid (cols per row, scale 2, numbered in file order)
		void writeSheet(int cols = 4) const {
			if (entries_.empty()) return;
			const int s = 2, gap = 6, cw = entries_[0].w * s, ch = entries_[0].h * s;
			const int rows = static_cast<int>((entries_.size() + cols - 1) / cols);
			const int W = cols * (cw + gap) + gap, H = rows * (ch + gap) + gap;
			std::vector<uint8_t> rgb(static_cast<std::size_t>(W) * H * 3, 0x60);
			for (std::size_t i = 0; i < entries_.size(); i++) {
				const Entry &e = entries_[i];
				int ox = gap + static_cast<int>(i % cols) * (cw + gap), oy = gap + static_cast<int>(i / cols) * (ch + gap);
				for (int y = 0; y < e.h * s; y++)
					for (int x = 0; x < e.w * s; x++) {
						const uint8_t *c = e.bits[static_cast<std::size_t>(y / s) * e.w + x / s] ? ON : OFF;
						uint8_t *p		 = &rgb[(static_cast<std::size_t>(oy + y) * W + ox + x) * 3];
						p[0] = c[0], p[1] = c[1], p[2] = c[2];
					}
			}
			writePng(dir_ + "/sheet.png", W, H, rgb);
		}

	  private:
		std::string dir_;
		int scale_;
		std::vector<Entry> entries_;
	};

} // namespace shot
