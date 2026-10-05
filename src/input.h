// Keyboard events for the Cardputer ADV. The M5Cardputer library reports which keys are held; this turns that
// into discrete press events (with repeat for held navigation keys) and decodes Fn combos into arrows/Esc.
#pragma once
#include <stdint.h>

enum class Key : uint8_t { None, Char, Enter, Backspace, Delete, Tab, Up, Down, Left, Right, Esc };

struct KeyEvent {
    Key key;
    char ch; // for Key::Char
    bool fn, ctrl, alt, opt, shift;
};

void inputBegin();              // call after M5Cardputer.begin(cfg, false)
void inputPoll();               // call once per loop
bool inputNext(KeyEvent &out);  // pops the next queued event
