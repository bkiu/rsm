#include "crypto.h"

#include <AES.h>
#include <CTR.h>
#include <Curve25519.h>
#include <SHA256.h>
#include <bootloader_random.h>
#include <esp_random.h>
#include <mbedtls/ccm.h>
#include <string.h>

static const uint8_t DEFAULT_PSK[16] = {0xd4, 0xf1, 0xbb, 0x3a, 0x20, 0x29, 0x07, 0x59,
                                        0xf0, 0xbc, 0xff, 0xab, 0xcf, 0x4e, 0x69, 0x01};

ChannelKey expandPsk(const uint8_t *psk, size_t pskLen)
{
    ChannelKey k;
    memset(&k, 0, sizeof(k));
    if (pskLen == 1) {
        uint8_t idx = psk[0];
        if (idx == 0)
            return k; // explicitly unencrypted
        memcpy(k.bytes, DEFAULT_PSK, 16);
        k.bytes[15] += idx - 1;
        k.length = 16;
    } else if (pskLen == 0) {
        return k;
    } else {
        memcpy(k.bytes, psk, pskLen > 32 ? 32 : pskLen);
        k.length = pskLen <= 16 ? 16 : 32; // short keys are zero padded, as the firmware does
    }
    return k;
}

uint8_t channelHash(const char *name, const ChannelKey &key)
{
    uint8_t h = 0;
    for (const char *p = name; *p; p++)
        h ^= (uint8_t)*p;
    for (int i = 0; i < key.length; i++)
        h ^= key.bytes[i];
    return h;
}

// nonce = packetId (u64 LE) | fromNode (u32 LE) | 0; the PKI path overwrites bytes 4..7 with the extra nonce.
static void initNonce(uint8_t nonce[16], uint32_t fromNode, uint32_t packetId, uint32_t extraNonce)
{
    memset(nonce, 0, 16);
    uint64_t id64 = packetId;
    memcpy(nonce, &id64, 8);
    memcpy(nonce + 8, &fromNode, 4);
    if (extraNonce)
        memcpy(nonce + 4, &extraNonce, 4);
}

void channelCrypt(const ChannelKey &key, uint32_t fromNode, uint32_t packetId, uint8_t *buf, size_t len)
{
    if (key.length == 0)
        return;
    uint8_t nonce[16];
    initNonce(nonce, fromNode, packetId, 0);
    if (key.length == 16) {
        CTR<AES128> ctr;
        ctr.setKey(key.bytes, 16);
        ctr.setIV(nonce, 16);
        ctr.setCounterSize(4);
        ctr.encrypt(buf, buf, len);
    } else {
        CTR<AES256> ctr;
        ctr.setKey(key.bytes, 32);
        ctr.setIV(nonce, 16);
        ctr.setCounterSize(4);
        ctr.encrypt(buf, buf, len);
    }
}

void cryptoGenerateKeyPair(uint8_t priv[32], uint8_t pub[32])
{
    // The RF subsystem is off at this point, so enable the SAR-ADC entropy source for true randomness.
    bootloader_random_enable();
    do {
        esp_fill_random(priv, 32);
        priv[0] &= 0xF8;
        priv[31] = (priv[31] & 0x7F) | 0x40;
        Curve25519::eval(pub, priv, nullptr);
    } while (Curve25519::isWeakPoint(pub));
    bootloader_random_disable();
}

bool cryptoPublicKey(uint8_t priv[32], uint8_t pub[32])
{
    priv[0] &= 0xF8;
    priv[31] = (priv[31] & 0x7F) | 0x40;
    Curve25519::eval(pub, priv, nullptr);
    return !Curve25519::isWeakPoint(pub);
}

static bool sharedKey(const uint8_t myPriv[32], const uint8_t theirPub[32], uint8_t out[32])
{
    uint8_t priv[32];
    memcpy(out, theirPub, 32);
    memcpy(priv, myPriv, 32); // dh2 wipes its private-key argument
    if (!Curve25519::dh2(out, priv))
        return false;
    SHA256 sha;
    sha.update(out, 32);
    sha.finalize(out, 32);
    return true;
}

bool pkiEncrypt(const uint8_t myPriv[32], const uint8_t theirPub[32], uint32_t fromNode, uint32_t packetId,
                const uint8_t *in, size_t len, uint8_t *out)
{
    uint8_t key[32];
    if (!sharedKey(myPriv, theirPub, key))
        return false;
    uint32_t extraNonce = esp_random();
    if (extraNonce == 0)
        extraNonce = 1;
    uint8_t nonce[16];
    initNonce(nonce, fromNode, packetId, extraNonce);

    mbedtls_ccm_context ctx;
    mbedtls_ccm_init(&ctx);
    bool ok = mbedtls_ccm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, 256) == 0 &&
              mbedtls_ccm_encrypt_and_tag(&ctx, len, nonce, 13, nullptr, 0, in, out, out + len, 8) == 0;
    mbedtls_ccm_free(&ctx);
    memcpy(out + len + 8, &extraNonce, 4);
    memset(key, 0, sizeof(key));
    return ok;
}

bool pkiDecrypt(const uint8_t myPriv[32], const uint8_t theirPub[32], uint32_t fromNode, uint32_t packetId,
                const uint8_t *in, size_t len, uint8_t *out)
{
    if (len <= PKI_OVERHEAD)
        return false;
    size_t plainLen = len - PKI_OVERHEAD;
    uint32_t extraNonce;
    memcpy(&extraNonce, in + plainLen + 8, 4);
    uint8_t key[32];
    if (!sharedKey(myPriv, theirPub, key))
        return false;
    uint8_t nonce[16];
    initNonce(nonce, fromNode, packetId, extraNonce);

    mbedtls_ccm_context ctx;
    mbedtls_ccm_init(&ctx);
    bool ok = mbedtls_ccm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, 256) == 0 &&
              mbedtls_ccm_auth_decrypt(&ctx, plainLen, nonce, 13, nullptr, 0, in, out, in + plainLen, 8) == 0;
    mbedtls_ccm_free(&ctx);
    memset(key, 0, sizeof(key));
    return ok;
}
