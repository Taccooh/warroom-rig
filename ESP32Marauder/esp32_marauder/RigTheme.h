#pragma once

#ifndef RigTheme_h
#define RigTheme_h

// =========================================================================
// RigTheme — the rig's visual vocabulary, in one place
// =========================================================================
// These values used to live as local constants inside drawRigHome(), which was
// fine while the console was the only screen that looked like ours. It is not
// anymore: the tool list and the session modal are drawn in the same language,
// and three copies of "gold is 0xEDA9" is how a UI starts drifting apart one
// screen at a time.
//
// RGB565. The palette is deliberately narrow — two panel fills, one accent, and
// a three-step text ramp. A screen that needs a colour outside this set is
// usually a screen that has not decided what it is yet.
// =========================================================================

namespace RigTheme {
    // Accent — selection, active state, anything the eye should land on first.
    static const uint16_t GOLD    = 0xEDA9;
    static const uint16_t GOLD_D  = 0x9BC6;   // idle icons, chevrons

    // Text ramp, brightest to faintest.
    static const uint16_t INK     = 0xEF3B;   // warm off-white — primary label
    static const uint16_t DIM     = 0xCE79;   // secondary label
    static const uint16_t DIM2    = 0x9CF3;   // hint, footer, disabled

    // The two greys were 0x8C0E / 0x5AC9, picked on a desk. In daylight,
    // through a windscreen, at the angle a rig actually sits, they were not
    // dim -- they were gone. A label you cannot read is not a quiet label,
    // it is a missing one, so the ramp now starts where it stays legible and
    // uses size and weight to signal hierarchy instead of fading out.

    // Surfaces.
    static const uint16_t PANEL   = 0x1081;   // idle card fill
    static const uint16_t PANEL_S = 0x18C2;   // selected card fill
    static const uint16_t OUTLINE = 0x2902;   // idle card outline

    // Status. Used for state dots and verdicts, never as decoration.
    static const uint16_t GREEN   = 0x2E6B;
    static const uint16_t AMBER   = 0xFD00;
    static const uint16_t RED     = 0xF8A6;

    // Geometry shared by the list-style screens, so rows line up across them.
    static const int16_t  RADIUS      = 8;    // card corner
    static const int16_t  PAD_X       = 8;    // margin from the screen edge
    static const int16_t  HEADER_H    = 46;   // y where content starts
    static const int16_t  FOOTER_H    = 22;   // reserved for the button hint
    static const int16_t  ROW_H       = 30;   // list row height
    static const int16_t  ROW_GAP     = 4;
    static const int16_t  ACCENT_W    = 4;    // selected-row accent bar
}

#endif  // RigTheme_h
