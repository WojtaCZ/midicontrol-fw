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

    // Access to the checkboxes in the settings
    ui::MenuItem& checkboxLedMaster();
    ui::MenuItem& checkboxMidiRole();

    // Pairing result from the menu ("Sparovano", ...) on the "Parovani" screen, if it is open
    void showPairingResult(const char* text);

    // Show/hide the "Přehrávání" screen
    void showNowPlaying(const char* songName);
    void dismissNowPlaying();

    // Remote update relayed via the Base: progress screen; hint must be a string literal (it is compared by pointer).
    // On the first display it empties the stack (main menu + screen); Now Playing and the song list are not pushed during the update.
    void showFwProgress(const char* hint, uint8_t percent);
    // End of the update: the screen is removed from the stack (back to the main menu)
    void hideFwProgress();

    // Update of the info on the "Now Playing" screen (song/verse/letter from display::)
    void updateNowPlayingInfo();

    // Section label from SIGNAL_ANCHOR ("Sloka 2")
    void setNowPlayingAnchor(const char* label);

    // "Napajeni Varhan" checkbox according to the actual SSR state (e.g. after SET_CURRENT from the PC)
    void syncPowerCheckbox();

    // Registers the callback for stopping playback (called on dismiss from the OLED)
    using StopCallback = void (*)();
    void setStopCallback(StopCallback cb);

    // Callbacks for the song list and playback
    using RequestSongListCallback = void (*)(uint8_t offset);
    using PlaySongCallback = void (*)(const char* songName);
    void setRequestSongListCallback(RequestSongListCallback cb);
    void setPlaySongCallback(PlaySongCallback cb);

    // Handling of the song list response from the PC
    void handleSongListResponse(const char* payload, uint8_t len, bool hasMore);

    // Internal: timeout handler for song list loading
    void songListTimeout();

    // Internal: delayed loading screen handler
    void songListDelayShow();
}

#endif
