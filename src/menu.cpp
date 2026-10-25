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

// Schedulery — definovány na konci tohoto souboru
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
    theme.leftMargin     = 0;
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
static void ledMasterCallback(ui::MenuItem* item, void* userData);
static void midiRoleCallback(ui::MenuItem* item, void* userData);
static void pairCallback(ui::MenuItem* item, void* userData);

// ── Settings menu items & screen (defined first so mainItems can reference it) ─

static ui::MenuItem settingsItems[] = {
    ui::MenuItem::checkbox("LED",  true,  ledMasterCallback),
    ui::MenuItem::checkbox("Ident. MIDI", true, midiRoleCallback),
    ui::MenuItem("Verze FW", firmwareCallback),
    ui::MenuItem("Displej",  displayCallback),
    ui::MenuItem("Sparovat ovladac", pairCallback),
    ui::MenuItem::back("Zpet"),
};

static ui::MenuScreen settingsScreen("Nastaveni", settingsItems, 6);

// ── Main menu items & screen ──────────────────────────────────────────

static ui::MenuItem mainItems[] = {
    ui::MenuItem("Prehraj",  playCallback),
    ui::MenuItem("Nahraj",   recordCallback),
    ui::MenuItem::checkbox("Napajeni Varhan", false, powerCallback),
    ui::MenuItem("Nastaveni", static_cast<ui::Screen*>(&settingsScreen)),
};

static ui::MenuScreen mainScreen("Hlavni menu", mainItems, 4);

// ── Splash screen ─────────────────────────────────────────────────────

static ui::SplashScreen splashScreen("", "", "");

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

// ── Párování s ovladačem z menu (průběh a výsledek) ───────────────────

static bool _pairStatusShown = false;

static void onPairStatusDismiss(void* userData) {
    (void)userData;
    // Zavře jen obrazovku; párování doběhne (nebo vyprší) samo
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
    // Pop song list — NowPlaying bude pushed existujícím PLAY_SONG handlerem
    nav.pop();
}

// ── Obrazovka aktualizace Remote (relay z PC) ──────────────────────────

static ui::ProgressScreen fwProgressScreen("Aktualizace", "");
static bool fwPushed = false; // obrazovka je ve stacku navigatoru (vzdy jako jediná nad hlavním menu)

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
    // Z, F → sken; připojení k MIDIControlRemote, B a timeout řídí hlavní smyčka (main.cpp)
    // RN4870 nefunguje (běh bez BLE, spec R21): okamžitá zpráva místo čekání na timeout
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
                  display::isConnected() ? "ano" : "ne");
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

// ── Splash dismiss → main menu ────────────────────────────────────────

static void onSplashDismiss(void* userData) {
    (void)userData;
    nav.reset(&mainScreen);
}

// ── Public API ────────────────────────────────────────────────────────

void init() {
    initTheme();
    nav.setTheme(theme);

    // Splash: revize a větev na samostatných řádcích (dlouhá větev roluje)
    splashScreen = ui::SplashScreen(
        base::DEVICE_NAME.c_str(),
        base::DEVICE_TYPE.c_str(),
        git::revision.c_str(),
        onSplashDismiss
    );
    splashScreen.setComment2(git::branch.c_str());

    // Synchronizace checkboxů z uložených nastavení
    settingsItems[0].setChecked(settings::ledMasterEnable());
    settingsItems[1].setChecked(settings::midiRoleIdentifier());

    nav.push(&splashScreen);
}

void render() {
    nav.tickScroll(30);

    bool dirty = nav.render(frameBuffer);

    // Status ikony pouze na menu obrazovkách (ne na SplashScreen, ParagraphScreen, ConfirmScreen)
    ui::Screen* cur = nav.current();

    // Bez uloženého ovladače (nová base, upgrade z nastavení v1): nápověda místo titulku hlavního menu.
    // Base se v tom stavu k ničemu nepřipojuje, dokud uživatel nespáruje ovladač z menu Nastaveni.
    // 14 znaků 7x10 = 98 px, vejde se vlevo od ikony PC (x = 104).
    if (dirty && cur == &mainScreen && settings::remoteAddr()[0] == '\0') {
        gfx::drawFilledRect(frameBuffer, 0, 0, theme.displayWidth, theme.titleBarHeight, (uint8_t)0);
        gfx::text::drawText(frameBuffer, *theme.titleFont, "Sparuj ovladac", 0, 0, gfx::Anchor::TopLeft, (uint8_t)1);
    }
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

    // Ikona PC připojení (vlevo od BLE ikony)
    if (showStatusIcons && isPcAlive()) {
        int pcIconX = theme.displayOffsetX + theme.displayWidth - 14 - 2 - 10; // 14=BLE šířka, 2=mezera, 10=PC šířka
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

// noinline + used: test_fault.py ji volá přes gdb (vynucené překreslení OLED)
[[gnu::used, gnu::noinline]] void renderForce() {
    nav.forceRender();
}

ui::Navigator& navigator() {
    return nav;
}

ui::MenuItem& checkboxPower() {
    return mainItems[2];
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

    // Pri aktualizaci Remote zustava nahore obrazovka s postupem; Now Playing se nepushuje
    // (po aktualizaci se neukaze, dalsi SIGNAL_PLAYING ji zobrazi)
    if (fwPushed) return;

    // Opakovaný SIGNAL_PLAYING jen aktualizuje obrazovku, nepřidává ji znovu na zásobník
    if (!_nowPlayingActive) {
        _nowPlayingActive = true;
        nav.push(&nowPlayingScreen);
    }
    nav.forceRender();
}

void dismissNowPlaying() {
    if (_nowPlayingActive) {
        _nowPlayingActive = false;
        // Odebrat jen pokud je navrchu (nad ní může být např. dotaz na párování)
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
        // Rozjety dotaz na seznam pisni a stavove flagy obrazovek, ktere reset stacku odstrani
        _songListState = SongListState::Idle;
        _loadingScreenShown = false;
        songListTimeoutScheduler.stop();
        songListDelayScheduler.stop();
        _pairStatusShown = false;
        _nowPlayingActive = false;
        // Stack = hlavni menu + obrazovka aktualizace (zadny splash/podmenu pod ni)
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
        nav.reset(&mainScreen); // nemelo by nastat: nad ni se nic nepushuje
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
        // Sekce od PC ("Predehra", "Sloka 2")
        std::snprintf(_nowPlayingInfo, sizeof(_nowPlayingInfo), "%s [STOP]", _nowPlayingAnchor);
    } else {
        // Stav displeje po číslicích, tak jak je zobrazen v kostele (" 401", "1 ")
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
    mainItems[2].setChecked(base::current::isEnabled());
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

    // Uložení aktuální pozice pro append
    int savedIndex = isFirstPage ? 0 : songListScreen.selectedIndex();

    if (isFirstPage) {
        _songCount = 0;
    }

    // Parsování středníkem oddělených jmen písní
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

    // Prázdná složka (pouze pro první stránku)
    if (isFirstPage && _songCount == 0) {
        if (!_loadingScreenShown) {
            // Loading screen ještě nebyla zobrazena — zobrazíme ji s chybou
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

    // Přidání položky Zpět
    _songListItems[_songCount] = ui::MenuItem::back("Zpet");

    // Rekonstrukce song list screen s aktuálním počtem
    songListScreen = ui::MenuScreen("Pisne", _songListItems, _songCount + 1);
    songListScreen.setSelectedIndex(savedIndex);

    if (isFirstPage) {
        // Pop loading screen pokud byla zobrazena, push song list
        if (_loadingScreenShown) {
            nav.pop();
            _loadingScreenShown = false;
        }
        nav.push(&songListScreen);
    }
    // Pro append: screen je již na stacku, stačí forceRender

    nav.forceRender();
}

// Voláno z delay scheduleru (500ms) — zobrazí loading screen pokud stále čekáme
void songListDelayShow() {
    if (_songListState == SongListState::WaitingFirst && !_loadingScreenShown) {
        songListLoadingScreen.setSubtitle("Nacitani...");
        songListLoadingScreen.setComment("Cekam na PC");
        nav.push(&songListLoadingScreen);
        _loadingScreenShown = true;
        nav.forceRender();
    }
}

// Voláno z timeout scheduleru (3s)
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

// One-shot: timeout pro načítání seznamu písní (3s)
stmcpp::scheduler::Scheduler songListTimeoutScheduler(3000_ms, GUI::songListTimeout, false, false);
