// SpectroCraft — self-contained SHA-256 (FIPS 180-4). See Sha256.h.

#include "AI/Sha256.h"

#include <algorithm>
#include <cstring>

namespace sc { namespace ai {

namespace {

// Round constants: fractional parts of the cube roots of the first 64 primes.
// Table identical to OpenSSL's K256 (verified against it and Wikipedia).
constexpr uint32_t kK[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
    0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

constexpr uint32_t kH0[8] = {
    0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
};

inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

} // namespace

Sha256::Sha256()
{
    std::memcpy (_state, kH0, sizeof (kH0));
}

void Sha256::processBlock(const uint32_t block[16])
{
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) w[i] = block[i];
    for (int i = 16; i < 64; ++i)
    {
        const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = _state[0], b = _state[1], c = _state[2], d = _state[3];
    uint32_t e = _state[4], f = _state[5], g = _state[6], h = _state[7];

    for (int i = 0; i < 64; ++i)
    {
        const uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        const uint32_t ch = (e & f) ^ (~e & g);
        const uint32_t t1 = h + S1 + ch + kK[i] + w[i];
        const uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    _state[0] += a; _state[1] += b; _state[2] += c; _state[3] += d;
    _state[4] += e; _state[5] += f; _state[6] += g; _state[7] += h;
}

void Sha256::update(const uint8_t* data, size_t len)
{
    _totalBytes += len;
    while (len > 0)
    {
        const size_t space = 64 - _bufferLen;
        const size_t n = std::min(space, len);
        std::memcpy(_buffer + _bufferLen, data, n);
        _bufferLen += n;
        data += n;
        len -= n;

        if (_bufferLen == 64)
        {
            uint32_t block[16];
            for (int i = 0; i < 16; ++i)
                block[i] = ((uint32_t) _buffer[4 * i] << 24) | ((uint32_t) _buffer[4 * i + 1] << 16)
                         | ((uint32_t) _buffer[4 * i + 2] << 8) | (uint32_t) _buffer[4 * i + 3];
            processBlock(block);
            _bufferLen = 0;
        }
    }
}

std::string Sha256::hexDigest()
{
    const uint64_t bitLen = _totalBytes * 8;

    // Padding: 0x80, zeros until length ≡ 56 (mod 64), then 64-bit big-endian length.
    _buffer[_bufferLen++] = 0x80;
    if (_bufferLen > 56)
    {
        while (_bufferLen < 64) _buffer[_bufferLen++] = 0;
        uint32_t block[16];
        for (int i = 0; i < 16; ++i)
            block[i] = ((uint32_t) _buffer[4 * i] << 24) | ((uint32_t) _buffer[4 * i + 1] << 16)
                     | ((uint32_t) _buffer[4 * i + 2] << 8) | (uint32_t) _buffer[4 * i + 3];
        processBlock(block);
        _bufferLen = 0;
    }
    while (_bufferLen < 56) _buffer[_bufferLen++] = 0;
    // 64-bit big-endian bit length in the final eight bytes.
    for (int i = 0; i < 8; ++i)
        _buffer[56 + (size_t) i] = (uint8_t) ((bitLen >> (56 - 8 * i)) & 0xFF);

    uint32_t block[16];
    for (int i = 0; i < 16; ++i)
        block[i] = ((uint32_t) _buffer[4 * i] << 24) | ((uint32_t) _buffer[4 * i + 1] << 16)
                 | ((uint32_t) _buffer[4 * i + 2] << 8) | (uint32_t) _buffer[4 * i + 3];
    processBlock(block);

    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (int v = 0; v < 8; ++v)
        for (int shift = 28; shift >= 0; shift -= 4)
            out.push_back(hex[(_state[v] >> shift) & 0xF]);

    // Reset for reuse.
    std::memcpy(_state, kH0, sizeof(kH0));
    _bufferLen = 0;
    _totalBytes = 0;
    return out;
}

std::string sha256Hex(const void* data, size_t len)
{
    Sha256 h;
    if (len > 0) h.update((const uint8_t*) data, len);
    return h.hexDigest();
}

}} // namespace sc::ai
