#include "RigView.h"

#ifdef HAS_SCREEN

#include "Display.h"
#include "RigTheme.h"
#include "RigInput.h"       // RIG_HINT_*, RigInput::down
#include "WiFiScan.h"
#include "GorillaMini.h"

extern Display  display_obj;
extern WiFiScan wifi_scan_obj;

// The scan result lists live in WiFiScan.cpp as plain globals; there is no
// header that declares them, so every file that reads them says so itself.
extern LinkedList<AccessPoint>* access_points;
extern LinkedList<Station>*     stations;
extern LinkedList<BleDevice>*   ble_devices;

#ifdef HAS_GPS
    #include "GpsInterface.h"
    extern GpsInterface gps_obj;
#endif
#ifdef HAS_BATTERY
    #include "BatteryInterface.h"
    extern BatteryInterface battery_obj;
#endif
#if defined(HAS_SD) && !defined(HAS_C5_SD)
    #include "SDInterface.h"
    extern SDInterface sd_obj;
#endif

RigView rig_view_obj;

// ---------------------------------------------------------------------------
// Palette. Aliased from RigTheme so the case tracks the rest of the UI; the two
// signal greens/ambers match WardriveCore's node table on purpose, so a row in
// the feed and a node in Rig Mode read as the same strength.
// ---------------------------------------------------------------------------
static const uint16_t RV_GOLD   = RigTheme::GOLD;
static const uint16_t RV_GOLD_D = RigTheme::GOLD_D;
static const uint16_t RV_INK    = RigTheme::INK;
static const uint16_t RV_DIM    = RigTheme::DIM;
static const uint16_t RV_DIM2   = RigTheme::DIM2;
static const uint16_t RV_PANEL  = RigTheme::PANEL;
static const uint16_t RV_OUT    = RigTheme::OUTLINE;
static const uint16_t RV_GREEN  = 0x6E6D;   // strong signal
static const uint16_t RV_AMBER  = 0xFD20;   // mid signal
static const uint16_t RV_RED    = RigTheme::RED;

// ---------------------------------------------------------------------------
// Which scan modes RigView owns, with their title, band and instrument. A mode
// absent from this table keeps the stock behaviour untouched -- the attacks,
// the packet-monitor oscilloscope, the GPS screens, Rig Mode's own view.
// Titles are kept short: the bronze bar has room for about nine characters
// between the mascot and the pulse.
//
// FEED rows only for modes that narrate their sightings into
// display_obj.display_buffer. Modes that paint their own graphics are either
// given the instrument that reads their data directly (RANK / SPECTRUM /
// SERIES / METER, with their stock drawing suppressed in WiFiScan and
// MenuFunctions) or left alone.
// ---------------------------------------------------------------------------
namespace {
struct RVMode { uint8_t mode; const char* title; uint8_t band; uint8_t kind; };
const RVMode RV_MODES[] = {
    // ---- FEED: console narrators ----
    { WIFI_SCAN_PROBE,          "PROBES",    0, RigView::FEED },   // RunProbeScan
    { WIFI_SCAN_AP,             "BEACONS",   0, RigView::FEED },   // RunBeaconScan
    { WIFI_SCAN_DEAUTH,         "DEAUTH",    0, RigView::FEED },   // RunDeauthScan (detector)
    { WIFI_SCAN_PWN,            "PWNGOTCHI", 0, RigView::FEED },   // RunPwnScan
    { WIFI_SCAN_PINESCAN,       "PINE AP",   0, RigView::FEED },   // RunPineScan
    { WIFI_SCAN_MULTISSID,      "MULTISSID", 0, RigView::FEED },   // RunMultiSSIDScan
    { WIFI_SCAN_AP_STA,         "AP + STA",  0, RigView::FEED },   // RunAPScan
    { WIFI_SCAN_WAR_DRIVE,      "WARDRIVE",  0, RigView::FEED },   // RunBeaconScan + BT; hero = its counters
    { BT_SCAN_ALL,              "BT SCAN",   1, RigView::FEED },   // RunBluetoothScan
    { BT_SCAN_SIMPLE,           "BT SIMPLE", 1, RigView::FEED },
    { BT_SCAN_SIMPLE_TWO,       "BT SIMPLE", 1, RigView::FEED },
    { BT_SCAN_AIRTAG,           "AIRTAGS",   1, RigView::FEED },
    { BT_SCAN_FLIPPER,          "FLIPPERS",  1, RigView::FEED },
    { BT_SCAN_SKIMMERS,         "SKIMMERS",  1, RigView::FEED },
    { BT_SCAN_RAYBAN,           "RAY-BAN",   1, RigView::FEED },
    { BT_SCAN_FLOCK,            "FLOCK",     1, RigView::FEED },
    // ---- RANK: structured lists read from WiFiScan ----
    { WIFI_SCAN_DETECT_FOLLOW,  "MAC MON",   0, RigView::RANK },   // build_top10_for_ui
    { WIFI_SCAN_PACKET_RATE,    "PKT COUNT", 0, RigView::RANK },   // selected APs/stations .packets
    // ---- SPECTRUM / SERIES: the graph analyzers ----
    { WIFI_SCAN_CHAN_ACT,       "CHANNELS",  0, RigView::SPECTRUM },
    { WIFI_SCAN_CHAN_ANALYZER,  "ANALYZER",  0, RigView::SERIES },
    { BT_SCAN_ANALYZER,         "BT ANALYZ", 1, RigView::SERIES },
    // ---- METER: Fox Hunt ----
    { WIFI_SCAN_SIG_STREN,      "FOX HUNT",  0, RigView::METER },
    { BT_SCAN_FOX_HUNT,         "FOX HUNT",  1, RigView::METER },
};
const RVMode* rvLookup(uint8_t m) {
    for (const RVMode& e : RV_MODES) if (e.mode == m) return &e;
    return nullptr;
}

// Signal class from RSSI, the same ladder the feed stripes use.
uint16_t rvSignalColor(int8_t rssi) {
    if (rssi == 0)    return RV_DIM2;
    if (rssi >= -60)  return RV_GREEN;
    if (rssi >= -75)  return RV_AMBER;
    return RV_DIM2;
}

// Compact a count so it never overflows a hero column.
void rvFmt(char* out, size_t n, uint32_t v) {
    if (v < 10000) snprintf(out, n, "%lu", (unsigned long)v);
    else           snprintf(out, n, "%luk", (unsigned long)(v / 1000));
}

// Four rising signal bars, lit by RSSI -- the same ladder WardriveCore uses.
// rssi == 0 means the line carried no strength; all bars stay dim.
void rvBars(int x, int y, int8_t rssi, uint16_t col) {
    int lvl = 0;
    if (rssi != 0) {
        if      (rssi >= -55) lvl = 4;
        else if (rssi >= -68) lvl = 3;
        else if (rssi >= -78) lvl = 2;
        else                  lvl = 1;
    }
    for (int i = 0; i < 4; i++) {
        int bh = 3 + i * 2;
        display_obj.tft.fillRect(x + i * 4, y + 10 - bh, 3, bh, (i < lvl) ? col : RV_DIM2);
    }
}

// A MAC as the short form the rows have room for.
void rvMacShort(char* out, size_t n, const uint8_t* mac) {
    snprintf(out, n, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}
}  // namespace

const char* RigView::title(uint8_t scan_mode) {
    const RVMode* e = rvLookup(scan_mode);
    return e ? e->title : nullptr;
}

// ---------------------------------------------------------------------------

void RigView::begin(uint8_t scan_mode) {
    mode_  = scan_mode;
    const RVMode* e = rvLookup(scan_mode);
    title_ = e ? e->title : nullptr;
    band_  = e ? e->band : 0;
    kind_  = e ? e->kind : FEED;

    write_idx_ = 0;
    filled_    = 0;
    total_     = 0;
    peak_rssi_ = 0;
    changes_   = 0;
    drawn_changes_ = 0xFFFFFFFF;
    for (auto& r : ring_) { r.text[0] = '\0'; r.rssi = 0; r.ch = 0; r.klass = K_WEAK; r.hits = 0; }
    for (auto& v : pulse_) v = 0;
    for (auto& v : rate_)  v = 0;
    rate_head_ = 0;
    pulse_accum_ = 0;

    for (auto& v : spec_peak_) v = 0;
    series_max_ = series_avg_ = 0;
    for (auto& v : meter_hist_) v = 0;
    meter_hist_n_ = 0;
    meter_rssi_ = meter_peak_ = 0;
    meter_name_[0] = '\0';
    rank_sig_ = 0;

    need_full_ = true;
    drawn_ch_  = 0xFF;
    uint32_t now = millis();
    pulse_step_ms_ = rate_step_ms_ = last_frame_ms_ = last_feed_ms_ = now;
    spec_peak_ms_ = meter_step_ms_ = now;
}

void RigView::pushLine(const String& raw) {
    String s = raw;

    // Leading colour key -> class hint. ;red; is how the stock deauth detector
    // flags an alert; the rest are informational and fall through to RSSI.
    bool alert = false;
    if (s.length() && s[0] == ';') {
        if (s.startsWith(";red;")) alert = true;
        int semi = s.indexOf(';', 1);
        if (semi >= 0) s.remove(0, semi + 1);   // drop ";xxx;"
    }
    s.trim();
    if (s.length() == 0) return;

    // RSSI: the first "-NN" token in a plausible dBm range, not glued to a word.
    int8_t rssi = 0;
    for (int i = 0; i + 1 < (int)s.length(); i++) {
        if (s[i] != '-' || s[i + 1] < '0' || s[i + 1] > '9') continue;
        char p = (i > 0) ? s[i - 1] : ' ';
        if ((p >= '0' && p <= '9') || (p >= 'A' && p <= 'Z') || (p >= 'a' && p <= 'z')) continue;
        int v = 0, j = i + 1;
        while (j < (int)s.length() && s[j] >= '0' && s[j] <= '9' && j < i + 4) { v = v * 10 + (s[j] - '0'); j++; }
        if (v >= 20 && v <= 110) { rssi = (int8_t)(-v); break; }
    }

    // Channel: after a "CH:" / "ch " style key.
    uint8_t ch = 0;
    static const char* CH_KEYS[] = { "CH:", "Ch:", "ch:", "CH ", "Ch ", "ch " };
    int ci = -1;
    for (const char* k : CH_KEYS) { ci = s.indexOf(k); if (ci >= 0) { ci += (int)strlen(k); break; } }
    if (ci >= 0) {
        while (ci < (int)s.length() && s[ci] == ' ') ci++;
        int v = 0, n = 0;
        while (ci < (int)s.length() && s[ci] >= '0' && s[ci] <= '9' && n < 3) { v = v * 10 + (s[ci] - '0'); ci++; n++; }
        if (n > 0 && v >= 1 && v <= 196) ch = (uint8_t)v;
    }

    uint8_t klass = alert ? K_ALERT
                  : (rssi == 0)     ? K_WEAK
                  : (rssi >= -60)   ? K_STRONG
                  : (rssi >= -75)   ? K_MID
                  :                   K_WEAK;

    if (rssi < 0 && (peak_rssi_ == 0 || rssi > peak_rssi_)) peak_rssi_ = rssi;

    // Collapse a line identical to the newest one (a beacon re-heard) into a
    // hit count instead of flooding the feed with duplicates.
    if (filled_ > 0) {
        FeedRow& top = ring_[(write_idx_ + RING - 1) % RING];
        if (strncmp(top.text, s.c_str(), sizeof(top.text) - 1) == 0) {
            if (top.hits < 65535) top.hits++;
            top.rssi = rssi; top.ch = ch; top.klass = klass;
            total_++; pulse_accum_++; rate_[rate_head_]++;
            changes_++;
            return;
        }
    }

    FeedRow& slot = ring_[write_idx_];
    strncpy(slot.text, s.c_str(), sizeof(slot.text) - 1);
    slot.text[sizeof(slot.text) - 1] = '\0';
    slot.rssi = rssi; slot.ch = ch; slot.klass = klass; slot.hits = 1;
    write_idx_ = (write_idx_ + 1) % RING;
    if (filled_ < RING) filled_++;
    total_++; pulse_accum_++; rate_[rate_head_]++;
    changes_++;
}

void RigView::drainBuffer() {
    LinkedList<String>* b = display_obj.display_buffer;
    if (!b) return;
    int guard = 0;
    while (b->size() > 0 && guard++ < 64) pushLine(b->shift());
    // If the radio is out-narrating us, don't let the tail pile up unbounded.
    if (b->size() > 0) b->clear();
}

// Per-kind data refresh. FEED reads the console buffer; the others read the
// scanner's own structures, which are public and updated by its callbacks.
void RigView::gather(uint32_t now) {
    switch (kind_) {
        case FEED:
            drainBuffer();
            // Some narrators also keep counters that the stock screen showed
            // instead of a feed; the console buffer may stay quiet while they
            // still tick. Let the pulse follow them.
            if (mode_ == WIFI_SCAN_WAR_DRIVE) {
                static uint32_t last_frames = 0;
                uint32_t frames = wifi_scan_obj.beacon_frames + (uint32_t)wifi_scan_obj.bt_frames;
                if (frames > last_frames) pulse_accum_ += (uint16_t)(frames - last_frames);
                last_frames = frames;
            }
            break;

        case SPECTRUM: {
            // channel_activity is cleared by the scanner every 5 s, so the bars
            // sawtooth; a peak-hold per bar that decays slowly reads as a
            // spectrum instead of a flicker.
            const int n = (CHAN_PER_PAGE < SPEC_MAX) ? CHAN_PER_PAGE : SPEC_MAX;
            const int base = (wifi_scan_obj.activity_page - 1) * CHAN_PER_PAGE;
            uint32_t total = 0;
            for (int i = 0; i < n; i++) {
                uint8_t v = wifi_scan_obj.channel_activity[base + i];
                total += v;
                if (v > spec_peak_[i]) spec_peak_[i] = v;
            }
            if (now - spec_peak_ms_ >= 1000) {
                spec_peak_ms_ = now;
                for (int i = 0; i < n; i++) if (spec_peak_[i]) spec_peak_[i]--;
            }
            pulse_accum_ = (uint16_t)((total > 255) ? 255 : total);
            changes_++;
            break;
        }

        case SERIES: {
            // Newest sample sits at index 0. Negative entries mark a channel
            // change and are not values.
            int16_t mx = 0; int32_t sum = 0; int cnt = 0;
            const int W = SCREEN_WIDTH;
            for (int i = 0; i < W; i++) {
                int16_t v = wifi_scan_obj._analyzer_values[i];
                if (v < 0) continue;
                if (v > mx) mx = v;
                sum += v; cnt++;
            }
            series_max_ = mx;
            series_avg_ = cnt ? (int16_t)(sum / cnt) : 0;
            int16_t newest = wifi_scan_obj._analyzer_values[0];
            pulse_accum_ = (newest > 0) ? (uint16_t)(newest / BASE_MULTIPLIER) : 0;
            changes_++;
            break;
        }

        case METER: {
            int8_t rssi = 0; const char* name = nullptr; String nm;
            #ifdef HAS_BT
            if (mode_ == BT_SCAN_FOX_HUNT && ble_devices) {
                for (int i = 0; i < ble_devices->size(); i++) {
                    if (ble_devices->get(i).selected) {
                        rssi = (int8_t)ble_devices->get(i).rssi;
                        nm = ble_devices->get(i).name; name = nm.c_str();
                        break;
                    }
                }
            }
            #endif
            if (mode_ == WIFI_SCAN_SIG_STREN && access_points) {
                for (int i = 0; i < access_points->size(); i++) {
                    if (access_points->get(i).selected) {
                        rssi = (int8_t)access_points->get(i).rssi;
                        nm = access_points->get(i).essid; name = nm.c_str();
                        break;
                    }
                }
            }
            if (name) { strncpy(meter_name_, name, sizeof(meter_name_) - 1); meter_name_[sizeof(meter_name_) - 1] = '\0'; }
            if (rssi != meter_rssi_) { pulse_accum_ += (uint16_t)abs((int)rssi - (int)meter_rssi_); changes_++; }
            meter_rssi_ = rssi;
            if (rssi < 0 && (meter_peak_ == 0 || rssi > meter_peak_)) meter_peak_ = rssi;
            if (now - meter_step_ms_ >= 500) {
                meter_step_ms_ = now;
                if (meter_hist_n_ < METER_HIST) meter_hist_[meter_hist_n_++] = rssi;
                else {
                    for (int i = 0; i < METER_HIST - 1; i++) meter_hist_[i] = meter_hist_[i + 1];
                    meter_hist_[METER_HIST - 1] = rssi;
                }
                changes_++;
            }
            break;
        }

        case RANK:
            pulse_accum_ += 1;
            changes_++;   // the rows are re-read and hashed at paint time
            break;
    }
}

void RigView::tick(uint32_t now) {
    if (wifi_scan_obj.currentScanMode != mode_) begin(wifi_scan_obj.currentScanMode);
    if (title_ == nullptr) return;   // RigUI only routes owned modes here, but be safe

    gather(now);

    if (now - pulse_step_ms_ >= 250) {
        pulse_step_ms_ = now;
        for (int i = 0; i < RigTheme::PULSE_N - 1; i++) pulse_[i] = pulse_[i + 1];
        uint16_t v = pulse_accum_;
        if (v > (uint16_t)RigTheme::PULSE_H) v = RigTheme::PULSE_H;
        pulse_[RigTheme::PULSE_N - 1] = (uint8_t)v;
        pulse_accum_ = 0;
    }
    if (now - rate_step_ms_ >= 5000) {
        rate_step_ms_ = now;
        rate_head_ = (rate_head_ + 1) % RATE_BUCKETS;
        rate_[rate_head_] = 0;
    }

    if (need_full_) {
        drawFrame();
        need_full_ = false;
        last_frame_ms_ = now;
        last_feed_ms_  = now;
        return;
    }
    if (now - last_feed_ms_ >= 250) {   // body, hero and the animated pulse
        last_feed_ms_ = now;
        drawBar(false);
        drawHero();
        drawBody();
    }
    if (now - last_frame_ms_ >= 1000) { // slower-moving chrome
        last_frame_ms_ = now;
        drawStatus();
        drawRibbon();
    }
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

// The three hero columns, per instrument.
static void rvHeroLabels(uint8_t kind, uint8_t mode, const char*& a, const char*& b, const char*& c) {
    a = "SEEN"; b = "RATE"; c = "PEAK";
    if (kind == RigView::FEED && mode == WIFI_SCAN_WAR_DRIVE) { a = "WIFI"; b = "BT";    c = "FLOCK"; }
    else if (kind == RigView::RANK && mode == WIFI_SCAN_DETECT_FOLLOW) { a = "TRACKED"; b = "FOLLOW"; c = "TOP TX"; }
    else if (kind == RigView::RANK)      { a = "TARGETS"; b = "PACKETS"; c = "TOP"; }
    else if (kind == RigView::SPECTRUM)  { a = "BUSIEST"; b = "TOTAL";   c = "PAGE"; }
    else if (kind == RigView::SERIES)    { a = "NOW";     b = "MAX";     c = "AVG"; }
    else if (kind == RigView::METER)     { a = "RSSI";    b = "PEAK";    c = "TREND"; }
}

void RigView::drawFrame() {
    auto& tft = display_obj.tft;
    display_obj.clearScreen();

    drawBar(true);
    drawStatus();
    drawRibbon();

    // Hero tile: gold-spined panel, three headline columns.
    const int hy = RigTheme::TOOL_HERO_Y, hh = RigTheme::TOOL_HERO_H;
    tft.fillRoundRect(4, hy, SCREEN_WIDTH - 8, hh, 4, RV_PANEL);
    tft.drawRoundRect(4, hy, SCREEN_WIDTH - 8, hh, 4, RV_OUT);
    tft.fillRect(4, hy + 2, 3, hh - 4, RV_GOLD);
    tft.setTextSize(1);
    tft.setTextColor(RV_DIM, RV_PANEL);
    const int lblY = hy + RigTheme::TOOL_LBL_DY;
    const char *la, *lb, *lc;
    rvHeroLabels(kind_, mode_, la, lb, lc);
    tft.setCursor(14,  lblY); tft.print(la);
    tft.setCursor(96,  lblY); tft.print(lb);
    tft.setCursor(166, lblY); tft.print(lc);

    // Footer hint.
    tft.drawFastHLine(RigTheme::PAD_X, RigTheme::TOOL_FOOT_Y,
                      SCREEN_WIDTH - 2 * RigTheme::PAD_X, RV_OUT);
    tft.setTextSize(1);
    tft.setTextColor(RV_DIM2, TFT_BLACK);
    tft.setCursor(RigTheme::PAD_X, RigTheme::TOOL_HINT_Y);
    tft.print(RIG_HINT_EXIT);

    drawn_changes_ = 0xFFFFFFFF;   // force the body to paint
    rank_sig_ = 0;
    drawHero();
    drawBody();
}

void RigView::drawBar(bool full) {
    auto& tft = display_obj.tft;
    const int barH = RigTheme::BAR_H;

    if (full) {
        tft.fillRect(0, 0, SCREEN_WIDTH, barH, STATUSBAR_COLOR);
        tft.setTextDatum(TL_DATUM);
        tft.setTextColor(RV_GOLD, STATUSBAR_COLOR);
        if (RigTheme::COMPACT) {
            tft.drawString(title_ ? title_ : "RIG", 4, 4, 1);
        } else {
            tft.setSwapBytes(true);
            tft.pushImage(3, 1, GORILLA_MINI_W, GORILLA_MINI_H, gorilla_mini);
            tft.setSwapBytes(false);
            // Truncate to the gap before the pulse so the title can never run
            // into it.
            char t[12];
            strncpy(t, title_ ? title_ : "RIG", sizeof(t) - 1);
            t[sizeof(t) - 1] = '\0';
            tft.drawString(t, 30, 5, 2);
        }
    }

    // ---- traffic pulse (redrawn every body tick) ----
    const int px = RigTheme::PULSE_X, py = RigTheme::PULSE_Y;
    const int ph = RigTheme::PULSE_H, n = RigTheme::PULSE_N, cw = RigTheme::PULSE_COL_W;
    tft.fillRect(px, 0, n * cw + 1, barH, STATUSBAR_COLOR);
    for (int i = 0; i < n; i++) {
        int h = pulse_[i]; if (h > ph) h = ph;
        int x = px + i * cw;
        tft.drawPixel(x, py + ph - 1, RV_GOLD_D);            // baseline tick
        if (h > 0) tft.fillRect(x, py + ph - h, 1, h, RV_GOLD);
    }

    // ---- battery (mirror of drawRigHeader) ----
    #ifdef HAS_BATTERY
        int8_t batt = battery_obj.battery_level;
        tft.fillRect(156, 0, SCREEN_WIDTH - 156, barH, STATUSBAR_COLOR);
        if (batt >= 0) {
            if (batt > 100) batt = 100;
            uint16_t bc = (batt >= 40) ? RV_GREEN : (batt >= 20 ? RV_AMBER : RV_RED);
            const int bx = 160, by = (barH - 11) / 2;
            tft.drawRoundRect(bx, by, 20, 11, 2, bc);
            tft.fillRect(bx + 20, by + 3, 2, 5, bc);
            int fillw = (16 * batt) / 100;
            if (fillw > 0) tft.fillRect(bx + 2, by + 2, fillw, 7, bc);
            tft.setTextDatum(MR_DATUM);
            tft.setTextColor(bc, STATUSBAR_COLOR);
            tft.drawString(String((int)batt) + "%", SCREEN_WIDTH - 6, barH / 2,
                           RigTheme::COMPACT ? 1 : 2);
            tft.setTextDatum(TL_DATUM);
        }
    #endif
}

void RigView::drawStatus() {
    auto& tft = display_obj.tft;
    const int stat_y = RigTheme::STATUS_Y + RigTheme::STATUS_H / 2;

    tft.fillRect(0, RigTheme::STATUS_Y, SCREEN_WIDTH, RigTheme::STATUS_H, TFT_BLACK);

    uint16_t dotc = RV_DIM2, txtc = RV_DIM;
    String gtxt = "NO GPS";
    #ifdef HAS_GPS
        if (gps_obj.getGpsModuleStatus()) {
            int sats = gps_obj.getNumSats();
            if (gps_obj.getFixStatus()) { dotc = RV_GREEN; txtc = RV_INK; gtxt = "GPS FIX  " + String(sats) + " sats"; }
            else                        { dotc = RV_AMBER; txtc = RV_DIM; gtxt = "ACQUIRING  " + String(sats) + " sats"; }
        }
    #endif
    tft.fillCircle(9, stat_y, 3, dotc);
    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(txtc, TFT_BLACK);
    tft.drawString(gtxt, 17, stat_y, 1);

    // On the short screen the channel rides here (no room for the ribbon); on the
    // tall one the SD state does, with the ribbon showing the channel.
    tft.setTextDatum(MR_DATUM);
    if (RigTheme::COMPACT && band_ == 0) {
        tft.setTextColor(RV_DIM, TFT_BLACK);
        tft.drawString("CH " + String((int)wifi_scan_obj.set_channel), SCREEN_WIDTH - 6, stat_y, 1);
    } else {
        #if defined(HAS_SD) && !defined(HAS_C5_SD)
            bool sdok = sd_obj.supported;
            tft.setTextColor(sdok ? RV_GREEN : RV_RED, TFT_BLACK);
            tft.drawString(sdok ? "SD OK" : "NO SD", SCREEN_WIDTH - 6, stat_y, 1);
        #endif
    }
    tft.setTextDatum(TL_DATUM);
}

void RigView::drawRibbon() {
    if (!RigTheme::SHOW_RIBBON) return;
    auto& tft = display_obj.tft;
    const int y = RigTheme::RIBBON_Y, h = RigTheme::RIBBON_H;

    // BLE has no channel ribbon -- the band is not swept the same way. Clear the
    // strip so a mode switch leaves nothing behind and leave it blank.
    if (band_ != 0) {
        tft.fillRect(0, y, SCREEN_WIDTH, h, TFT_BLACK);
        drawn_ch_ = 0xFF;
        return;
    }

    uint8_t cur = wifi_scan_obj.set_channel;
    if (cur == drawn_ch_) return;
    drawn_ch_ = cur;

    // 14 segments for the 2.4 GHz channels. Current is gold, the two it just
    // left fade back through dim gold, the rest are outline.
    const int segs = 14, x0 = 8, pitch = 16, w = 15;
    tft.fillRect(0, y, SCREEN_WIDTH, h, TFT_BLACK);
    for (int c = 1; c <= segs; c++) {
        uint16_t col = RV_OUT;
        if (c == cur)                              col = RV_GOLD;
        else if (c == cur - 1 || c == cur - 2)     col = RV_GOLD_D;
        tft.fillRect(x0 + (c - 1) * pitch, y, w, h, col);
    }
}

void RigView::drawHero() {
    auto& tft = display_obj.tft;
    const int hy = RigTheme::TOOL_HERO_Y;
    const int valY = hy + (RigTheme::COMPACT ? 11 : 16);
    const uint8_t vf = RigTheme::TOOL_VAL_SZ;   // font id for the big numbers
    char a[12], b[12], c[12];
    uint16_t ca = RV_GOLD, cb = RV_INK, cc = RV_INK;

    switch (kind_) {
        case FEED:
            if (mode_ == WIFI_SCAN_WAR_DRIVE) {
                rvFmt(a, sizeof(a), wifi_scan_obj.beacon_frames);
                rvFmt(b, sizeof(b), (uint32_t)((wifi_scan_obj.bt_frames < 0) ? 0 : wifi_scan_obj.bt_frames));
                rvFmt(c, sizeof(c), wifi_scan_obj.flock_devices);
                ca = wifi_scan_obj.beacon_frames ? RV_GOLD : RV_DIM2;
                cb = wifi_scan_obj.bt_frames ? RV_INK : RV_DIM2;
                cc = wifi_scan_obj.flock_devices ? RV_RED : RV_DIM2;
            } else {
                uint32_t rate = 0;
                for (auto v : rate_) rate += v;
                rvFmt(a, sizeof(a), total_);
                rvFmt(b, sizeof(b), rate);
                if (peak_rssi_) snprintf(c, sizeof(c), "%d", (int)peak_rssi_); else snprintf(c, sizeof(c), "--");
                ca = total_ ? RV_GOLD : RV_DIM2;
                cb = rate ? RV_INK : RV_DIM2;
                cc = peak_rssi_ ? RV_INK : RV_DIM2;
            }
            break;

        case RANK: {
            uint32_t n = 0, following = 0, top = 0, total = 0;
            if (mode_ == WIFI_SCAN_DETECT_FOLLOW) {
                MacEntry list[10];
                n = wifi_scan_obj.build_top10_for_ui(list, MacSortMode::MOST_FRAMES);
                for (uint32_t i = 0; i < n; i++) {
                    if (list[i].following) following++;
                    if (list[i].frame_count > top) top = list[i].frame_count;
                }
                rvFmt(a, sizeof(a), n); rvFmt(b, sizeof(b), following); rvFmt(c, sizeof(c), top);
                cb = following ? RV_RED : RV_DIM2;
            } else {
                if (access_points) for (int i = 0; i < access_points->size(); i++) {
                    AccessPoint ap = access_points->get(i);
                    if (!ap.selected) continue;
                    n++; total += ap.packets; if ((uint32_t)ap.packets > top) top = ap.packets;
                }
                if (stations) for (int i = 0; i < stations->size(); i++) {
                    Station st = stations->get(i);
                    if (!st.selected) continue;
                    n++; total += st.packets; if ((uint32_t)st.packets > top) top = st.packets;
                }
                rvFmt(a, sizeof(a), n); rvFmt(b, sizeof(b), total); rvFmt(c, sizeof(c), top);
            }
            ca = n ? RV_GOLD : RV_DIM2;
            break;
        }

        case SPECTRUM: {
            const int n = (CHAN_PER_PAGE < SPEC_MAX) ? CHAN_PER_PAGE : SPEC_MAX;
            const int base = (wifi_scan_obj.activity_page - 1) * CHAN_PER_PAGE;
            uint32_t total = 0; int busiest = 0; uint8_t bv = 0;
            for (int i = 0; i < n; i++) {
                uint8_t v = spec_peak_[i];
                total += wifi_scan_obj.channel_activity[base + i];
                if (v > bv) { bv = v; busiest = i; }
            }
            if (bv) snprintf(a, sizeof(a), "%d", base + busiest + 1); else snprintf(a, sizeof(a), "--");
            rvFmt(b, sizeof(b), total);
            snprintf(c, sizeof(c), "%u", (unsigned)wifi_scan_obj.activity_page);
            ca = bv ? RV_GOLD : RV_DIM2;
            break;
        }

        case SERIES: {
            int16_t newest = wifi_scan_obj._analyzer_values[0];
            if (newest < 0) newest = 0;
            rvFmt(a, sizeof(a), (uint32_t)(newest / BASE_MULTIPLIER));
            rvFmt(b, sizeof(b), (uint32_t)(series_max_ / BASE_MULTIPLIER));
            rvFmt(c, sizeof(c), (uint32_t)(series_avg_ / BASE_MULTIPLIER));
            ca = newest ? RV_GOLD : RV_DIM2;
            break;
        }

        case METER: {
            if (meter_rssi_) snprintf(a, sizeof(a), "%d", (int)meter_rssi_); else snprintf(a, sizeof(a), "--");
            if (meter_peak_) snprintf(b, sizeof(b), "%d", (int)meter_peak_); else snprintf(b, sizeof(b), "--");
            // Trend over the last few samples: getting warmer or colder.
            int8_t trend = 0;
            if (meter_hist_n_ >= 4) {
                int recent = meter_hist_[meter_hist_n_ - 1] + meter_hist_[meter_hist_n_ - 2];
                int older  = meter_hist_[meter_hist_n_ - 3] + meter_hist_[meter_hist_n_ - 4];
                trend = (recent > older + 2) ? 1 : (recent < older - 2) ? -1 : 0;
            }
            snprintf(c, sizeof(c), trend > 0 ? "UP" : trend < 0 ? "DOWN" : "FLAT");
            ca = rvSignalColor(meter_rssi_);
            if (ca == RV_DIM2 && meter_rssi_) ca = RV_AMBER;
            cc = trend > 0 ? RV_GREEN : trend < 0 ? RV_RED : RV_DIM;
            break;
        }
    }

    // Clear the value band (spine and labels are static), then the three values.
    tft.fillRect(8, valY - 1, SCREEN_WIDTH - 16, RigTheme::COMPACT ? 18 : 28, RV_PANEL);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(ca, RV_PANEL); tft.drawString(a, 14,  valY, vf);
    tft.setTextColor(cb, RV_PANEL); tft.drawString(b, 96,  valY, vf);
    tft.setTextColor(cc, RV_PANEL); tft.drawString(c, 166, valY, vf);
}

void RigView::drawBody() {
    switch (kind_) {
        case FEED:     drawFeed();     break;
        case RANK:     drawRank();     break;
        case SPECTRUM: drawSpectrum(); break;
        case SERIES:   drawSeries();   break;
        case METER:    drawMeter();    break;
    }
}

// ---- FEED ------------------------------------------------------------------

void RigView::drawFeed() {
    if (drawn_changes_ == changes_) return;   // nothing new since last paint
    drawn_changes_ = changes_;

    auto& tft = display_obj.tft;
    const int by = RigTheme::TOOL_BODY_Y, bend = RigTheme::TOOL_BODY_END;
    const int pitch = RigTheme::FEED_PITCH, rh = RigTheme::FEED_ROW_H;
    const int fits = (bend - by) / pitch;

    tft.fillRect(0, by, SCREEN_WIDTH, bend - by, TFT_BLACK);

    if (filled_ == 0) {
        tft.setTextDatum(MC_DATUM);
        tft.setTextColor(RV_DIM2, TFT_BLACK);
        tft.drawString(band_ ? "listening for devices..." : "listening for traffic...",
                       SCREEN_WIDTH / 2, by + (bend - by) / 2, 1);
        tft.setTextDatum(TL_DATUM);
        return;
    }

    int rows = fits < filled_ ? fits : filled_;
    const int textLimit = RigTheme::FEED_BARS_X - 6;
    const int maxch = (textLimit - 14) / 6;

    for (int r = 0; r < rows; r++) {
        int idx = (write_idx_ + RING - 1 - r) % RING;
        FeedRow& fr = ring_[idx];
        int y = by + r * pitch;

        uint16_t col = fr.klass == K_ALERT  ? RV_RED
                     : fr.klass == K_STRONG ? RV_GREEN
                     : fr.klass == K_MID    ? RV_AMBER
                     :                        RV_DIM2;

        tft.fillRect(6, y, 3, rh, col);   // class stripe

        // Name (truncated). A repeat count rides along when a line has been
        // re-heard, so a busy beacon shows how loud it is without N rows.
        String label = fr.text;
        if (fr.hits > 1) label = "(" + String(fr.hits) + ") " + label;
        if ((int)label.length() > maxch) label = label.substring(0, maxch);
        tft.setTextDatum(ML_DATUM);
        tft.setTextColor(fr.klass == K_ALERT ? RV_RED : RV_INK, TFT_BLACK);
        tft.drawString(label, 14, y + rh / 2, 1);

        rvBars(RigTheme::FEED_BARS_X, y + (rh - 10) / 2, fr.rssi, col);

        if (!RigTheme::COMPACT && fr.ch) {
            tft.setTextDatum(MR_DATUM);
            tft.setTextColor(RV_DIM2, TFT_BLACK);
            tft.drawString("CH " + String((int)fr.ch), RigTheme::FEED_RSSI_X, y + rh / 2, 1);
        }
    }
    tft.setTextDatum(TL_DATUM);
}

// ---- RANK ------------------------------------------------------------------
// A ranked list with a proportional bar under each row. Rows are re-read from
// WiFiScan on every paint and only redrawn when their content changed.

namespace {
struct RankRow { char text[32]; uint32_t value; int8_t rssi; uint16_t stripe; uint16_t sub; };
}

void RigView::drawRank() {
    RankRow rows[10];
    int n = 0;
    uint32_t top = 0, sig = 0;

    if (mode_ == WIFI_SCAN_DETECT_FOLLOW) {
        MacEntry list[10];
        n = wifi_scan_obj.build_top10_for_ui(list, MacSortMode::MOST_FRAMES);
        if (n > 10) n = 10;
        for (int i = 0; i < n; i++) {
            char mac[20]; rvMacShort(mac, sizeof(mac), list[i].mac);
            uint32_t age = (millis() - list[i].last_seen_ms) / 1000;
            snprintf(rows[i].text, sizeof(rows[i].text), "%s  %us", mac, (unsigned)(age > 999 ? 999 : age));
            rows[i].value  = list[i].frame_count;
            rows[i].rssi   = list[i].rssi;
            rows[i].stripe = list[i].following ? RV_RED : (list[i].bt ? RV_GOLD_D : rvSignalColor(list[i].rssi));
            rows[i].sub    = list[i].following ? RV_RED : RV_INK;
            if (rows[i].value > top) top = rows[i].value;
            sig = sig * 31 + rows[i].value + (uint32_t)list[i].mac[5] + (list[i].following ? 7 : 0);
        }
    } else {
        if (access_points) for (int i = 0; i < access_points->size() && n < 10; i++) {
            AccessPoint ap = access_points->get(i);
            if (!ap.selected) continue;
            strncpy(rows[n].text, ap.essid.c_str(), sizeof(rows[n].text) - 1);
            rows[n].text[sizeof(rows[n].text) - 1] = '\0';
            rows[n].value = ap.packets; rows[n].rssi = (int8_t)ap.rssi;
            rows[n].stripe = rvSignalColor((int8_t)ap.rssi); rows[n].sub = RV_INK;
            if (rows[n].value > top) top = rows[n].value;
            sig = sig * 31 + rows[n].value + i;
            n++;
        }
        if (stations) for (int i = 0; i < stations->size() && n < 10; i++) {
            Station st = stations->get(i);
            if (!st.selected) continue;
            rvMacShort(rows[n].text, sizeof(rows[n].text), st.mac);
            rows[n].value = st.packets; rows[n].rssi = 0;
            rows[n].stripe = RV_GOLD_D; rows[n].sub = RV_INK;
            if (rows[n].value > top) top = rows[n].value;
            sig = sig * 31 + rows[n].value + 100 + i;
            n++;
        }
    }
    sig = sig * 31 + (uint32_t)n + 1;
    if (sig == rank_sig_) return;
    rank_sig_ = sig;

    auto& tft = display_obj.tft;
    const int by = RigTheme::TOOL_BODY_Y, bend = RigTheme::TOOL_BODY_END;
    const int pitch = RigTheme::FEED_PITCH, rh = RigTheme::FEED_ROW_H;
    const int fits = (bend - by) / pitch;
    tft.fillRect(0, by, SCREEN_WIDTH, bend - by, TFT_BLACK);

    if (n == 0) {
        tft.setTextDatum(MC_DATUM);
        tft.setTextColor(RV_DIM2, TFT_BLACK);
        tft.drawString(mode_ == WIFI_SCAN_DETECT_FOLLOW ? "listening for devices..." : "no targets selected",
                       SCREEN_WIDTH / 2, by + (bend - by) / 2, 1);
        tft.setTextDatum(TL_DATUM);
        return;
    }

    const int barW = RigTheme::FEED_BARS_X - 20;   // proportional bar span
    const int maxch = (RigTheme::FEED_BARS_X - 6 - 14) / 6;
    int rows_shown = fits < n ? fits : n;
    for (int r = 0; r < rows_shown; r++) {
        int y = by + r * pitch;
        tft.fillRect(6, y, 3, rh, rows[r].stripe);

        String label = rows[r].text;
        if ((int)label.length() > maxch) label = label.substring(0, maxch);
        tft.setTextDatum(TL_DATUM);
        tft.setTextColor(rows[r].sub, TFT_BLACK);
        tft.drawString(label, 14, y + 2, 1);

        // Proportional bar against the top entry, count right-aligned above it.
        int w = top ? (int)((uint64_t)barW * rows[r].value / top) : 0;
        if (w < 2 && rows[r].value) w = 2;
        tft.fillRect(14, y + rh - 5, barW, 3, RV_OUT);
        if (w) tft.fillRect(14, y + rh - 5, w, 3, rows[r].stripe == RV_RED ? RV_RED : RV_GOLD);

        char cnt[12]; rvFmt(cnt, sizeof(cnt), rows[r].value);
        tft.setTextDatum(TR_DATUM);
        tft.setTextColor(RV_DIM, TFT_BLACK);
        tft.drawString(cnt, RigTheme::FEED_RSSI_X, y + 2, 1);

        if (rows[r].rssi) rvBars(RigTheme::FEED_BARS_X, y + (rh - 10) / 2 + 2, rows[r].rssi, rows[r].stripe);
    }
    tft.setTextDatum(TL_DATUM);
}

// ---- SPECTRUM --------------------------------------------------------------
// One bar per channel of the current page, live value in gold with a peak cap
// that decays. Channel numbers under the bars.

void RigView::drawSpectrum() {
    auto& tft = display_obj.tft;
    const int by = RigTheme::TOOL_BODY_Y, bend = RigTheme::TOOL_BODY_END;
    const int n = (CHAN_PER_PAGE < SPEC_MAX) ? CHAN_PER_PAGE : SPEC_MAX;
    const int base = (wifi_scan_obj.activity_page - 1) * CHAN_PER_PAGE;
    const int labelH = 10;
    const int floorY = bend - labelH;          // bar baseline
    const int topY   = by + 4;
    const int H      = floorY - topY;
    const int x0 = 8, span = SCREEN_WIDTH - 16;
    const int pitch = span / n, w = pitch - 2;

    // Scale to the tallest peak on the page so the busiest bar fills the body.
    uint8_t mx = 1;
    for (int i = 0; i < n; i++) if (spec_peak_[i] > mx) mx = spec_peak_[i];

    tft.fillRect(0, by, SCREEN_WIDTH, bend - by, TFT_BLACK);
    tft.drawFastHLine(x0, floorY, span, RV_OUT);

    int busiest = -1; uint8_t bv = 0;
    for (int i = 0; i < n; i++) if (spec_peak_[i] > bv) { bv = spec_peak_[i]; busiest = i; }

    tft.setTextDatum(TC_DATUM);
    for (int i = 0; i < n; i++) {
        const int x = x0 + i * pitch;
        const uint8_t live = wifi_scan_obj.channel_activity[base + i];
        const uint8_t peak = spec_peak_[i];
        int hl = (int)((uint32_t)H * live / mx); if (live && hl < 1) hl = 1;
        int hp = (int)((uint32_t)H * peak / mx);
        if (hl) tft.fillRect(x, floorY - hl, w, hl, (i == busiest) ? RV_GOLD : RV_GOLD_D);
        if (hp > hl) tft.fillRect(x, floorY - hp, w, 2, RV_INK);       // peak cap
        tft.setTextColor((i == busiest) ? RV_GOLD : RV_DIM2, TFT_BLACK);
        tft.drawString(String(base + i + 1), x + w / 2, floorY + 2, 1);
    }
    tft.setTextDatum(TL_DATUM);
}

// ---- SERIES ----------------------------------------------------------------
// The analyzer's history as a scrolling histogram, newest at the right edge.
// Channel-change markers (negative samples) become thin red ticks.

void RigView::drawSeries() {
    auto& tft = display_obj.tft;
    const int by = RigTheme::TOOL_BODY_Y, bend = RigTheme::TOOL_BODY_END;
    const int floorY = bend - 2, topY = by + 10;
    const int H = floorY - topY;
    const int cw = 2;
    const int x0 = 8, span = SCREEN_WIDTH - 16;
    int cols = span / cw;
    if (cols > SCREEN_WIDTH) cols = SCREEN_WIDTH;

    int16_t mx = series_max_ > 0 ? series_max_ : 1;

    tft.fillRect(0, by, SCREEN_WIDTH, bend - by, TFT_BLACK);
    tft.drawFastHLine(x0, floorY, span, RV_OUT);

    // Average line for reference.
    if (series_avg_ > 0) {
        int ya = floorY - (int)((int32_t)H * series_avg_ / mx);
        for (int x = x0; x < x0 + span; x += 4) tft.drawPixel(x, ya, RV_GOLD_D);
    }

    for (int i = 0; i < cols; i++) {
        int16_t v = wifi_scan_obj._analyzer_values[i];
        const int x = x0 + span - (i + 1) * cw;   // index 0 = newest = right edge
        if (v < 0) {
            tft.drawFastVLine(x, topY, H, RV_RED);
            continue;
        }
        if (v == 0) continue;
        int h = (int)((int32_t)H * v / mx); if (h < 1) h = 1;
        tft.fillRect(x, floorY - h, cw - 1, h, (i == 0) ? RV_GOLD : RV_GOLD_D);
    }

    // What the units are, small, top-left of the body.
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(RV_DIM2, TFT_BLACK);
    tft.drawString(band_ ? "beacons / 100 ms" : "frames / 100 ms", x0, by, 1);
}

// ---- METER -----------------------------------------------------------------
// One signal, big: the target's name, its RSSI in large digits, a bar on a
// -100..-20 scale, and a short history so the eye can see "warmer / colder".

void RigView::drawMeter() {
    if (drawn_changes_ == changes_) return;
    drawn_changes_ = changes_;

    auto& tft = display_obj.tft;
    const int by = RigTheme::TOOL_BODY_Y, bend = RigTheme::TOOL_BODY_END;
    tft.fillRect(0, by, SCREEN_WIDTH, bend - by, TFT_BLACK);

    const uint16_t col = meter_rssi_ ? (rvSignalColor(meter_rssi_) == RV_DIM2 ? RV_AMBER : rvSignalColor(meter_rssi_)) : RV_DIM2;

    // Target name.
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(RV_DIM, TFT_BLACK);
    String nm = meter_name_[0] ? String(meter_name_) : String(band_ ? "no device selected" : "no network selected");
    if ((int)nm.length() > 36) nm = nm.substring(0, 36);
    tft.drawString(nm, 12, by + 2, 1);

    if (RigTheme::COMPACT) {
        // 54 px of body: the number and the bar, nothing else.
        char v[8]; if (meter_rssi_) snprintf(v, sizeof(v), "%d", (int)meter_rssi_); else snprintf(v, sizeof(v), "--");
        tft.setTextColor(col, TFT_BLACK);
        tft.drawString(v, 12, by + 12, 4);
        const int bx = 90, bw = SCREEN_WIDTH - bx - 8, byy = by + 20, bh = 10;
        tft.drawRect(bx, byy, bw, bh, RV_OUT);
        int fill = meter_rssi_ ? (int)((int32_t)(bw - 2) * (meter_rssi_ + 100) / 80) : 0;
        if (fill < 0) fill = 0; if (fill > bw - 2) fill = bw - 2;
        if (fill) tft.fillRect(bx + 1, byy + 1, fill, bh - 2, col);
        return;
    }

    // Big digits (GLCD scaled: always present, no font-load dependency).
    tft.setTextSize(5);
    tft.setTextColor(col, TFT_BLACK);
    tft.setCursor(12, by + 16);
    if (meter_rssi_) tft.print((int)meter_rssi_); else tft.print("--");
    tft.setTextSize(1);
    tft.setTextColor(RV_DIM2, TFT_BLACK);
    tft.setCursor(12 + 5 * 6 * 3 + 6, by + 16 + 40 - 8);
    tft.print("dBm");

    // Bar on a -100..-20 scale with ticks.
    const int bx = 12, bw = SCREEN_WIDTH - 24, byy = by + 66, bh = 12;
    tft.drawRect(bx, byy, bw, bh, RV_OUT);
    int fill = meter_rssi_ ? (int)((int32_t)(bw - 2) * (meter_rssi_ + 100) / 80) : 0;
    if (fill < 0) fill = 0; if (fill > bw - 2) fill = bw - 2;
    if (fill) tft.fillRect(bx + 1, byy + 1, fill, bh - 2, col);
    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(RV_DIM2, TFT_BLACK);
    const int ticks[5] = { -100, -80, -60, -40, -20 };
    for (int t = 0; t < 5; t++) {
        int tx = bx + (int)((int32_t)bw * (ticks[t] + 100) / 80);
        if (t == 4) tx = bx + bw;
        tft.drawFastVLine(tx, byy + bh, 3, RV_OUT);
        tft.drawString(String(ticks[t]), (t == 0) ? tx + 10 : (t == 4) ? tx - 10 : tx, byy + bh + 5, 1);
    }

    // History: one column per half-second, newest at the right.
    const int hy = byy + bh + 20, hh = bend - hy - 2;
    if (hh > 16 && meter_hist_n_) {
        const int cwid = 4, x0 = 12;
        const int cols = (SCREEN_WIDTH - 24) / cwid;
        tft.drawFastHLine(x0, hy + hh, cols * cwid, RV_OUT);
        int start = (meter_hist_n_ > cols) ? meter_hist_n_ - cols : 0;
        for (int i = start; i < meter_hist_n_; i++) {
            int8_t r = meter_hist_[i];
            if (r == 0) continue;
            int h = (int)((int32_t)hh * (r + 100) / 80); if (h < 1) h = 1; if (h > hh) h = hh;
            int x = x0 + (i - start) * cwid;
            uint16_t c = rvSignalColor(r); if (c == RV_DIM2) c = RV_AMBER;
            tft.fillRect(x, hy + hh - h, cwid - 1, h, (i == meter_hist_n_ - 1) ? c : RV_GOLD_D);
        }
    }
    tft.setTextDatum(TL_DATUM);
}

// ---------------------------------------------------------------------------
// Brightness modal
// ---------------------------------------------------------------------------
// The stock screen read only touch: tap top = brighter, bottom = dimmer,
// middle = save, and after three quiet seconds it saved by itself. On a button
// board that is a screen you can look at for three seconds and not change.
// This one takes UP/DOWN and SELECT (and the same touch zones where there is a
// panel), previews the level live, and still saves itself after a pause.

#ifndef HAS_MINI_SCREEN
extern void    brightnessSave(uint8_t level);
extern uint8_t getBrightnessLevel();
#endif

void RigView::brightnessModal() {
    #ifndef HAS_MINI_SCREEN
    auto& tft = display_obj.tft;

    static const uint8_t levels[] = {26, 51, 77, 102, 128, 153, 179, 204, 230, 255};
    static const uint8_t numLevels = 10;
    uint8_t level = getBrightnessLevel();
    if (level >= numLevels) level = numLevels - 1;
    const uint8_t start_level = level;

    #if ESP_ARDUINO_VERSION_MAJOR >= 3
        #define RV_BL_PREVIEW(duty) ledcWrite(TFT_BL, (duty))
    #else
        #define RV_BL_PREVIEW(duty) ledcWrite(0, (duty))
    #endif

    // Let go of the key that opened this, or it reads as an adjustment at once.
    // Bounded: a key latched by a lost release must not own the rig for good.
    {
        const uint32_t deadline = millis() + 1000;
        while (RigInput::down(RigInput::SELECT) && (int32_t)(millis() - deadline) < 0) delay(10);
        delay(50);
    }

    // Panel, centred.
    const int16_t pw = SCREEN_WIDTH - 2 * RigTheme::PAD_X;
    const int16_t ph = RigTheme::COMPACT ? 96 : 150;
    const int16_t px = RigTheme::PAD_X, py = (SCREEN_HEIGHT - ph) / 2;
    const int16_t bx = px + 16, bw = pw - 32;
    const int16_t bh = RigTheme::COMPACT ? 14 : 24;
    const int16_t byy = py + (RigTheme::COMPACT ? 40 : 64);

    auto paintLevel = [&]() {
        tft.drawRect(bx, byy, bw, bh, RV_OUT);
        int fillW = (bw - 4) * (level + 1) / numLevels;
        tft.fillRect(bx + 2, byy + 2, bw - 4, bh - 4, RV_PANEL);
        tft.fillRect(bx + 2, byy + 2, fillW, bh - 4, RV_GOLD);
        // Level ticks so the ten steps read as steps.
        for (int i = 1; i < numLevels; i++) {
            int tx = bx + 2 + (bw - 4) * i / numLevels;
            tft.drawFastVLine(tx, byy + 2, bh - 4, RV_PANEL);
        }
        char pct[8]; snprintf(pct, sizeof(pct), "%u%%", (unsigned)(levels[level] * 100 / 255));
        tft.fillRect(bx, byy + bh + 6, bw, RigTheme::COMPACT ? 12 : 28, RV_PANEL);
        tft.setTextDatum(TC_DATUM);
        tft.setTextColor(RV_INK, RV_PANEL);
        tft.drawString(pct, px + pw / 2, byy + bh + 6, RigTheme::COMPACT ? 2 : 4);
        tft.setTextDatum(TL_DATUM);
    };

    tft.fillRoundRect(px, py, pw, ph, RigTheme::RADIUS, RV_PANEL);
    tft.drawRoundRect(px, py, pw, ph, RigTheme::RADIUS, RV_GOLD);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(RV_GOLD, RV_PANEL);
    tft.drawString("BRIGHTNESS", px + 12, py + (RigTheme::COMPACT ? 4 : 12), RigTheme::FONT_TITLE);
    tft.setTextColor(RV_DIM2, RV_PANEL);
    #ifdef RIG_HAS_NAV
        tft.drawString(RigTheme::COMPACT ? "U/D adjust  C save" : "U/D adjust   C save   L cancel",
                       px + 12, py + ph - 14, 1);
    #else
        tft.drawString("tap top/bottom   middle saves", px + 12, py + ph - 14, 1);
    #endif
    paintLevel();

    uint32_t last_input = millis();
    bool pu = false, pd = false, pc = false, pl = false;
    bool done = false, save = false;

    while (!done) {
        // Save by itself after a pause, like before -- but only if something
        // was changed, so opening and closing the panel is not a write.
        if (millis() - last_input >= 3000) { save = (level != start_level); done = true; break; }

        #ifdef RIG_HAS_NAV
            bool u = RigInput::down(RigInput::UP), d = RigInput::down(RigInput::DOWN);
            bool c = RigInput::down(RigInput::SELECT);
            bool l = RigInput::down(RigInput::LEFT) || RigInput::down(RigInput::BACK);
            if (u && !pu && level < numLevels - 1) { level++; RV_BL_PREVIEW(levels[level]); paintLevel(); last_input = millis(); }
            if (d && !pd && level > 0)             { level--; RV_BL_PREVIEW(levels[level]); paintLevel(); last_input = millis(); }
            if (c && !pc) { save = true; done = true; }
            if (l && !pl) { save = false; done = true; }
            pu = u; pd = d; pc = c; pl = l;
        #endif

        #ifdef HAS_TOUCH
            uint16_t tx, ty;
            if (display_obj.updateTouch(&tx, &ty)) {
                last_input = millis();
                while (display_obj.updateTouch(&tx, &ty)) delay(10);
                if (ty < SCREEN_HEIGHT / 4)            { if (level < numLevels - 1) { level++; RV_BL_PREVIEW(levels[level]); paintLevel(); } }
                else if (ty >= SCREEN_HEIGHT * 3 / 4)  { if (level > 0)             { level--; RV_BL_PREVIEW(levels[level]); paintLevel(); } }
                else                                   { save = true; done = true; }
                delay(120);
            }
        #endif

        delay(20);
    }

    if (save) brightnessSave(level);
    else      RV_BL_PREVIEW(levels[start_level]);   // put the panel back the way it was

    // Wait for the closing key to be released so the list underneath does not
    // read it as its own press. Bounded, as above.
    {
        const uint32_t deadline = millis() + 1000;
        while ((RigInput::down(RigInput::SELECT) || RigInput::down(RigInput::LEFT)) &&
               (int32_t)(millis() - deadline) < 0) delay(10);
    }
    #undef RV_BL_PREVIEW
    #endif  // HAS_MINI_SCREEN
}

#endif  // HAS_SCREEN
