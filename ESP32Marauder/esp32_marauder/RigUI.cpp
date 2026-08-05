#include "RigUI.h"

#ifdef HAS_SCREEN

#include "Display.h"
#include "MenuFunctions.h"
#include "WiFiScan.h"

extern Display display_obj;
extern MenuFunctions menu_function_obj;
extern WiFiScan wifi_scan_obj;

// How often the home header's live line (GPS fix, satellites, SD, battery) is
// repainted while sitting on the console. Same cadence the old framework used
// for its status bar, so the information feels no less current than before.
#ifndef RIGUI_HEADER_REFRESH_MS
    #define RIGUI_HEADER_REFRESH_MS 1000
#endif

// Hold-to-repeat is deliberately absent: the console has a handful of entries
// and a stray repeat on a rig you are holding one-handed in the dark costs more
// than the convenience is worth.

void RigUI::init() {
    screen = Screen::HOME;
    cursor = 0;
    last_header_refresh_ms = 0;

    Menu* home = menu_function_obj.getMainMenu();
    if (home) home->selected = cursor;

    menu_function_obj.current_menu = home;
    drawHome();
}

uint8_t RigUI::entryCount() const {
    Menu* home = menu_function_obj.getMainMenu();
    if (!home || !home->list) return 0;
    return (uint8_t)home->list->size();
}

void RigUI::drawHome(int only) {
    Menu* home = menu_function_obj.getMainMenu();
    if (home) home->selected = cursor;   // the renderer reads the highlight from here
    menu_function_obj.drawRigHome(only);
}

// Run the entry the cursor sits on. The node's own callable does the work —
// it is the single definition of what each console entry means, shared with
// the legacy code path, so there is no second list here to drift out of sync.
void RigUI::activate(uint8_t index) {
    Menu* home = menu_function_obj.getMainMenu();
    if (!home || !home->list || index >= home->list->size()) return;

    MenuNode node = home->list->get(index);
    if (!node.callable) return;

    node.callable();

    // Where we landed is read from the resulting state rather than from the
    // index: a rig function leaves a scan mode running, the Tools door leaves
    // us on a different menu. Nothing here needs to know which slot is which.
    if (wifi_scan_obj.currentScanMode != WIFI_SCAN_OFF) {
        running_is_legacy = false;   // one of ours: self-drawing, self-exiting
        screen = Screen::RUNNING;
    } else if (menu_function_obj.current_menu != home) {
        screen = Screen::LEGACY;
    } else {
        drawHome();   // entry did nothing lasting — repaint and stay
    }
}

void RigUI::handleHomeInput(uint32_t currentTime) {
    const uint8_t n = entryCount();
    if (n == 0) return;

    // ---- Touch boards (Marauder V8): the cards are tapped directly ----------
    // These boards have no nav buttons at all, so without this the console
    // would render and accept nothing. Same geometry as the button path uses
    // for highlighting — only the input differs.
    #ifdef HAS_TOUCH
        if (!menu_function_obj.disable_touch) {
            uint16_t t_x, t_y;
            if (display_obj.updateTouch(&t_x, &t_y)) {
                int idx = menu_function_obj.rigHomeHitTest(t_x, t_y);
                if (idx >= 0 && idx < (int)n) {
                    uint16_t rx, ry;
                    while (display_obj.updateTouch(&rx, &ry)) delay(5);  // consume the tap
                    cursor = (uint8_t)idx;
                    drawHome(idx);            // flash the tapped card
                    activate(cursor);
                }
            }
        }
    #endif

    // ---- Button boards (Marauder V7) ---------------------------------------
    #if defined(HAS_BUTTONS) && (C_BTN >= 0) && (U_BTN >= 0) && (D_BTN >= 0) && \
        !defined(MARAUDER_CARDPUTER) && !defined(MARAUDER_CARDPUTER_ADV)

        // UP / DOWN — move the highlight. Only the two affected entries are
        // repainted, so navigating does not flash the whole console.
        bool u = (digitalRead(U_BTN) == LOW);
        if (u && !nav_up_down && cursor > 0) {
            uint8_t prev = cursor--;
            drawHome(prev);
            drawHome(cursor);
        }
        nav_up_down = u;

        bool d = (digitalRead(D_BTN) == LOW);
        if (d && !nav_dn_down && cursor + 1 < n) {
            uint8_t prev = cursor++;
            drawHome(prev);
            drawHome(cursor);
        }
        nav_dn_down = d;

        // CENTER — activate on release, so a press that turns into a hold does
        // not fire first and then leave the run-view seeing the same hold as
        // its own exit gesture.
        bool c = (digitalRead(C_BTN) == LOW);
        if (c && !c_down) {
            c_down = true;
            c_press_start_ms = currentTime;
        } else if (!c && c_down) {
            c_down = false;
            if (swallow_c_release) {
                // This is the tail of the hold that closed a run-view, not a
                // fresh press. Drop it.
                swallow_c_release = false;
            } else {
                activate(cursor);
            }
        }
    #endif
}

void RigUI::main(uint32_t currentTime) {
    // Modes that signal their exit by raising exit_draw rather than clearing the
    // scan mode themselves. The old dispatcher translated that into "stop"; for
    // our own run-views we have to do it here, or the mode would never end.
    if (screen == Screen::RUNNING && !running_is_legacy && display_obj.exit_draw) {
        if (wifi_scan_obj.currentScanMode != WIFI_CONNECTED)
            wifi_scan_obj.currentScanMode = WIFI_SCAN_OFF;
        display_obj.exit_draw = false;
    }

    // A run-view can end at any time from its own input or an error. Whichever
    // way it went, currentScanMode returning to OFF is the handover signal.
    if (screen == Screen::RUNNING && wifi_scan_obj.currentScanMode == WIFI_SCAN_OFF) {
        screen = Screen::HOME;
        running_is_legacy = false;
        menu_function_obj.current_menu = menu_function_obj.getMainMenu();
        display_obj.clearScreen();
        drawHome();
        // The exit gesture is a CENTER hold, so the button is still down right
        // now. Adopt that as an in-progress press and mark its release to be
        // dropped -- otherwise letting go immediately relaunches the mode.
        #if defined(HAS_BUTTONS) && (C_BTN >= 0) && \
            !defined(MARAUDER_CARDPUTER) && !defined(MARAUDER_CARDPUTER_ADV)
            c_down = (digitalRead(C_BTN) == LOW);
            swallow_c_release = c_down;
        #endif
        c_press_start_ms = currentTime;
    }

    switch (screen) {
        case Screen::HOME: {
            handleHomeInput(currentTime);
            if (currentTime - last_header_refresh_ms >= RIGUI_HEADER_REFRESH_MS) {
                last_header_refresh_ms = currentTime;
                menu_function_obj.drawRigHeader(false);   // live line only
            }
            break;
        }

        case Screen::RUNNING: {
            if (running_is_legacy) {
                // Legacy scans keep the whole old dispatcher: their exit taps,
                // screen orientation and status bar all live in there.
                menu_function_obj.main(currentTime);
                break;
            }
            // Rig Mode's session control (start / stop / re-sync) hangs off the
            // R button — the on-screen hint says "R: session". That press used
            // to be read by the legacy dispatcher, so bypassing it left Rig Mode
            // with no way to start collecting at all. Read it here instead.
            #if defined(MARAUDER_CORE_MODE) && defined(HAS_BUTTONS) && (R_BTN >= 0) && \
                !defined(MARAUDER_CARDPUTER) && !defined(MARAUDER_CARDPUTER_ADV)
                if (wifi_scan_obj.currentScanMode == WIFI_SCAN_WAR_DRIVE_CORE) {
                    bool r = (digitalRead(R_BTN) == LOW);
                    if (r && !nav_r_down) {
                        nav_r_down = true;
                        menu_function_obj.runCoreSessionMenu();   // blocking modal
                    } else if (!r) {
                        nav_r_down = false;
                    }
                    break;   // Rig Mode owns the rest of its screen
                }
            #endif

            // Ours draw themselves. Rig Mode owns its full screen including the
            // header; Upload and File Server deliberately leave the framework
            // status bar in place at the top and draw below it, so that strip
            // still needs refreshing or it would sit frozen for the whole run.
            #if defined(MARAUDER_WDGWARS_UPLOAD) || defined(MARAUDER_FILE_SERVER_AP)
                bool wants_bar = false;
                #ifdef MARAUDER_WDGWARS_UPLOAD
                    wants_bar |= (wifi_scan_obj.currentScanMode == WIFI_SCAN_WDGWARS_UPLOAD);
                #endif
                #ifdef MARAUDER_FILE_SERVER_AP
                    wants_bar |= (wifi_scan_obj.currentScanMode == WIFI_SCAN_FILE_SERVER_AP);
                #endif
                if (wants_bar &&
                    currentTime - last_header_refresh_ms >= RIGUI_HEADER_REFRESH_MS) {
                    last_header_refresh_ms = currentTime;
                    menu_function_obj.updateStatusBar();
                }
            #endif
            break;
        }

        case Screen::LEGACY: {
            menu_function_obj.main(currentTime);
            // Backing out of the tool tree lands on the console's own menu node.
            // displayCurrentMenu() already repainted the console on the way out,
            // so we only re-sync state here — clearing and redrawing would put a
            // visible blank frame on screen for no gain.
            if (menu_function_obj.current_menu == menu_function_obj.getMainMenu() &&
                wifi_scan_obj.currentScanMode == WIFI_SCAN_OFF) {
                screen = Screen::HOME;
                cursor = menu_function_obj.getMainMenu()->selected;
                if (cursor >= entryCount()) cursor = 0;
                last_header_refresh_ms = currentTime;
            } else if (wifi_scan_obj.currentScanMode != WIFI_SCAN_OFF) {
                // A legacy tool started a scan — it keeps the screen until it
                // ends, then RUNNING hands us back to the console.
                running_is_legacy = true;
                screen = Screen::RUNNING;
            }
            break;
        }
    }
}

#endif  // HAS_SCREEN
