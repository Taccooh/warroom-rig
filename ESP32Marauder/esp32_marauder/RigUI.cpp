#include "RigUI.h"

#ifdef HAS_SCREEN

#include "Display.h"
#include "MenuFunctions.h"
#include "RigTheme.h"
#include "TrackView.h"
#include "WiFiScan.h"

#if defined(HAS_GPS)
    extern TrackView track_view_obj;
#endif

#ifdef MARAUDER_CORE_MODE
    #include "WardriveCore.h"
    extern WardriveCore wardrive_core_obj;
#endif

#if defined(HAS_BUTTONS)
    #include "Switches.h"
    extern Switches u_btn, d_btn, l_btn, r_btn, c_btn;
#endif

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
        enterMenu();          // the Tools door — we draw the tree ourselves now
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

// =========================================================================
// Rig Mode session modal — start / stop / re-sync
// =========================================================================
// Same three actions as before, drawn in the console's language instead of a
// cyan box with a green highlight bar. It is the one control that decides
// whether the rig is actually collecting, so it gets the accent treatment the
// rest of the UI reserves for the thing you are about to commit to.
//
// Blocking on purpose: it owns the buttons for its whole duration, like the
// Core Mode and Upload pick lists, so nothing else can see the presses meant
// for it.
void RigUI::runSessionMenu() {
    #if defined(MARAUDER_CORE_MODE) && defined(HAS_BUTTONS) && (C_BTN >= 0) && \
        !defined(MARAUDER_CARDPUTER) && !defined(MARAUDER_CARDPUTER_ADV)

    auto& tft = display_obj.tft;

    // Let go of the R press that opened this, or it reads as cancel immediately.
    while (digitalRead(R_BTN) == LOW) delay(10);
    delay(50);

    const bool live = wardrive_core_obj.isCollecting();
    const char* opts[3];
    const char* subs[3];
    uint16_t tint[3];
    int nopts;
    if (live) {
        opts[0] = "Re-Sync";      subs[0] = "re-arm every node";   tint[0] = RigTheme::GOLD;
        opts[1] = "Stop Session"; subs[1] = "close the log";       tint[1] = RigTheme::RED;
        opts[2] = "Cancel";       subs[2] = "keep collecting";     tint[2] = RigTheme::DIM;
        nopts = 3;
    } else {
        opts[0] = "Start Session"; subs[0] = "begin collecting";   tint[0] = RigTheme::GREEN;
        opts[1] = "Cancel";        subs[1] = "back to the view";   tint[1] = RigTheme::DIM;
        nopts = 2;
    }

    // Panel sized to the option count, centred.
    const int16_t pw = TFT_WIDTH - 2 * RigTheme::PAD_X;
    const int16_t rh = 44, gap = 6;
    const int16_t ph = 40 + nopts * (rh + gap) + 10;
    const int16_t px = RigTheme::PAD_X, py = (TFT_HEIGHT - ph) / 2;

    int  sel = 0;
    bool done = false, chosen = false, dirty = true;
    bool pu = false, pd = false, pc = false, plr = false;

    while (!done) {
        if (dirty) {
            tft.fillRoundRect(px, py, pw, ph, RigTheme::RADIUS, RigTheme::PANEL);
            tft.drawRoundRect(px, py, pw, ph, RigTheme::RADIUS, RigTheme::GOLD);
            tft.setTextDatum(TL_DATUM);
            tft.setTextColor(RigTheme::GOLD, RigTheme::PANEL);
            tft.drawString("SESSION", px + 12, py + 10, 4);
            tft.setTextDatum(TR_DATUM);
            tft.setTextColor(live ? RigTheme::GREEN : RigTheme::DIM2, RigTheme::PANEL);
            tft.drawString(live ? "COLLECTING" : "IDLE", px + pw - 12, py + 18, 1);

            for (int i = 0; i < nopts; i++) {
                bool s = (i == sel);
                int16_t y = py + 40 + i * (rh + gap);
                uint16_t fill = s ? RigTheme::PANEL_S : RigTheme::PANEL;
                tft.fillRoundRect(px + 8, y, pw - 16, rh, 6, fill);
                if (s) {
                    tft.drawRoundRect(px + 8, y, pw - 16, rh, 6, tint[i]);
                    tft.fillRect(px + 13, y + 7, RigTheme::ACCENT_W, rh - 14, tint[i]);
                }
                tft.setTextDatum(TL_DATUM);
                tft.setTextColor(s ? tint[i] : RigTheme::INK, fill);
                tft.drawString(opts[i], px + 26, y + 6, 2);
                tft.setTextColor(RigTheme::DIM, fill);
                tft.drawString(subs[i], px + 26, y + 26, 1);
            }

            tft.setTextDatum(TL_DATUM);
            tft.setTextColor(RigTheme::DIM2, RigTheme::PANEL);
            tft.drawString("U/D move   C confirm   L/R cancel",
                           px + 12, py + ph - 14, 1);
            dirty = false;
        }

        bool u = (digitalRead(U_BTN) == LOW);
        if (u && !pu) { sel = (sel + nopts - 1) % nopts; dirty = true; }
        pu = u;
        bool d = (digitalRead(D_BTN) == LOW);
        if (d && !pd) { sel = (sel + 1) % nopts; dirty = true; }
        pd = d;
        bool lr = (digitalRead(L_BTN) == LOW) || (digitalRead(R_BTN) == LOW);
        if (lr && !plr) { done = true; }
        plr = lr;
        bool c = (digitalRead(C_BTN) == LOW);
        if (!c && pc) { chosen = true; done = true; }   // act on release
        pc = c;
        delay(15);
    }

    const char* flash = nullptr;
    uint16_t flash_tint = RigTheme::GOLD;
    if (chosen) {
        if (live) {
            if      (sel == 0) { wardrive_core_obj.resyncSession(); flash = "RE-SYNCED"; }
            else if (sel == 1) { wardrive_core_obj.stopSession();   flash = "STOPPED";
                                 flash_tint = RigTheme::RED; }
        } else {
            if (sel == 0)      { wardrive_core_obj.startSession();  flash = "STARTED";
                                 flash_tint = RigTheme::GREEN; }
        }
    }

    if (flash) {
        tft.fillRoundRect(px, py, pw, ph, RigTheme::RADIUS, RigTheme::PANEL);
        tft.drawRoundRect(px, py, pw, ph, RigTheme::RADIUS, flash_tint);
        tft.setTextDatum(MC_DATUM);
        tft.setTextColor(flash_tint, RigTheme::PANEL);
        tft.drawString(flash, px + pw / 2, py + ph / 2, 4);
        delay(600);
    }

    // Hand the screen back: refreshCoreDisplay repaints on its next tick, and
    // clearing here avoids the modal outline surviving underneath it.
    tft.setTextDatum(TL_DATUM);
    display_obj.clearScreen();
    #endif
}

// =========================================================================
// Tool tree — same tree, our language
// =========================================================================
// The nodes, their callables and the parent links are unchanged; only the
// painting is ours. Marauder drew these as raised keypad buttons with a status
// bar on top, which is the look that kept giving the rig away. Rows in the
// console's vocabulary — dark panel, gold accent on the selected one — make the
// scanners feel like a drawer of this device rather than a different program
// wearing its case.

static uint8_t menuVisibleRows() {
    int usable = TFT_HEIGHT - RigTheme::HEADER_H - RigTheme::FOOTER_H;
    int rows = usable / (RigTheme::ROW_H + RigTheme::ROW_GAP);
    if (rows < 1) rows = 1;
    if (rows > 24) rows = 24;
    return (uint8_t)rows;
}

void RigUI::drawMenuList() {
    Menu* m = menu_function_obj.current_menu;
    if (!m || !m->list) return;
    auto& tft = display_obj.tft;

    const uint8_t n    = (uint8_t)m->list->size();
    const uint8_t rows = menuVisibleRows();
    const uint8_t sel  = (uint8_t)m->selected;

    // Keep the selection inside the scroll window.
    if (sel < menu_top) menu_top = sel;
    else if (sel >= menu_top + rows) menu_top = sel - rows + 1;
    if (menu_top + rows > n) menu_top = (n > rows) ? (n - rows) : 0;

    tft.fillScreen(TFT_BLACK);

    // ---- header: where you are, and that there is a way back ----
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(RigTheme::GOLD, TFT_BLACK);
    tft.drawString(m->name, RigTheme::PAD_X, 10, 4);
    tft.drawFastHLine(RigTheme::PAD_X, 38, TFT_WIDTH - 2 * RigTheme::PAD_X,
                      RigTheme::OUTLINE);

    // Scroll position, only when there is something to scroll.
    if (n > rows) {
        tft.setTextDatum(TR_DATUM);
        tft.setTextColor(RigTheme::DIM2, TFT_BLACK);
        char pos[16];
        snprintf(pos, sizeof(pos), "%u/%u", (unsigned)(sel + 1), (unsigned)n);
        tft.drawString(pos, TFT_WIDTH - RigTheme::PAD_X, 22, 2);
    }

    // ---- rows ----
    const int16_t x = RigTheme::PAD_X;
    const int16_t w = TFT_WIDTH - 2 * RigTheme::PAD_X;
    for (uint8_t r = 0; r < rows; r++) {
        uint8_t i = menu_top + r;
        if (i >= n) break;
        MenuNode node = m->list->get(i);
        bool s = (i == sel);
        int16_t y = RigTheme::HEADER_H + r * (RigTheme::ROW_H + RigTheme::ROW_GAP);
        uint16_t fill = s ? RigTheme::PANEL_S : RigTheme::PANEL;

        tft.fillRoundRect(x, y, w, RigTheme::ROW_H, RigTheme::RADIUS, fill);
        if (s) {
            tft.drawRoundRect(x, y, w, RigTheme::ROW_H, RigTheme::RADIUS, RigTheme::GOLD);
            tft.fillRect(x + 5, y + 6, RigTheme::ACCENT_W,
                         RigTheme::ROW_H - 12, RigTheme::GOLD);
        } else {
            tft.drawRoundRect(x, y, w, RigTheme::ROW_H, RigTheme::RADIUS, RigTheme::OUTLINE);
        }

        // Toggle nodes carry their state in `selected`; show it as a dot rather
        // than the old green/red button frame, which read as "this is broken".
        bool is_toggle = (node.icon == SETTINGS);
        int16_t tx = x + 16;
        if (is_toggle) {
            tft.fillCircle(x + 18, y + RigTheme::ROW_H / 2, 4,
                           node.selected ? RigTheme::GREEN : RigTheme::DIM2);
            tx = x + 32;
        }

        tft.setTextDatum(ML_DATUM);
        tft.setTextColor(s ? RigTheme::GOLD : RigTheme::INK, fill);
        String label = node.name;
        uint8_t maxch = (uint8_t)((w - (tx - x) - 20) / 11);   // font 2 is ~11 px/char
        if (label.length() > maxch) label = label.substring(0, maxch);
        tft.drawString(label, tx, y + RigTheme::ROW_H / 2, 2);

        if (s) {   // chevron: this row is the one C acts on
            int ax = x + w - 14, ay = y + RigTheme::ROW_H / 2;
            tft.fillTriangle(ax, ay - 5, ax, ay + 5, ax + 6, ay, RigTheme::GOLD);
        }
    }

    // ---- footer ----
    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(RigTheme::DIM2, TFT_BLACK);
    tft.drawString("U/D move   C select   L back",
                   RigTheme::PAD_X, TFT_HEIGHT - 11, 1);
    tft.setTextDatum(TL_DATUM);
}

void RigUI::enterMenu() {
    menu_top = 0;
    screen = Screen::MENU;
    drawMenuList();
}

void RigUI::handleMenuInput(uint32_t currentTime) {
    #if defined(HAS_BUTTONS) && (C_BTN >= 0) && (U_BTN >= 0) && (D_BTN >= 0) && \
        !defined(MARAUDER_CARDPUTER) && !defined(MARAUDER_CARDPUTER_ADV)

        Menu* m = menu_function_obj.current_menu;
        if (!m || !m->list) return;
        const uint8_t n = (uint8_t)m->list->size();
        if (n == 0) return;

        bool u = (digitalRead(U_BTN) == LOW);
        if (u && !nav_up_down) {
            m->selected = (m->selected == 0) ? (n - 1) : (m->selected - 1);
            drawMenuList();
        }
        nav_up_down = u;

        bool d = (digitalRead(D_BTN) == LOW);
        if (d && !nav_dn_down) {
            m->selected = (m->selected + 1 >= n) ? 0 : (m->selected + 1);
            drawMenuList();
        }
        nav_dn_down = d;

        // LEFT — up one level. The tree also has explicit "Back" rows; both work.
        #if (L_BTN >= 0)
        bool l = (digitalRead(L_BTN) == LOW);
        if (l && !nav_l_down) {
            nav_l_down = true;
            if (m->parentMenu) {
                menu_function_obj.current_menu = m->parentMenu;
                if (menu_function_obj.current_menu == menu_function_obj.getMainMenu()) {
                    screen = Screen::HOME;
                    cursor = (uint8_t)menu_function_obj.getMainMenu()->selected;
                    if (cursor >= entryCount()) cursor = 0;
                    display_obj.clearScreen();
                    drawHome();
                } else {
                    menu_top = 0;
                    drawMenuList();
                }
                return;
            }
        } else if (!l) {
            nav_l_down = false;
        }
        #endif

        bool c = (digitalRead(C_BTN) == LOW);
        if (c && !c_down) {
            c_down = true;
            c_press_start_ms = currentTime;
        } else if (!c && c_down) {
            c_down = false;
            if (swallow_c_release) { swallow_c_release = false; return; }

            Menu* before = m;
            MenuNode node = m->list->get(m->selected);
            if (node.callable) node.callable();

            if (wifi_scan_obj.currentScanMode != WIFI_SCAN_OFF) {
                running_is_legacy = true;      // legacy views keep the old dispatcher
                screen = Screen::RUNNING;
            } else if (menu_function_obj.current_menu != before) {
                // Navigated somewhere — including back out to the console.
                if (menu_function_obj.current_menu == menu_function_obj.getMainMenu()) {
                    screen = Screen::HOME;
                    cursor = (uint8_t)menu_function_obj.getMainMenu()->selected;
                    if (cursor >= entryCount()) cursor = 0;
                    display_obj.clearScreen();
                    drawHome();
                } else {
                    menu_top = 0;
                    drawMenuList();
                }
            } else {
                // Stayed put: a toggle flipped, or a blocking view drew over us
                // and has since returned. Either way the list needs repainting.
                drawMenuList();
            }
        }
    #endif
}

void RigUI::main(uint32_t currentTime) {
    // Record the trail regardless of which screen is up, so opening TRACK shows
    // the whole drive rather than starting from wherever you happened to look.
    // sample() rate-limits and rejects jitter itself, so this is cheap.
    #if defined(HAS_GPS)
        track_view_obj.sample(currentTime);
    #endif

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
        // Come back to where the mode was started from. A scanner launched out
        // of Tools should return to that list, not dump the user on the console
        // several levels away from what they were doing.
        Menu* back_to = menu_function_obj.current_menu;
        bool to_menu = running_is_legacy && back_to &&
                       back_to != menu_function_obj.getMainMenu();
        running_is_legacy = false;
        display_obj.clearScreen();
        if (to_menu) {
            screen = Screen::MENU;
            drawMenuList();
        } else {
            screen = Screen::HOME;
            menu_function_obj.current_menu = menu_function_obj.getMainMenu();
            drawHome();
        }
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
                        runSessionMenu();   // blocking modal, rig-styled
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

        case Screen::MENU:
            handleMenuInput(currentTime);
            break;
    }
}

#endif  // HAS_SCREEN
