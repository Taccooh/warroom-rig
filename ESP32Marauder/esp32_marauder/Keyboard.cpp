#include "Keyboard.h"

#ifdef MARAUDER_CARDPUTER_ADV

volatile bool Keyboard_Class::_tca_interrupt = false;

void IRAM_ATTR Keyboard_Class::_tca_isr() {
    _tca_interrupt = true;
}

void Keyboard_Class::begin()
{
    Wire.begin(8, 9);  // SDA=GPIO8, SCL=GPIO9
    _tca_initialized = _tca8418.begin(TCA8418_DEFAULT_ADDR, &Wire);
    if (_tca_initialized) {
        _tca8418.matrix(7, 8);  // 7 rows x 8 cols
        _tca8418.flush();
        _tca8418.enableInterrupts();
        pinMode(11, INPUT_PULLUP);
        attachInterrupt(digitalPinToInterrupt(11), _tca_isr, FALLING);
        _tca_interrupt = true;  // Force initial scan
    } else {
        Serial.println("[ERROR] TCA8418 keyboard not found on I2C (addr 0x34)");
    }
}

uint8_t Keyboard_Class::getKey(Point2D_t keyCoor)
{
    uint8_t ret = 0;

    if ((keyCoor.x < 0) || (keyCoor.y < 0))
    {
        return 0;
    }
    if (_keys_state_buffer.ctrl || _keys_state_buffer.shift ||
        _is_caps_locked)
    {
        ret = _key_value_map[keyCoor.y][keyCoor.x].value_second;
    }
    else
    {
        ret = _key_value_map[keyCoor.y][keyCoor.x].value_first;
    }
    return ret;
}

void Keyboard_Class::updateKeyList()
{
    _key_list_buffer.clear();
    Point2D_t coor;

    if (!_tca_initialized) return;

    // Drain the TCA8418 FIFO. The interrupt alone is not a sufficient trigger:
    // INT is edge-triggered and INT_STAT is cleared below, *after* the drain, so
    // an event that lands between the last getEvent() and that write leaves the
    // FIFO non-empty with the pin already deasserted and no further falling edge
    // coming. If the lost event is a release, the key stays in
    // _tca_pressed_keys forever -- the rig would read it as held, and every
    // "wait until the user lets go" loop in the UI would never end. Asking the
    // controller how many events it is actually holding costs one register read
    // per poll (RigInput rate-limits those to one per 5 ms) and closes the
    // window instead of narrowing it.
    if (_tca_interrupt || _tca8418.available()) {
        _tca_interrupt = false;

        int evt;
        while ((evt = _tca8418.getEvent()) != 0) {
            // Bit 7 = 1 means key press per TCA8418 datasheet (SCPS162).
            // Note: the vendored Adafruit library comments have this backwards.
            bool pressed = (evt & 0x80) != 0;
            int key = (evt & 0x7F) - 1;  // Convert from 1-indexed key code to 0-indexed
            if (key < 0 || key >= 70) continue;
            // TCA8418 uses 10-column stride internally
            int row = key / 10;
            int col = key % 10;
            if (row >= 7 || col >= 8) continue;

            // Remap to match _key_value_map[4][14] coordinate system
            coor.x = (row * 2) + (col > 3 ? 1 : 0);
            coor.y = col % 4;

            if (coor.x >= 14 || coor.y >= 4) continue;

            if (pressed) {
                // Prevent duplicate tracking (e.g. on missed release event)
                bool already = false;
                for (auto &k : _tca_pressed_keys) {
                    if (k.x == coor.x && k.y == coor.y) { already = true; break; }
                }
                if (!already) _tca_pressed_keys.push_back(coor);
            } else {
                // Remove released key from tracked state
                for (auto it = _tca_pressed_keys.begin(); it != _tca_pressed_keys.end(); ++it) {
                    if (it->x == coor.x && it->y == coor.y) {
                        _tca_pressed_keys.erase(it);
                        break;
                    }
                }
            }
        }
        // Check for FIFO overflow (bit 3) and recover
        uint8_t int_stat = _tca8418.readRegister(TCA8418_REG_INT_STAT);
        if (int_stat & 0x08) {
            _tca8418.flush();
            _tca_pressed_keys.clear();
        }
        // Clear all INT_STAT bits so the INT pin deasserts and can fire again
        _tca8418.writeRegister(TCA8418_REG_INT_STAT, 0x0F);
    }

    // Always populate buffer from currently-held keys
    for (auto &k : _tca_pressed_keys) {
        _key_list_buffer.push_back(k);
    }
}

uint8_t Keyboard_Class::isPressed()
{
    return _key_list_buffer.size();
}

bool Keyboard_Class::isChange()
{
    if (_last_key_size != _key_list_buffer.size())
    {
        _last_key_size = _key_list_buffer.size();
        return true;
    }
    else
    {
        return false;
    }
}

String Keyboard_Class::getPressedKeysString() {
    updateKeyList();
    String pressed = "";

    for (auto &keyCoor : _key_list_buffer) {
        // Cast, or String picks operator+=(unsigned char) and appends the code
        // point as decimal digits instead of the character it stands for.
        pressed += (char)getKey(keyCoor);
    }

    return pressed;
}

bool Keyboard_Class::isKeyPressed(char c)
{
    if (_key_list_buffer.size())
    {
        for (const auto &i : _key_list_buffer)
        {
            if (getKey(i) == c)
                return true;
        }
    }
    return false;
}

// Unshifted lookup -- see the declaration for why control keys need it.
bool Keyboard_Class::isPhysicalKeyPressed(uint8_t v)
{
    for (const auto &i : _key_list_buffer)
    {
        if (getKeyValue(i).value_first == v)
            return true;
    }
    return false;
}

#include <cstring>

void Keyboard_Class::updateKeysState()
{
    _keys_state_buffer.reset();
    _key_pos_print_keys.clear();
    _key_pos_hid_keys.clear();
    _key_pos_modifier_keys.clear();

    // Get special keys
    for (auto &i : _key_list_buffer)
    {
        // modifier
        if (getKeyValue(i).value_first == KEY_FN)
        {
            _keys_state_buffer.fn = true;
            continue;
        }
        if (getKeyValue(i).value_first == KEY_OPT)
        {
            _keys_state_buffer.opt = true;
            continue;
        }

        if (getKeyValue(i).value_first == KEY_LEFT_CTRL)
        {
            _keys_state_buffer.ctrl = true;
            _key_pos_modifier_keys.push_back(i);
            continue;
        }

        if (getKeyValue(i).value_first == KEY_LEFT_SHIFT)
        {
            _keys_state_buffer.shift = true;
            _key_pos_modifier_keys.push_back(i);
            continue;
        }

        if (getKeyValue(i).value_first == KEY_LEFT_ALT)
        {
            _keys_state_buffer.alt = true;
            _key_pos_modifier_keys.push_back(i);
            continue;
        }

        // function
        if (getKeyValue(i).value_first == KEY_TAB)
        {
            _keys_state_buffer.tab = true;
            _key_pos_hid_keys.push_back(i);
            continue;
        }

        if (getKeyValue(i).value_first == KEY_BACKSPACE)
        {
            _keys_state_buffer.del = true;
            _key_pos_hid_keys.push_back(i);
            continue;
        }

        if (getKeyValue(i).value_first == KEY_ENTER)
        {
            _keys_state_buffer.enter = true;
            _key_pos_hid_keys.push_back(i);
            continue;
        }

        if (getKeyValue(i).value_first == ' ')
        {
            _keys_state_buffer.space = true;
        }
        _key_pos_hid_keys.push_back(i);
        _key_pos_print_keys.push_back(i);
    }

    for (auto &i : _key_pos_modifier_keys)
    {
        uint8_t key = getKeyValue(i).value_first;
        _keys_state_buffer.modifier_keys.push_back(key);
    }

    for (auto &k : _keys_state_buffer.modifier_keys)
    {
        _keys_state_buffer.modifiers |= (1 << (k - 0x80));
    }

    for (auto &i : _key_pos_hid_keys)
    {
        uint8_t k = getKeyValue(i).value_first;
        if (k == KEY_TAB || k == KEY_BACKSPACE || k == KEY_ENTER)
        {
            _keys_state_buffer.hid_keys.push_back(k);
            continue;
        }
        // Everything that reaches here is printable ASCII -- the modifiers and
        // the three function keys were taken out of the list above -- but the
        // table only has 128 entries and the map can hold values up to 0xff, so
        // say so rather than rely on the loop above staying complete.
        if (k >= sizeof(_kb_asciimap))
            continue;
        uint8_t key = _kb_asciimap[k];
        if (key)
        {
            _keys_state_buffer.hid_keys.push_back(key);
        }
    }

    // Deal what left
    for (auto &i : _key_pos_print_keys)
    {
        if (_keys_state_buffer.ctrl || _keys_state_buffer.shift ||
            _is_caps_locked)
        {
            _keys_state_buffer.word.push_back(getKeyValue(i).value_second);
        }
        else
        {
            _keys_state_buffer.word.push_back(getKeyValue(i).value_first);
        }
    }
}

#endif