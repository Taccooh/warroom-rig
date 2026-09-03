#pragma once

#ifndef RigTheme_h
#define RigTheme_h

#include "configs.h"
#include <stdint.h>

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

    // ---------------------------------------------------------------------
    // Geometry
    // ---------------------------------------------------------------------
    // Two screen shapes now. The Marauder V7/V8 panel is 240x320 portrait; the
    // Cardputer ADV is the same 240 wide and 135 tall in landscape. Identical
    // width is the lucky part -- every horizontal measurement carries over
    // untouched, and only the vertical rhythm has to compress.
    //
    // Layout must be written against SCREEN_WIDTH / SCREEN_HEIGHT and never
    // TFT_WIDTH / TFT_HEIGHT: the TFT_* pair describes the panel in its native
    // portrait orientation, so on the ADV they read 135 wide by 240 tall -- the
    // screen on its side. On the V7 the two pairs are the same macro, which is
    // exactly why that mistake survives unnoticed there.
    //
    // Assert the panel is the one this board is supposed to have, in every
    // translation unit that lays anything out. TFT_eSPI's driver headers define
    // TFT_WIDTH / TFT_HEIGHT with no #ifndef guard, so reaching the library
    // before configs.h silently replaces the board's geometry -- and since
    // SCREEN_* are macros that expand where they are *used*, the damage appears
    // only in the files that include things in that order. That is exactly what
    // happened here: two .cpp files laid the Cardputer's 240x135 screen out as
    // if it were 240x320, and the build stayed green throughout. arduino-cli's
    // default -w hides the redefinition warning, so nothing short of a check
    // like this one says a word.
#if defined(MARAUDER_CARDPUTER_ADV)
    static_assert(SCREEN_WIDTH == 240 && SCREEN_HEIGHT == 135,
                  "Cardputer ADV panel geometry was overridden -- TFT_eSPI was "
                  "reached before configs.h. Pass the panel macros as build "
                  "flags; see the ADV build in RELEASING.md.");
#elif defined(MARAUDER_V7) || defined(MARAUDER_V7_1) || defined(MARAUDER_V8)
    static_assert(SCREEN_WIDTH == 240 && SCREEN_HEIGHT == 320,
                  "Marauder panel geometry was overridden -- TFT_eSPI was "
                  "reached before configs.h.");
#endif

    static const bool COMPACT = (SCREEN_HEIGHT < 200);

    static const int16_t  RADIUS      = COMPACT ?  5 :  8;   // card corner
    static const int16_t  PAD_X       = COMPACT ?  6 :  8;   // margin from the edge
    static const int16_t  HEADER_H    = COMPACT ? 30 : 46;   // y where content starts
    static const int16_t  FOOTER_H    = COMPACT ? 12 : 22;   // reserved for the hint
    static const int16_t  ROW_H       = COMPACT ? 20 : 30;   // list row height
    static const int16_t  ROW_GAP     = COMPACT ?  3 :  4;
    static const int16_t  ACCENT_W    = 4;                   // selected-row bar

    // TFT_eSPI built-in fonts: 1 is ~8 px tall, 2 is ~16, 4 is ~26. On the short
    // screen a 26 px title eats a fifth of the display, so everything steps down
    // one rung and the subtitles go away rather than being shrunk into noise.
    static const uint8_t  FONT_TITLE  = COMPACT ? 2 : 4;
    static const uint8_t  FONT_ROW    = COMPACT ? 1 : 2;
    static const uint8_t  FONT_HINT   = 1;
    static const int16_t  ROW_CHAR_W  = COMPACT ? 6 : 11;    // px/char at FONT_ROW
    static const bool     SHOW_SUBS   = !COMPACT;            // room for a second line?

    // Header: bronze identity bar, then the live GPS / SD / battery line.
    static const int16_t  BAR_H       = COMPACT ? 16 : 24;
    static const int16_t  STATUS_Y    = BAR_H;
    static const int16_t  STATUS_H    = HEADER_H - BAR_H;

    // Home console: three action cards, then the Tools strip along the bottom.
    static const int16_t  CARD_H      = COMPACT ? 24 : 68;
    static const int16_t  CARD_Y0     = HEADER_H;
    static const int16_t  CARD_PITCH  = COMPACT ? 27 : 78;
    // The strip has to stay at least ICON_H tall or the Tools glyph hangs off
    // the bottom edge. On the compact screen that lands it directly under the
    // last card, with the card gap doing the separating.
    static const int16_t  TOOLS_Y     = COMPACT ? (CARD_Y0 + 3 * CARD_PITCH) : 294;
    static const int16_t  TOOLS_H     = SCREEN_HEIGHT - TOOLS_Y;

    // Session modal rows. The full-size panel gives each option 44 px and a
    // subtitle underneath; compact drops the subtitle and the height with it.
    static const int16_t  MODAL_ROW_H = COMPACT ? 22 : 44;
    static const int16_t  MODAL_HEAD  = COMPACT ? 22 : 40;

    // ---------------------------------------------------------------------
    // Tool run-views — the shared instrument case (see RigView)
    // ---------------------------------------------------------------------
    // Every scanner that used to draw the green Marauder console now runs inside
    // one case: the bronze identity bar (with a live traffic pulse), the GPS/SD
    // status line, a channel ribbon, a hero tile of headline counters, then the
    // body — a live feed, a meter, a spectrum or a data sheet depending on the
    // tool. The bar and status line reuse BAR_H / STATUS_Y / STATUS_H / HEADER_H
    // above so the case lines up pixel-for-pixel with the home console and the
    // menu; only the pieces below the status line are new.

    // Channel ribbon: a thin band of channel segments under the status line, on
    // the tall screen only — the short screen has no room and shows the channel
    // in the status line instead.
    static const int16_t  RIBBON_Y    = HEADER_H + 1;             // just under status
    static const int16_t  RIBBON_H    = 4;
    static const bool     SHOW_RIBBON = !COMPACT;

    // Hero tile: the same gold-spined panel WardriveCore uses, holding up to
    // three headline numbers for the tool.
    static const int16_t  TOOL_HERO_Y = COMPACT ? 31 : 54;
    static const int16_t  TOOL_HERO_H = COMPACT ? 34 : 48;
    static const int16_t  TOOL_LBL_DY = COMPACT ?  1 :  4;       // label baseline
    static const int16_t  TOOL_VAL_DY = COMPACT ?  9 : 11;       // big value baseline
    static const int16_t  TOOL_SUB_DY = COMPACT ? 25 : 37;       // strip baseline
    static const uint8_t  TOOL_VAL_SZ = COMPACT ?  2 :  4;       // hero number font

    // Body: whatever the instrument draws. Starts under the hero, ends above the
    // footer hint.
    static const int16_t  TOOL_BODY_Y   = COMPACT ? 67 : 108;
    static const int16_t  TOOL_FOOT_Y   = SCREEN_HEIGHT - (COMPACT ? 12 : 22); // rule
    static const int16_t  TOOL_HINT_Y   = SCREEN_HEIGHT - (COMPACT ?  9 : 11); // text
    static const int16_t  TOOL_BODY_END = TOOL_FOOT_Y - 2;

    // Feed rows: newest at the top. A signal-class stripe on the left, the name
    // and a sub line, four rising bars and the raw RSSI on the right.
    static const int16_t  FEED_ROW_H  = COMPACT ? 17 : 21;
    static const int16_t  FEED_PITCH  = COMPACT ? 19 : 23;
    static const int16_t  FEED_BARS_X = COMPACT ? 150 : 176;     // bars origin
    static const int16_t  FEED_RSSI_X = SCREEN_WIDTH - 6;        // right-aligned

    // Traffic pulse in the bronze bar: a short activity sparkline that lives
    // between the title and the battery glyph. One column per sampling slot.
    static const int16_t  PULSE_X     = COMPACT ? 108 : 124;
    static const uint8_t  PULSE_N     = COMPACT ? 10 : 14;   // *2 px wide, clears 156
    static const int16_t  PULSE_COL_W = 2;
    static const int16_t  PULSE_Y     = COMPACT ? 4 : 5;
    static const int16_t  PULSE_H     = COMPACT ? 8 : 14;
}

#endif  // RigTheme_h
