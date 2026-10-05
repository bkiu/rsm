#!/usr/bin/env bash
# Builds and runs the host protocol tests. Needs the PlatformIO libdeps (run `pio run` once) and mbedtls.
# On NixOS: nix-shell -p gcc mbedtls --run test/host/run.sh
set -euo pipefail
cd "$(dirname "$0")"
ROOT=$(cd ../.. && pwd)
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
# Copy the Crypto library so our stub RNG.h shadows the Arduino-dependent one.
cp -r "$ROOT"/.pio/libdeps/cardputer-adv/Crypto/. "$OUT"/
cp stubs/RNG.h "$OUT"/RNG.h
C=$OUT
g++ -std=c++17 -O1 -DTEST_CURVE25519_FIELD_OPS -Istubs -I"$C" -I"$ROOT"/lib/meshtastic_protobufs/src \
    -I"$ROOT"/.pio/libdeps/cardputer-adv/Nanopb \
    test_protocol.cpp "$ROOT"/src/crypto.cpp "$ROOT"/src/regions.cpp \
    $C/AES128.cpp $C/AES256.cpp $C/AESCommon.cpp $C/BlockCipher.cpp $C/Cipher.cpp $C/CTR.cpp $C/Curve25519.cpp \
    $C/BigNumberUtil.cpp $C/Crypto.cpp $C/Hash.cpp $C/SHA256.cpp -lmbedcrypto -o "$OUT"/test
"$OUT"/test
