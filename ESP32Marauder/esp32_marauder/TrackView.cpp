#include "TrackView.h"

#if defined(HAS_SCREEN) && defined(HAS_GPS)

#include <math.h>

#include "Display.h"
#include "GpsInterface.h"
#include "RigTheme.h"

extern Display display_obj;
extern GpsInterface gps_obj;

#if defined(HAS_BUTTONS)
    #include "Switches.h"
#endif

// =========================================================================
// Sampling
// =========================================================================
void TrackView::sample(uint32_t now) {
    if (now - last_sample_ms < SAMPLE_INTERVAL_MS) return;
    last_sample_ms = now;

    if (!gps_obj.getFixStatus()) return;

    // GpsInterface hands out strings; parse once here rather than storing text.
    double lat = gps_obj.getLat().toDouble();
    double lon = gps_obj.getLon().toDouble();
    // A fix reported at exactly 0,0 is the null-island artefact, not a position.
    if (lat == 0.0 && lon == 0.0) return;

    int32_t la = (int32_t)lround(lat * 1000000.0);
    int32_t lo = (int32_t)lround(lon * 1000000.0);

    if (have_last) {
        int32_t dla = la - last_lat_u;
        int32_t dlo = lo - last_lon_u;
        // Longitude degrees shrink with latitude; without the cosine the filter
        // is far too permissive near the poles and too strict at the equator.
        double cosl = cos(lat * M_PI / 180.0);
        double dlo_eq = (double)dlo * cosl;
        double move_u = sqrt((double)dla * dla + dlo_eq * dlo_eq);
        if (move_u < MIN_MOVE_U) return;          // jitter, not travel
        dist_m += (float)(move_u * 0.111);        // 1e-6 deg latitude ~ 0.111 m
    }

    lat_u[head] = la;
    lon_u[head] = lo;
    head = (uint16_t)((head + 1) % MAX_POINTS);
    if (count < MAX_POINTS) count++;

    last_lat_u = la;
    last_lon_u = lo;
    have_last = true;
}

// =========================================================================
// Drawing
// =========================================================================
void TrackView::draw() {
    auto& tft = display_obj.tft;
    tft.fillScreen(TFT_BLACK);

    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(RigTheme::GOLD, TFT_BLACK);
    tft.drawString("TRACK", RigTheme::PAD_X, 10, 4);
    tft.drawFastHLine(RigTheme::PAD_X, 38, TFT_WIDTH - 2 * RigTheme::PAD_X,
                      RigTheme::OUTLINE);

    const int16_t mx = RigTheme::PAD_X;
    const int16_t my = 48;
    const int16_t mw = TFT_WIDTH - 2 * RigTheme::PAD_X;
    const int16_t mh = TFT_HEIGHT - my - 74;
    tft.drawRoundRect(mx, my, mw, mh, RigTheme::RADIUS, RigTheme::OUTLINE);

    if (count < 2) {
        tft.setTextDatum(MC_DATUM);
        tft.setTextColor(RigTheme::DIM, TFT_BLACK);
        tft.drawString(gps_obj.getFixStatus() ? "waiting for movement"
                                              : "no GPS fix yet",
                       TFT_WIDTH / 2, my + mh / 2, 2);
        tft.setTextDatum(TL_DATUM);
        tft.setTextColor(RigTheme::DIM2, TFT_BLACK);
        tft.drawString("The trail records while Rig Mode runs.",
                       mx, TFT_HEIGHT - 16, 1);
        return;
    }

    // ---- fit the trail to the box, preserving aspect ------------------------
    int32_t la_min = lat_u[idx(0)], la_max = la_min;
    int32_t lo_min = lon_u[idx(0)], lo_max = lo_min;
    for (uint16_t i = 1; i < count; i++) {
        int32_t la = lat_u[idx(i)], lo = lon_u[idx(i)];
        if (la < la_min) la_min = la;
        if (la > la_max) la_max = la;
        if (lo < lo_min) lo_min = lo;
        if (lo > lo_max) lo_max = lo;
    }

    // Metres-per-degree differ by axis, so scale longitude by cos(lat) before
    // fitting -- otherwise a north-south drive looks diagonal.
    double mid_lat = ((double)la_min + la_max) / 2.0 / 1000000.0;
    double kx = cos(mid_lat * M_PI / 180.0);
    if (kx < 0.05) kx = 0.05;

    double span_x = (double)(lo_max - lo_min) * kx;
    double span_y = (double)(la_max - la_min);
    if (span_x < 1.0) span_x = 1.0;
    if (span_y < 1.0) span_y = 1.0;

    const int16_t pad = 10;
    double sx = (double)(mw - 2 * pad) / span_x;
    double sy = (double)(mh - 2 * pad) / span_y;
    double s  = (sx < sy) ? sx : sy;      // one scale for both axes

    // Centre the fitted trail inside the box.
    double ox = mx + mw / 2.0 - ((double)(lo_min + lo_max) / 2.0) * kx * s;
    double oy = my + mh / 2.0 + ((double)(la_min + la_max) / 2.0) * s;

    auto px = [&](uint16_t i) { return (int16_t)lround((double)lon_u[idx(i)] * kx * s + ox); };
    // Screen y grows downward, latitude grows north: negate.
    auto py = [&](uint16_t i) { return (int16_t)lround(oy - (double)lat_u[idx(i)] * s); };

    // ---- the trail ----------------------------------------------------------
    // Older segments are dimmer, so the direction of travel reads at a glance
    // without drawing arrowheads that would be illegible at this size.
    for (uint16_t i = 1; i < count; i++) {
        uint16_t col = (i * 3 > count * 2) ? RigTheme::GOLD
                     : (i * 3 > count)     ? RigTheme::GOLD_D
                                           : RigTheme::DIM2;
        tft.drawLine(px(i - 1), py(i - 1), px(i), py(i), col);
    }

    // Start and current position.
    tft.fillCircle(px(0), py(0), 3, RigTheme::DIM);
    int16_t cxp = px(count - 1), cyp = py(count - 1);
    tft.fillCircle(cxp, cyp, 4, RigTheme::GREEN);
    tft.drawCircle(cxp, cyp, 7, RigTheme::GREEN);

    // ---- scale bar ----------------------------------------------------------
    // Pick a round distance that fits in a third of the box.
    const double m_per_u = 0.111;                     // metres per lat micro-degree
    double max_m = (mw / 3.0) / s * m_per_u;
    const double steps[] = { 50, 100, 200, 500, 1000, 2000, 5000 };
    double bar_m = steps[0];
    for (double st : steps) if (st <= max_m) bar_m = st;
    int16_t bar_px = (int16_t)lround(bar_m / m_per_u * s);
    int16_t bx = mx + 10, by = my + mh - 12;
    tft.drawFastHLine(bx, by, bar_px, RigTheme::DIM);
    tft.drawFastVLine(bx, by - 3, 6, RigTheme::DIM);
    tft.drawFastVLine(bx + bar_px, by - 3, 6, RigTheme::DIM);
    tft.setTextDatum(BL_DATUM);
    tft.setTextColor(RigTheme::DIM, TFT_BLACK);
    char sb[16];
    if (bar_m >= 1000) snprintf(sb, sizeof(sb), "%.0f km", bar_m / 1000.0);
    else               snprintf(sb, sizeof(sb), "%.0f m", bar_m);
    tft.drawString(sb, bx, by - 5, 1);

    // ---- stats --------------------------------------------------------------
    int16_t ty = TFT_HEIGHT - 60;
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(RigTheme::DIM, TFT_BLACK);
    tft.drawString("DISTANCE", mx, ty, 1);
    tft.drawString("POINTS", mx + 120, ty, 1);
    tft.setTextColor(RigTheme::INK, TFT_BLACK);
    char buf[24];
    if (dist_m >= 1000.0f) snprintf(buf, sizeof(buf), "%.2f km", dist_m / 1000.0f);
    else                   snprintf(buf, sizeof(buf), "%.0f m", dist_m);
    tft.drawString(buf, mx, ty + 12, 4);
    snprintf(buf, sizeof(buf), "%u", (unsigned)count);
    tft.drawString(buf, mx + 120, ty + 12, 4);

    tft.setTextColor(RigTheme::DIM2, TFT_BLACK);
    tft.drawString("C hold: exit", mx, TFT_HEIGHT - 12, 1);
    tft.setTextDatum(TL_DATUM);
}

// =========================================================================
// Blocking view
// =========================================================================
void TrackView::run() {
    #if defined(HAS_BUTTONS) && (C_BTN >= 0) && \
        !defined(MARAUDER_CARDPUTER) && !defined(MARAUDER_CARDPUTER_ADV)

    while (digitalRead(C_BTN) == LOW) delay(10);   // release the opening press
    delay(50);

    draw();
    uint32_t last_redraw = millis();
    bool held = false;
    uint32_t hold_start = 0;

    for (;;) {
        uint32_t now = millis();
        sample(now);                    // keep recording while the view is open

        // Repaint on the sampling cadence; the trail only changes that often.
        if (now - last_redraw >= SAMPLE_INTERVAL_MS) {
            last_redraw = now;
            draw();
        }

        bool c = (digitalRead(C_BTN) == LOW);
        if (c) {
            if (!held) { held = true; hold_start = now; }
            else if (now - hold_start >= 800) {
                while (digitalRead(C_BTN) == LOW) delay(10);
                display_obj.clearScreen();
                return;
            }
        } else {
            held = false;
        }
        delay(15);
    }
    #else
        display_obj.clearScreen();
    #endif
}

#endif  // HAS_SCREEN && HAS_GPS
