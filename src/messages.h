// Conversation history. One thread per node (direct messages) plus one for the primary channel.
#pragma once
#include <stddef.h>
#include <stdint.h>

// Channel conversations use thread ids counting down from 0xffffffff (channel 0 = primary), well clear of node numbers.
inline uint32_t channelThread(int ch) { return 0xffffffffu - (uint32_t)ch; }
inline bool isChannelThread(uint32_t t) { return t >= 0xfffffff8u; }
inline int threadChannel(uint32_t t) { return (int)(0xffffffffu - t); }
static constexpr uint32_t CHANNEL_THREAD = 0xffffffff; // primary channel

enum class MsgStatus : uint8_t {
    Received,
    WaitingForKey, // DM queued until we learn the recipient's public key
    NoKey,         // key requests went unanswered; still sends automatically if the key arrives later
    Queued,        // ready to transmit
    Sending,       // transmitted, waiting for an ACK
    Relayed,       // a neighbour rebroadcast it (implicit ACK) but the recipient hasn't confirmed yet
    Delivered,     // recipient ACKed
    Failed,
};

struct Message {
    uint32_t id;     // packet id (0 for none)
    uint32_t thread; // node number of the other party, or channelThread(n)
    uint32_t from;   // sender node number
    uint32_t when;   // millis() when sent/received
    MsgStatus status;
    char detail[40]; // failure reason / extra status text
    char text[201];
};

class MessageStore
{
  public:
    static constexpr int MAX_MESSAGES = 120;

    void load();
    void save();
    void saveSoon();
    void tick();

    Message *add(uint32_t thread, uint32_t from, uint32_t id, const char *text, MsgStatus st);
    Message *findById(uint32_t id);
    int count() const { return n; }
    Message &at(int i) { return msgs[(start + i) % MAX_MESSAGES]; } // oldest first

    // Collects the messages of one thread, oldest first.
    int thread(uint32_t thread, Message **out, int max);
    void clearThread(uint32_t thread);

  private:
    Message msgs[MAX_MESSAGES];
    int start = 0, n = 0;
    uint32_t saveAt = 0;
};

extern MessageStore messages;

const char *statusLabel(MsgStatus s);
