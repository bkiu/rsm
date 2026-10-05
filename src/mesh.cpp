#include "mesh.h"

#include <Arduino.h>
#include <esp_random.h>
#include <pb_decode.h>
#include <pb_encode.h>

#include "gps.h"
#include "nodedb.h"
#include "regions.h"
#include "settings.h"

Mesh mesh;

static constexpr uint8_t FLAG_HOP_LIMIT_MASK = 0x07;
static constexpr uint8_t FLAG_WANT_ACK = 0x08;
static constexpr uint8_t FLAG_HOP_START_SHIFT = 5;
static constexpr uint8_t BITFIELD_WANT_RESPONSE = 1 << 1;
static constexpr size_t HEADER_LEN = 16;

static constexpr int MAX_TRIES = 3;
static constexpr int MAX_KEY_REQUESTS = 3;
static constexpr uint32_t KEY_REQUEST_INTERVAL_MS = 45000;
static constexpr uint32_t RELAYED_ACK_WAIT_MS = 90000;
static constexpr uint32_t NODEINFO_BROADCAST_MS = 3UL * 60 * 60 * 1000;
static constexpr uint32_t NODEINFO_PER_DEST_MIN_MS = 15000;
static constexpr uint32_t UNKNOWN_ASK_SPACING_MS = 30000;

static bool decodeData(const uint8_t *buf, size_t len, meshtastic_Data &d)
{
    memset(&d, 0, sizeof(d));
    pb_istream_t s = pb_istream_from_buffer(buf, len);
    return pb_decode(&s, meshtastic_Data_fields, &d) && d.portnum != meshtastic_PortNum_UNKNOWN_APP;
}

void Mesh::begin()
{
    applyChannels();
    nextNodeInfoBroadcast = millis() + 15000 + random(0, 10000);
}

void Mesh::applyChannels()
{
    for (int i = 0; i < MAX_CHANNELS; i++) {
        if (!channelUsed(i))
            continue;
        chanKey[i] = i == 0 ? expandPsk(settings.psk, settings.pskLen)
                            : expandPsk(settings.channels[i - 1].psk, settings.channels[i - 1].pskLen);
        chanHash[i] = channelHash(channelName(i), chanKey[i]);
    }
    uiDirty = true;
}

bool Mesh::channelUsed(int ch) const
{
    return ch == 0 || (ch > 0 && ch < MAX_CHANNELS && settings.channels[ch - 1].used);
}

const char *Mesh::channelName(int ch) const
{
    if (ch == 0)
        return settings.channelName[0] ? settings.channelName : presetName(settings.preset);
    return channelUsed(ch) ? settings.channels[ch - 1].name : "";
}

bool Mesh::channelSharesPosition(int ch) const
{
    if (ch == 0)
        return settings.sharePosition;
    return channelUsed(ch) && settings.channels[ch - 1].sharePosition;
}

uint32_t Mesh::newPacketId()
{
    uint32_t id;
    do {
        id = esp_random();
    } while (id == 0);
    return id;
}

uint8_t Mesh::responseHopLimit(int hops) const
{
    // Same rule as the firmware's RoutingModule::getHopLimitForResponse.
    uint8_t limit = settings.hopLimit;
    if (hops >= 0) {
        if (hops > limit)
            return hops;
        if (hops + 2 < limit)
            return hops + 2;
    }
    return limit;
}

uint32_t Mesh::retransmitTimeout() const
{
    // Roughly the firmware's getRetransmissionMsec(): long enough for a neighbour to rebroadcast or ACK.
    uint32_t t = 2 * radio.airtimeMs(200) + (8 + 16 + 32) * radio.slotTimeMs() + 500;
    return t < 6000 ? 6000 : t;
}

// ---------------------------------------------------------------------------------------------------------
// Transmit path

bool Mesh::encodeAndQueue(uint32_t to, uint32_t id, meshtastic_Data &d, bool wantAck, bool pki, uint8_t hopLimit,
                          uint32_t delayMs, int ch)
{
    if (!pki && !channelUsed(ch))
        return false;
    d.has_bitfield = true;
    d.bitfield = d.want_response ? BITFIELD_WANT_RESPONSE : 0;

    uint8_t plain[MAX_FRAME];
    pb_ostream_t os = pb_ostream_from_buffer(plain, sizeof(plain));
    if (!pb_encode(&os, meshtastic_Data_fields, &d))
        return false;
    size_t n = os.bytes_written;

    uint8_t frame[MAX_FRAME];
    size_t total = HEADER_LEN + n + (pki ? PKI_OVERHEAD : 0);
    if (total > MAX_FRAME)
        return false;

    Header h;
    h.to = to;
    h.from = myNodeNum;
    h.id = id;
    hopLimit &= FLAG_HOP_LIMIT_MASK;
    h.flags = hopLimit | (wantAck ? FLAG_WANT_ACK : 0) | (hopLimit << FLAG_HOP_START_SHIFT);
    h.channel = pki ? 0 : chanHash[ch];
    h.nextHop = 0;
    h.relayNode = myNodeNum & 0xff;
    memcpy(frame, &h, HEADER_LEN);

    if (pki) {
        Node *node = nodeDB.get(to);
        if (!node || !node->hasKey)
            return false;
        if (!pkiEncrypt(settings.privateKey, node->publicKey, myNodeNum, id, plain, n, frame + HEADER_LEN))
            return false;
    } else {
        memcpy(frame + HEADER_LEN, plain, n);
        channelCrypt(chanKey[ch], myNodeNum, id, frame + HEADER_LEN, n);
    }

    // Remember our own packet so its echo from a relay is recognised rather than processed.
    Seen &s = seen[seenNext];
    seenNext = (seenNext + 1) % SEEN_SIZE;
    s = {myNodeNum, id, millis(), false};

    return radio.enqueue(frame, total, delayMs, myNodeNum, id);
}

void Mesh::sendRouting(uint32_t to, uint32_t requestId, meshtastic_Routing_Error err, uint8_t hopLimit, int ch)
{
    meshtastic_Routing r = meshtastic_Routing_init_zero;
    r.which_variant = meshtastic_Routing_error_reason_tag;
    r.error_reason = err;

    meshtastic_Data d = meshtastic_Data_init_zero;
    d.portnum = meshtastic_PortNum_ROUTING_APP;
    d.request_id = requestId;
    pb_ostream_t os = pb_ostream_from_buffer(d.payload.bytes, sizeof(d.payload.bytes));
    pb_encode(&os, meshtastic_Routing_fields, &r);
    d.payload.size = os.bytes_written;
    encodeAndQueue(to, newPacketId(), d, false, false, hopLimit, random(0, 8) * radio.slotTimeMs(), ch);
}

void Mesh::sendNodeInfo(uint32_t dest, bool wantResponse, int ch)
{
    // Don't spam a single destination, but always let a broadcast through.
    uint32_t now = millis();
    if (dest != BROADCAST_ADDR) {
        for (int i = 0; i < 8; i++)
            if (lastNodeInfoTo[i] == dest && now - lastNodeInfoAt[i] < NODEINFO_PER_DEST_MIN_MS)
                return;
        int slot = 0;
        for (int i = 1; i < 8; i++)
            if (lastNodeInfoAt[i] < lastNodeInfoAt[slot])
                slot = i;
        lastNodeInfoTo[slot] = dest;
        lastNodeInfoAt[slot] = now;
    }

    meshtastic_User u = meshtastic_User_init_zero;
    snprintf(u.id, sizeof(u.id), "!%08x", (unsigned)myNodeNum);
    strlcpy(u.long_name, settings.longName, sizeof(u.long_name));
    strlcpy(u.short_name, settings.shortName, sizeof(u.short_name));
    u.hw_model = meshtastic_HardwareModel_M5STACK_CARDPUTER_ADV;
    u.role = meshtastic_Config_DeviceConfig_Role_CLIENT;
    u.public_key.size = 32;
    memcpy(u.public_key.bytes, settings.publicKey, 32);

    meshtastic_Data d = meshtastic_Data_init_zero;
    d.portnum = meshtastic_PortNum_NODEINFO_APP;
    d.want_response = wantResponse;
    pb_ostream_t os = pb_ostream_from_buffer(d.payload.bytes, sizeof(d.payload.bytes));
    pb_encode(&os, meshtastic_User_fields, &u);
    d.payload.size = os.bytes_written;
    // A node we know is several hops out needs a hop limit that reaches it, even if ours is set lower.
    uint8_t hopLimit = settings.hopLimit;
    if (dest != BROADCAST_ADDR)
        if (Node *n = nodeDB.get(dest))
            hopLimit = responseHopLimit(n->hopsAway);
    encodeAndQueue(dest, newPacketId(), d, false, false, hopLimit, random(0, 8) * radio.slotTimeMs(), ch);
}

void Mesh::requestKey(uint32_t node)
{
    // Ask on the channel we last heard them on: it may be the only one we share.
    Node *n = nodeDB.get(node);
    sendNodeInfo(node, true, n && channelUsed(n->channel) ? n->channel : 0);
}

bool Mesh::sendPosition(uint32_t dest, bool wantResponse, int ch)
{
    if (!gpsState.hasFix || !radio.ready())
        return false;
    meshtastic_Position pos = meshtastic_Position_init_zero;
    int32_t lat = gpsState.latitudeI, lon = gpsState.longitudeI;
    uint8_t bits = settings.positionPrecision;
    if (bits > 0 && bits < 32) {
        // Same coarsening as the firmware: keep the top bits, then centre in the remaining box.
        lat = (int32_t)(((uint32_t)lat & (UINT32_MAX << (32 - bits))) + (1u << (31 - bits)));
        lon = (int32_t)(((uint32_t)lon & (UINT32_MAX << (32 - bits))) + (1u << (31 - bits)));
    }
    pos.has_latitude_i = pos.has_longitude_i = true;
    pos.latitude_i = lat;
    pos.longitude_i = lon;
    pos.has_altitude = true;
    pos.altitude = gpsState.altitudeM;
    pos.time = gpsState.unixTime;
    pos.location_source = meshtastic_Position_LocSource_LOC_INTERNAL;
    pos.precision_bits = bits;
    pos.sats_in_view = gpsState.sats;

    meshtastic_Data d = meshtastic_Data_init_zero;
    d.portnum = meshtastic_PortNum_POSITION_APP;
    d.want_response = wantResponse;
    pb_ostream_t os = pb_ostream_from_buffer(d.payload.bytes, sizeof(d.payload.bytes));
    if (!pb_encode(&os, meshtastic_Position_fields, &pos))
        return false;
    d.payload.size = os.bytes_written;
    return encodeAndQueue(dest, newPacketId(), d, false, false, settings.hopLimit, random(0, 8) * radio.slotTimeMs(), ch);
}

void Mesh::handlePosition(const Header &h, const meshtastic_Data &d, int ch)
{
    meshtastic_Position pos = meshtastic_Position_init_zero;
    pb_istream_t s = pb_istream_from_buffer(d.payload.bytes, d.payload.size);
    if (pb_decode(&s, meshtastic_Position_fields, &pos) && pos.has_latitude_i && pos.has_longitude_i &&
        (pos.latitude_i != 0 || pos.longitude_i != 0)) {
        Node *n = nodeDB.getOrCreate(h.from);
        n->latitudeI = pos.latitude_i;
        n->longitudeI = pos.longitude_i;
        n->hasPosition = true;
        nodeDB.saveSoon();
    }
    // Answer "send me your position" only if the user shares position on the channel it was asked on.
    if (d.want_response && h.to == myNodeNum && channelSharesPosition(ch))
        sendPosition(h.from, false, ch);
}

// ---------------------------------------------------------------------------------------------------------
// Outgoing messages

Mesh::Pending *Mesh::findPending(uint32_t msgId)
{
    for (auto &p : pend)
        if (p.used && p.msgId == msgId)
            return &p;
    return nullptr;
}

Mesh::Pending *Mesh::addPending(Message *m)
{
    Pending *slot = nullptr;
    for (auto &p : pend)
        if (!p.used) {
            slot = &p;
            break;
        }
    if (!slot) {
        snprintf(m->detail, sizeof(m->detail), "too many messages in flight");
        m->status = MsgStatus::Failed;
        return nullptr;
    }
    memset(slot, 0, sizeof(*slot));
    slot->used = true;
    slot->msgId = m->id;
    slot->to = isChannelThread(m->thread) ? BROADCAST_ADDR : m->thread;
    slot->nextAt = millis();
    return slot;
}

Message *Mesh::sendText(uint32_t thread, const char *text)
{
    Message *m = messages.add(thread, myNodeNum, newPacketId(), text, MsgStatus::Queued);
    uiDirty = true;
    if (!radio.ready()) {
        m->status = MsgStatus::Failed;
        snprintf(m->detail, sizeof(m->detail), "radio off: %s", radio.status());
        return m;
    }
    if (isChannelThread(thread) && !channelUsed(threadChannel(thread))) {
        m->status = MsgStatus::Failed;
        snprintf(m->detail, sizeof(m->detail), "channel was removed");
        return m;
    }
    if (!isChannelThread(thread)) {
        Node *n = nodeDB.get(thread);
        if (!n || !n->hasKey)
            m->status = MsgStatus::WaitingForKey;
    }
    addPending(m);
    return m;
}

void Mesh::retry(uint32_t msgId)
{
    Message *m = messages.findById(msgId);
    if (!m || m->from != myNodeNum)
        return;
    Pending *p = findPending(msgId);
    if (p)
        p->used = false;
    m->id = newPacketId(); // receivers de-duplicate on id, so a retry must look like a new packet
    m->detail[0] = 0;
    bool dm = !isChannelThread(m->thread);
    Node *n = dm ? nodeDB.get(m->thread) : nullptr;
    m->status = (dm && (!n || !n->hasKey)) ? MsgStatus::WaitingForKey : MsgStatus::Queued;
    addPending(m);
    messages.saveSoon();
    uiDirty = true;
}

bool Mesh::transmitMessage(Message *m, Pending *p)
{
    meshtastic_Data d = meshtastic_Data_init_zero;
    d.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;
    size_t len = strlen(m->text);
    memcpy(d.payload.bytes, m->text, len);
    d.payload.size = len;
    bool pki = p->to != BROADCAST_ADDR;
    int ch = isChannelThread(m->thread) ? threadChannel(m->thread) : 0;
    return encodeAndQueue(p->to, m->id, d, true, pki, settings.hopLimit, random(0, 4) * radio.slotTimeMs(), ch);
}

void Mesh::onKeyLearned(uint32_t node)
{
    // Wake up anything that was waiting on this key, including messages that had given up asking.
    for (int i = 0; i < messages.count(); i++) {
        Message &m = messages.at(i);
        if (m.thread != node || m.from != myNodeNum)
            continue;
        if (m.status == MsgStatus::NoKey) {
            m.status = MsgStatus::Queued;
            m.detail[0] = 0;
            addPending(&m);
        } else if (m.status == MsgStatus::WaitingForKey) {
            m.status = MsgStatus::Queued;
            m.detail[0] = 0;
            if (Pending *p = findPending(m.id))
                p->nextAt = millis();
        }
    }
    uiDirty = true;
}

void Mesh::onImplicitAck(uint32_t id)
{
    Pending *p = findPending(id);
    Message *m = messages.findById(id);
    if (!p || !m || m->status != MsgStatus::Sending)
        return;
    m->status = MsgStatus::Relayed;
    if (p->to == BROADCAST_ADDR) {
        p->used = false; // a relay hearing a channel message is the best confirmation there is
    } else {
        snprintf(m->detail, sizeof(m->detail), "waiting for recipient");
        p->nextAt = millis() + RELAYED_ACK_WAIT_MS;
    }
    messages.saveSoon();
    uiDirty = true;
}

// ---------------------------------------------------------------------------------------------------------
// Receive path

Mesh::Seen *Mesh::findSeen(uint32_t from, uint32_t id)
{
    for (auto &s : seen)
        if (s.id == id && s.from == from && s.at && millis() - s.at < 10 * 60 * 1000)
            return &s;
    return nullptr;
}

void Mesh::maybeRelay(const RxFrame &f, const Header &h)
{
    uint8_t hopLimit = h.flags & FLAG_HOP_LIMIT_MASK;
    uint8_t me = myNodeNum & 0xff;
    if (!settings.relay || h.to == myNodeNum || hopLimit == 0)
        return;
    if (h.nextHop != 0 && h.nextHop != me)
        return; // the sender picked a different relay

    uint8_t copy[MAX_FRAME];
    memcpy(copy, f.data, f.len);
    Header *c = (Header *)copy;
    c->flags = (h.flags & ~FLAG_HOP_LIMIT_MASK) | (hopLimit - 1);
    c->nextHop = 0;
    c->relayNode = me;

    // SNR-weighted contention window, as the firmware does for CLIENT: distant (low SNR) nodes relay first.
    float snr = f.snr < -20 ? -20 : (f.snr > 10 ? 10 : f.snr);
    int cw = 3 + (int)((snr + 20) * 5 / 30);
    uint32_t delay = (2 * 8 + random(0, 1 << cw)) * radio.slotTimeMs();
    radio.enqueue(copy, f.len, delay, h.from, h.id);
}

void Mesh::onFrame(const RxFrame &f)
{
    if (f.len < HEADER_LEN)
        return;
    Header h;
    memcpy(&h, f.data, HEADER_LEN);
    lastRxAt = millis();

    if (h.from == myNodeNum) {
        onImplicitAck(h.id); // a neighbour rebroadcast something we sent
        return;
    }

    uint8_t hopLimit = h.flags & FLAG_HOP_LIMIT_MASK;
    uint8_t hopStart = h.flags >> FLAG_HOP_START_SHIFT;
    bool wantAck = h.flags & FLAG_WANT_ACK;
    int hops = (hopStart && hopStart >= hopLimit) ? hopStart - hopLimit : -1;

    if (logPackets)
        Serial.printf("rx !%08x -> %s%08x id=%08x hops=%d snr=%d%s", (unsigned)h.from, h.to == BROADCAST_ADDR ? "all " : "!",
                      (unsigned)h.to, (unsigned)h.id, hops, (int)f.snr, findSeen(h.from, h.id) ? " (dup)\n" : "");

    if (Seen *s = findSeen(h.from, h.id)) {
        radio.cancel(h.from, h.id); // someone else already relayed it
        if (s->decodedForUs && wantAck)
            sendRouting(h.from, h.id, meshtastic_Routing_Error_NONE, responseHopLimit(hops), s->ch); // our ACK got lost
        return;
    }
    Seen &seenEntry = seen[seenNext];
    seenNext = (seenNext + 1) % SEEN_SIZE;
    seenEntry = {h.from, h.id, millis() ? millis() : 1, false};

    maybeRelay(f, h);

    const uint8_t *payload = f.data + HEADER_LEN;
    size_t plen = f.len - HEADER_LEN;
    uint8_t buf[MAX_FRAME];
    meshtastic_Data d;
    bool decoded = false, viaPki = false;
    int ch = 0; // channel index the packet decoded on (PKI DMs count as the primary, as in the firmware)

    if (h.channel == 0 && h.to == myNodeNum && plen > PKI_OVERHEAD) {
        Node *n = nodeDB.get(h.from);
        if (n && n->hasKey && pkiDecrypt(settings.privateKey, n->publicKey, h.from, h.id, payload, plen, buf))
            decoded = viaPki = decodeData(buf, plen - PKI_OVERHEAD, d);
    }
    // Several channels can share a hash; try each match until one decodes.
    for (int i = 0; !decoded && i < MAX_CHANNELS; i++) {
        if (!channelUsed(i) || h.channel != chanHash[i])
            continue;
        memcpy(buf, payload, plen);
        channelCrypt(chanKey[i], h.from, h.id, buf, plen);
        if (decodeData(buf, plen, d)) {
            decoded = true;
            ch = i;
        }
    }

    if (logPackets) {
        if (decoded)
            Serial.printf(" %s port=%d\n", viaPki ? "pki" : channelName(ch), (int)d.portnum);
        else
            Serial.printf(" undecoded (channel hash 0x%02x)\n", h.channel);
    }

    if (!decoded) {
        if (h.to == myNodeNum && wantAck && h.channel == 0) {
            // A DM we can't read because we lack the sender's key: tell them, which makes stock firmware send
            // us its NodeInfo, and ask for it ourselves too.
            sendRouting(h.from, h.id, meshtastic_Routing_Error_PKI_UNKNOWN_PUBKEY, responseHopLimit(hops), 0);
            requestKey(h.from);
        }
        if (Node *n = nodeDB.get(h.from)) {
            n->lastHeard = millis();
            n->hopsAway = hops;
        }
        return;
    }

    Node *node = nodeDB.getOrCreate(h.from);
    node->lastHeard = millis();
    node->hopsAway = hops;
    if (hops == 0)
        node->snr = (int8_t)f.snr;
    if (!viaPki)
        node->channel = ch;
    uiDirty = true;

    // Like stock firmware, ask a node we hear but know nothing about for its NodeInfo. Distant nodes only
    // broadcast it every few hours, so otherwise they sit in the list as a bare ID with no key for a long time.
    if (!node->hasUser && !node->infoAsked && d.portnum != meshtastic_PortNum_NODEINFO_APP && radio.ready() &&
        (int32_t)(millis() - nextUnknownAskAt) >= 0) {
        node->infoAsked = true;
        nextUnknownAskAt = millis() + UNKNOWN_ASK_SPACING_MS;
        sendNodeInfo(h.from, true, channelUsed(node->channel) ? node->channel : 0);
    }

    if (h.to == myNodeNum) {
        seenEntry.decodedForUs = true;
        seenEntry.ch = ch;
    }

    switch (d.portnum) {
    case meshtastic_PortNum_TEXT_MESSAGE_APP:
        handleText(h, d, ch);
        break;
    case meshtastic_PortNum_NODEINFO_APP:
        handleNodeInfo(h, d, ch);
        break;
    case meshtastic_PortNum_ROUTING_APP:
        handleRouting(h, d);
        break;
    case meshtastic_PortNum_POSITION_APP:
        handlePosition(h, d, ch);
        break;
    default:
        break;
    }

    if (h.to == myNodeNum && wantAck && d.portnum != meshtastic_PortNum_ROUTING_APP && d.request_id == 0)
        sendRouting(h.from, h.id, meshtastic_Routing_Error_NONE, responseHopLimit(hops), ch);
}

void Mesh::handleText(const Header &h, const meshtastic_Data &d, int ch)
{
    uint32_t thread;
    if (h.to == myNodeNum)
        thread = h.from;
    else if (h.to == BROADCAST_ADDR)
        thread = channelThread(ch);
    else
        return; // a legacy DM between two other nodes

    char text[sizeof(Message::text)];
    size_t len = d.payload.size < sizeof(text) - 1 ? d.payload.size : sizeof(text) - 1;
    memcpy(text, d.payload.bytes, len);
    text[len] = 0;
    messages.add(thread, h.from, 0, text, MsgStatus::Received);

    if (isChannelThread(thread))
        channelUnread[ch]++;
    else if (Node *n = nodeDB.get(h.from))
        n->unread++;
    if (onIncomingText)
        onIncomingText(thread);
}

void Mesh::handleNodeInfo(const Header &h, const meshtastic_Data &d, int ch)
{
    meshtastic_User u = meshtastic_User_init_zero;
    pb_istream_t s = pb_istream_from_buffer(d.payload.bytes, d.payload.size);
    if (!pb_decode(&s, meshtastic_User_fields, &u))
        return;

    Node *n = nodeDB.getOrCreate(h.from);
    strlcpy(n->longName, u.long_name, sizeof(n->longName));
    strlcpy(n->shortName, u.short_name, sizeof(n->shortName));
    n->hwModel = (uint8_t)u.hw_model;
    n->role = (uint8_t)u.role;
    n->hasUser = true;
    nodeDB.saveSoon();

    if (u.public_key.size == 32 && nodeDB.setKey(h.from, u.public_key.bytes))
        onKeyLearned(h.from);

    if (d.want_response && (h.to == myNodeNum || h.to == BROADCAST_ADDR))
        sendNodeInfo(h.from, false, ch);
}

void Mesh::handleRouting(const Header &h, const meshtastic_Data &d)
{
    if (h.to != myNodeNum || d.request_id == 0)
        return;
    meshtastic_Routing r = meshtastic_Routing_init_zero;
    pb_istream_t s = pb_istream_from_buffer(d.payload.bytes, d.payload.size);
    if (!pb_decode(&s, meshtastic_Routing_fields, &r) || r.which_variant != meshtastic_Routing_error_reason_tag)
        return;

    Pending *p = findPending(d.request_id);
    Message *m = messages.findById(d.request_id);
    if (!p || !m)
        return;

    switch (r.error_reason) {
    case meshtastic_Routing_Error_NONE:
        if (p->to == BROADCAST_ADDR || h.from == p->to) {
            m->status = p->to == BROADCAST_ADDR ? MsgStatus::Relayed : MsgStatus::Delivered;
            m->detail[0] = 0;
            p->used = false;
        }
        break;

    case meshtastic_Routing_Error_PKI_UNKNOWN_PUBKEY:
    case meshtastic_Routing_Error_PKI_FAILED:
    case meshtastic_Routing_Error_NO_CHANNEL:
        // The recipient couldn't decrypt: it doesn't have our key, or one of us has a stale key for the other.
        // Swap keys both ways, then resend as a fresh packet.
        if (!p->keyRetryDone) {
            p->keyRetryDone = true;
            requestKey(p->to);
            m->id = newPacketId();
            p->msgId = m->id;
            p->tries = 0;
            m->status = MsgStatus::Queued;
            snprintf(m->detail, sizeof(m->detail), "exchanged keys, resending");
            p->nextAt = millis() + 8000;
        } else {
            m->status = MsgStatus::Failed;
            snprintf(m->detail, sizeof(m->detail), "recipient can't decrypt (%s)", routingErrorName(r.error_reason));
            p->used = false;
        }
        break;

    default:
        m->status = MsgStatus::Failed;
        snprintf(m->detail, sizeof(m->detail), "%s", routingErrorName(r.error_reason));
        p->used = false;
        break;
    }
    messages.saveSoon();
    uiDirty = true;
}

// ---------------------------------------------------------------------------------------------------------

void Mesh::tick()
{
    uint32_t now = millis();

    // Announce ourselves (name + public key) on every channel, so members of a private channel who don't share
    // our primary can still DM us.
    if (radio.ready() && (int32_t)(now - nextNodeInfoBroadcast) >= 0) {
        for (int i = 0; i < MAX_CHANNELS; i++)
            if (channelUsed(i))
                sendNodeInfo(BROADCAST_ADDR, false, i);
        nextNodeInfoBroadcast = now + NODEINFO_BROADCAST_MS;
    }

    // Position broadcasts, on the channels that share it: soon after the first fix, then every interval.
    if (gpsState.hasFix && (int32_t)(now - nextPositionAt) >= 0) {
        bool sent = false;
        for (int i = 0; i < MAX_CHANNELS; i++)
            if (channelSharesPosition(i))
                sent |= sendPosition(BROADCAST_ADDR, false, i);
        if (sent)
            nextPositionAt = now + (uint32_t)settings.positionIntervalMin * 60000UL;
    }

    for (auto &p : pend) {
        if (!p.used)
            continue;
        Message *m = messages.findById(p.msgId);
        if (!m) {
            p.used = false;
            continue;
        }
        if ((int32_t)(now - p.nextAt) < 0)
            continue;

        switch (m->status) {
        case MsgStatus::WaitingForKey: {
            Node *n = nodeDB.get(p.to);
            if (n && n->hasKey) {
                m->status = MsgStatus::Queued;
                m->detail[0] = 0;
                break;
            }
            if (p.keyRequests < MAX_KEY_REQUESTS) {
                p.keyRequests++;
                requestKey(p.to);
                snprintf(m->detail, sizeof(m->detail), "asked for key (%d/%d)", p.keyRequests, MAX_KEY_REQUESTS);
                p.nextAt = now + KEY_REQUEST_INTERVAL_MS;
            } else {
                // Stop asking, but keep the message: it goes out automatically if the key ever shows up
                // (their periodic NodeInfo broadcast), or the user can press R to ask again.
                m->status = MsgStatus::NoKey;
                snprintf(m->detail, sizeof(m->detail), "no reply; auto-sends when key arrives");
                p.used = false;
                messages.saveSoon();
            }
            uiDirty = true;
            break;
        }

        case MsgStatus::Queued:
            if (transmitMessage(m, &p)) {
                p.tries++;
                m->status = MsgStatus::Sending;
                if (p.tries > 1)
                    snprintf(m->detail, sizeof(m->detail), "try %d/%d", p.tries, MAX_TRIES);
                p.nextAt = now + retransmitTimeout();
            } else {
                m->status = MsgStatus::Failed;
                snprintf(m->detail, sizeof(m->detail), radio.ready() ? "couldn't encode/queue" : "radio off");
                p.used = false;
            }
            uiDirty = true;
            break;

        case MsgStatus::Sending:
            if (p.tries < MAX_TRIES) {
                m->status = MsgStatus::Queued; // retransmit on the next pass, same packet id
                p.nextAt = now;
            } else {
                m->status = MsgStatus::Failed;
                snprintf(m->detail, sizeof(m->detail),
                         p.to == BROADCAST_ADDR ? "nobody heard it" : "no ACK - out of range?");
                p.used = false;
                messages.saveSoon();
            }
            uiDirty = true;
            break;

        case MsgStatus::Relayed:
            // Heard a relay but never an end-to-end ACK. Not proof of failure; leave it marked relayed.
            snprintf(m->detail, sizeof(m->detail), "recipient hasn't confirmed");
            p.used = false;
            messages.saveSoon();
            uiDirty = true;
            break;

        default:
            p.used = false;
            break;
        }
    }
}

const char *routingErrorName(meshtastic_Routing_Error e)
{
    switch (e) {
    case meshtastic_Routing_Error_NONE: return "OK";
    case meshtastic_Routing_Error_NO_ROUTE: return "no route";
    case meshtastic_Routing_Error_GOT_NAK: return "NAK";
    case meshtastic_Routing_Error_TIMEOUT: return "timeout";
    case meshtastic_Routing_Error_NO_INTERFACE: return "no interface";
    case meshtastic_Routing_Error_MAX_RETRANSMIT: return "max retransmit";
    case meshtastic_Routing_Error_NO_CHANNEL: return "no channel";
    case meshtastic_Routing_Error_TOO_LARGE: return "too large";
    case meshtastic_Routing_Error_NO_RESPONSE: return "no response";
    case meshtastic_Routing_Error_DUTY_CYCLE_LIMIT: return "duty cycle limit";
    case meshtastic_Routing_Error_BAD_REQUEST: return "bad request";
    case meshtastic_Routing_Error_NOT_AUTHORIZED: return "not authorized";
    case meshtastic_Routing_Error_PKI_FAILED: return "PKI failed";
    case meshtastic_Routing_Error_PKI_UNKNOWN_PUBKEY: return "they lack our key";
    case meshtastic_Routing_Error_RATE_LIMIT_EXCEEDED: return "rate limited";
    default: return "error";
    }
}
