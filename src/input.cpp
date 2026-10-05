#include "input.h"

#include <M5Cardputer.h>
#include <algorithm>
#include <memory>
#include <utility/Adafruit_TCA8418/Adafruit_TCA8418.h>
#include <utility/Adafruit_TCA8418/Adafruit_TCA8418_registers.h>
#include <vector>

// Replacement for the library's TCA8418 reader. The library only reads the keyboard chip after an interrupt
// edge, and can lose one: a key event arriving while it clears its flag leaves INT held low with no new edge,
// and the keyboard is dead until reboot. This drains the chip's event FIFO whenever INT is low, and every
// 100 ms regardless, so a missed edge costs at most a short delay.
class PolledKeyboardReader : public KeyboardReader
{
  public:
    void begin() override
    {
        if (!tca.begin()) {
            Serial.println("keyboard: TCA8418 not found");
            return;
        }
        tca.matrix(7, 8);
        tca.flush();
        tca.enableInterrupts(); // only drives the INT pin, which we poll
        pinMode(INT_PIN, INPUT_PULLUP);
        ok = true;
    }

    void update() override
    {
        if (!ok)
            return;
        uint32_t now = millis();
        if (digitalRead(INT_PIN) != LOW && now - lastPoll < 100)
            return;
        lastPoll = now;
        uint8_t n = tca.readRegister8(TCA8418_REG_KEY_LCK_EC) & 0x0f;
        for (uint8_t i = 0; i < n; i++) {
            uint8_t ev = tca.getEvent();
            if (!ev)
                break;
            apply(ev);
        }
        uint8_t status = tca.readRegister8(TCA8418_REG_INT_STAT);
        if (status & 0x08)
            _key_list.clear(); // FIFO overflowed, so releases may be lost: don't leave keys stuck down
        tca.writeRegister8(TCA8418_REG_INT_STAT, 0x1f);
    }

  private:
    static constexpr int INT_PIN = 11;
    Adafruit_TCA8418 tca;
    bool ok = false;
    uint32_t lastPoll = 0;

    void apply(uint8_t ev)
    {
        // Same coordinate remap as the library, so M5Cardputer's key map still applies.
        uint8_t idx = (ev & 0x7f) - 1;
        uint8_t row = idx / 10, col = idx % 10;
        Point2D_t p;
        p.x = row * 2 + (col > 3 ? 1 : 0);
        p.y = col % 4;
        auto it = std::find(_key_list.begin(), _key_list.end(), p);
        if (ev & 0x80) {
            if (it == _key_list.end())
                _key_list.push_back(p);
        }
        else if (it != _key_list.end())
            _key_list.erase(it);
    }
};

void inputBegin()
{
    M5Cardputer.Keyboard.begin(std::unique_ptr<KeyboardReader>(new PolledKeyboardReader()));
}

static std::vector<Point2D_t> prevKeys;
static KeyEvent queue[16];
static int qHead = 0, qLen = 0;
static KeyEvent repeatEv;
static uint32_t repeatAt = 0;

static void push(const KeyEvent &e)
{
    if (qLen < 16) {
        queue[(qHead + qLen) % 16] = e;
        qLen++;
    }
}

bool inputNext(KeyEvent &out)
{
    if (!qLen)
        return false;
    out = queue[qHead];
    qHead = (qHead + 1) % 16;
    qLen--;
    return true;
}

static bool isModifier(uint8_t code)
{
    return code == KEY_FN || code == KEY_OPT || code == KEY_LEFT_CTRL || code == KEY_LEFT_SHIFT || code == KEY_LEFT_ALT;
}

void inputPoll()
{
    auto &kb = M5Cardputer.Keyboard;
    kb.updateKeyList();
    const std::vector<Point2D_t> &keys = kb.keyList();

    KeyEvent base = {};
    for (const auto &p : keys) {
        switch (kb.getKeyValue(p).value_first) {
        case KEY_FN: base.fn = true; break;
        case KEY_OPT: base.opt = true; break;
        case KEY_LEFT_CTRL: base.ctrl = true; break;
        case KEY_LEFT_SHIFT: base.shift = true; break;
        case KEY_LEFT_ALT: base.alt = true; break;
        }
    }

    int nonMods = 0;
    for (const auto &p : keys) {
        KeyValue_t kv = kb.getKeyValue(p);
        uint8_t code = kv.value_first;
        if (isModifier(code))
            continue;
        nonMods++;
        if (std::find(prevKeys.begin(), prevKeys.end(), p) != prevKeys.end())
            continue; // still held from before

        KeyEvent e = base;
        if (code == KEY_BACKSPACE)
            e.key = base.fn ? Key::Delete : Key::Backspace;
        else if (code == KEY_TAB)
            e.key = Key::Tab;
        else if (code == KEY_ENTER)
            e.key = Key::Enter;
        else if (base.fn && code == ';')
            e.key = Key::Up;
        else if (base.fn && code == '.')
            e.key = Key::Down;
        else if (base.fn && code == ',')
            e.key = Key::Left;
        else if (base.fn && code == '/')
            e.key = Key::Right;
        else if (code == '`' && !base.fn && !base.shift)
            e.key = Key::Esc; // the key is labelled Esc; Fn+` types a backtick, Shift+` a tilde
        else if (code == '`' && base.fn) {
            e.key = Key::Char;
            e.ch = '`';
        }
        else {
            e.key = Key::Char;
            e.ch = (base.shift || kb.capslocked()) ? kv.value_second : kv.value_first;
        }
        push(e);
        repeatEv = e;
        repeatAt = millis() + 450;
    }

    // Auto-repeat for a single held key that makes sense to repeat.
    bool repeatable = repeatEv.key == Key::Backspace || repeatEv.key == Key::Delete || repeatEv.key == Key::Up ||
                      repeatEv.key == Key::Down || repeatEv.key == Key::Left || repeatEv.key == Key::Right;
    if (nonMods == 1 && repeatable && repeatAt && (int32_t)(millis() - repeatAt) >= 0) {
        push(repeatEv);
        repeatAt = millis() + 70;
    }
    if (nonMods == 0)
        repeatAt = 0;

    prevKeys = keys;
}
