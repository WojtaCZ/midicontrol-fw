// Screen export tool for the base: runs the real GUI code (src/menu.cpp, lib/ui, lib/gfx) on
// the host with stubbed hardware and saves every screen as a PNG plus an index.html contact
// sheet and sheet.png. Usage: screens <output dir> [scale]
// (tools/screens/export.sh builds and runs it).

#include "host.hpp"
#include "menu.hpp"
#include "oled.hpp"
#include "screenshot.hpp"

#include <stmcpp/systick.hpp>

#include <cstdlib>
#include <cstring>

extern Oled::OledBuffer frameBuffer;

using E = ui::InputEvent;

static shot::Exporter *exporter = nullptr;
static int			   counter	= 0;

// Song codes as the PC player lists them (file stems in midicontrolpy-sw/songs)
static const char SONGS[] = "401;403;406A;406B;407;512;513;516;517;519;523";

static void snap(const char *name, const char *caption) {
	GUI::renderForce();
	GUI::render();
	char file[64];
	std::snprintf(file, sizeof(file), "%02d_%s", ++counter, name);
	exporter->save(file, caption, 128, 64, [](int x, int y) { return frameBuffer.getPixel(x + 2, y) != 0; });
}

static void key(E event) {
	GUI::keypress(event);
	GUI::render();
}

static void down(int n) {
	for (int i = 0; i < n; i++) key(E::Down);
}

// Lets the GUI run for ms milliseconds (text scrolling); render() advances it by 30 ms
static void run(int ms) {
	for (int t = 0; t < ms; t += 30) {
		stmcpp::systick::ticks_ += 30;
		GUI::render();
	}
}

static void songList(const char *list, bool hasMore) {
	GUI::handleSongListResponse(list, static_cast<uint8_t>(std::strlen(list)), hasMore);
	GUI::render();
}

int main(int argc, char **argv) {
	if (argc < 2) {
		std::fprintf(stderr, "usage: %s <output dir> [scale]\n", argv[0]);
		return 2;
	}
	shot::Exporter ex(argv[1], argc > 2 ? std::atoi(argv[2]) : 4);
	exporter = &ex;
	stmcpp::systick::ticks_ = 10000; // BLE activity animation idle

	GUI::init();
	GUI::setRequestSongListCallback([](uint8_t) { host::state.requests++; });
	GUI::setPlaySongCallback([](const char *) {});
	GUI::setStopCallback([] {});
	snap("splash", "Splash after power-on");

	key(E::Enter); // the splash timer presses Enter
	snap("main_play", "Main menu, Prehraj selected");
	key(E::Down);
	snap("main_record", "Main menu, Nahraj selected");
	key(E::Down);
	snap("main_settings", "Main menu, Nastaveni selected");

	key(E::Enter);
	snap("settings_led", "Settings: LED (checkbox)");
	key(E::Down);
	snap("settings_midi", "Settings: Ident. MIDI (checkbox)");
	key(E::Down);
	snap("settings_power", "Settings: organ power (checkbox, off)");
	key(E::Enter);
	snap("settings_power_on", "Settings: organ power on");
	key(E::Enter);
	key(E::Down);
	snap("settings_firmware", "Settings: Verze FW");
	key(E::Down);
	snap("settings_display", "Settings: Displej");
	key(E::Down);
	snap("settings_pair", "Settings: Sparovat ovladac");
	run(4000);
	snap("settings_pair_scrolled", "Settings: long item scrolled");
	key(E::Down);
	snap("settings_back", "Settings: Zpet (back)");

	key(E::Up);
	key(E::Up);
	key(E::Up);
	key(E::Enter);
	snap("info_firmware", "Firmware info");
	key(E::Enter);
	key(E::Down);
	key(E::Enter);
	snap("info_display", "Song display info");
	key(E::Enter);
	key(E::Down);
	key(E::Enter);
	snap("pairing_search", "Pairing: searching for the remote");
	GUI::showPairingResult("Sparovano");
	snap("pairing_done", "Pairing: paired");
	GUI::showPairingResult("Ovladac nenalezen");
	snap("pairing_not_found", "Pairing: remote not found");
	key(E::Enter);
	key(E::Down);
	key(E::Enter);

	// Song list
	key(E::Up);
	key(E::Up);
	key(E::Up);
	key(E::Enter);
	GUI::songListDelayShow();
	snap("songs_loading", "Song list, loading");
	songList(SONGS, false);
	snap("songs_first", "Song list, first entry");
	down(2);
	snap("songs_middle", "Song list, middle");
	down(20);
	snap("songs_last", "Song list, end (Zpet)");
	key(E::Enter);
	key(E::Enter);
	GUI::songListTimeout();
	snap("songs_timeout", "Song list, PC does not answer");
	key(E::Enter);
	key(E::Enter);
	songList("", false);
	snap("songs_empty", "Song list, empty folder");
	key(E::Enter);

	// Playback
	GUI::showNowPlaying("406A");
	GUI::updateNowPlayingInfo();
	snap("now_playing", "Now playing, display state");
	GUI::setNowPlayingAnchor("Sloka 2");
	snap("now_playing_anchor", "Now playing, section from the PC");
	GUI::dismissNowPlaying();

	GUI::showFwProgress("Prenos do ovladace", 42);
	snap("fw_remote_update", "Remote firmware update relayed");
	GUI::hideFwProgress();

	host::state.pcAlive = false;
	snap("no_pc", "PC not connected: overlay over the main menu");
	key(E::Enter);
	snap("no_pc_settings", "PC not connected: Nastaveni opened from the overlay");
	down(5);
	key(E::Enter);
	snap("no_pc_pairing", "PC not connected: pairing from Nastaveni");
	key(E::Enter);
	key(E::Down);
	key(E::Enter);
	snap("no_pc_back", "PC not connected: Zpet returns to the overlay");
	host::state.pcAlive = true;
	snap("pc_back", "PC connected again: main menu");
	host::state.connected = false;
	snap("status_no_remote", "Main menu, remote not connected");
	host::state.remoteAddr = "";
	snap("main_unpaired", "Main menu, no remote paired yet");

	ex.writeSheet();
	ex.writeIndex("MIDIControl base - screens");
	return 0;
}
