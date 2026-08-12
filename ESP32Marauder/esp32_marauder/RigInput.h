#pragma once

#ifndef RigInput_h
#define RigInput_h

#include "configs.h"
#include <Arduino.h>

// =========================================================================
// RigInput — one name for "the user pressed up", whatever the board has
// =========================================================================
// The rig's three run-views and its console all poll buttons directly with
// digitalRead(), because Marauder's Switches wrapper is edge-triggered only and
// cannot answer "is it still held" — which is the gesture that leaves a view.
// That was fine while every supported board had the same five buttons.
//
// The Cardputer ADV has none of them. It has a keyboard behind a TCA8418 I2C
// controller, and its config sets U/D/L/R_BTN to -1. Every input path in the
// rig was written as `digitalRead(U_BTN) == LOW`, so on that board they all
// compiled out and the firmware booted into a console that accepted nothing —
// the failure that made the supported-hardware gate necessary in the first
// place.
//
// So the read primitive moves here and nothing else changes. Callers keep their
// own edge and hold tracking, exactly as they had it; they just ask RigInput
// instead of the GPIO. On the button boards down() inlines to the same
// digitalRead comparison it replaces, so the V7 image is unaffected.
//
// down() is meant to be called from a busy-wait loop: the keyboard backend
// refreshes its own cache on read (rate-limited), and that refresh polls the
// TCA8418's event counter rather than waiting for the controller's interrupt,
// so a release event cannot be stranded in the FIFO by a missed edge. That
// property is why this exposes button *state* rather than events: it makes
// replacing a digitalRead a local edit rather than a redesign of the loop
// around it.
//
// It is not a guarantee, though, and callers should not write one. A release
// dropped by a FIFO overflow leaves the key latched until the next overflow
// recovery, and a controller that stops answering on I2C answers "still held"
// forever. Every `while (down(x))` in the tree therefore carries a deadline;
// waiting on a key is a convenience, and no convenience is worth a rig that
// stops responding in the field.
// =========================================================================

// Which backend this board gets. RIG_HAS_NAV means "there is a directional
// input at all" and stands in for the old
// `defined(HAS_BUTTONS) && (C_BTN >= 0) && (U_BTN >= 0) && (D_BTN >= 0)`
// spelled out at every call site. Touch-only boards (V8) define neither and
// keep their own tap paths.
#if defined(HAS_BUTTONS) && (C_BTN >= 0) && (U_BTN >= 0) && (D_BTN >= 0)
  #define RIG_HAS_NAV
  #define RIG_NAV_GPIO
#elif defined(MARAUDER_CARDPUTER_ADV)
  #define RIG_HAS_NAV
  #define RIG_NAV_KEYBOARD
  #include "Keyboard.h"
#endif

// Per-key availability, for the call sites that used to test a pin number.
// `#if (L_BTN >= 0)` is not a question about the board any more: the ADV
// defines every direction as -1 and still has all four arrows.
#if defined(RIG_NAV_KEYBOARD) || (defined(RIG_NAV_GPIO) && (L_BTN >= 0))
  #define RIG_HAS_LEFT
#endif
#if defined(RIG_NAV_KEYBOARD) || (defined(RIG_NAV_GPIO) && (R_BTN >= 0))
  #define RIG_HAS_RIGHT
#endif
// A key that means "leave", separate from holding SELECT. Only the keyboard
// has one; the button boards spend all five on directions.
#ifdef RIG_NAV_KEYBOARD
  #define RIG_HAS_BACK
#endif

// What to tell the user to press. The footer hints used to be literals in the
// drawing code, which is fine until the same screen runs on a board whose keys
// have different names -- then every screen lies. Naming lives with the input
// layer that decides what the keys are.
#ifdef RIG_NAV_KEYBOARD
  #define RIG_HINT_LIST  "arrows move   ENTER open   ESC back"
  #define RIG_HINT_MODAL "arrows move   ENTER ok   ESC cancel"
  #define RIG_HINT_HOME  "arrows move   ENTER select"
  #define RIG_HINT_EXIT  "hold ENTER or press ESC to leave"
  #define RIG_HINT_SESSION "RIGHT: session   ESC: exit"
  #define RIG_HINT_PICK    "ENTER pick  <- all  -> go  ESC exit"
  #define RIG_HINT_CONFIRM "ENTER to upload"
  #define RIG_HINT_CANCEL  "ESC to cancel"
#else
  #define RIG_HINT_LIST  "U/D move   C select   L back"
  #define RIG_HINT_MODAL "U/D move   C confirm   L/R cancel"
  #define RIG_HINT_HOME  "U/D move   C select"
  #define RIG_HINT_EXIT  "hold C to leave"
  #define RIG_HINT_SESSION "R: session   C hold: exit"
  #define RIG_HINT_PICK    "C pick  L all  R GO  holdC exit"
  #define RIG_HINT_CONFIRM "Tap CENTER to upload"
  #define RIG_HINT_CANCEL  "Hold 2s to cancel"
#endif

namespace RigInput {

    enum Key : uint8_t {
        UP,
        DOWN,
        LEFT,
        RIGHT,
        SELECT,   // confirm / activate — CENTER on buttons, ENTER on the keyboard
        BACK,     // leave the screen. Only some boards have a key for it; where
                  // they do not, has(BACK) is false and callers fall back to
                  // the hold-SELECT gesture they already implement.
    };

    void begin();

    // Is this key held down right now?
    bool down(Key k);

    // Does this board have that key at all? Constant at runtime; a function so
    // call sites read the same on every target.
    bool has(Key k);

#ifdef RIG_NAV_KEYBOARD
    // The single keyboard instance. There must be exactly one: reading an event
    // drains it from the TCA8418's FIFO, so a second object would silently eat
    // half the keystrokes of the first. MenuFunctions' text entry borrows this
    // one rather than owning its own.
    Keyboard_Class &keyboard();
#endif

}  // namespace RigInput

#ifdef RIG_NAV_GPIO
// Inline so the button boards compile to exactly the comparison they used to
// have, with no call and no dispatch left over.
namespace RigInput {

    inline bool down(Key k) {
        switch (k) {
            case UP:     return digitalRead(U_BTN) == LOW;
            case DOWN:   return digitalRead(D_BTN) == LOW;
            case SELECT: return digitalRead(C_BTN) == LOW;
#if (L_BTN >= 0)
            case LEFT:   return digitalRead(L_BTN) == LOW;
#endif
#if (R_BTN >= 0)
            case RIGHT:  return digitalRead(R_BTN) == LOW;
#endif
            default:     return false;   // BACK, and any pin this board lacks
        }
    }

    inline bool has(Key k) {
        switch (k) {
            case UP:
            case DOWN:
            case SELECT: return true;
            case LEFT:   return (L_BTN >= 0);
            case RIGHT:  return (R_BTN >= 0);
            default:     return false;   // no dedicated back button
        }
    }

}  // namespace RigInput
#endif  // RIG_NAV_GPIO

#endif  // RigInput_h
