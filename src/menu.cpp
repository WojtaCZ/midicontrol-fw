#include "menu.hpp"
#include <stmcpp/scheduler.hpp>
#include <stmcpp/units.hpp>
#include "oled.hpp"
#include "base.hpp"
#include "git.hpp"
#include "display.hpp"
#include "bluetooth.hpp"
#include "settings.hpp"
#include "router.hpp"

#include <ui/progress_screen.hpp>
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
    theme.backSelected       = ui::makeIconRef(arrow_left_filled);
    theme.backUnselected     = ui::makeIconRef(arrow_left_empty);
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

static ui::MenuItem settingsItems[] = {
    ui::MenuItem::checkbox("LED",  true,  ledMasterCallback),
    ui::MenuItem::checkbox("Ident. MIDI", true, midiRoleCallback),
    ui::MenuItem::checkbox("Napajeni varhan", false, powerCallback),
    ui::MenuItem("Verze FW", firmwareCallback),
    ui::MenuItem("Displej",  displayCallback),
    ui::MenuItem("Bluetooth", bluetoothCallback),
    ui::MenuItem("Sparovat ovladac", pairCallback),
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

static ui::SplashScreen noPcScreen("Odpojeno", "", "Cekam na PC", onNoPcEnter);

// Pushes the overlay when the main menu is on top without a PC, removes it when it is on top and the PC is back
static void syncNoPcScreen() {
    ui::Screen* cur = nav.current();
    if (!isPcAlive()) {
        if (cur == &mainScreen) nav.push(&noPcScreen);
    } else if (cur == &noPcScreen) {
        nav.pop();
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
    changed |= setLine(bleLine0, "%s", Bluetooth::name());
    changed |= setLine(bleLine1, "Spojeni: %s", Bluetooth::isConnected() ? "Ano" : "Ne");
    changed |= setLine(bleLine2, "Sparovano: %s", Bluetooth::isPeerAccepted() ? "Ano" : "Ne");
    changed |= setLine(bleLine3, "Ovladac: %s", router::isRemoteAlive() ? "Ano" : "Ne");
    changed |= setLine(bleLine4, "MAC: %s", remote[0] ? remote : "--");

    if (changed) bluetoothScreen.markDirty();
}

// ── Now Playing screen ─────────────────────────────────────────────────

static StopCallback _stopCallback = nullptr;
static char _nowPlayingSong[64] = "";
static char _nowPlayingInfo[32] = "";
static char _nowPlayingAnchor[24] = "";
static bool _nowPlayingActive = false;

// ── Song list data ──────────────────────────────────────────────────────

static constexpr int MAX_SONGS = 32;
static char _songNames[MAX_SONGS][24];       // 23 chars + null
static ui::MenuItem _songListItems[MAX_SONGS + 1]; // items + Back
static int _songCount = 0;
static bool _hasMoreSongs = false;
static bool _loadingScreenShown = false;

enum class SongListState : uint8_t { Idle, WaitingFirst, WaitingMore };
static SongListState _songListState = SongListState::Idle;

static RequestSongListCallback _requestSongListCallback = nullptr;
static PlaySongCallback _playSongCallback = nullptr;

static void onNowPlayingDismiss(void* userData) {
    (void)userData;
    _nowPlayingActive = false;
    nav.pop();
    if (_stopCallback) {
        _stopCallback();
    }
}

static ui::SplashScreen nowPlayingScreen(
    "Prehravani", "", "[STOP]", onNowPlayingDismiss
);

// ── Pairing with the remote from the menu (progress and result) ───────

static bool _pairStatusShown = false;

static void onPairStatusDismiss(void* userData) {
    (void)userData;
    // Closes only the screen; the pairing finishes (or times out) on its own
    _pairStatusShown = false;
    nav.pop();
}

static ui::SplashScreen pairStatusScreen("Parovani", "", "Hledam ovladac...", onPairStatusDismiss);

// ── Song list screens ─────────────────────────────────────────────────

static void onLoadingDismiss(void* userData) {
    (void)userData;
    _songListState = SongListState::Idle;
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
    if (_playSongCallback && item) {
        _playSongCallback(item->title());
    }
    // Pop song list — NowPlaying will be pushed by the existing PLAY_SONG handler
    nav.pop();
}

// ── Remote update screen (relayed from the PC) ─────────────────────────

static ui::ProgressScreen fwProgressScreen("Aktualizace", "");
static bool fwPushed = false; // the screen is in the navigator stack (always as the only one above the main menu)

// ── Callbacks ─────────────────────────────────────────────────────────

static void playCallback(ui::MenuItem* item, void* userData) {
    (void)item; (void)userData;
    if (!isPcAlive()) return;
    if (!_requestSongListCallback) return;

    _songCount = 0;
    _hasMoreSongs = false;
    _loadingScreenShown = false;
    _songListState = SongListState::WaitingFirst;
    _requestSongListCallback(0);
    songListDelayScheduler.start();   // 500ms delay before showing loading screen
    songListTimeoutScheduler.start(); // 3s timeout
}

static void recordCallback(ui::MenuItem* item, void* userData) {
    (void)item; (void)userData;
}

static void powerCallback(ui::MenuItem* item, void* userData) {
    (void)userData;
    if (base::current::isEnabled()) {
        base::current::disable();
    } else {
        base::current::enable();
    }
    if (item) item->setChecked(base::current::isEnabled());
}

static void firmwareCallback(ui::MenuItem* item, void* userData) {
    (void)item; (void)userData;
    std::snprintf(fwLine0, sizeof(fwLine0), "Revize: %s", git::revision.c_str());
    std::snprintf(fwLine1, sizeof(fwLine1), "Vetev: %s",  git::branch.c_str());
    std::snprintf(fwLine2, sizeof(fwLine2), "Datum: %s",  git::build_date.c_str());
    std::snprintf(fwLine3, sizeof(fwLine3), "Cas: %s",    git::build_time.c_str());
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
        git::revision.c_str(),
        onSplashDismiss
    );
    splashScreen.setComment2(git::branch.c_str());

    firmwareScreen.setTitle("Verze FW");
    displayScreen.setTitle("Displej");
    bluetoothScreen.setTitle("Bluetooth");
    noPcScreen.setComment2("Stisk: Nastaveni");

    // Synchronize the checkboxes from the saved settings
    settingsItems[0].setChecked(settings::ledMasterEnable());
    settingsItems[1].setChecked(settings::midiRoleIdentifier());

    nav.push(&splashScreen);
}

void render() {
    nav.tickScroll(30);
    syncNoPcScreen();

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

    // Status icons only on menu screens (not on SplashScreen, ParagraphScreen, ConfirmScreen)
    ui::Screen* cur = nav.current();

    bool showStatusIcons = (cur == &mainScreen || cur == &settingsScreen || cur == &songListScreen);

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
        int pcIconX = theme.displayOffsetX + theme.displayWidth - 14 - 2 - 10; // 14=BLE width, 2=gap, 10=PC width
        int pcIconY = 1;
        gfx::drawFilledRect(frameBuffer, pcIconX, pcIconY, 10, 8, (uint8_t)0);
        gfx::drawIcon(frameBuffer, pc_monitor, pcIconX, pcIconY, gfx::Anchor::TopLeft, (uint8_t)1);
        dirty = true;
    }

    if (dirty) {
        Oled::update();
    }
}

void keypress(ui::InputEvent event) {
    if (Oled::isSleeping()) {
        Oled::wakeupCallback();
        nav.forceRender();
        return;
    }

    Oled::wakeupCallback();

    // For MenuScreen Enter: handle Back and Submenu item types manually
    // because the navigator doesn't have RTTI to auto-detect MenuScreen
    ui::Screen* cur = nav.current();
    if (event == ui::InputEvent::Enter && cur) {
        if (cur == &mainScreen || cur == &settingsScreen || cur == &songListScreen) {
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

    nav.handleInput(event);

    // Auto-load next page when scrolling near bottom of song list
    if (event == ui::InputEvent::Down && cur == &songListScreen && _songListState == SongListState::Idle
        && _hasMoreSongs && _songCount < MAX_SONGS) {
        auto* menu = static_cast<ui::MenuScreen*>(cur);
        if (menu->selectedIndex() >= _songCount - 2 && _requestSongListCallback) {
            _songListState = SongListState::WaitingMore;
            _requestSongListCallback(static_cast<uint8_t>(_songCount));
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
    return settingsItems[2];
}

ui::MenuItem& checkboxLedMaster() {
    return settingsItems[0];
}

ui::MenuItem& checkboxMidiRole() {
    return settingsItems[1];
}

void showPairingResult(const char* text) {
    if (!_pairStatusShown) return;
    pairStatusScreen.setComment(text);
    nav.forceRender();
}

void showNowPlaying(const char* songName) {
    std::strncpy(_nowPlayingSong, songName, sizeof(_nowPlayingSong) - 1);
    _nowPlayingSong[sizeof(_nowPlayingSong) - 1] = '\0';
    nowPlayingScreen.setSubtitle(_nowPlayingSong);
    nowPlayingScreen.setComment("[STOP]");
    _nowPlayingInfo[0] = '\0';
    _nowPlayingAnchor[0] = '\0';

    // During a Remote update the progress screen stays on top; Now Playing is not pushed
    // (it will not show after the update, the next SIGNAL_PLAYING will display it)
    if (fwPushed) return;

    // A repeated SIGNAL_PLAYING only updates the screen, it does not push it onto the stack again
    if (!_nowPlayingActive) {
        _nowPlayingActive = true;
        nav.push(&nowPlayingScreen);
    }
    nav.forceRender();
}

void dismissNowPlaying() {
    if (_nowPlayingActive) {
        _nowPlayingActive = false;
        // Remove only if it is on top (above it there may be e.g. a pairing prompt)
        if (nav.current() == &nowPlayingScreen) {
            nav.pop();
        }
        nav.forceRender();
    }
}

void showFwProgress(const char* hint, uint8_t percent) {
    fwProgressScreen.setHint(hint);
    fwProgressScreen.setPercent(percent);
    if (!fwPushed) {
        // A song list request in progress and the screen state flags that the stack reset removes
        _songListState = SongListState::Idle;
        _loadingScreenShown = false;
        songListTimeoutScheduler.stop();
        songListDelayScheduler.stop();
        _pairStatusShown = false;
        _nowPlayingActive = false;
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
    std::strncpy(_nowPlayingAnchor, label, sizeof(_nowPlayingAnchor) - 1);
    _nowPlayingAnchor[sizeof(_nowPlayingAnchor) - 1] = '\0';
    updateNowPlayingInfo();
}

void updateNowPlayingInfo() {
    if (!_nowPlayingActive) return;

    if (_nowPlayingAnchor[0] != '\0') {
        // Section from the PC ("Predehra", "Sloka 2")
        std::snprintf(_nowPlayingInfo, sizeof(_nowPlayingInfo), "%s [STOP]", _nowPlayingAnchor);
    } else {
        // Display state digit by digit, as shown in the church (" 401", "1 ")
        char song[5];
        char verse[3];
        display::formatSong(song);
        display::formatVerse(verse);
        char letter = display::getLetter();
        std::snprintf(_nowPlayingInfo, sizeof(_nowPlayingInfo), "P:%s%c S:%s [STOP]", song, letter, verse);
    }
    nowPlayingScreen.setComment(_nowPlayingInfo);
    nav.forceRender();
}

void syncPowerCheckbox() {
    settingsItems[2].setChecked(base::current::isEnabled());
    nav.forceRender();
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
    if (_songListState == SongListState::Idle) return;

    bool isFirstPage = (_songListState == SongListState::WaitingFirst);
    _songListState = SongListState::Idle;
    _hasMoreSongs = hasMore;

    if (isFirstPage) {
        songListTimeoutScheduler.stop();
        songListDelayScheduler.stop();
    }

    // Save the current position for append
    int savedIndex = isFirstPage ? 0 : songListScreen.selectedIndex();

    if (isFirstPage) {
        _songCount = 0;
    }

    // Parse the semicolon-separated song names
    int nameStart = 0;
    for (int i = 0; i <= len && _songCount < MAX_SONGS; ++i) {
        if (i == len || payload[i] == ';') {
            int nameLen = i - nameStart;
            if (nameLen > 0) {
                if (nameLen > 23) nameLen = 23;
                std::memcpy(_songNames[_songCount], &payload[nameStart], nameLen);
                _songNames[_songCount][nameLen] = '\0';
                _songListItems[_songCount] = ui::MenuItem(_songNames[_songCount], onSongSelected);
                _songCount++;
            }
            nameStart = i + 1;
        }
    }

    // Empty folder (first page only)
    if (isFirstPage && _songCount == 0) {
        if (!_loadingScreenShown) {
            // The loading screen has not been shown yet — show it with the error
            songListLoadingScreen.setSubtitle("Zadne pisne");
            songListLoadingScreen.setComment("Stiskni pro navrat");
            nav.push(&songListLoadingScreen);
            _loadingScreenShown = true;
        } else {
            songListLoadingScreen.setSubtitle("Zadne pisne");
            songListLoadingScreen.setComment("Stiskni pro navrat");
        }
        nav.forceRender();
        return;
    }

    // Add the Back item
    _songListItems[_songCount] = ui::MenuItem::back("Zpet");

    // Rebuild the song list screen with the current count
    songListScreen = ui::MenuScreen("Pisne", _songListItems, _songCount + 1);
    songListScreen.setSelectedIndex(savedIndex);

    if (isFirstPage) {
        // Pop the loading screen if it was shown, push the song list
        if (_loadingScreenShown) {
            nav.pop();
            _loadingScreenShown = false;
        }
        nav.push(&songListScreen);
    }
    // For append: the screen is already on the stack, forceRender is enough

    nav.forceRender();
}

// Called from the delay scheduler (500ms) — shows the loading screen if we are still waiting
void songListDelayShow() {
    if (_songListState == SongListState::WaitingFirst && !_loadingScreenShown) {
        songListLoadingScreen.setSubtitle("Nacitani...");
        songListLoadingScreen.setComment("Cekam na PC");
        nav.push(&songListLoadingScreen);
        _loadingScreenShown = true;
        nav.forceRender();
    }
}

// Called from the timeout scheduler (3s)
void songListTimeout() {
    if (_songListState == SongListState::WaitingFirst) {
        songListDelayScheduler.stop();
        if (!_loadingScreenShown) {
            songListLoadingScreen.setSubtitle("PC neodpovida");
            songListLoadingScreen.setComment("Stiskni pro navrat");
            nav.push(&songListLoadingScreen);
            _loadingScreenShown = true;
        } else {
            songListLoadingScreen.setSubtitle("PC neodpovida");
            songListLoadingScreen.setComment("Stiskni pro navrat");
        }
        nav.forceRender();
    }
}

} // namespace GUI

// ── Scheduler ─────────────────────────────────────────────────────────

stmcpp::scheduler::Scheduler guiRenderScheduler(30_ms, &GUI::render, true, true);

// One-shot: delayed loading screen (500ms grace period)
stmcpp::scheduler::Scheduler songListDelayScheduler(500_ms, GUI::songListDelayShow, false, false);

// One-shot: timeout for loading the song list (3s)
stmcpp::scheduler::Scheduler songListTimeoutScheduler(3000_ms, GUI::songListTimeout, false, false);
