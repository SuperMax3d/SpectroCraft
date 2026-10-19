// SpectroCraft — self-contained SHA-256 (FIPS 180-4).
//
// This vendored JUCE build does not ship juce::MessageDigest, so the model
// manager verifies downloaded weights with this small standalone implementation.
// The algorithm was cross-checked against OpenSSL's reference table and a
// hashlib-based Python oracle for empty input, "abc", 55/56/63/64-byte inputs
// (padding boundaries) and multi-block data.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace sc { namespace ai {

class Sha256 {
public:
    Sha256();

    void update(const uint8_t* data, size_t len);

    // 64 lowercase hex chars; resets the hasher to its initial state.
    std::string hexDigest();

private:
    void processBlock(const uint32_t block[16]);

    uint32_t _state[8];
    uint8_t _buffer[64];
    size_t _bufferLen = 0;
    uint64_t _totalBytes = 0;
};

// One-shot helper.
std::string sha256Hex(const void* data, size_t len);

}} // namespace sc::ai
