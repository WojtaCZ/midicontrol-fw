#include "menu.hpp"
#include <stmcpp/scheduler.hpp>
#include <stmcpp/units.hpp>
#include "oled.hpp"
#include "base.hpp"
#include "git.hpp"
#include "display.hpp"
#include "bluetooth.hpp"

#include <gfx/text.hpp>
#include <gfx/icon.hpp>
#include <gfx/shapes.hpp>

#include <cstdio>
#include <cstring>

using namespace stmcpp::units;

// External framebuffer defined in oled.cpp
extern Oled::OledBuffer frameBuffer;

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

// ── Settings menu items & screen (defined first so mainItems can reference it) ─

static ui::MenuItem settingsItems[] = {
    ui::MenuItem("Firmware", firmwareCallback),
    ui::MenuItem("Display",  displayCallback),
    ui::MenuItem::back("Zpet"),
};

static ui::MenuScreen settingsScreen("Nastaveni", settingsItems, 3);

// ── Main menu items & screen ──────────────────────────────────────────

static ui::MenuItem mainItems[] = {
    ui::MenuItem("Prehraj",  playCallback),
    ui::MenuItem("Nahraj",   recordCallback),
    ui::MenuItem::checkbox("Napajeni Varhan", false, powerCallback),
    ui::MenuItem("Nastaveni", static_cast<ui::Screen*>(&settingsScreen)),
};

static ui::MenuScreen mainScreen("Hlavni menu", mainItems, 4);

// ── Splash screen ─────────────────────────────────────────────────────

static char splashComment[32];

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

// ── Pairing confirm screen ─────────────────────────────────────────────

static void onPairResult(bool confirmed, void* userData) {
    (void)userData;
    if (!confirmed) {
        Bluetooth::sendCommand("K,1");
    }
    nav.pop();
}

static ui::ConfirmScreen pairingScreen("Pair device?", onPairResult);

// ── Callbacks ─────────────────────────────────────────────────────────

static void playCallback(ui::MenuItem* item, void* userData) {
    (void)item; (void)userData;
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
}

static void firmwareCallback(ui::MenuItem* item, void* userData) {
    (void)item; (void)userData;
    std::snprintf(fwLine0, sizeof(fwLine0), "Build: %s",  git::revision.c_str());
    std::snprintf(fwLine1, sizeof(fwLine1), "Branch: %s", git::branch.c_str());
    std::snprintf(fwLine2, sizeof(fwLine2), "Date: %s",   git::build_date.c_str());
    std::snprintf(fwLine3, sizeof(fwLine3), "Time: %s",   git::build_time.c_str());
    nav.push(&firmwareScreen);
}

static void displayCallback(ui::MenuItem* item, void* userData) {
    (void)item; (void)userData;

    const char* led = "OFF";
    switch (display::getLed()) {
        case display::ledColor::RED:    led = "RED";    break;
        case display::ledColor::GREEN:  led = "GREEN";  break;
        case display::ledColor::BLUE:   led = "BLUE";   break;
        case display::ledColor::YELLOW: led = "YELLOW"; break;
        case display::ledColor::OFF:    led = "OFF";    break;
    }

    std::snprintf(dispLine0, sizeof(dispLine0), "Connected: %s",
                  display::isConnected() ? "true" : "false");
    std::snprintf(dispLine1, sizeof(dispLine1), "Song: %u",
                  static_cast<unsigned>(display::getSong()));
    std::snprintf(dispLine2, sizeof(dispLine2), "Verse: %u",
                  static_cast<unsigned>(display::getVerse()));
    std::snprintf(dispLine3, sizeof(dispLine3), "Letter: %c",
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

    // Build splash comment string
    std::snprintf(splashComment, sizeof(splashComment), "%s (%s)",
                  git::revision.c_str(), git::branch.c_str());

    // Re-create splash with dismiss callback
    splashScreen = ui::SplashScreen(
        base::DEVICE_NAME.c_str(),
        base::DEVICE_TYPE.c_str(),
        splashComment,
        onSplashDismiss
    );

    nav.push(&splashScreen);
}

void render() {
    nav.tickScroll(30);

    bool dirty = nav.render(frameBuffer);

    if (Bluetooth::isConnected()) {
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
        if (cur == &mainScreen || cur == &settingsScreen) {
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
    nav.forceRender();
}

void renderForce() {
    nav.forceRender();
}

ui::Navigator& navigator() {
    return nav;
}

ui::MenuItem& checkboxPower() {
    return mainItems[2];
}

void showPairingConfirm() {
    nav.push(&pairingScreen);
}

} // namespace GUI

// ── Scheduler ─────────────────────────────────────────────────────────

stmcpp::scheduler::Scheduler guiRenderScheduler(30_ms, &GUI::render, true, true);
