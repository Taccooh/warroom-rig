#pragma once

#ifndef TrackView_h
#define TrackView_h

#include "configs.h"

#if defined(HAS_SCREEN) && defined(HAS_GPS)

#include <Arduino.h>

// =========================================================================
// TrackView — where you have already driven
// =========================================================================
// The one question a wardriver cannot answer from the rig today is "have I
// done this street yet?". Everything on screen describes the present moment:
// nodes online, packets per minute, satellites. Nothing remembers the shape of
// the drive, so on a long session you re-cover ground you already own and skip
// streets you meant to take.
//
// This keeps a breadcrumb of the session and draws it to scale. It is not a
// map -- there is no basemap and no north-up convention worth trusting on a
// 240x320 panel -- it is the outline of your own drive, which is the part
// you actually need to see against the windscreen.
//
// Sampling runs from RigUI's main loop whenever there is a fix, so the trail
// accumulates during Rig Mode and is simply there when the view is opened.
//
// Memory: MAX_POINTS * 8 bytes, fixed, allocated in the object. No heap churn
// on a device that has to survive a whole day of collecting.
// =========================================================================

class TrackView {
public:
    // Called every loop; rate-limits and de-duplicates internally, so it is
    // cheap to call unconditionally.
    void sample(uint32_t now);

    // Blocking full-screen view. Returns when the user leaves.
    void run();

    uint16_t pointCount() const { return count; }
    float    distanceMeters() const { return dist_m; }

private:
    // 384 points at ~8 m spacing covers roughly 3 km of trail; beyond that the
    // oldest end is dropped. A longer buffer buys little on this screen -- the
    // whole point is the shape of the recent drive, and detail past a few
    // hundred points is smaller than a pixel once the view scales to fit.
    static const uint16_t MAX_POINTS = 384;

    // Micro-degrees: 1e-6 deg is ~0.11 m, well inside GPS noise, and int32
    // keeps the whole planet in range without float rounding at high zoom.
    int32_t lat_u[MAX_POINTS];
    int32_t lon_u[MAX_POINTS];
    uint16_t head = 0;          // next write slot
    uint16_t count = 0;         // valid points, <= MAX_POINTS
    bool     have_last = false;
    int32_t  last_lat_u = 0, last_lon_u = 0;
    uint32_t last_sample_ms = 0;
    float    dist_m = 0.0f;

    // A point is kept when it is far enough from the previous one to be a real
    // move rather than GPS jitter, and not more often than the interval.
    static const uint32_t SAMPLE_INTERVAL_MS = 2000;
    static const int32_t  MIN_MOVE_U = 70;      // ~8 m in latitude micro-degrees

    uint16_t idx(uint16_t i) const {            // oldest-first indexing
        uint16_t start = (count < MAX_POINTS) ? 0 : head;
        return (uint16_t)((start + i) % MAX_POINTS);
    }
    void draw();
};

extern TrackView track_view_obj;

#endif  // HAS_SCREEN && HAS_GPS
#endif  // TrackView_h
