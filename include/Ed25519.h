// SpectroCraft — self-contained SHA-512 + Ed25519 (RFC 8032).
//
// Plain C++20: no JUCE, no external crypto library. Same style as the vendored
// SHA-256 in include/AI/Sha256.h (this JUCE build ships neither MessageDigest
// nor a signature primitive). Both halves of the update flow need them:
//
//   • the app verifies the signature of the downloaded update-manifest.json
//     against the public key embedded in the binary (sc::update, docs/updates.md);
//   • spectrocraft-updater --sign produces that signature when a release is
//     built (tools/make_update_manifest.py calls it).
//
// Correctness: the field/group code was written against RFC 8032 §5 and checked
// against the RFC 8032 §7.1 Ed25519 test vectors (public key, signature,
// verify, tampered-message rejection) and against signatures made by OpenSSL
// 3.2 — see tests/regression/updates.inc.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace sc { namespace crypto {

// ---------------------------------------------------------------- SHA-512
// FIPS 180-4. Streaming interface; hexDigest() returns 128 lowercase hex chars
// and resets the hasher.
class Sha512 {
public:
    Sha512();

    void update (const uint8_t* data, size_t len);

    // 64 raw bytes; resets the hasher to its initial state.
    void rawDigest (uint8_t out[64]);

    // 128 lowercase hex chars.
    std::string hexDigest ();

private:
    void processBlock (const uint64_t block[16]);

    uint64_t _state[8];
    uint8_t _buffer[128];
    size_t _bufferLen = 0;
    uint64_t _totalBytes = 0;
};

// One-shot helper.
std::string sha512Hex (const void* data, size_t len);

// ---------------------------------------------------------------- Ed25519
// RFC 8032 Ed25519 (pure Ed25519, no prehash, no context).

// Verify sig (64 bytes) over msg with pub (32 bytes). False on any malformed
// key/signature or failed check.
bool ed25519Verify (const uint8_t pub[32], const uint8_t* msg, size_t msgLen,
                    const uint8_t sig[64]);

// Sign msg with the 32-byte private seed; sigOut receives 64 bytes.
bool ed25519Sign (const uint8_t seed[32], const uint8_t* msg, size_t msgLen,
                  uint8_t sigOut[64]);

// Public key (32 bytes) belonging to a private seed.
bool ed25519PublicFromSeed (const uint8_t seed[32], uint8_t pubOut[32]);

// ---------------------------------------------------------------- hex helpers
// 64 hex chars (upper or lower case) -> 32 bytes. False if the string is not
// exactly 64 hex digits.
bool keyFromHex (const std::string& hex, uint8_t out[32]);

// Lowercase hex, e.g. for a .sig file.
std::string toHex (const uint8_t* data, size_t len);

// Reads a hex signature from text (accepts surrounding whitespace).
bool sigFromHex (const std::string& text, uint8_t out[64]);

}} // namespace sc::crypto
