#include "messages.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <algorithm>

MessageStore messages;

static const char *MSG_FILE = "/messages.bin";
static constexpr uint32_t MSG_VERSION = 1;

void MessageStore::load()
{
    start = n = 0;
    File f = LittleFS.open(MSG_FILE, "r");
    if (!f)
        return;
    uint32_t ver = 0, count = 0;
    if (f.read((uint8_t *)&ver, 4) == 4 && ver == MSG_VERSION && f.read((uint8_t *)&count, 4) == 4) {
        for (uint32_t i = 0; i < count && n < MAX_MESSAGES; i++) {
            Message &m = msgs[n];
            if (f.read((uint8_t *)&m, sizeof(Message)) != sizeof(Message))
                break;
            m.when = 0;
            // Anything still in flight when we powered off never got confirmed.
            if (m.status == MsgStatus::Queued || m.status == MsgStatus::Sending || m.status == MsgStatus::WaitingForKey) {
                m.status = MsgStatus::Failed;
                snprintf(m.detail, sizeof(m.detail), "interrupted by restart");
            }
            n++;
        }
    }
    f.close();
}

void MessageStore::save()
{
    saveAt = 0;
    File f = LittleFS.open(MSG_FILE, "w");
    if (!f)
        return;
    uint32_t ver = MSG_VERSION, count = n;
    f.write((uint8_t *)&ver, 4);
    f.write((uint8_t *)&count, 4);
    for (int i = 0; i < n; i++)
        f.write((uint8_t *)&at(i), sizeof(Message));
    f.close();
}

void MessageStore::saveSoon()
{
    if (!saveAt)
        saveAt = millis() + 10000;
}

void MessageStore::tick()
{
    if (saveAt && (int32_t)(millis() - saveAt) >= 0)
        save();
}

Message *MessageStore::add(uint32_t thread, uint32_t from, uint32_t id, const char *text, MsgStatus st)
{
    Message *m;
    if (n < MAX_MESSAGES) {
        m = &msgs[(start + n) % MAX_MESSAGES];
        n++;
    } else {
        m = &msgs[start];
        start = (start + 1) % MAX_MESSAGES;
    }
    memset(m, 0, sizeof(*m));
    m->id = id;
    m->thread = thread;
    m->from = from;
    m->when = millis();
    m->status = st;
    strlcpy(m->text, text, sizeof(m->text));
    saveSoon();
    return m;
}

Message *MessageStore::findById(uint32_t id)
{
    if (!id)
        return nullptr;
    for (int i = n - 1; i >= 0; i--)
        if (at(i).id == id && at(i).status != MsgStatus::Received)
            return &at(i);
    return nullptr;
}

int MessageStore::thread(uint32_t thread, Message **out, int max)
{
    int k = 0;
    for (int i = 0; i < n && k < max; i++)
        if (at(i).thread == thread)
            out[k++] = &at(i);
    return k;
}

void MessageStore::clearThread(uint32_t thread)
{
    // start is only non-zero once the ring is full; straighten it so we can compact in place.
    std::rotate(msgs, msgs + start, msgs + MAX_MESSAGES);
    start = 0;
    int w = 0;
    for (int i = 0; i < n; i++)
        if (msgs[i].thread != thread)
            msgs[w++] = msgs[i];
    n = w;
    save();
}

const char *statusLabel(MsgStatus s)
{
    switch (s) {
    case MsgStatus::WaitingForKey: return "getting key...";
    case MsgStatus::NoKey: return "no key yet";
    case MsgStatus::Queued: return "queued";
    case MsgStatus::Sending: return "sending...";
    case MsgStatus::Relayed: return "relayed";
    case MsgStatus::Delivered: return "delivered";
    case MsgStatus::Failed: return "FAILED";
    default: return "";
    }
}
