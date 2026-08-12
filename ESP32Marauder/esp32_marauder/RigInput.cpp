#include "RigInput.h"

// =========================================================================
// Keyboard backend — Cardputer ADV
// =========================================================================
#ifdef RIG_NAV_KEYBOARD

namespace {

    Keyboard_Class kb;
    uint32_t last_refresh_ms = 0;
    bool     had_keys_down = false;

    // Keys are identified by their position in the 4x14 matrix, not by the
    // character they produce. getKey() returns the *shifted* value while shift,
    // ctrl or caps-lock is held, so a char comparison means navigation dies the
    // moment someone rests a thumb on shift -- ';' becomes ':' and "up" stops
    // existing. The physical key never moves, so match on that.
    //
    // Coordinates are {x, y} into _key_value_map[y][x] (see Keyboard.h). The
    // four arrows are the Cardputer's own printed arrow cluster.
    struct KeyPos { int8_t x, y; };

    const KeyPos POS_UP     = {11, 2};  // ;  (marked as up arrow)
    const KeyPos POS_DOWN   = {11, 3};  // .  (down arrow)
    const KeyPos POS_LEFT   = {10, 3};  // ,  (left arrow)
    const KeyPos POS_RIGHT  = {12, 3};  // /  (right arrow)
    const KeyPos POS_ENTER  = {13, 2};  // ENTER
    const KeyPos POS_ESC    = { 0, 0};  // `  (marked esc)
    const KeyPos POS_BKSP   = {13, 0};  // BACKSPACE

    // Refresh at most this often. down() is called from busy-wait loops that
    // would otherwise hammer the I2C bus for no new information; the controller
    // is interrupt-driven anyway, so between events there is nothing to read.
    const uint32_t REFRESH_INTERVAL_MS = 5;

    void refresh() {
        const uint32_t now = millis();
        if (now - last_refresh_ms < REFRESH_INTERVAL_MS) return;
        last_refresh_ms = now;

        kb.updateKeyList();
        // Modifier state is only needed by text entry, and rebuilding it
        // allocates three vectors. Skip it when nothing is held, which is the
        // overwhelmingly common case while the rig just sits there scanning --
        // but run it once more on the poll that goes empty. updateKeysState()
        // is what resets the state buffer, so skipping it outright leaves the
        // last shift latched after the key is released and getKey() keeps
        // handing out shifted characters from then on.
        const bool keys_down = kb.isPressed();
        if (keys_down || had_keys_down) kb.updateKeysState();
        had_keys_down = keys_down;
    }

    bool heldAt(const KeyPos &p) {
        for (const auto &k : kb.keyList()) {
            if (k.x == p.x && k.y == p.y) return true;
        }
        return false;
    }

}  // namespace

namespace RigInput {

    void begin() {
        kb.begin();
    }

    Keyboard_Class &keyboard() {
        return kb;
    }

    bool down(Key k) {
        refresh();
        switch (k) {
            case UP:     return heldAt(POS_UP);
            case DOWN:   return heldAt(POS_DOWN);
            case LEFT:   return heldAt(POS_LEFT);
            case RIGHT:  return heldAt(POS_RIGHT);
            case SELECT: return heldAt(POS_ENTER);
            // Either of the two keys a person reaches for to back out. Esc is
            // the obvious one; backspace is what upstream Marauder trained
            // Cardputer users to press.
            case BACK:   return heldAt(POS_ESC) || heldAt(POS_BKSP);
            default:     return false;
        }
    }

    bool has(Key) {
        return true;   // the keyboard has all six
    }

}  // namespace RigInput

#elif !defined(RIG_NAV_GPIO)

// =========================================================================
// No directional input on this board (V8: touch only)
// =========================================================================
// The touch screens drive their own tap handling and never ask RigInput for a
// direction. These exist so the symbols resolve and so a call site that forgets
// its RIG_HAS_NAV guard fails as "nothing is pressed" rather than as a linker
// error nobody reads.
namespace RigInput {
    void begin() {}
    bool down(Key)  { return false; }
    bool has(Key)   { return false; }
}  // namespace RigInput

#else

// =========================================================================
// Button backend — the whole thing is inline in the header
// =========================================================================
namespace RigInput {
    void begin() {
        // Marauder's own setup already puts the button pins in INPUT_PULLUP;
        // nothing to do here beyond existing.
    }
}  // namespace RigInput

#endif
