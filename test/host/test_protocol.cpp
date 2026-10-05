// Host-side checks that our crypto and frequency math match the official Meshtastic firmware.
// Vectors come from meshtastic/firmware test/test_crypto (a captured PKI direct message).
#include <stdio.h>
#include <string.h>
#include <string>

#include "../../src/crypto.h"
#include "../../src/regions.h"
#include "../../src/settings.h"
#include "RNG.h"

RNGClass RNG;
Settings settings;
uint32_t myNodeNum;

static int failures = 0;
#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                    \
            failures++;                                                                                                \
        } else {                                                                                                       \
            printf("ok   %s\n", #cond);                                                                                \
        }                                                                                                              \
    } while (0)

static void hex(uint8_t *out, const std::string &h)
{
    for (size_t i = 0; i < h.size() / 2; i++)
        out[i] = (uint8_t)strtoul(h.substr(i * 2, 2).c_str(), nullptr, 16);
}

int main()
{
    // Default channel: "LongFast" with PSK index 1 hashes to 8 (well known value).
    uint8_t psk1 = 1;
    ChannelKey k = expandPsk(&psk1, 1);
    CHECK(k.length == 16);
    CHECK(channelHash("LongFast", k) == 8);

    // US LongFast lands on frequency slot 20 = 906.875 MHz.
    memset(&settings, 0, sizeof(settings));
    strcpy(settings.region, "US");
    settings.preset = 0; // LONG_FAST
    settings.txPower = 30;
    RadioParams rp;
    CHECK(computeRadioParams(&rp));
    CHECK(rp.slot == 20);
    CHECK(rp.freqMHz > 906.874f && rp.freqMHz < 906.876f);
    CHECK(rp.power == 22);
    // EU_868 LongFast: a single slot at 869.525 MHz
    strcpy(settings.region, "EU_868");
    CHECK(computeRadioParams(&rp));
    CHECK(rp.freqMHz > 869.524f && rp.freqMHz < 869.526f);

    // Captured PKI DM: decrypt with the recipient's private key and the sender's public key.
    uint8_t myPriv[32], theirPub[32], radioBytes[64], out[64], expected[10];
    hex(theirPub, "db18fc50eea47f00251cb784819a3cf5fc361882597f589f0d7ff820e8064457");
    hex(myPriv, "a00330633e63522f8a4d81ec6d9d1e6617f6c8ffd3a4c698229537d44e522277");
    hex(radioBytes, "8c646d7a2909000062d6b2136b00000040df24abfcc30a17a3d9046726099e796a1c036a792b");
    hex(expected, "08011204746573744800");
    uint32_t from = 0x0929, id = 0x13b2d662;
    CHECK(pkiDecrypt(myPriv, theirPub, from, id, radioBytes + 16, 22, out));
    CHECK(memcmp(out, expected, 10) == 0);

    // Header layout: to, from, id, flags, channel hash (0 = PKI), next hop, relay node.
    uint32_t hdrFrom, hdrId;
    memcpy(&hdrFrom, radioBytes + 4, 4);
    memcpy(&hdrId, radioBytes + 8, 4);
    CHECK(hdrFrom == from && hdrId == id && radioBytes[13] == 0);

    // Tampered ciphertext must fail authentication.
    radioBytes[20] ^= 1;
    CHECK(!pkiDecrypt(myPriv, theirPub, from, id, radioBytes + 16, 22, out));
    radioBytes[20] ^= 1;

    // Round trip through our own encrypt.
    uint8_t enc[64], dec[64];
    CHECK(pkiEncrypt(myPriv, theirPub, from, id, expected, 10, enc));
    CHECK(pkiDecrypt(myPriv, theirPub, from, id, enc, 22, dec));
    CHECK(memcmp(dec, expected, 10) == 0);

    // Key pair generation yields a public key that agrees with DH from the other side.
    uint8_t aPriv[32], aPub[32], bPriv[32], bPub[32];
    cryptoGenerateKeyPair(aPriv, aPub);
    cryptoGenerateKeyPair(bPriv, bPub);
    CHECK(pkiEncrypt(aPriv, bPub, 1, 2, expected, 10, enc));
    CHECK(pkiDecrypt(bPriv, aPub, 1, 2, enc, 22, dec));
    CHECK(memcmp(dec, expected, 10) == 0);

    // Channel CTR is its own inverse.
    uint8_t buf[20] = "hello mesh world!!!";
    channelCrypt(k, 0x1234, 0x5678, buf, 19);
    CHECK(memcmp(buf, "hello mesh world!!!", 19) != 0);
    channelCrypt(k, 0x1234, 0x5678, buf, 19);
    CHECK(memcmp(buf, "hello mesh world!!!", 19) == 0);

    printf(failures ? "\n%d FAILURE(S)\n" : "\nall passed\n", failures);
    return failures != 0;
}
