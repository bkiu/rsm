// Meshtastic protocol: packet framing, encryption, flooding relay, ACKs, NodeInfo exchange, and the outgoing
// message pipeline. The important part for usability is that sending a DM never fails silently: if we don't
// have the recipient's public key we ask for it (NodeInfo with want_response), hold the message, and send it
// the moment the key arrives; every state is visible on the message itself.
#pragma once
#include <stdint.h>

#include "crypto.h"
#include "messages.h"
#include "meshtastic/mesh.pb.h"
#include "radio.h"
#include "settings.h"

static constexpr uint32_t BROADCAST_ADDR = 0xffffffff;

class Mesh
{
  public:
    void begin();
    void applyChannels(); // recompute channel keys/hashes after settings change
    void tick();
    void onFrame(const RxFrame &f);

    // thread is a node number or channelThread(ch). Returns the stored message (status shows progress).
    Message *sendText(uint32_t thread, const char *text);
    // Manual retry of a failed / key-less message: fresh key request and fresh packet id.
    void retry(uint32_t msgId);
    void requestKey(uint32_t node); // asks the node for its NodeInfo (and so its public key)
    bool sendPosition(uint32_t dest, bool wantResponse, int ch); // false if there is no GPS fix

    bool channelUsed(int ch) const;
    const char *channelName(int ch) const; // primary with no name set shows the preset name, e.g. "LongFast"
    bool channelSharesPosition(int ch) const;
    const char *channelDisplayName() const { return channelName(0); }
    uint32_t channelUnread[MAX_CHANNELS] = {};
    uint32_t lastRxAt = 0;
    bool logPackets = false; // print a line per received packet on serial ('log on')
    bool uiDirty = true; // set whenever something the UI shows changed
    void (*onIncomingText)(uint32_t thread) = nullptr;

  private:
    struct Header {
        uint32_t to, from, id;
        uint8_t flags, channel, nextHop, relayNode;
    } __attribute__((packed));

    struct Pending {
        bool used;
        uint32_t msgId; // current packet id of the message
        uint32_t to;
        uint8_t tries;
        uint8_t keyRequests;
        bool keyRetryDone; // already recovered once from a PKI error NAK
        uint32_t nextAt;
    };
    static constexpr int MAX_PENDING = 8;
    Pending pend[MAX_PENDING] = {};

    struct Seen {
        uint32_t from, id, at;
        bool decodedForUs; // a packet to us we could decode (so a duplicate means our ACK was lost)
        uint8_t ch;        // channel it decoded on, for re-sending that ACK
    };
    static constexpr int SEEN_SIZE = 64;
    Seen seen[SEEN_SIZE] = {};
    int seenNext = 0;

    ChannelKey chanKey[MAX_CHANNELS] = {};
    uint8_t chanHash[MAX_CHANNELS] = {};
    uint32_t nextNodeInfoBroadcast = 0;
    uint32_t nextPositionAt = 0;
    uint32_t lastNodeInfoTo[8] = {}, lastNodeInfoAt[8] = {};
    uint32_t nextUnknownAskAt = 0;

    uint32_t newPacketId();
    Seen *findSeen(uint32_t from, uint32_t id);
    bool encodeAndQueue(uint32_t to, uint32_t id, meshtastic_Data &d, bool wantAck, bool pki, uint8_t hopLimit,
                        uint32_t delayMs, int ch = 0);
    void sendRouting(uint32_t to, uint32_t requestId, meshtastic_Routing_Error err, uint8_t hopLimit, int ch);
    void sendNodeInfo(uint32_t dest, bool wantResponse, int ch);
    void maybeRelay(const RxFrame &f, const Header &h);
    uint8_t responseHopLimit(int hops) const;
    uint32_t retransmitTimeout() const;

    Pending *findPending(uint32_t msgId);
    Pending *addPending(Message *m);
    bool transmitMessage(Message *m, Pending *p);
    void onKeyLearned(uint32_t node);
    void onImplicitAck(uint32_t id);

    void handleText(const Header &h, const meshtastic_Data &d, int ch);
    void handleNodeInfo(const Header &h, const meshtastic_Data &d, int ch);
    void handleRouting(const Header &h, const meshtastic_Data &d);
    void handlePosition(const Header &h, const meshtastic_Data &d, int ch);
};

extern Mesh mesh;

const char *routingErrorName(meshtastic_Routing_Error e);
