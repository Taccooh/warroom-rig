#pragma once

#ifndef Keyboard_h
#define Keyboard_h

#include "configs.h"

#ifdef MARAUDER_CARDPUTER_ADV

/**
 * @file keyboard.h
 * @author Forairaaaaa
 * @brief
 * @version 0.1
 * @date 2023-09-22
 *
 * @copyright Copyright (c) 2023
 *
 */
// Upstream drove two different keyboards from this file: the original
// Cardputer's 3x7 GPIO scan matrix and the ADV's TCA8418 I2C keypad
// controller. Only the ADV is a target here, so the matrix half is gone --
// including its "#define digitalRead(pin) gpio_get_level(...)" macro override,
// which quietly redefined those calls for everything below it in the file.
#include <iostream>
#include <vector>
#include "Arduino.h"
#include "Keyboard_def.h"
#include "configs.h"

#include <Wire.h>
#include "Adafruit_TCA8418.h"

struct Point2D_t
{
    int x;
    int y;
};

// One byte here carries two different alphabets: printable ASCII for the
// letter keys, and the modifier/function codes from Keyboard_def.h, which
// start at 0x80 and run up to KEY_FN's 0xff. Upstream stored both in a `char`,
// which is signed on Xtensa (the ADV build passes no -funsigned-char), so the
// modifiers came back out as -128..-126 and -1. Every test of the shape
// `value_first == KEY_LEFT_SHIFT` then compared -127 against 129 and was
// always false: shift, ctrl, alt and fn could not register at all, so no
// capital or shifted symbol was typeable, and those keys fell through to the
// HID branch where _kb_asciimap[128] got indexed with 0x80..0xff -- up to 127
// bytes past the end, every poll, for as long as the key was held. Unsigned is
// the type the values were always written as.
struct KeyValue_t
{
    const uint8_t value_first;
    const uint8_t value_second;
};

const KeyValue_t _key_value_map[4][14] = {{{'`', '~'},
                                           {'1', '!'},
                                           {'2', '@'},
                                           {'3', '#'},
                                           {'4', '$'},
                                           {'5', '%'},
                                           {'6', '^'},
                                           {'7', '&'},
                                           {'8', '*'},
                                           {'9', '('},
                                           {'0', ')'},
                                           {'-', '_'},
                                           {'=', '+'},
                                           {KEY_BACKSPACE, KEY_BACKSPACE}},
                                          {{KEY_TAB, KEY_TAB},
                                           {'q', 'Q'},
                                           {'w', 'W'},
                                           {'e', 'E'},
                                           {'r', 'R'},
                                           {'t', 'T'},
                                           {'y', 'Y'},
                                           {'u', 'U'},
                                           {'i', 'I'},
                                           {'o', 'O'},
                                           {'p', 'P'},
                                           {'[', '{'},
                                           {']', '}'},
                                           {'\\', '|'}},
                                          {{KEY_FN, KEY_FN},
                                           {KEY_LEFT_SHIFT, KEY_LEFT_SHIFT},
                                           {'a', 'A'},
                                           {'s', 'S'},
                                           {'d', 'D'},
                                           {'f', 'F'},
                                           {'g', 'G'},
                                           {'h', 'H'},
                                           {'j', 'J'},
                                           {'k', 'K'},
                                           {'l', 'L'},
                                           {';', ':'},
                                           {'\'', '\"'},
                                           {KEY_ENTER, KEY_ENTER}},
                                          {{KEY_LEFT_CTRL, KEY_LEFT_CTRL},
                                           {KEY_OPT, KEY_OPT},
                                           {KEY_LEFT_ALT, KEY_LEFT_ALT},
                                           {'z', 'Z'},
                                           {'x', 'X'},
                                           {'c', 'C'},
                                           {'v', 'V'},
                                           {'b', 'B'},
                                           {'n', 'N'},
                                           {'m', 'M'},
                                           {',', '<'},
                                           {'.', '>'},
                                           {'/', '?'},
                                           {' ', ' '}}};

class Keyboard_Class
{
public:
    struct KeysState
    {
        bool tab = false;
        bool fn = false;
        bool shift = false;
        bool ctrl = false;
        bool opt = false;
        bool alt = false;
        bool del = false;
        bool enter = false;
        bool space = false;
        uint8_t modifiers = 0;

        std::vector<char> word;
        std::vector<uint8_t> hid_keys;
        std::vector<uint8_t> modifier_keys;

        void reset()
        {
            tab = false;
            fn = false;
            shift = false;
            ctrl = false;
            opt = false;
            alt = false;
            del = false;
            enter = false;
            space = false;
            modifiers = 0;
            word.clear();
            hid_keys.clear();
            modifier_keys.clear();
        }
    };

private:
    std::vector<Point2D_t> _key_list_buffer;
    std::vector<Point2D_t> _key_pos_print_keys; // only text: eg A,B,C
    std::vector<Point2D_t> _key_pos_hid_keys;   // print key + space, enter, del
    std::vector<Point2D_t>
        _key_pos_modifier_keys; // modifier key: eg shift, ctrl, alt
    KeysState _keys_state_buffer;
    bool _is_caps_locked;
    uint8_t _last_key_size;

    Adafruit_TCA8418 _tca8418;
    bool _tca_initialized = false;
    static volatile bool _tca_interrupt;
    static void IRAM_ATTR _tca_isr();
    std::vector<Point2D_t> _tca_pressed_keys;  // Currently held keys

public:
    const char _ascii_list[95] = {'a','b','c','d','e','f','g','h','i','j','k','l','m','n','o','p',
                            'q','r','s','t','u','v','w','x','y','z','A','B','C','D','E','F',
                            'G','H','I','J','K','L','M','N','O','P','Q','R','S','T','U','V',
                            'W','X','Y','Z',' ','0','1','2','3','4','5','6','7','8','9','-',
                            '=','[',']',';','\'',',','.','/','`','\\','_','+','{','}',':',
                            '"','<','>','?','~','|','!','@','#','$','%','^','&','*','(',')'};
                            
    Keyboard_Class() : _is_caps_locked(false)
    {
    }

    void begin();
    uint8_t getKey(Point2D_t keyCoor);

    void updateKeyList();
    inline std::vector<Point2D_t> &keyList()
    {
        return _key_list_buffer;
    }

    inline KeyValue_t getKeyValue(const Point2D_t &keyCoor)
    {
        return _key_value_map[keyCoor.y][keyCoor.x];
    }

    uint8_t isPressed();
    bool isChange();
    bool isKeyPressed(char c);

    // Is this physical key down, whatever the modifiers say.
    //
    // isKeyPressed() resolves through shift, which is what text entry wants and
    // exactly what a control binding does not. Two of the function codes are
    // also printable characters that live on this keyboard: KEY_BACKSPACE is
    // 0x2a, which is '*' -- shift+8; KEY_ENTER is 0x28, which is '(' -- shift+9.
    // A control test against the resolved value therefore fires on those two
    // chords. The reverse holds as well: while shift is held every key resolves
    // to value_second, so ';' reads as ':' and the nav bindings stop matching
    // at all. Comparing value_first asks the question control bindings mean.
    bool isPhysicalKeyPressed(uint8_t v);

    String getPressedKeysString();

    void updateKeysState();
    inline KeysState &keysState()
    {
        return _keys_state_buffer;
    }

    inline bool capslocked(void)
    {
        return _is_caps_locked;
    }
    inline void setCapsLocked(bool isLocked)
    {
        _is_caps_locked = isLocked;
    }
};
#endif

#endif