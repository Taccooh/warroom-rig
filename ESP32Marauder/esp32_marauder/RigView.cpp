#include "RigView.h"

#ifdef HAS_SCREEN

#include "Display.h"
#include "RigTheme.h"
#include "RigInput.h"       // RIG_HINT_EXIT
#include "WiFiScan.h"
#include "GorillaMini.h"

extern Display  display_obj;
extern WiFiScan wifi_scan_obj;

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
// Which scan modes RigView owns, their title and their band. A mode absent from
// this table keeps the stock behaviour untouched -- attacks, the graph views,
// the GPS screens, Rig Mode's own full-screen table. Titles are kept short: the
// bronze bar has room for about eight characters between the mascot and the
// pulse before it would run into them.
// ---------------------------------------------------------------------------
namespace {
struct RVMode { uint8_t mode; const char* title; uint8_t band; };  // band 0=2.4GHz 1=BLE
const RVMode RV_MODES[] = {
    // Only modes that narrate their sightings into display_obj.display_buffer --
    // the ones that drew the green scrolling console. Modes that paint their own
    // graphics (the EAPOL/raw-capture channel-scale UI, the channel and BT graph
    // analyzers, the packet monitor, Fox Hunt's meter) are deliberately absent:
    // clearing the screen for a feed would wipe a UI that never comes back.
    // Those become the Meter and Spectrum instruments in a later pass.
    { WIFI_SCAN_PROBE,             "PROBES",   0 },   // RunProbeScan
    { WIFI_SCAN_AP,                "BEACONS",  0 },   // RunBeaconScan
    { WIFI_SCAN_DEAUTH,            "DEAUTH",   0 },   // RunDeauthScan (detector)
    { WIFI_SCAN_PWN,              "PWNGOTCHI", 0 },   // RunPwnScan
    { WIFI_SCAN_PINESCAN,         "PINE AP",   0 },   // RunPineScan
    { WIFI_SCAN_MULTISSID,       "MULTISSID",  0 },   // RunMultiSSIDScan
    { WIFI_SCAN_AP_STA,          "AP + STA",   0 },   // RunAPScan
    { WIFI_SCAN_WAR_DRIVE,       "WARDRIVE",   0 },   // RunBeaconScan + BT
    { WIFI_SCAN_DETECT_FOLLOW,   "MAC MON",    0 },   // RunProbeScan + BT
    { BT_SCAN_ALL,               "BT SCAN",    1 },   // RunBluetoothScan
    { BT_SCAN_SIMPLE,            "BT SIMPLE",  1 },
    { BT_SCAN_SIMPLE_TWO,        "BT SIMPLE",  1 },
    { BT_SCAN_AIRTAG,            "AIRTAGS",    1 },
    { BT_SCAN_FLIPPER,           "FLIPPERS",   1 },
    { BT_SCAN_SKIMMERS,          "SKIMMERS",   1 },
    { BT_SCAN_RAYBAN,            "RAY-BAN",    1 },
    { BT_SCAN_FLOCK,             "FLOCK",      1 },
};
const RVMode* rvLookup(uint8_t m) {
    for (const RVMode& e : RV_MODES) if (e.mode == m) return &e;
    return nullptr;
}
}  // namespace

const char* RigView::title(uint8_t scan_mode) {
    const RVMode* e = rvLookup(scan_mode);
    return e ? e->title : nullptr;
}

// ---------------------------------------------------------------------------
// Four rising signal bars, lit by RSSI -- the same ladder WardriveCore uses.
// rssi == 0 means the line carried no strength; all bars stay dim.
// ---------------------------------------------------------------------------
static void rvBars(int x, int y, int8_t rssi, uint16_t col) {
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

// ---------------------------------------------------------------------------

void RigView::begin(uint8_t scan_mode) {
    mode_  = scan_mode;
    const RVMode* e = rvLookup(scan_mode);
    title_ = e ? e->title : nullptr;
    band_  = e ? e->band : 0;

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

    need_full_ = true;
    drawn_ch_  = 0xFF;
    uint32_t now = millis();
    pulse_step_ms_ = rate_step_ms_ = last_frame_ms_ = last_feed_ms_ = now;
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

void RigView::tick(uint32_t now) {
    if (wifi_scan_obj.currentScanMode != mode_) begin(wifi_scan_obj.currentScanMode);
    if (title_ == nullptr) return;   // RigUI only routes owned modes here, but be safe

    drainBuffer();

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
    if (now - last_feed_ms_ >= 250) {   // feed, hero and the animated pulse
        last_feed_ms_ = now;
        drawBar(false);
        drawHero();
        drawFeed();
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
    tft.setCursor(14,  lblY); tft.print("SEEN");
    tft.setCursor(96,  lblY); tft.print("RATE");
    tft.setCursor(166, lblY); tft.print("PEAK");

    // Footer hint.
    tft.drawFastHLine(RigTheme::PAD_X, RigTheme::TOOL_FOOT_Y,
                      SCREEN_WIDTH - 2 * RigTheme::PAD_X, RV_OUT);
    tft.setTextSize(1);
    tft.setTextColor(RV_DIM2, TFT_BLACK);
    tft.setCursor(RigTheme::PAD_X, RigTheme::TOOL_HINT_Y);
    tft.print(RIG_HINT_EXIT);

    drawHero();
    drawFeed();
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

    // ---- traffic pulse (redrawn every feed tick) ----
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

// Compact a count so it never overflows a hero column.
static void rvFmt(char* out, size_t n, uint32_t v) {
    if (v < 10000) snprintf(out, n, "%lu", (unsigned long)v);
    else           snprintf(out, n, "%luk", (unsigned long)(v / 1000));
}

void RigView::drawHero() {
    auto& tft = display_obj.tft;
    const int hy = RigTheme::TOOL_HERO_Y;
    const int valY = hy + (RigTheme::COMPACT ? 11 : 16);
    const uint8_t vf = RigTheme::TOOL_VAL_SZ;   // font id for the big numbers
    char buf[12];

    // Clear the value band (spine and labels are static).
    tft.fillRect(8, valY - 1, SCREEN_WIDTH - 16,
                 RigTheme::COMPACT ? 18 : 28, RV_PANEL);

    tft.setTextDatum(TL_DATUM);

    uint32_t rate = 0;
    for (auto v : rate_) rate += v;

    tft.setTextColor(total_ ? RV_GOLD : RV_DIM2, RV_PANEL);
    rvFmt(buf, sizeof(buf), total_);
    tft.drawString(buf, 14, valY, vf);

    tft.setTextColor(rate ? RV_INK : RV_DIM2, RV_PANEL);
    rvFmt(buf, sizeof(buf), rate);
    tft.drawString(buf, 96, valY, vf);

    tft.setTextColor(peak_rssi_ ? RV_INK : RV_DIM2, RV_PANEL);
    if (peak_rssi_) snprintf(buf, sizeof(buf), "%d", (int)peak_rssi_);
    else            snprintf(buf, sizeof(buf), "--");
    tft.drawString(buf, 166, valY, vf);
}

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

#endif  // HAS_SCREEN
