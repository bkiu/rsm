// Known nodes, including their public keys. Persisted to LittleFS so keys survive reboots — losing them is
// the main reason direct messages stop working after a restart.
#pragma once
#include <stddef.h>
#include <stdint.h>

struct Node {
    uint32_t num;
    char longName[25];
    char shortName[5];
    uint8_t publicKey[32];
    bool hasKey;
    bool hasUser;     // we've received their NodeInfo
    uint8_t hwModel;
    uint8_t role;
    int8_t hopsAway;  // -1 = unknown
    int8_t snr;       // last SNR of a direct (0-hop) packet, dB
    uint32_t lastHeard; // millis(); 0 = not heard since boot
    uint32_t unread;
    // --- added in nodes.bin v2 ---
    int32_t latitudeI, longitudeI; // 1e-7 degrees
    bool hasPosition;
    uint8_t channel; // channel index we last heard them on (sits in what was padding, so old files read as 0)
    bool infoAsked;  // we've asked for their NodeInfo this boot (runtime only; also in former padding)
};

class NodeDB
{
  public:
    static constexpr int MAX_NODES = 150;

    void load();
    void save();       // writes now
    void saveSoon();   // coalesces writes
    void tick();

    Node *get(uint32_t num);
    Node *getOrCreate(uint32_t num);
    int count() const { return n; }
    Node &at(int i) { return nodes[i]; }

    // Nodes ordered for display: most recently heard first (unheard since boot last, by name).
    int sorted(Node **out, int max);

    // Stores a public key. Returns true if it is new or changed.
    bool setKey(uint32_t num, const uint8_t key[32]);

    const char *displayName(uint32_t num); // long name, or "!abcd1234" when unknown

  private:
    Node nodes[MAX_NODES];
    int n = 0;
    uint32_t saveAt = 0;
    Node *evictOne();
};

extern NodeDB nodeDB;
