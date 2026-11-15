#pragma once
// Host shim over lib/gfx/inc/gfx/text.hpp: forwards drawText to the real renderer and records the
// string (screen position, font height) in host::texts, so the sim driver can list the visible
// text lines. Only the host tools see this file (stubs/ comes first in the include path).
#include "host.hpp"

#define drawText drawTextReal
#include_next <gfx/text.hpp>
#undef drawText

namespace gfx::text {
	template <typename PixelType, typename FontType>
	void drawText(gfx::IFrameBuffer<PixelType> &fb, const FontType &f, const char *text, int x, int y, gfx::Anchor anchor, PixelType color) {
		if (text && *text) host::texts.push_back({x, y, int(f.height()), text});
		drawTextReal(fb, f, text, x, y, anchor, color);
	}
}
