#include "nodedb.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <algorithm>

NodeDB nodeDB;

static const char *NODES_FILE = "/nodes.bin";
static constexpr uint32_t NODES_VERSION = 2;
static constexpr size_t NODE_V1_SIZE = offsetof(Node, latitudeI); // v1 records lack the position fields

void NodeDB::load()
{
    n = 0;
    File f = LittleFS.open(NODES_FILE, "r");
    if (!f)
        return;
    uint32_t ver = 0, count = 0;
    if (f.read((uint8_t *)&ver, 4) == 4 && (ver == 1 || ver == NODES_VERSION) && f.read((uint8_t *)&count, 4) == 4) {
        size_t recSize = ver == 1 ? NODE_V1_SIZE : sizeof(Node);
        count = std::min<uint32_t>(count, MAX_NODES);
        for (uint32_t i = 0; i < count; i++) {
            memset(&nodes[n], 0, sizeof(Node));
            if (f.read((uint8_t *)&nodes[n], recSize) != recSize)
                break;
            nodes[n].lastHeard = 0; // millis() from a previous boot is meaningless
            nodes[n].hopsAway = -1;
            nodes[n].unread = 0;
            nodes[n].infoAsked = false;
            n++;
        }
    }
    f.close();
}

void NodeDB::save()
{
    saveAt = 0;
    File f = LittleFS.open(NODES_FILE, "w");
    if (!f)
        return;
    uint32_t ver = NODES_VERSION, count = n;
    f.write((uint8_t *)&ver, 4);
    f.write((uint8_t *)&count, 4);
    f.write((uint8_t *)nodes, sizeof(Node) * n);
    f.close();
}

void NodeDB::saveSoon()
{
    if (!saveAt)
        saveAt = millis() + 30000;
}

void NodeDB::tick()
{
    if (saveAt && (int32_t)(millis() - saveAt) >= 0)
        save();
}

Node *NodeDB::get(uint32_t num)
{
    for (int i = 0; i < n; i++)
        if (nodes[i].num == num)
            return &nodes[i];
    return nullptr;
}

Node *NodeDB::evictOne()
{
    // Prefer dropping nodes we know least about: no key, not heard this boot, oldest.
    int victim = -1;
    for (int i = 0; i < n; i++) {
        const Node &c = nodes[i];
        if (c.unread)
            continue;
        if (victim < 0) {
            victim = i;
            continue;
        }
        const Node &v = nodes[victim];
        int cs = (c.hasKey ? 2 : 0) + (c.lastHeard ? 1 : 0);
        int vs = (v.hasKey ? 2 : 0) + (v.lastHeard ? 1 : 0);
        if (cs < vs || (cs == vs && c.lastHeard < v.lastHeard))
            victim = i;
    }
    if (victim < 0)
        victim = 0;
    return &nodes[victim];
}

Node *NodeDB::getOrCreate(uint32_t num)
{
    Node *e = get(num);
    if (e)
        return e;
    e = (n < MAX_NODES) ? &nodes[n++] : evictOne();
    memset(e, 0, sizeof(*e));
    e->num = num;
    e->hopsAway = -1;
    snprintf(e->shortName, sizeof(e->shortName), "%04x", (unsigned)(num & 0xffff));
    saveSoon();
    return e;
}

bool NodeDB::setKey(uint32_t num, const uint8_t key[32])
{
    Node *e = getOrCreate(num);
    if (e->hasKey && memcmp(e->publicKey, key, 32) == 0)
        return false;
    memcpy(e->publicKey, key, 32);
    e->hasKey = true;
    save(); // keys are precious; write immediately
    return true;
}

int NodeDB::sorted(Node **out, int max)
{
    int m = std::min(n, max);
    Node *tmp[MAX_NODES];
    for (int i = 0; i < n; i++)
        tmp[i] = &nodes[i];
    std::sort(tmp, tmp + n, [](const Node *a, const Node *b) {
        if ((a->unread > 0) != (b->unread > 0))
            return a->unread > 0;
        if ((a->lastHeard != 0) != (b->lastHeard != 0))
            return a->lastHeard != 0;
        if (a->lastHeard != b->lastHeard)
            return a->lastHeard > b->lastHeard;
        return strcasecmp(a->longName[0] ? a->longName : a->shortName, b->longName[0] ? b->longName : b->shortName) < 0;
    });
    memcpy(out, tmp, sizeof(Node *) * m);
    return m;
}

const char *NodeDB::displayName(uint32_t num)
{
    static char buf[12];
    Node *e = get(num);
    if (e && e->longName[0])
        return e->longName;
    snprintf(buf, sizeof(buf), "!%08x", (unsigned)num);
    return buf;
}

// nodes.bin records are read by size; v2 files must stay readable.
static_assert(offsetof(Node, latitudeI) == 80, "v1 node layout changed");
static_assert(sizeof(Node) == 92, "v2 node layout changed");
