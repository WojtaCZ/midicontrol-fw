// Scripted UI simulator for the base: runs the real GUI code (src/menu.cpp is included as one
// translation unit to reach the screen objects) against the host stubs and executes a flow script.
// Usage: sim <script> [out.jsonl]      exit code 1 if any expect fails
//
// Script (one command per line, '#' starts a comment):
//   key <up|down|enter|back|left|right|0-9>   one key event (the encoder only produces up/down/enter)
//   tick <ms>                                 time passes: 30 ms render steps, one-shot schedulers fire,
//                                             the splash auto-dismisses at 3 s, the OLED sleeps after 30 s idle
//   state <k>=<v>                             pc, connected, ble, remote ("-" = none), display, song, verse,
//                                             letter, sleeping, power, led, midi
//   event <name> [args]                       playing <song> | stopped | anchor <label> | songs <a;b;c|-> [more]
//                                             | songs_delay | songs_timeout | pairing <text> | fw <transfer|install|error> <pct>
//                                             | fw_end | fail <GET_SONG_LIST|PLAY_SONG|STOP_SONG> <text> | sync_power
//   expect screen <id>                        id: splash main settings firmware display bluetooth pairing
//                                             songs_loading songs nowplaying fwupdate nopc
//   expect text <line> | expect notext <line> exact match of one drawn text line (trimmed; a line wider
//                                             than the display shows its clipped window, as on the OLED)
//   expect scrolled <line>                    advances time (<= 12 s) until a scrolling line was seen from start to end
//   expect var <k>=<v>                        requests lastOffset played stops power led midi sleeping
// Output: one JSON object per step: step, cmd, screen, text[] (drawn text, top to bottom), frame (FNV-1a of
// the visible 128x64 pixels), ok.

#include "host.hpp"
#include "../../src/menu.cpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>


namespace GUI {
using E = ui::InputEvent;

static int		  g_requests = 0, g_lastOffset = -1, g_stops = 0;
static std::string g_played;

static stmcpp::scheduler::Scheduler splashSched(3000_ms, [] { GUI::splashAutoDismiss(); }, false, false);

static const char *screenId() {
	ui::Screen *c = nav.current();
	if (c == &splashScreen) return "splash";
	if (c == &mainScreen) return "main";
	if (c == &settingsScreen) return "settings";
	if (c == &firmwareScreen) return "firmware";
	if (c == &displayScreen) return "display";
	if (c == &bluetoothScreen) return "bluetooth";
	if (c == &pairStatusScreen) return "pairing";
	if (c == &songListLoadingScreen) return "songs_loading";
	if (c == &songListScreen) return "songs";
	if (c == &nowPlayingScreen) return "nowplaying";
	if (c == &fwProgressScreen) return "fwupdate";
	if (c == &noPcScreen) return "nopc";
	return "unknown";
}

static std::string trim(std::string s) {
	size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
	return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

static std::vector<std::string> lines; // visible text of the last frame
static std::string				frameHash;

static void snapshot() {
	host::texts.clear();
	GUI::renderForce();
	GUI::render();
	std::vector<host::Text> t = host::texts;
	std::stable_sort(t.begin(), t.end(), [](const host::Text &a, const host::Text &b) { return a.y != b.y ? a.y < b.y : a.x < b.x; });
	lines.clear();
	for (auto &x : t) lines.push_back(x.s);
	uint64_t h = 1469598103934665603ull;
	for (int y = 0; y < 64; y++)
		for (int x = 0; x < 128; x++) {
			h ^= frameBuffer.getPixel(x + 2, y) ? 0x31 : 0x30;
			h *= 1099511628211ull;
		}
	char buf[24];
	std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(h));
	frameHash = buf;
}

static void tick(uint32_t ms) {
	while (ms) {
		uint32_t d = ms < 30 ? ms : 30;
		ms -= d;
		stmcpp::systick::ticks_ += d;
		stmcpp::scheduler::Scheduler::advance(d);
		if (!host::state.sleeping && stmcpp::systick::ticks_ - host::state.lastWake >= 30000) host::state.sleeping = true;
		GUI::render();
	}
}

static bool parseKey(const std::string &n, E &e) {
	static const struct { const char *n; E e; } K[] = {
		{"up", E::Up}, {"down", E::Down}, {"left", E::Left}, {"right", E::Right}, {"enter", E::Enter}, {"back", E::Back},
		{"0", E::Digit0}, {"1", E::Digit1}, {"2", E::Digit2}, {"3", E::Digit3}, {"4", E::Digit4},
		{"5", E::Digit5}, {"6", E::Digit6}, {"7", E::Digit7}, {"8", E::Digit8}, {"9", E::Digit9}};
	for (auto &k : K)
		if (n == k.n) { e = k.e; return true; }
	return false;
}

static std::string json(const std::string &s) {
	std::string o = "\"";
	for (char c : s) {
		if (c == '"' || c == '\\') { o += '\\'; o += c; }
		else if (static_cast<unsigned char>(c) < 0x20) o += ' ';
		else o += c;
	}
	return o + "\"";
}

static bool boolv(const std::string &v) { return v == "1" || v == "true" || v == "yes"; }

static const char *remoteStore = nullptr;
static std::string remoteBuf;

// Returns "" on success, else an error text
static std::string setState(const std::string &k, const std::string &v) {
	auto &s = host::state;
	if (k == "pc") s.pcAlive = boolv(v);
	else if (k == "connected") s.connected = boolv(v);
	else if (k == "ble") s.bleUp = boolv(v);
	else if (k == "display") s.display = boolv(v);
	else if (k == "remote") { remoteBuf = v == "-" ? "" : v; s.remoteAddr = remoteBuf.c_str(); }
	else if (k == "song") s.song = static_cast<uint16_t>(std::atoi(v.c_str()));
	else if (k == "verse") s.verse = static_cast<uint8_t>(std::atoi(v.c_str()));
	else if (k == "letter") s.letter = v.empty() ? ' ' : v[0];
	else if (k == "sleeping") s.sleeping = boolv(v);
	else if (k == "power") { s.power = boolv(v); GUI::syncPowerCheckbox(); }
	else if (k == "led") s.ledMaster = boolv(v);
	else if (k == "midi") s.midiRole = boolv(v);
	else return "unknown state key " + k;
	return "";
}

static std::string getVar(const std::string &k) {
	auto &s = host::state;
	if (k == "requests") return std::to_string(g_requests);
	if (k == "lastOffset") return std::to_string(g_lastOffset);
	if (k == "played") return g_played;
	if (k == "stops") return std::to_string(g_stops);
	if (k == "power") return s.power ? "1" : "0";
	if (k == "led") return s.ledMaster ? "1" : "0";
	if (k == "midi") return s.midiRole ? "1" : "0";
	if (k == "sleeping") return s.sleeping ? "1" : "0";
	return "?";
}

static std::string event(const std::string &name, const std::string &rest) {
	std::istringstream is(rest);
	std::string a, b;
	is >> a;
	std::getline(is, b);
	b = trim(b);
	static std::string keep[4]; // payloads the GUI keeps pointers to
	if (name == "playing") { keep[0] = rest; GUI::showNowPlaying(keep[0].c_str()); }
	else if (name == "stopped") GUI::dismissNowPlaying();
	else if (name == "anchor") { keep[1] = rest; GUI::setNowPlayingAnchor(keep[1].c_str()); }
	else if (name == "songs") {
		std::string list = a == "-" ? "" : a;
		GUI::handleSongListResponse(list.c_str(), static_cast<uint8_t>(list.size()), b == "more");
	}
	else if (name == "songs_delay") GUI::songListDelayShow();
	else if (name == "songs_timeout") GUI::songListTimeout();
	else if (name == "pairing") GUI::showPairingResult((keep[2] = rest).c_str());
	else if (name == "fw") {
		const char *h = a == "transfer" ? "Prenos do ovladace" : a == "install" ? "Instalace ovladace" : a == "error" ? "Chyba aktualizace" : nullptr;
		if (!h) return "fw: transfer|install|error";
		GUI::showFwProgress(h, static_cast<uint8_t>(std::atoi(b.c_str())));
	}
	else if (name == "fw_end") GUI::hideFwProgress();
	else if (name == "fail") {
		protocol::MsgType t;
		if (a == "GET_SONG_LIST") t = protocol::MsgType::GET_SONG_LIST;
		else if (a == "PLAY_SONG") t = protocol::MsgType::PLAY_SONG;
		else if (a == "STOP_SONG") t = protocol::MsgType::STOP_SONG;
		else return "fail: GET_SONG_LIST|PLAY_SONG|STOP_SONG";
		GUI::requestFailed(t, (keep[3] = b).c_str());
	}
	else if (name == "sync_power") GUI::syncPowerCheckbox();
	else return "unknown event " + name;
	return "";
}

static int runScript(const char *path, const char *outPath) {
	std::ifstream in(path);
	if (!in) { std::fprintf(stderr, "sim: cannot open %s\n", path); return 2; }
	FILE *out = outPath ? std::fopen(outPath, "w") : stdout;
	if (!out) { std::fprintf(stderr, "sim: cannot write %s\n", outPath); return 2; }

	stmcpp::systick::ticks_ = 10000;
	host::state.lastWake = 10000;
	GUI::init();
	GUI::setRequestSongListCallback([](uint16_t off) { host::state.requests++; g_requests++; g_lastOffset = off; });
	GUI::setPlaySongCallback([](const char *n) { g_played = n; });
	GUI::setStopCallback([] { g_stops++; });
	splashSched.start();
	snapshot();

	std::string line;
	int step = 0, failures = 0, lineNo = 0;
	while (std::getline(in, line)) {
		lineNo++;
		size_t h = line.find('#');
		if (h != std::string::npos) line.resize(h);
		line = trim(line);
		if (line.empty()) continue;
		std::istringstream is(line);
		std::string cmd, rest;
		is >> cmd;
		std::getline(is, rest);
		rest = trim(rest);
		std::string err;
		bool		isExpect = false;

		if (cmd == "key") {
			E e;
			if (!parseKey(rest, e)) err = "unknown key " + rest;
			else { GUI::keypress(e); GUI::render(); }
		} else if (cmd == "tick") {
			tick(static_cast<uint32_t>(std::atoi(rest.c_str())));
		} else if (cmd == "state") {
			size_t eq = rest.find('=');
			err = eq == std::string::npos ? "state k=v" : setState(trim(rest.substr(0, eq)), trim(rest.substr(eq + 1)));
		} else if (cmd == "event") {
			std::istringstream es(rest);
			std::string name, args;
			es >> name;
			std::getline(es, args);
			err = event(name, trim(args));
		} else if (cmd == "expect") {
			isExpect = true;
			std::istringstream es(rest);
			std::string what, arg;
			es >> what;
			std::getline(es, arg);
			arg = trim(arg);
			snapshot();
			if (what == "screen") {
				if (arg != screenId()) err = "expected screen " + arg + ", got " + screenId();
			} else if (what == "text" || what == "notext") {
				bool found = std::find_if(lines.begin(), lines.end(), [&](const std::string &l) { return trim(l) == arg; }) != lines.end();
				if (what == "text" && !found) err = "text not shown: [" + arg + "]";
				if (what == "notext" && found) err = "text shown: [" + arg + "]";
			} else if (what == "scrolled") {
				// long text scrolls: advance up to 12 s and require the start and the end of the line to be seen
				bool head = false, tail = false;
				for (int i = 0; i < 400 && !(head && tail); i++) {
					for (auto &l : lines) {
						std::string w = trim(l);
						if (w.empty()) continue;
						if (w == arg) head = tail = true;
						else if (arg.compare(0, w.size(), w) == 0) head = true;
						else if (arg.size() >= w.size() && arg.compare(arg.size() - w.size(), w.size(), w) == 0) tail = true;
					}
					if (head && tail) break;
					tick(30);
					snapshot();
				}
				if (!(head && tail)) err = std::string("line never fully visible while scrolling: [") + arg + "]";
			} else if (what == "var") {
				size_t eq = arg.find('=');
				if (eq == std::string::npos) err = "var k=v";
				else {
					std::string k = trim(arg.substr(0, eq)), v = trim(arg.substr(eq + 1)), g = getVar(k);
					if (g != v) err = "var " + k + " = [" + g + "], expected [" + v + "]";
				}
			} else err = "unknown expect " + what;
		} else err = "unknown command " + cmd;

		if (!isExpect) snapshot();
		step++;
		if (!err.empty()) failures++;
		std::fprintf(out, "{\"step\":%d,\"line\":%d,\"cmd\":%s,\"t\":%u,\"screen\":\"%s\",\"text\":[", step, lineNo, json(line).c_str(),
					 static_cast<unsigned>(stmcpp::systick::ticks_ - 10000), screenId());
		for (size_t i = 0; i < lines.size(); i++) std::fprintf(out, "%s%s", i ? "," : "", json(lines[i]).c_str());
		std::fprintf(out, "],\"frame\":\"%s\",\"ok\":%s", frameHash.c_str(), err.empty() ? "true" : "false");
		if (!err.empty()) {
			std::fprintf(out, ",\"error\":%s", json(err).c_str());
			std::fprintf(stderr, "%s:%d: FAIL: %s\n", path, lineNo, err.c_str());
		}
		std::fprintf(out, "}\n");
	}
	if (outPath) std::fclose(out);
	std::fprintf(stderr, "%s: %d steps, %d failed\n", path, step, failures);
	return failures ? 1 : 0;
}
} // namespace GUI

int main(int argc, char **argv) {
	if (argc < 2) {
		std::fprintf(stderr, "usage: %s <script> [out.jsonl]\n", argv[0]);
		return 2;
	}
	return GUI::runScript(argv[1], argc > 2 ? argv[2] : nullptr);
}
