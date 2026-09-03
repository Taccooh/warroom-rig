#pragma once

#ifndef RigView_h
#define RigView_h

#include "configs.h"

#ifdef HAS_SCREEN

#include <Arduino.h>

// =========================================================================
// RigView — one instrument case for every scanner
// =========================================================================
// The rig's home console, menu and Rig Mode all speak the same visual language;
// the Marauder scanners launched from Tools did not. They still drew the stock
// green scrolling console -- the "the tools themselves are still original
// Marauder" complaint. RigView is the case those run-views now live in: a bronze
// identity bar with a live traffic pulse, the honest GPS/SD status line, a
// channel ribbon, a hero tile of headline counters, and a body that is a live
// feed of sightings, newest at the top.
//
// How it hooks in without touching thirty scattered scan callbacks: every stock
// scanner already narrates itself into display_obj.display_buffer, one line per
// sighting. RigUI routes those modes here instead of to the Marauder console;
// tick() drains that buffer, turns each line into a feed row (parsing out the
// signal strength and channel where the line carries them), and paints the case.
// Nothing in WiFiScan has to know the case exists.
//
// Drawing happens only from tick(), which is called from loop() by RigUI. The
// scan callbacks run in radio/BLE context and must never touch the TFT; they add
// to the buffer, we read it. Repaints are throttled -- the case at 1 Hz, the
// feed and pulse at 4 Hz -- so a busy channel cannot pin the draw at the packet
// rate.
// =========================================================================

class RigView {
public:
    // Is this scan mode one RigView draws? Used by RigUI to route it here and by
    // MenuFunctions to suppress the Marauder console/status-bar for it. Returns
    // the display title, or nullptr for a mode RigView does not own (attacks,
    // analyzers, the GPS screens, Rig Mode's own full-screen view).
    static const char* title(uint8_t scan_mode);

    // Called every loop() while an owned scan is running. Lazily (re)initialises
    // when the running mode changes, drains the console buffer into the feed, and
    // repaints on its own cadence.
    void tick(uint32_t now);

private:
    // A signal class, from the strongest bar colour down. Also carries "alert",
    // the red a stock line asks for with its ;red; prefix (a detected deauth).
    enum Klass : uint8_t { K_WEAK = 0, K_MID, K_STRONG, K_ALERT };

    struct FeedRow {
        char     text[40];   // the sighting line, colour keys stripped
        int8_t   rssi;       // parsed dBm, or 0 if the line carried none
        uint8_t  ch;         // parsed channel, or 0
        uint8_t  klass;      // Klass
        uint16_t hits;       // how many times this exact line repeated
    };

    static const uint8_t RING = 20;   // sightings kept; the tall screen shows ~8

    FeedRow  ring_[RING];
    uint8_t  write_idx_ = 0;          // next slot to fill
    uint8_t  filled_    = 0;          // slots in use (caps at RING)
    bool     need_full_ = true;       // a full repaint is due (mode just began)
    uint16_t total_     = 0;          // sightings this run (with repeats)
    uint32_t changes_   = 0;          // bumped whenever the feed content changes
    uint32_t drawn_changes_ = 0xFFFFFFFF;
    int8_t   peak_rssi_ = 0;          // strongest seen, 0 = none

    // Rolling rate: sightings in the last minute, in ~5 s buckets.
    static const uint8_t RATE_BUCKETS = 12;
    uint16_t rate_[RATE_BUCKETS] = {};
    uint8_t  rate_head_ = 0;
    uint32_t rate_step_ms_ = 0;

    // Traffic pulse: one column per sampling slot, most recent on the right.
    uint8_t  pulse_[16] = {};         // RigTheme::PULSE_N <= 16
    uint16_t pulse_accum_ = 0;        // sightings since the last slot closed
    uint32_t pulse_step_ms_ = 0;

    uint8_t  mode_        = 0;        // WIFI_SCAN_OFF -> "not initialised"
    uint8_t  band_        = 0;        // 0 = 2.4 GHz Wi-Fi, 1 = BLE
    const char* title_    = nullptr;

    uint32_t last_frame_ms_ = 0;      // status line / ribbon / battery cadence
    uint32_t last_feed_ms_  = 0;      // feed + hero + pulse cadence
    uint8_t  drawn_ch_      = 0xFF;   // ribbon: last channel drawn

    void begin(uint8_t scan_mode);
    void drainBuffer();
    void pushLine(const String& raw);

    void drawFrame();                 // full: clear + static chrome
    void drawBar(bool full);          // bronze bar: title (full) + pulse + battery
    void drawStatus();                // GPS / SD line
    void drawRibbon();                // channel band
    void drawHero();                  // headline counters
    void drawFeed();                  // the sightings
};

extern RigView rig_view_obj;

#endif  // HAS_SCREEN
#endif  // RigView_h
