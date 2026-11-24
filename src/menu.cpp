#include "menu.hpp"
#include <ui/confirm_screen.hpp>
#include <stmcpp/scheduler.hpp>
#include <stmcpp/units.hpp>
#include "oled.hpp"
#include "base.hpp"
#include "git.hpp"
#include "display.hpp"
#include "bluetooth.hpp"
#include "logic/ble_status.hpp"
#include "logic/now_playing.hpp"
#include "logic/splash_guard.hpp"
#include "logic/hold_timer.hpp"
#include "hold_screen.hpp"
#include "logic/song_window.hpp"
#include "settings.hpp"
#include "router.hpp"

#include <ui/progress_screen.hpp>
#include <ui/list.hpp>
#include <ui/text_scroll.hpp>
#include <stmcpp/systick.hpp>
#include <gfx/text.hpp>
#include <gfx/icon.hpp>
#include <gfx/shapes.hpp>

#include <cstdio>
#include <cstring>

using namespace stmcpp::units;

// External framebuffer defined in oled.cpp
extern Oled::OledBuffer frameBuffer;

using router::isPcAlive;

// Schedulers — defined at the end of this file
extern stmcpp::scheduler::Scheduler songListTimeoutScheduler;
extern stmcpp::scheduler::Scheduler songListDelayScheduler;

namespace GUI {

// ── Theme ─────────────────────────────────────────────────────────────

static ui::Theme theme;

static void initTheme() {
    theme.displayOffsetX = 2;
    theme.displayWidth  = 128;
    theme.displayHeight = 64;

    theme.titleFont = &gfx::text::Font_7x10;
    theme.itemFont  = &gfx::text::Font_11x18;
    theme.smallFont = &gfx::text::Font_7x10;

    theme.titleBarHeight = 12;
    theme.itemStartY     = 21;
    theme.itemSpacing    = 3;
    theme.leftMargin     = 2;
    theme.iconTextGap    = 3;
    theme.visibleItems   = 2;
    theme.scrollArrowX   = 117;
    theme.maxItemChars   = 9;

    theme.dotSelected        = ui::makeIconRef(dot_filled);
    theme.dotUnselected      = ui::makeIconRef(dot_outline);
    theme.arrowUp            = ui::makeIconRef(arrow_up);
    theme.arrowDown          = ui::makeIconRef(arrow_down);
    theme.backSelected       = ui::makeIconRef(back_filled);
    theme.backUnselected     = ui::makeIconRef(back_outline);
    theme.checkboxSelEmpty   = ui::makeIconRef(checkbox_filled);
    theme.checkboxSelChecked = ui::makeIconRef(checkbox_filled_checked);
    theme.checkboxUnselEmpty   = ui::makeIconRef(checkbox_empty);
    theme.checkboxUnselChecked = ui::makeIconRef(checkbox_empty_checked);

    theme.scrollPauseMs = 1000;
    theme.scrollStepMs  = 500;
}

// ── Navigator (declared early so callbacks can use it) ────────────────

static ui::Navigator nav;

// ── Callback forward declarations ─────────────────────────────────────

static void playCallback(ui::MenuItem* item, void* userData);
static void recordCallback(ui::MenuItem* item, void* userData);
static void powerCallback(ui::MenuItem* item, void* userData);
static void firmwareCallback(ui::MenuItem* item, void* userData);
static void displayCallback(ui::MenuItem* item, void* userData);
static void bluetoothCallback(ui::MenuItem* item, void* userData);
static void ledMasterCallback(ui::MenuItem* item, void* userData);
static void midiRoleCallback(ui::MenuItem* item, void* userData);
static void pairCallback(ui::MenuItem* item, void* userData);

// ── Settings menu items & screen (defined first so mainItems can reference it) ─

// C7 (Q2-29c): the service area; "Ident. MIDI" lives here, out of the normal user's way.
static ui::MenuItem serviceItems[] = {
    ui::MenuItem::checkbox("Ident. MIDI", true, midiRoleCallback),
    ui::MenuItem::back("Zpet"),
};

static ui::MenuScreen serviceScreen("Servis", serviceItems, 2);

static ui::MenuItem settingsItems[] = {
    ui::MenuItem::checkbox("LED",  true,  ledMasterCallback),
    ui::MenuItem::checkbox("Napajeni varhan", false, powerCallback),
    ui::MenuItem("Verze FW", firmwareCallback),
    ui::MenuItem("Displej",  displayCallback),
    ui::MenuItem("Bluetooth", bluetoothCallback),
    ui::MenuItem("Sparovat ovladac", pairCallback),
    ui::MenuItem("Servis", static_cast<ui::Screen*>(&serviceScreen)),
    ui::MenuItem::back("Zpet"),
};

static ui::MenuScreen settingsScreen("Nastaveni", settingsItems, 8);

// ── Main menu items & screen ──────────────────────────────────────────

static ui::MenuItem mainItems[] = {
    ui::MenuItem("Prehraj",  playCallback),
    ui::MenuItem("Nahraj",   recordCallback),
    ui::MenuItem("Nastaveni", static_cast<ui::Screen*>(&settingsScreen)),
};

static ui::MenuScreen mainScreen("Hlavni menu", mainItems, 3);

// ── Splash screen ─────────────────────────────────────────────────────

static ui::SplashScreen splashScreen("", "", "");

// ── No PC overlay ─────────────────────────────────────────────────────
// Covers the main menu while the PC player is not connected (everything there needs it).
// Enter opens Nastaveni; leaving Nastaveni comes back here. Screens pushed later (pairing,
// remote update, now playing) go on top of it.

static void onNoPcEnter(void* userData) {
    (void)userData;
    nav.push(&settingsScreen);
}

// D4/item 16: the big word is the SUBTITLE and the title row stays empty, so the BLE icon (top right) does not collide with a centered
// 11x18 title. Same layout as the remote noBaseScreen.
static ui::SplashScreen noPcScreen("", "Odpojeno", "Cekam na PC", onNoPcEnter);

// Pushes the overlay when the main menu is on top without a PC, removes it when it is on top and the PC is back
static void syncNoPcScreen() {
    // Without a stored remote the overlay hides the main menu title hint, so it carries the hint itself
    // (set only on a change: setComment marks the screen dirty and restarts the scroll)
    static const char* noPcComment = nullptr;
    const char* comment = settings::remoteAddr()[0] == '\0' ? "Sparuj ovladac" : "Cekam na PC";
    if (comment != noPcComment) {
        noPcComment = comment;
        noPcScreen.setComment(comment);
        Oled::wakeupCallback();
    }

    // The overlay appearing or going away is a state change: wake the display so it is seen
    ui::Screen* cur = nav.current();
    if (!isPcAlive()) {
        if (cur == &mainScreen) {
            nav.push(&noPcScreen);
            Oled::wakeupCallback();
        }
    } else if (cur == &noPcScreen) {
        nav.pop();
        Oled::wakeupCallback();
    }
}

// ── Paragraph screens (firmware / display info) ───────────────────────

static char fwLine0[40];
static char fwLine1[40];
static char fwLine2[40];
static char fwLine3[40];
static const char* fwLines[] = { fwLine0, fwLine1, fwLine2, fwLine3 };

static void onFirmwareDismiss(void* userData) {
    (void)userData;
    nav.pop();
}

static ui::ParagraphScreen firmwareScreen(fwLines, 4, onFirmwareDismiss);

static char dispLine0[40];
static char dispLine1[40];
static char dispLine2[40];
static char dispLine3[40];
static char dispLine4[40];
static const char* dispLines[] = { dispLine0, dispLine1, dispLine2, dispLine3, dispLine4 };

static void onDisplayDismiss(void* userData) {
    (void)userData;
    nav.pop();
}

static ui::ParagraphScreen displayScreen(dispLines, 5, onDisplayDismiss);

// Bluetooth info: values change at runtime, refreshed in render() while the screen is on top
static char bleLine0[40];
static char bleLine1[40];
static char bleLine2[40];
static char bleLine3[40];
static char bleLine4[40];
static const char* bleLines[] = { bleLine0, bleLine1, bleLine2, bleLine3, bleLine4 };

static ui::ParagraphScreen bluetoothScreen(bleLines, 5, onDisplayDismiss);

// Writes a line; returns true when the text changed
static bool setLine(char (&line)[40], const char* fmt, const char* value) {
    char buf[sizeof(line)];
    std::snprintf(buf, sizeof(buf), fmt, value);
    if (!std::strcmp(buf, line)) return false;
    std::memcpy(line, buf, sizeof(line));
    return true;
}

static void updateBluetoothInfo() {
    bool changed = false;
    const char* remote = settings::remoteAddr();
    // Module self-test instead of the fixed name: after a re-solder the joints can be checked without a probe
    bool starting = Bluetooth::configState() == Bluetooth::ConfigState::PENDING || Bluetooth::restartState() == Bluetooth::RestartState::RUNNING;
    changed |= setLine(bleLine0, "%s", blestatus::healthText(blestatus::health(starting, Bluetooth::isUp(), Bluetooth::resetConfirmed(), Bluetooth::rxByteCount())));
    changed |= setLine(bleLine1, "Spojeni: %s", Bluetooth::isConnected() ? "Ano" : "Ne");
    changed |= setLine(bleLine2, "Sparovano: %s", Bluetooth::isPeerAccepted() ? "Ano" : "Ne");
    changed |= setLine(bleLine3, "Ovladac: %s", router::isRemoteAlive() ? "Ano" : "Ne");
    changed |= setLine(bleLine4, "MAC: %s", remote[0] ? remote : "--");

    if (changed) bluetoothScreen.markDirty();
}

// ── Now Playing screen ─────────────────────────────────────────────────

static StopCallback _stopCallback = nullptr;
static AddVerseCallback _addVerseCallback = nullptr;
static bool _nowPlayingActive = false;

// ── Song list data ──────────────────────────────────────────────────────

// A9: no cap on the list: a WINDOW of songwin::Window::CAP entries; _win holds the absolute index (in the PC's list)
// of item 0 and the paging state (src/logic/song_window.hpp).
static constexpr int MAX_SONGS = songwin::Window::CAP;
static char _songNames[MAX_SONGS][24];       // 23 chars + null
static ui::MenuItem _songListItems[MAX_SONGS + 1]; // items + Back
static songwin::Window _win;
static bool _loadingScreenShown = false;

using SongListState = songwin::Window::State;

static RequestSongListCallback _requestSongListCallback = nullptr;
static PlaySongCallback _playSongCallback = nullptr;

// PC section label -> display text: "Sloka 2" -> "Sloka: 2", "Sloka 2 +3" (3 repeats pending)
// -> "Sloka: 2 (x3)", "Predehra +1" -> "Predehra (x1)"; anything else is kept as it is
static void formatSection(const char* label, char* out, size_t size) {
    auto isNumber = [](const char* b, const char* e) {
        if (b == e) return false;
        for (const char* c = b; c < e; c++) if (*c < '0' || *c > '9') return false;
        return true;
    };
    const char* end = label + std::strlen(label);

    // Trailing " +N": repeats left
    const char* reps = nullptr;
    const char* sp = end;
    while (sp > label && sp[-1] != ' ') sp--;
    if (sp > label && sp[0] == '+' && isNumber(sp + 1, end)) {
        reps = sp + 1;
        end  = sp - 1;
    }

    // Trailing " N": the verse number
    const char* num = end;
    while (num > label && num[-1] != ' ') num--;
    int n;
    if (num > label && isNumber(num, end)) {
        n = std::snprintf(out, size, "%.*s: %.*s", (int)(num - 1 - label), label, (int)(end - num), num);
    } else {
        n = std::snprintf(out, size, "%.*s", (int)(end - label), label);
    }
    if (reps && n >= 0 && (size_t)n < size) std::snprintf(out + n, size - n, " (x%s)", reps);
}

// Same layout as the remote's player: the state in the title, the song code in the big
// font, the section from the PC centered below it ("Sloka: 2 (x1)") and the Stop icon button,
// the only input here (OK). After SIGNAL_STOPPED it shows "Zastaveno" for a moment.
class NowPlayingScreen : public ui::Screen {
public:
    using State = nowplaying::State; // transitions and timeouts: src/logic/now_playing.*

    static constexpr int INFO_CHARS = 18; // one 7x10 line

    void start(const char* code) {
        if (std::strcmp(code, code_) != 0) machine_.forgetAnchor();
        std::strncpy(code_, code, sizeof(code_) - 1);
        code_[sizeof(code_) - 1] = '\0';
        info_[0] = '\0';
        machine_.start();
        markDirty();
    }

    // Section label from SIGNAL_ANCHOR
    void setSection(const char* label) {
        formatSection(label, info_, sizeof(info_));
        machine_.anchorSeen();
        infoScroll_.reset();
        markDirty();
    }

    void stopped() {
        machine_.stopped(stmcpp::systick::getTicks());
        markDirty();
    }

    // STOP_SONG failed (PM-1): "Prehravam" again with the reason in the info line; OK sends STOP again
    void stopFailed(const char* text) {
        if (machine_.state() != State::Stopping) return;
        machine_.stopFailed();
        std::strncpy(info_, text, sizeof(info_) - 1);
        info_[sizeof(info_) - 1] = '\0';
        infoScroll_.reset();
        markDirty();
    }

    State state() const { return machine_.state(); }
    // "Zastaveno" shown long enough, or "Zastavuji" without an answer (SM-1)
    bool expired() const { return machine_.expired(stmcpp::systick::getTicks()); }

    bool handleInput(ui::InputEvent event) override {
        if (event == ui::InputEvent::Up || event == ui::InputEvent::Down) {
            machine_.toggle(); // C4: Stop <-> Pridat sloku (after the first anchor)
            markDirty();
            return true;
        }
        if (event != ui::InputEvent::Enter) return true;
        const nowplaying::Action a = machine_.press(stmcpp::systick::getTicks());
        if (a == nowplaying::Action::Leave) {
            leave();
            return true;
        }
        markDirty();
        if (a == nowplaying::Action::SendAddVerse) {
            if (_addVerseCallback) _addVerseCallback();
        } else if (_stopCallback) {
            _stopCallback();
        }
        return true;
    }

    void tickScroll(int elapsedMs) override {
        int len = static_cast<int>(std::strlen(info_));
        if (infoScroll_.tick(elapsedMs, len, INFO_CHARS)) markDirty();
    }

    void render(gfx::IFrameBuffer<uint8_t>& fb, const ui::Theme& theme) override {
        gfx::fill(fb, (uint8_t)0);

        const State state_ = machine_.state();
        const char* title = "Prehravam";
        if (state_ == State::Stopping) title = "Zastavuji";
        if (state_ == State::Stopped) title = "Zastaveno";
        ui::drawTitleBar(fb, theme, title);

        // Song code (Font_11x18): 5 underlined slots as on the remote
        const int fw    = (int)theme.itemFont->width();
        const int fh    = (int)theme.itemFont->height();
        const int yD    = 14;
        const int len   = (int)std::strlen(code_);
        const int slots = 5;
        if (len <= slots) {
            const int gap   = 2;
            const int total = slots * fw + (slots - 1) * gap;
            const int x0    = (theme.displayWidth - total) / 2;
            char dc[2] = { 0, 0 };
            for (int i = 0; i < slots; i++) {
                int x = x0 + i * (fw + gap);
                dc[0] = (i < len) ? code_[i] : (i < 4 ? '-' : ' ');
                gfx::text::drawText(fb, *theme.itemFont, dc, x, yD, gfx::Anchor::TopLeft, (uint8_t)1);
                gfx::drawHLine(fb, x, yD + fh + 2, fw, (uint8_t)1);
            }
        } else {
            gfx::text::drawText(fb, *theme.itemFont, code_, theme.displayWidth / 2, yD, gfx::Anchor::TopCenter, (uint8_t)1);
        }

        // Info line: the section from the PC, centered; a long one scrolls
        const int infoY = ui::hintY(theme) - (int)theme.smallFont->height() - 6;
        const int ilen  = static_cast<int>(std::strlen(info_));
        if (state_ != State::Stopped && ilen <= INFO_CHARS) {
            gfx::text::drawText(fb, *theme.smallFont, info_, theme.displayWidth / 2, infoY, gfx::Anchor::TopCenter, (uint8_t)1);
        } else if (state_ != State::Stopped) {
            char buf[INFO_CHARS + 1];
            std::snprintf(buf, sizeof(buf), "%.18s", info_ + infoScroll_.offset(ilen, INFO_CHARS));
            gfx::text::drawText(fb, *theme.smallFont, buf, theme.leftMargin, infoY, gfx::Anchor::TopLeft, (uint8_t)1);
        }

        // Buttons: Stop alone (selected) until the first anchor; then Stop | Pridat sloku, the encoder picks one
        if (state_ != State::Stopped) {
            const int y = theme.displayHeight - (int)btn_stop_selected.height;
            if (!machine_.addVerseAvailable()) {
                const int x = (theme.displayWidth - (int)btn_stop_selected.width) / 2;
                gfx::drawIcon(fb, btn_stop_selected, x, y, gfx::Anchor::TopLeft, (uint8_t)1);
            } else {
                const bool addSel = machine_.selection() == nowplaying::Selection::AddVerse;
                const int sw = (int)btn_stop_selected.width;
                const int x0 = theme.leftMargin + 4;
                gfx::drawIcon(fb, addSel ? btn_stop_normal : btn_stop_selected, x0, y, gfx::Anchor::TopLeft, (uint8_t)1);
                const char* lbl = "Pridat sloku";
                const int lw = 12 * (int)theme.smallFont->width() + 6;
                const int bx = x0 + sw + 8;
                const int bh = (int)btn_stop_selected.height;
                if (addSel) gfx::drawFilledRect(fb, bx, y, lw, bh, (uint8_t)1);
                else gfx::drawRect(fb, bx, y, lw, bh, (uint8_t)1);
                gfx::text::drawText(fb, *theme.smallFont, lbl, bx + 3, y + (bh - (int)theme.smallFont->height()) / 2,
                                    gfx::Anchor::TopLeft, (uint8_t)(addSel ? 0 : 1));
            }
        }

        clearDirty();
    }

    void leave();

private:
    nowplaying::Machine machine_{};
    char     code_[12]  = {};
    char     info_[32]  = {};
    ui::TextScroll infoScroll_;
};

static NowPlayingScreen nowPlayingScreen;

void NowPlayingScreen::leave() {
    _nowPlayingActive = false;
    // Remove only if it is on top (above it there may be e.g. a pairing prompt)
    if (nav.current() == this) nav.pop();
}

// ── Pairing with the remote from the menu (progress and result) ───────

static bool _pairStatusShown = false;

static void onPairStatusDismiss(void* userData) {
    (void)userData;
    // Closes only the screen; the pairing finishes (or times out) on its own
    _pairStatusShown = false;
    nav.pop();
}

static ui::SplashScreen pairStatusScreen("Parovani", "", "Hledam ovladac...", onPairStatusDismiss);

// D3 (2026-11-24): confirmation of the pairing code (main.cpp, protocol/pairing.hpp). Starts on "Ne" so a stray click does not pair.
static char pairingPrompt[32] = "Sparovat?";
static void (*pairAnswerHandler)(bool) = nullptr;
void setPairAnswerHandler(void (*handler)(bool)) { pairAnswerHandler = handler; }
static void onPairConfirmResult(bool confirmed, void* userData) {
    (void)userData;
    nav.pop();
    if (pairAnswerHandler) pairAnswerHandler(confirmed);
}
static ui::ConfirmScreen pairingConfirmScreen(pairingPrompt, onPairConfirmResult);

// ── Song list screens ─────────────────────────────────────────────────

static void onLoadingDismiss(void* userData) {
    (void)userData;
    _win.abort();
    _loadingScreenShown = false;
    songListTimeoutScheduler.stop();
    songListDelayScheduler.stop();
    nav.pop();
}

static ui::SplashScreen songListLoadingScreen(
    "Pisne", "Nacitani...", "Cekam na PC", onLoadingDismiss
);

static void onSongSelected(ui::MenuItem* item, void* userData);

static ui::MenuScreen songListScreen("Pisne", _songListItems, 0);

static void onSongSelected(ui::MenuItem* item, void* userData) {
    (void)userData;
    // Pop the song list first: a failure reported at once (requestFailed) pushes its message on top.
    // NowPlaying is pushed by SIGNAL_PLAYING. The title points into _songNames, valid after the pop.
    nav.pop();
    if (_playSongCallback && item) {
        _playSongCallback(item->title());
    }
}

// ── Remote update screen (relayed from the PC) ─────────────────────────

static ui::ProgressScreen fwProgressScreen("Aktualizace", "");
static bool fwPushed = false; // the screen is in the navigator stack (always as the only one above the main menu)

// ── Callbacks ─────────────────────────────────────────────────────────

static void playCallback(ui::MenuItem* item, void* userData) {
    (void)item; (void)userData;
    if (!isPcAlive()) return;
    if (!_requestSongListCallback) return;

    _loadingScreenShown = false;
    uint16_t offset = _win.startFirst();
    songListDelayScheduler.start();   // 500ms delay before showing loading screen
    songListTimeoutScheduler.start(); // 3s timeout
    _requestSongListCallback(offset); // last: a failure reported at once stops both schedulers
}

static recording::Machine _recording;
static GUI::RecordCallback _recordCallback = nullptr;

// C3: the recording screen follows recording::Machine. "Priprava" while the PC warms the organ up, "Nahravam"
// with the Stop button, "Zastavuji" until SIGNAL_STOPPED; a refusal (BUSY, protocol v1 PC, any other failure) stays
// until Enter. "Nelze: hraje pisen" is the base's own refusal while a song plays (one at a time).
static bool _recordShown = false;     // the screen is in the navigator stack
static bool _recordRefused = false;   // the base refused (playing): not a machine state

static void onRecordEnter(void* userData);

// Same look as NowPlayingScreen (the state in the title bar, a big line in the song code's place, the Stop icon
// button at the bottom, OK = Stop). A refusal is shown the way "Prehravam" shows a failed stop: the reason in the
// info line, no button; OK dismisses it.
class RecordScreen : public ui::Screen {
public:
    void set(const char* title, const char* big, const char* info, bool stopButton) {
        title_ = title;
        big_   = big;
        info_  = info;
        stop_  = stopButton;
        markDirty();
    }

    bool handleInput(ui::InputEvent event) override {
        if (event == ui::InputEvent::Enter) onRecordEnter(nullptr);
        return true;
    }

    void render(gfx::IFrameBuffer<uint8_t>& fb, const ui::Theme& theme) override {
        gfx::fill(fb, (uint8_t)0);
        ui::drawTitleBar(fb, theme, title_);
        gfx::text::drawText(fb, *theme.itemFont, big_, theme.displayWidth / 2, 14, gfx::Anchor::TopCenter, (uint8_t)1);
        const int infoY = ui::hintY(theme) - (int)theme.smallFont->height() - 6;
        gfx::text::drawText(fb, *theme.smallFont, info_, theme.displayWidth / 2, infoY, gfx::Anchor::TopCenter, (uint8_t)1);
        if (stop_) {
            const int x = (theme.displayWidth - (int)btn_stop_selected.width) / 2;
            gfx::drawIcon(fb, btn_stop_selected, x, theme.displayHeight - (int)btn_stop_selected.height, gfx::Anchor::TopLeft, (uint8_t)1);
        }
        clearDirty();
    }

private:
    const char* title_ = "";
    const char* big_   = "";
    const char* info_  = "";
    bool        stop_  = false;
};

static RecordScreen recordScreen;

static void syncRecordScreen() {
    const recording::State st = _recording.state();
    const char* title = "Nahravani";
    const char* big   = "";
    const char* info  = "";
    bool stop = false;
    if (_recordRefused) {
        info = "Nelze: hraje pisen";
    } else {
        switch (st) {
            case recording::State::Starting:   title = "Priprava";  big = "REC"; stop = true; break;
            case recording::State::Recording:  title = "Nahravam";  big = "REC"; stop = true; break;
            case recording::State::Stopping:   title = "Zastavuji"; big = "REC"; stop = true; break;
            case recording::State::Busy:       info = "Zaneprazdneno"; break;
            case recording::State::NoSupport:  info = "PC nema nahravani"; break;
            case recording::State::Failed:     info = "Nahravani selhalo"; break;
            case recording::State::Idle:       break;
        }
    }
    const bool want = _recordRefused || st != recording::State::Idle;
    if (want) {
        if (fwPushed) return; // the update screen owns the display
        recordScreen.set(title, big, info, stop);
        if (!_recordShown) {
            nav.push(&recordScreen);
            _recordShown = true;
        }
        Oled::wakeupCallback();
    } else if (_recordShown) {
        _recordShown = false;
        if (nav.current() == &recordScreen) nav.pop();
    }
    nav.forceRender();
}

static void onRecordEnter(void* userData) {
    (void)userData;
    if (_recordRefused) {
        _recordRefused = false;
    } else {
        switch (_recording.state()) {
            case recording::State::Recording:
            case recording::State::Stopping:
                if (_recording.press() != recording::Action::None && _recordCallback) _recordCallback(false);
                break;
            case recording::State::Starting: return; // wait for the PC (the organ warms up)
            default: _recording.dismiss(); break;
        }
    }
    syncRecordScreen();
}

// The base is the only place a recording starts (A7). The menu item starts it, or stops a running one.
static void recordCallback(ui::MenuItem* item, void* userData) {
    (void)item; (void)userData;
    if (_nowPlayingActive) { // one at a time: a song plays
        _recordRefused = true;
        syncRecordScreen();
        return;
    }
    const recording::Action a = _recording.press();
    if (_recordCallback && a != recording::Action::None) _recordCallback(a == recording::Action::SendStart);
    syncRecordScreen();
}

// C12 / Q2-29b (owner 2026-11-23): switching the organ off while a song plays or a recording runs needs a 3 s hold of
// the encoder button on the same hold screen as the remote's power off (inc/hold_screen.hpp); switching on, or off when
// idle, never asks. The checkbox follows the real state either way.
static void powerOffHeld() {
    base::current::disable();
    settingsItems[1].setChecked(base::current::isEnabled());
}

static void powerCallback(ui::MenuItem* item, void* userData) {
    (void)userData;
    if (base::current::isEnabled()) {
        const bool busy = _nowPlayingActive || _recording.state() != recording::State::Idle;
        if (busy) {
            if (item) item->setChecked(true); // stays on until the hold completes
            GUI::showHold("Vypnuti varhan", "Drzte 3 s", powerOffHeld);
            return;
        }
        base::current::disable();
    } else {
        base::current::setSource(organsrc::Source::MENU);
        base::current::enable();
    }
    if (item) item->setChecked(base::current::isEnabled());
}

static void firmwareCallback(ui::MenuItem* item, void* userData) {
    (void)item; (void)userData;
    std::snprintf(fwLine0, sizeof(fwLine0), "Revize: %s", git::revision);
    std::snprintf(fwLine1, sizeof(fwLine1), "Vetev: %s",  git::branch);
    std::snprintf(fwLine2, sizeof(fwLine2), "Datum: %s",  git::build_date);
    std::snprintf(fwLine3, sizeof(fwLine3), "Cas: %s",    git::build_time);
    nav.push(&firmwareScreen);
}

static void pairCallback(ui::MenuItem* item, void* userData) {
    (void)item; (void)userData;
    // Z, F → scan; connecting to MIDIControlRemote, B and the timeout are handled by the main loop (main.cpp)
    // RN4870 is not working (running without BLE, spec R21): immediate message instead of waiting for the timeout
    bool up = Bluetooth::isUp();
    if (up && Bluetooth::startPairing() != 0) return;
    pairStatusScreen.setComment(up ? "Hledam ovladac..." : "BLE nefunguje");
    if (!_pairStatusShown) {
        _pairStatusShown = true;
        nav.push(&pairStatusScreen);
    }
}

static void ledMasterCallback(ui::MenuItem* item, void* userData) {
    (void)item; (void)userData;
    settings::setLedMasterEnable(!settings::ledMasterEnable());
    settings::save();
}

static void midiRoleCallback(ui::MenuItem* item, void* userData) {
    (void)item; (void)userData;
    settings::setMidiRoleIdentifier(!settings::midiRoleIdentifier());
    settings::save();
}

static void displayCallback(ui::MenuItem* item, void* userData) {
    (void)item; (void)userData;

    const char* led = "vyp";
    switch (display::getLed()) {
        case display::ledColor::RED:    led = "cervena"; break;
        case display::ledColor::GREEN:  led = "zelena";  break;
        case display::ledColor::BLUE:   led = "modra";   break;
        case display::ledColor::YELLOW: led = "zluta";   break;
        case display::ledColor::OFF:    led = "vyp";     break;
    }

    std::snprintf(dispLine0, sizeof(dispLine0), "Pripojen: %s",
                  display::isConnected() ? "Ano" : "Ne");
    char song[5];
    char verse[3];
    display::formatSong(song);
    display::formatVerse(verse);
    std::snprintf(dispLine1, sizeof(dispLine1), "Pisen: [%s]", song);
    std::snprintf(dispLine2, sizeof(dispLine2), "Sloka: [%s]", verse);
    std::snprintf(dispLine3, sizeof(dispLine3), "Pismeno: %c",
                  display::getLetter());
    std::snprintf(dispLine4, sizeof(dispLine4), "LED: %s", led);

    nav.push(&displayScreen);
}

static void bluetoothCallback(ui::MenuItem* item, void* userData) {
    (void)item; (void)userData;
    updateBluetoothInfo();
    nav.push(&bluetoothScreen);
}

// ── Splash dismiss → main menu ────────────────────────────────────────

static void onSplashDismiss(void* userData) {
    (void)userData;
    nav.reset(&mainScreen);
}

// ── Public API ────────────────────────────────────────────────────────

void init() {
    initTheme();
    nav.setTheme(theme);

    // Splash: revision and branch on separate lines (a long branch name scrolls)
    splashScreen = ui::SplashScreen(
        base::DEVICE_NAME.c_str(),
        base::DEVICE_TYPE.c_str(),
        git::revision,
        onSplashDismiss
    );
    splashScreen.setComment2(git::branch);

    firmwareScreen.setTitle("Verze FW");
    displayScreen.setTitle("Displej");
    bluetoothScreen.setTitle("Bluetooth");
    noPcScreen.setComment2("Stisk: Nastaveni");

    // Synchronize the checkboxes from the saved settings
    settingsItems[0].setChecked(settings::ledMasterEnable());
    serviceItems[0].setChecked(settings::midiRoleIdentifier());

    nav.push(&splashScreen);
}

void render() {
    nav.tickScroll(30);
    syncNoPcScreen();

    if (_nowPlayingActive && nowPlayingScreen.expired()) nowPlayingScreen.leave();

    // Bluetooth info: value refresh; the display does not sleep while the screen is open (changes are visible)
    if (nav.current() == &bluetoothScreen) {
        updateBluetoothInfo();
        Oled::wakeupCallback();
    }

    // Without a stored remote (new base, upgrade from settings v1): a hint instead of the main menu title.
    // In this state the Base connects to nothing until the user pairs a remote from the Nastaveni menu.
    // 14 characters of 7x10 = 98 px + 2 px margin, fits to the left of the PC icon (screen x = 102).
    mainScreen.setTitle(settings::remoteAddr()[0] == '\0' ? "Sparuj ovladac" : "Hlavni menu");

    bool dirty = nav.render(frameBuffer);

    // Status icons on the menu screens and on the now playing screen (and on the noPcScreen overlay; not on other SplashScreens, ParagraphScreen, ConfirmScreen)
    ui::Screen* cur = nav.current();

    bool showStatusIcons = (cur == &mainScreen || cur == &settingsScreen || cur == &serviceScreen || cur == &songListScreen || cur == &nowPlayingScreen || cur == &noPcScreen);

    if (showStatusIcons && Bluetooth::isConnected()) {
        int iconX = theme.displayOffsetX + theme.displayWidth - 14;
        int iconY = 0;

        duration dataTs = Bluetooth::getDataReceivedTimestamp();
        duration now = stmcpp::systick::getDuration();
        duration elapsed = now - dataTs;

        gfx::drawFilledRect(frameBuffer, iconX, iconY, 14, 10, (uint8_t)0);

        if (elapsed < 300_ms) {
            // Animate: 4 steps over 300ms, ~75ms each
            int step = static_cast<int>(elapsed / 75_ms);
            switch (step) {
                case 0: gfx::drawIcon(frameBuffer, wireless_1, iconX, iconY, gfx::Anchor::TopLeft, (uint8_t)1); break;
                case 1: gfx::drawIcon(frameBuffer, wireless_2, iconX, iconY, gfx::Anchor::TopLeft, (uint8_t)1); break;
                case 2: gfx::drawIcon(frameBuffer, wireless_3, iconX, iconY, gfx::Anchor::TopLeft, (uint8_t)1); break;
                default: gfx::drawIcon(frameBuffer, wireless_4, iconX, iconY, gfx::Anchor::TopLeft, (uint8_t)1); break;
            }
        } else {
            gfx::drawIcon(frameBuffer, wireless_4, iconX, iconY, gfx::Anchor::TopLeft, (uint8_t)1);
        }
        dirty = true;
    }

    // PC connection icon (to the left of the BLE icon)
    if (showStatusIcons && isPcAlive()) {
        int pcIconX = theme.displayOffsetX + theme.displayWidth - 14 - 1 - 10; // 14=BLE width, 1=gap (as the remote battery icon), 10=PC width
        int pcIconY = 1;
        gfx::drawFilledRect(frameBuffer, pcIconX, pcIconY, 10, 8, (uint8_t)0);
        gfx::drawIcon(frameBuffer, pc_monitor, pcIconX, pcIconY, gfx::Anchor::TopLeft, (uint8_t)1);
        dirty = true;
    }

    if (dirty) {
        Oled::update();
    }
}

static splashguard::Guard splashGuard;

static void handleKey(ui::InputEvent event);

void keypress(ui::InputEvent event) {
    splashGuard.realKey();   // P1-8: a real key cancels the splash's 3 s auto-dismiss
    handleKey(event);
}

// Hold-to-confirm: one screen, one timer; the owner passes the action that runs at 100 %
static HoldScreen holdScreen("", "");
static holdtimer::Timer _holdTimer;
static GUI::HoldCallback _holdDone = nullptr;
static bool _holdShown = false;

void showHold(const char* title, const char* hint, HoldCallback done) {
    holdScreen.set(title, hint);
    holdScreen.setPercent(0);
    _holdTimer.reset();
    _holdDone = done;
    if (!_holdShown && !fwPushed) {
        nav.push(&holdScreen);
        _holdShown = true;
    }
    Oled::wakeupCallback();
    nav.forceRender();
}

void hideHold() {
    if (!_holdShown) return;
    _holdShown = false;
    _holdDone = nullptr;
    if (nav.current() == &holdScreen) nav.pop();
    nav.forceRender();
}

bool holdActive() {
    return _holdShown;
}

void buttonHold(bool pressed, uint32_t dtMs) {
    if (!_holdShown) return;
    const bool done = _holdTimer.tick(pressed, dtMs);
    const uint8_t before = holdScreen.percent();
    holdScreen.setPercent(_holdTimer.percent());
    if (pressed) Oled::wakeupCallback();
    if (done) {
        const HoldCallback cb = _holdDone;
        hideHold();
        if (cb) cb();
        nav.forceRender();
    } else if (holdScreen.percent() != before) {
        nav.forceRender();
    }
}

void buttonLong() {
    if (Oled::isSleeping()) return;
    if (_holdShown) return; // the hold screen owns the button
    // Item 18: a long press leaves the song list (a short one selects)
    if (nav.current() == &songListScreen && nav.depth() > 1) {
        Oled::wakeupCallback();
        nav.pop();
        nav.forceRender();
    }
}

void splashAutoDismiss() {
    if (splashGuard.allowAutoDismiss()) handleKey(ui::InputEvent::Enter);
}

static void handleKey(ui::InputEvent event) {
    if (Oled::isSleeping()) {
        Oled::wakeupCallback();
        nav.forceRender();
        return;
    }

    Oled::wakeupCallback();

    // The hold screen: turning the encoder cancels; the button belongs to buttonHold()
    if (_holdShown) {
        if (event == ui::InputEvent::Up || event == ui::InputEvent::Down) hideHold();
        return;
    }

    // For MenuScreen Enter: handle Back and Submenu item types manually
    // because the navigator doesn't have RTTI to auto-detect MenuScreen
    ui::Screen* cur = nav.current();
    if (event == ui::InputEvent::Enter && cur) {
        if (cur == &mainScreen || cur == &settingsScreen || cur == &serviceScreen || cur == &songListScreen) {
            auto* menu = static_cast<ui::MenuScreen*>(cur);
            ui::MenuItem* sel = menu->selectedItem();

            if (sel->type() == ui::MenuItem::Type::Back) {
                if (nav.depth() > 1) {
                    nav.pop();
                }
                nav.forceRender();
                return;
            }

            if (sel->type() == ui::MenuItem::Type::Submenu && sel->submenu()) {
                nav.handleInput(event);
                nav.push(sel->submenu());
                return;
            }
        }
    }

    const int selBefore = (cur == &songListScreen) ? songListScreen.selectedIndex() : 0;
    nav.handleInput(event);

    // A9: page the song list window. Down near the end asks for start + count, Up at the first item asks for
    // max(0, start - 12); the response rebuilds the window by absolute index (song_window.hpp).
    if (cur == &songListScreen && _requestSongListCallback) {
        uint16_t offset = 0;
        bool ask = false;
        if (event == ui::InputEvent::Down) ask = _win.downRequest(songListScreen.selectedIndex(), offset);
        else if (event == ui::InputEvent::Up) ask = _win.upRequest(selBefore, offset);
        if (ask) {
            songListTimeoutScheduler.start(); // 3 s: no answer -> Idle, the next scroll asks again (D12)
            _requestSongListCallback(offset); // last: a failure reported at once stops the scheduler
        }
    }

    nav.forceRender();
}

// noinline + used: test_fault.py calls it via gdb (forced OLED redraw)
[[gnu::used, gnu::noinline]] void renderForce() {
    nav.forceRender();
}

ui::Navigator& navigator() {
    return nav;
}

ui::MenuItem& checkboxPower() {
    return settingsItems[1];
}

ui::MenuItem& checkboxLedMaster() {
    return settingsItems[0];
}

ui::MenuItem& checkboxMidiRole() {
    return serviceItems[0];
}

void showPairingResult(const char* text) {
    if (!_pairStatusShown) return;
    pairStatusScreen.setComment(text);
    Oled::wakeupCallback();
    nav.forceRender();
}

void showPairingConfirm(uint32_t code) {
    std::snprintf(pairingPrompt, sizeof(pairingPrompt), "Sparovat? Kod %06lu", static_cast<unsigned long>(code % 1000000u));
    if (nav.current() != &pairingConfirmScreen) {
        pairingConfirmScreen.~ConfirmScreen();
        new (&pairingConfirmScreen) ui::ConfirmScreen(pairingPrompt, onPairConfirmResult);
        pairingConfirmScreen.handleInput(ui::InputEvent::Right);
        nav.push(&pairingConfirmScreen);
    } else {
        pairingConfirmScreen.markDirty();
    }
    Oled::wakeupCallback();
    nav.forceRender();
}

void closePairingConfirm() {
    if (nav.current() == &pairingConfirmScreen) nav.pop();
}

void showNowPlaying(const char* songName) {
    nowPlayingScreen.start(songName);

    // During a Remote update the progress screen stays on top; Now Playing is not pushed
    // (it will not show after the update, the next SIGNAL_PLAYING will display it)
    if (fwPushed) return;

    // A repeated SIGNAL_PLAYING only updates the screen, it does not push it onto the stack again
    if (!_nowPlayingActive) {
        _nowPlayingActive = true;
        nav.push(&nowPlayingScreen);
    }
    Oled::wakeupCallback();
    nav.forceRender();
}

void dismissNowPlaying() {
    // "Zastaveno" for a moment; render() removes the screen afterwards
    if (_nowPlayingActive) {
        nowPlayingScreen.stopped();
        Oled::wakeupCallback();
        nav.forceRender();
    }
}

void showFwProgress(const char* hint, uint8_t percent) {
    fwProgressScreen.setHint(hint);
    fwProgressScreen.setPercent(percent);
    if (!fwPushed) {
        // A song list request in progress and the screen state flags that the stack reset removes
        _win.abort();
        _loadingScreenShown = false;
        songListTimeoutScheduler.stop();
        songListDelayScheduler.stop();
        _pairStatusShown = false;
        _nowPlayingActive = false;
        _recordShown = false;
        _holdShown = false;
        _holdDone = nullptr;
        // Stack = main menu + update screen (no splash/submenu below it)
        nav.reset(&mainScreen);
        nav.push(&fwProgressScreen);
        fwPushed = true;
    }
    Oled::wakeupCallback();
    nav.forceRender();
}

void hideFwProgress() {
    if (!fwPushed) return;
    fwPushed = false;
    if (nav.current() == &fwProgressScreen) {
        nav.pop();
    } else {
        nav.reset(&mainScreen); // should not happen: nothing is pushed above it
    }
    Oled::wakeupCallback();
    nav.forceRender();
}

void setNowPlayingAnchor(const char* label) {
    if (nowPlayingScreen.state() == NowPlayingScreen::State::Stopped) return;
    nowPlayingScreen.setSection(label);
    Oled::wakeupCallback();
    nav.forceRender();
}

void requestFailed(protocol::MsgType type, const char* text) {
    switch (type) {
        case protocol::MsgType::GET_SONG_LIST:
            if (_win.state() == SongListState::WaitingFirst) {
                // the reason instead of the generic "PC neodpovida" timeout
                _win.abort();
                songListDelayScheduler.stop();
                songListTimeoutScheduler.stop();
                songListLoadingScreen.setSubtitle(text);
                songListLoadingScreen.setComment("");
                if (!_loadingScreenShown) {
                    nav.push(&songListLoadingScreen);
                    _loadingScreenShown = true;
                }
            } else if (_win.state() == SongListState::WaitingMore) {
                _win.abort(); // the next scroll asks again
                songListTimeoutScheduler.stop();
                return;
            } else {
                return;
            }
            break;
        case protocol::MsgType::PLAY_SONG:
            if (fwPushed) return;
            // same splash as the song list; its dismiss only pops (the list state is Idle here)
            songListLoadingScreen.setSubtitle(text);
            songListLoadingScreen.setComment("");
            if (nav.current() != &songListLoadingScreen) nav.push(&songListLoadingScreen);
            break;
        case protocol::MsgType::STOP_SONG:
            if (!_nowPlayingActive) return;
            nowPlayingScreen.stopFailed(text);
            break;
        default:
            return;
    }
    Oled::wakeupCallback();
    nav.forceRender();
}

void syncPowerCheckbox() {
    settingsItems[1].setChecked(base::current::isEnabled());
    nav.forceRender();
}

void recordingSignal(bool recording) {
    if (recording) _recording.signalRecording();
    else _recording.signalStopped();
    syncRecordScreen();
}

void recordingFailed(routercore::Failure f, protocol::NakReason reason) {
    _recording.requestFailed(f, reason);
    syncRecordScreen();
}

recording::State recordingState() {
    return _recording.state();
}

void recordingPcLost() {
    _recording.pcLost();
    _recordRefused = false;
    syncRecordScreen();
}

void setRecordCallback(RecordCallback cb) {
    _recordCallback = cb;
}

void setAddVerseCallback(AddVerseCallback cb) {
    _addVerseCallback = cb;
}

void setStopCallback(StopCallback cb) {
    _stopCallback = cb;
}

void setRequestSongListCallback(RequestSongListCallback cb) {
    _requestSongListCallback = cb;
}

void setPlaySongCallback(PlaySongCallback cb) {
    _playSongCallback = cb;
}

void handleSongListResponse(const char* payload, uint8_t len, bool hasMore) {
    if (_win.state() == SongListState::Idle) return;

    bool isFirstPage = (_win.state() == SongListState::WaitingFirst);
    songListTimeoutScheduler.stop();
    if (isFirstPage) songListDelayScheduler.stop();

    // Count the codes first: the window offset advances by the codes RECEIVED (RF-18)
    int received = 0;
    for (int i = 0, nameStart = 0; i <= len; ++i) {
        if (i == len || payload[i] == ';') {
            if (i > nameStart) ++received;
            nameStart = i + 1;
        }
    }

    const int oldCount = _win.count();
    // live cursor: Down/Up presses made while the page was loading are kept (bench 2.12)
    const songwin::Window::Plan plan = _win.onResponse(received, hasMore, isFirstPage ? -1 : songListScreen.selectedIndex());
    if (!plan.ok) return;

    // Keep the old entries that stay (shift them to the front), then store the first keepNew codes of the page
    if (plan.dropFront > 0 && plan.dropFront < oldCount) {
        std::memmove(_songNames[0], _songNames[plan.dropFront], static_cast<size_t>(oldCount - plan.dropFront) * sizeof(_songNames[0]));
    }
    int n = (plan.dropFront < oldCount) ? oldCount - plan.dropFront : 0;
    for (int i = 0, nameStart = 0; i <= len && n < plan.count; ++i) {
        if (i == len || payload[i] == ';') {
            int nameLen = i - nameStart;
            if (nameLen > 0) {
                if (nameLen > 23) nameLen = 23;
                std::memcpy(_songNames[n], &payload[nameStart], nameLen);
                _songNames[n][nameLen] = '\0';
                n++;
            }
            nameStart = i + 1;
        }
    }
    for (int i = 0; i < plan.count; ++i) _songListItems[i] = ui::MenuItem(_songNames[i], onSongSelected);

    // Empty folder (first page only)
    if (isFirstPage && plan.count == 0) {
        if (!_loadingScreenShown) {
            // The loading screen has not been shown yet — show it with the error
            songListLoadingScreen.setSubtitle("Zadne pisne");
            songListLoadingScreen.setComment("");
            nav.push(&songListLoadingScreen);
            _loadingScreenShown = true;
        } else {
            songListLoadingScreen.setSubtitle("Zadne pisne");
            songListLoadingScreen.setComment("");
        }
        nav.forceRender();
        return;
    }

    // Add the Back item
    _songListItems[plan.count] = ui::MenuItem::back("Zpet");

    // Rebuild the song list screen with the current window
    songListScreen = ui::MenuScreen("Pisne", _songListItems, plan.count + 1);
    songListScreen.setSelectedIndex(plan.cursor);

    if (isFirstPage) {
        // Pop the loading screen if it was shown, push the song list
        if (_loadingScreenShown) {
            nav.pop();
            _loadingScreenShown = false;
        }
        nav.push(&songListScreen);
    }
    // For a later page: the screen is already on the stack, forceRender is enough

    nav.forceRender();
}

// Called from the delay scheduler (500ms) — shows the loading screen if we are still waiting
void songListDelayShow() {
    if (_win.state() == SongListState::WaitingFirst && !_loadingScreenShown) {
        songListLoadingScreen.setSubtitle("Nacitani...");
        songListLoadingScreen.setComment("Cekam na PC");
        nav.push(&songListLoadingScreen);
        _loadingScreenShown = true;
        nav.forceRender();
    }
}

// Called from the timeout scheduler (3s)
void songListTimeout() {
    if (_win.state() == SongListState::WaitingFirst) {
        songListDelayScheduler.stop();
        if (!_loadingScreenShown) {
            songListLoadingScreen.setSubtitle("PC neodpovida");
            songListLoadingScreen.setComment("");
            nav.push(&songListLoadingScreen);
            _loadingScreenShown = true;
        } else {
            songListLoadingScreen.setSubtitle("PC neodpovida");
            songListLoadingScreen.setComment("");
        }
        nav.forceRender();
    } else if (_win.state() == SongListState::WaitingMore) {
        // A9: no answer for the next/previous window: back to Idle, the next scroll asks again.
        // TODO(D12): visible feedback / one-action Back while waiting.
        _win.abort();
    }
}

} // namespace GUI

// ── Scheduler ─────────────────────────────────────────────────────────

stmcpp::scheduler::Scheduler guiRenderScheduler(30_ms, &GUI::render, true, true);

// One-shot: delayed loading screen (500ms grace period)
stmcpp::scheduler::Scheduler songListDelayScheduler(500_ms, GUI::songListDelayShow, false, false);

// One-shot: timeout for loading the song list (3s)
stmcpp::scheduler::Scheduler songListTimeoutScheduler(3000_ms, GUI::songListTimeout, false, false);
