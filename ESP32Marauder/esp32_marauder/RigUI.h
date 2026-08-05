#pragma once

#ifndef RigUI_h
#define RigUI_h

#include "configs.h"

#ifdef HAS_SCREEN

#include <Arduino.h>

// =========================================================================
// RigUI — the rig's own screen stack
// =========================================================================
// Owns control flow for every screen the rig actually uses: the home console,
// launching and leaving the three rig functions, and the door to the legacy
// Marauder tool tree.
//
// Why this exists. MenuFunctions::main() is a single dispatcher that assumes
// every screen is a Marauder button-list with a Marauder status bar, and it
// carries the mode table of a firmware that could still attack things. Our
// screens fit none of that, so each one had to opt out of the framework
// individually — and every spot that was missed leaked Marauder chrome through
// (the status bar flashing past on Rig Mode entry was exactly that). Inverting
// the ownership fixes the class of bug rather than instances of it: our screens
// are handled here and never touch the legacy dispatcher at all.
//
// Boundary with the old framework. The three rig functions are self-contained
// run-views: they paint their own screens and exit themselves, setting
// currentScanMode back to WIFI_SCAN_OFF (WardriveCore::deinit,
// WdgwarsUpload::deinit). So this class does not drive them — it starts them,
// then watches for that transition and takes the screen back.
//
// The legacy tool tree still runs on MenuFunctions. Picking "Tools" hands the
// display over; when the user backs out to the root we reclaim it. That keeps
// the Marauder scanners available without their framework touching our screens,
// and leaves dropping them later as a menu-tree edit rather than surgery.
// =========================================================================

class RigUI {
public:
    void init();                     // take the screen; draw home
    void main(uint32_t currentTime); // called from loop() in place of MenuFunctions::main
    void drawMenuList();             // paints any menu in the rig's language

private:
    enum class Screen : uint8_t {
        HOME,     // our console — we own input and drawing
        RUNNING,  // a rig function owns the screen; wait for it to hand back
        MENU,     // the tool tree, drawn by us in the rig's own language
    };

    Screen screen = Screen::HOME;
    uint8_t cursor = 0;              // highlighted home entry

    // Whether the mode currently running was started from the legacy tool tree.
    // Those still expect the old dispatcher for their exit gestures and screen
    // handling, so they keep getting it; only our own run-views bypass it.
    bool running_is_legacy = false;

    // Button edge/hold tracking. Polled with digitalRead for the same reason
    // the run-views do it: Marauder's Switches wrapper is edge-triggered only
    // and cannot report "still held".
    bool nav_up_down = false;
    bool nav_dn_down = false;
    bool nav_r_down = false;
    bool nav_l_down = false;
    bool c_down = false;
    uint32_t c_press_start_ms = 0;

    // Set when we take the screen back from a run-view that exited on a CENTER
    // hold. The button is still down at that moment, and its release would
    // otherwise read as "activate the entry under the cursor" — relaunching the
    // mode the user just left.
    bool swallow_c_release = false;

    uint32_t last_header_refresh_ms = 0;

    uint8_t entryCount() const;
    void drawHome(int only = -1);
    void handleHomeInput(uint32_t currentTime);
    void activate(uint8_t index);

    // ---- tool tree, drawn in the rig's language instead of Marauder's -------
    uint8_t menu_top = 0;            // first visible row (scroll window)
    void handleMenuInput(uint32_t currentTime);
    void enterMenu();                // adopt whatever menu we were handed
    void runSessionMenu();           // Rig Mode start/stop/re-sync, rig-styled
};

extern RigUI rig_ui_obj;

#endif  // HAS_SCREEN
#endif  // RigUI_h
