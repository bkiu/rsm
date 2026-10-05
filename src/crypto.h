// Meshtastic packet encryption, byte-compatible with the official firmware (src/mesh/CryptoEngine.cpp):
//  - channel traffic: AES-CTR keyed by the channel PSK
//  - direct messages: X25519 shared secret -> SHA-256 -> AES-256-CCM (8-byte tag + 4-byte extra nonce)
#pragma once
#include <stddef.h>
#include <stdint.h>

static constexpr size_t PKI_OVERHEAD = 12; // 8-byte CCM tag + 4-byte extra nonce

struct ChannelKey {
    uint8_t bytes[32];
    int8_t length; // 0 = no encryption, 16 or 32 = AES key
};

// Expand a configured PSK the way the firmware does (1-byte PSKs select a variant of the well-known key).
ChannelKey expandPsk(const uint8_t *psk, size_t pskLen);
uint8_t channelHash(const char *name, const ChannelKey &key);

// Symmetric; encrypts or decrypts in place.
void channelCrypt(const ChannelKey &key, uint32_t fromNode, uint32_t packetId, uint8_t *buf, size_t len);

void cryptoGenerateKeyPair(uint8_t priv[32], uint8_t pub[32]);
// Derives the public key for a (restored) private key. False if the private key is unusable.
bool cryptoPublicKey(uint8_t priv[32], uint8_t pub[32]);

// out must have room for len + PKI_OVERHEAD bytes. Returns false on a weak/invalid public key.
bool pkiEncrypt(const uint8_t myPriv[32], const uint8_t theirPub[32], uint32_t fromNode, uint32_t packetId,
                const uint8_t *in, size_t len, uint8_t *out);
// len includes PKI_OVERHEAD; plaintext is len - PKI_OVERHEAD bytes. False if authentication fails.
bool pkiDecrypt(const uint8_t myPriv[32], const uint8_t theirPub[32], uint32_t fromNode, uint32_t packetId,
                const uint8_t *in, size_t len, uint8_t *out);
