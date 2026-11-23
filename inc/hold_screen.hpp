#pragma once
// Hold-to-confirm screen: the title, a loading bar and a hint ("Drzte 3 s"). ONE look on the base and the remote
// (identical file in both repos): it is ui::drawProgress, the same bar as the firmware update and the remote's old
// power-off screen. Feed it with holdtimer::Timer::percent() (src/logic/hold_timer.hpp).
// Used by: remote power off (src/menu.cpp powerHoldScreen), base organ off while playing / recording (GUI::showHold).
// Reuse: construct one static HoldScreen, call set(title, hint) when it is shown and setPercent() while the button is held.

#include <ui/progress.hpp>
#include <ui/screen.hpp>
#include <ui/theme.hpp>

class HoldScreen : public ui::Screen {
public:
	HoldScreen(const char* title, const char* hint) : _title(title), _hint(hint) {}

	void set(const char* title, const char* hint) {
		_title = title;
		_hint  = hint;
		markDirty();
	}

	void setPercent(uint8_t percent) {
		if (percent > 100) percent = 100;
		if (percent != _percent) {
			_percent = percent;
			markDirty();
		}
	}

	uint8_t percent() const { return _percent; }

	// The keys do nothing here (the owner of the screen decides, e.g. GUI::keypress cancels on Up/Down)
	bool handleInput(ui::InputEvent) override { return true; }

	void render(gfx::IFrameBuffer<uint8_t>& fb, const ui::Theme& theme) override {
		if (theme.smallFont) ui::drawProgress(fb, *theme.smallFont, theme.displayWidth, _title, _hint, _percent);
		clearDirty();
	}

private:
	const char* _title;
	const char* _hint;
	uint8_t		_percent = 0;
};
