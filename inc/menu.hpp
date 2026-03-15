#ifndef MENU_H
#define MENU_H

#include <ui/navigator.hpp>
#include <ui/menu_screen.hpp>
#include <ui/splash_screen.hpp>
#include <ui/paragraph_screen.hpp>
#include <ui/confirm_screen.hpp>
#include <ui/item.hpp>
#include <ui/theme.hpp>
#include <ui/input.hpp>

namespace GUI {
    // Initialize the theme, screens, navigator; called once at startup
    void init();

    // Main loop hook — renders and ticks scroll (call at ~30ms interval)
    void render();

    // Input routing
    void keypress(ui::InputEvent event);

    // Force re-render
    void renderForce();

    // Access to navigator for pushing custom screens
    ui::Navigator& navigator();

    // Access to power checkbox for external state changes
    ui::MenuItem& checkboxPower();

    // Show the BLE pairing confirmation dialog
    void showPairingConfirm();
}

#endif
