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
    // Which instrument a mode gets inside the case. The case (bar, status line,
    // ribbon, hero, footer) is the same for all of them; only the body differs.
    enum Kind : uint8_t {
        FEED,       // sightings narrated to the console buffer, newest on top
        RANK,       // a ranked list read straight from WiFiScan's structures
        SPECTRUM,   // one bar per channel (Channel Summary)
        SERIES,     // a scrolling time series (Channel / BT Analyzer)
        METER,      // one signal, big: Fox Hunt
    };

    // Is this scan mode one RigView draws? Used by RigUI to route it here and by
    // MenuFunctions / WiFiScan to suppress their own drawing for it. Returns the
    // display title, or nullptr for a mode RigView does not own (attacks, the
    // packet-monitor oscilloscope, the GPS screens, Rig Mode's own view).
    static const char* title(uint8_t scan_mode);

    // Called every loop() while an owned scan is running. Lazily (re)initialises
    // when the running mode changes, gathers the instrument's data, and repaints
    // on its own cadence.
    void tick(uint32_t now);

    // Rig-styled brightness control: a blocking modal driven by UP/DOWN (or the
    // touch zones on touch boards), saved on SELECT or after a pause. Replaces
    // the stock touch-only screen, which on a button board could not be adjusted
    // at all and simply timed out.
    void brightnessModal();

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
    uint8_t  kind_        = FEED;     // Kind
    const char* title_    = nullptr;

    uint32_t last_frame_ms_ = 0;      // status line / ribbon / battery cadence
    uint32_t last_feed_ms_  = 0;      // body + hero + pulse cadence
    uint8_t  drawn_ch_      = 0xFF;   // ribbon: last channel drawn

    // ---- SPECTRUM: one bar per channel, with a peak cap that decays --------
    static const uint8_t SPEC_MAX = 14;   // channels shown per page
    uint8_t  spec_peak_[SPEC_MAX] = {};   // peak-hold value per bar
    uint32_t spec_peak_ms_ = 0;           // last decay step

    // ---- SERIES: the analyzer's scrolling history, plus its max/avg ---------
    int16_t  series_max_ = 0;
    int16_t  series_avg_ = 0;

    // ---- METER: the tracked signal and a short history of it ---------------
    static const uint8_t METER_HIST = 56;
    int8_t   meter_hist_[METER_HIST] = {};
    uint8_t  meter_hist_n_ = 0;
    int8_t   meter_rssi_   = 0;
    int8_t   meter_peak_   = 0;
    uint32_t meter_step_ms_ = 0;
    char     meter_name_[24] = "";

    // ---- RANK: rows are re-read from WiFiScan each paint --------------------
    uint32_t rank_sig_ = 0;               // hash of what was drawn last

    void begin(uint8_t scan_mode);
    void drainBuffer();
    void pushLine(const String& raw);
    void gather(uint32_t now);        // per-kind data refresh before a paint

    void drawFrame();                 // full: clear + static chrome
    void drawBar(bool full);          // bronze bar: title (full) + pulse + battery
    void drawStatus();                // GPS / SD line
    void drawRibbon();                // channel band
    void drawHero();                  // headline counters (per kind)
    void drawBody();                  // dispatches on kind_
    void drawFeed();                  // FEED
    void drawRank();                  // RANK
    void drawSpectrum();              // SPECTRUM
    void drawSeries();                // SERIES
    void drawMeter();                 // METER
};

extern RigView rig_view_obj;

#endif  // HAS_SCREEN
#endif  // RigView_h
