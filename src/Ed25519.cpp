// SpectroCraft — self-contained SHA-512 + Ed25519. See include/Ed25519.h.
//
// Field arithmetic: elements of GF(2^255-19) are four 64-bit limbs
// (little-endian). Multiplication is schoolbook with 64x64 -> 128 products
// built from 32-bit halves (MSVC has no standard __int128), and reduction uses
// 2^255 ≡ 19, i.e. 2^256 ≡ 38: fold the high half once, fold the remainder,
// then subtract p while the value is >= p. Every operation returns [0, p).
//
// Group arithmetic: extended twisted-Edwards coordinates (X:Y:Z:T) with
// x = X/Z, y = Y/Z, T = xyZ, exactly the point_add of RFC 8032 Appendix A.
// Doubling is point_add(P, P): the Edwards law is unified, so there is no
// separate doubling formula to get wrong (and one less thing to test).

#include "Ed25519.h"

#include <algorithm>
#include <cstring>

namespace sc { namespace crypto {

// ==========================================================================
// SHA-512 (FIPS 180-4)
// ==========================================================================
namespace {

// Round constants: fractional parts of the cube roots of the first 80 primes
// (generated with Python's decimal module and checked against the published
// table: K[0] = 0x428a2f98d728ae22).
constexpr uint64_t kK512[80] = {
    0x428A2F98D728AE22ull, 0x7137449123EF65CDull, 0xB5C0FBCFEC4D3B2Full, 0xE9B5DBA58189DBBCull,
    0x3956C25BF348B538ull, 0x59F111F1B605D019ull, 0x923F82A4AF194F9Bull, 0xAB1C5ED5DA6D8118ull,
    0xD807AA98A3030242ull, 0x12835B0145706FBEull, 0x243185BE4EE4B28Cull, 0x550C7DC3D5FFB4E2ull,
    0x72BE5D74F27B896Full, 0x80DEB1FE3B1696B1ull, 0x9BDC06A725C71235ull, 0xC19BF174CF692694ull,
    0xE49B69C19EF14AD2ull, 0xEFBE4786384F25E3ull, 0x0FC19DC68B8CD5B5ull, 0x240CA1CC77AC9C65ull,
    0x2DE92C6F592B0275ull, 0x4A7484AA6EA6E483ull, 0x5CB0A9DCBD41FBD4ull, 0x76F988DA831153B5ull,
    0x983E5152EE66DFABull, 0xA831C66D2DB43210ull, 0xB00327C898FB213Full, 0xBF597FC7BEEF0EE4ull,
    0xC6E00BF33DA88FC2ull, 0xD5A79147930AA725ull, 0x06CA6351E003826Full, 0x142929670A0E6E70ull,
    0x27B70A8546D22FFCull, 0x2E1B21385C26C926ull, 0x4D2C6DFC5AC42AEDull, 0x53380D139D95B3DFull,
    0x650A73548BAF63DEull, 0x766A0ABB3C77B2A8ull, 0x81C2C92E47EDAEE6ull, 0x92722C851482353Bull,
    0xA2BFE8A14CF10364ull, 0xA81A664BBC423001ull, 0xC24B8B70D0F89791ull, 0xC76C51A30654BE30ull,
    0xD192E819D6EF5218ull, 0xD69906245565A910ull, 0xF40E35855771202Aull, 0x106AA07032BBD1B8ull,
    0x19A4C116B8D2D0C8ull, 0x1E376C085141AB53ull, 0x2748774CDF8EEB99ull, 0x34B0BCB5E19B48A8ull,
    0x391C0CB3C5C95A63ull, 0x4ED8AA4AE3418ACBull, 0x5B9CCA4F7763E373ull, 0x682E6FF3D6B2B8A3ull,
    0x748F82EE5DEFB2FCull, 0x78A5636F43172F60ull, 0x84C87814A1F0AB72ull, 0x8CC702081A6439ECull,
    0x90BEFFFA23631E28ull, 0xA4506CEBDE82BDE9ull, 0xBEF9A3F7B2C67915ull, 0xC67178F2E372532Bull,
    0xCA273ECEEA26619Cull, 0xD186B8C721C0C207ull, 0xEADA7DD6CDE0EB1Eull, 0xF57D4F7FEE6ED178ull,
    0x06F067AA72176FBAull, 0x0A637DC5A2C898A6ull, 0x113F9804BEF90DAEull, 0x1B710B35131C471Bull,
    0x28DB77F523047D84ull, 0x32CAAB7B40C72493ull, 0x3C9EBE0A15C9BEBCull, 0x431D67C49C100D4Cull,
    0x4CC5D4BECB3E42B6ull, 0x597F299CFC657E2Aull, 0x5FCB6FAB3AD6FAECull, 0x6C44198C4A475817ull
};

// Initial state: fractional parts of the square roots of the first 8 primes.
constexpr uint64_t kH512[8] = {
    0x6A09E667F3BCC908ull, 0xBB67AE8584CAA73Bull, 0x3C6EF372FE94F82Bull, 0xA54FF53A5F1D36F1ull,
    0x510E527FADE682D1ull, 0x9B05688C2B3E6C1Full, 0x1F83D9ABFB41BD6Bull, 0x5BE0CD19137E2179ull
};

inline uint64_t rotr64 (uint64_t x, int n) { return (x >> n) | (x << (64 - n)); }

} // namespace

Sha512::Sha512 ()
{
    for (int i = 0; i < 8; ++i) _state[i] = kH512[i];
}

void Sha512::processBlock (const uint64_t block[16])
{
    uint64_t w[80];
    for (int i = 0; i < 16; ++i) w[i] = block[i];
    for (int i = 16; i < 80; ++i)
    {
        const uint64_t s0 = rotr64 (w[i-15], 1) ^ rotr64 (w[i-15], 8) ^ (w[i-15] >> 7);
        const uint64_t s1 = rotr64 (w[i-2], 19) ^ rotr64 (w[i-2], 61) ^ (w[i-2] >> 6);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }

    uint64_t a = _state[0], b = _state[1], c = _state[2], d = _state[3];
    uint64_t e = _state[4], f = _state[5], g = _state[6], h = _state[7];

    for (int i = 0; i < 80; ++i)
    {
        const uint64_t s1 = rotr64 (e, 14) ^ rotr64 (e, 18) ^ rotr64 (e, 41);
        const uint64_t ch = (e & f) ^ ((~e) & g);
        const uint64_t t1 = h + s1 + ch + kK512[i] + w[i];
        const uint64_t s0 = rotr64 (a, 28) ^ rotr64 (a, 34) ^ rotr64 (a, 39);
        const uint64_t maj = (a & b) ^ (a & c) ^ (b & c);
        const uint64_t t2 = s0 + maj;

        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    _state[0] += a; _state[1] += b; _state[2] += c; _state[3] += d;
    _state[4] += e; _state[5] += f; _state[6] += g; _state[7] += h;
}

void Sha512::update (const uint8_t* data, size_t len)
{
    _totalBytes += len;

    while (len > 0)
    {
        const size_t take = std::min (len, size_t (128) - _bufferLen);
        if (take > 0)
        {
            std::memcpy (_buffer + _bufferLen, data, take);
            _bufferLen += take;
            data += take;
            len -= take;
        }
        if (_bufferLen == 128)
        {
            uint64_t block[16];
            for (int i = 0; i < 16; ++i)
            {
                const uint8_t* p = _buffer + 8 * i;
                block[i] = (uint64_t (p[0]) << 56) | (uint64_t (p[1]) << 48)
                         | (uint64_t (p[2]) << 40) | (uint64_t (p[3]) << 32)
                         | (uint64_t (p[4]) << 24) | (uint64_t (p[5]) << 16)
                         | (uint64_t (p[6]) << 8)  |  uint64_t (p[7]);
            }
            processBlock (block);
            _bufferLen = 0;
        }
    }
}

void Sha512::rawDigest (uint8_t out[64])
{
    const uint64_t bitLen = _totalBytes * 8ull;

    // 0x80 padding, then zeros to length ≡ 112 (mod 128), then the 128-bit
    // big-endian bit count. _totalBytes < 2^61 in every use here, so the high
    // half of the length is always zero.
    uint8_t pad[256];
    size_t padLen = 0;
    pad[padLen++] = 0x80;
    // The padded message must end with a 16-byte length block: the byte count
    // before it is ≡ 112 (mod 128). Relative to _bufferLen, not to 128.
    while ((_bufferLen + padLen) % 128 != 112) pad[padLen++] = 0x00;
    for (int i = 0; i < 8; ++i) pad[padLen++] = 0x00;
    for (int i = 7; i >= 0; --i) pad[padLen++] = uint8_t ((bitLen >> (8 * i)) & 0xFF);
    update (pad, padLen);   // completes the final block(s)

    for (int i = 0; i < 8; ++i)
        for (int j = 0; j < 8; ++j)
            out[8 * i + j] = uint8_t ((_state[i] >> (56 - 8 * j)) & 0xFF);

    for (int i = 0; i < 8; ++i) _state[i] = kH512[i];
    _bufferLen = 0;
    _totalBytes = 0;
}

std::string Sha512::hexDigest ()
{
    uint8_t raw[64];
    rawDigest (raw);
    return toHex (raw, 64);
}

std::string sha512Hex (const void* data, size_t len)
{
    Sha512 h;
    h.update (reinterpret_cast<const uint8_t*> (data), len);
    return h.hexDigest ();
}

// ==========================================================================
// Field arithmetic mod p = 2^255 - 19
// ==========================================================================
namespace {

constexpr uint64_t kP[4] = {
    0xFFFFFFFFFFFFFFEDull, 0xFFFFFFFFFFFFFFFFull,
    0xFFFFFFFFFFFFFFFFull, 0x7FFFFFFFFFFFFFFFull
};

// Group order L = 2^252 + 27742317777372353535851937790883648493 (RFC 8032),
// only used for the s < L check and for the reductions in ed25519Sign.
constexpr uint64_t kL[4] = {
    0x5812631A5CF5D3EDull, 0x14DEF9DEA2F79CD6ull,
    0x0000000000000000ull, 0x1000000000000000ull
};

struct F { uint64_t v[4]; };

struct D128 { uint64_t lo, hi; };

// 64x64 -> 128 from 32-bit halves. mid = p1 + p2 can overflow: that adds
// 2^64 to mid, i.e. 2^96 to the product, which belongs in the high half.
inline D128 mul64 (uint64_t a, uint64_t b)
{
    const uint64_t a0 = a & 0xFFFFFFFFull, a1 = a >> 32;
    const uint64_t b0 = b & 0xFFFFFFFFull, b1 = b >> 32;
    const uint64_t p0 = a0 * b0, p1 = a0 * b1, p2 = a1 * b0, p3 = a1 * b1;

    const uint64_t mid = p1 + p2;
    const uint64_t midOverflow = (mid < p1) ? 1ull : 0ull;
    const uint64_t lo = p0 + (mid << 32);
    const uint64_t loCarry = (lo < p0) ? 1ull : 0ull;
    const uint64_t hi = p3 + (mid >> 32) + (midOverflow << 32) + loCarry;
    return { lo, hi };
}

// 256x256 -> 8 limbs. Column-wise: a column holds at most four 128-bit
// products, so a 192-bit running carry (three limbs) is enough; the carry of
// column k seeds columns k+1 and k+2.
void mul512 (const uint64_t a[4], const uint64_t b[4], uint64_t t[8])
{
    uint64_t c0 = 0, c1 = 0, c2 = 0;
    for (int k = 0; k < 8; ++k)
    {
        uint64_t lo = c0, mid = c1, hi = c2;
        const int i0 = k > 3 ? k - 3 : 0;
        const int i1 = k < 3 ? k : 3;
        for (int i = i0; i <= i1; ++i)
        {
            const D128 p = mul64 (a[i], b[k - i]);

            const uint64_t s1 = lo + p.lo;
            const uint64_t carry1 = (s1 < lo) ? 1ull : 0ull;
            lo = s1;

            const uint64_t m1 = mid + p.hi;
            const uint64_t carry2 = (m1 < mid) ? 1ull : 0ull;
            const uint64_t m2 = m1 + carry1;
            const uint64_t carry3 = (m2 < m1) ? 1ull : 0ull;
            mid = m2;
            hi += carry2 + carry3;   // column sums stay far below 2^128
        }
        t[k] = lo;
        c0 = mid; c1 = hi; c2 = 0;
    }
}

// Compare two 4-limb values: -1 / 0 / 1.
int cmp4 (const uint64_t a[4], const uint64_t b[4])
{
    for (int i = 3; i >= 0; --i)
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}

// Subtract p while the value is >= p (at most a couple of iterations: the
// callers hand in values below 2^256 + 400 and p > 2^254).
void reduce (F& x)
{
    for (;;)
    {
        if (cmp4 (x.v, kP) < 0) return;
        uint64_t borrow = 0;
        for (int i = 0; i < 4; ++i)
        {
            const uint64_t s = x.v[i] - kP[i];
            const uint64_t b1 = (s > x.v[i]) ? 1ull : 0ull;
            const uint64_t s2 = s - borrow;
            const uint64_t b2 = (s2 > s) ? 1ull : 0ull;
            x.v[i] = s2;
            borrow = b1 + b2;   // never 2: see the note in subF
        }
    }
}

// x (8 limbs) -> x mod p using 2^256 ≡ 19.
// Fold the top limb (value ≡ r[4] * 2^256) back into the low half.
// p = 2^255 - 19 gives 2^255 ≡ 19 and therefore 2^256 ≡ 38.
constexpr uint64_t kFold = 38ull;

void foldTop (uint64_t r[5])
{
    for (int guard = 0; guard < 8 && r[4] != 0; ++guard)
    {
        uint64_t carry = kFold * r[4];
        r[4] = 0;
        for (int i = 0; i < 4 && carry != 0; ++i)
        {
            const uint64_t s = r[i] + carry;
            carry = (s < r[i]) ? 1ull : 0ull;
            r[i] = s;
        }
        r[4] = carry;   // 0 or 1
    }
}

void fold (const uint64_t t[8], F& out)
{
    uint64_t r[5] = { 0, 0, 0, 0, 0 };

    // r = low half + 38 * high half
    uint64_t carry = 0;
    for (int i = 0; i < 4; ++i)
    {
        const D128 m = mul64 (t[4 + i], kFold);   // m.hi <= 37
        const uint64_t s1 = t[i] + m.lo;
        const uint64_t c1 = (s1 < t[i]) ? 1ull : 0ull;
        const uint64_t s2 = s1 + carry;
        const uint64_t c2 = (s2 < s1) ? 1ull : 0ull;
        r[i] = s2;
        carry = m.hi + c1 + c2;   // <= 40, no overflow
    }
    r[4] = carry;

    foldTop (r);

    for (int i = 0; i < 4; ++i) out.v[i] = r[i];
    reduce (out);
}

void setZero (F& x) { for (int i = 0; i < 4; ++i) x.v[i] = 0; }
void setOne (F& x) { x.v[0] = 1; x.v[1] = x.v[2] = x.v[3] = 0; }
bool isZero (const F& x) { return (x.v[0] | x.v[1] | x.v[2] | x.v[3]) == 0; }
bool equalF (const F& a, const F& b) { return cmp4 (a.v, b.v) == 0; }

void addF (const F& a, const F& b, F& out)
{
    uint64_t t[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    uint64_t carry = 0;
    for (int i = 0; i < 4; ++i)
    {
        const uint64_t s1 = a.v[i] + b.v[i];
        const uint64_t c1 = (s1 < a.v[i]) ? 1ull : 0ull;
        const uint64_t s2 = s1 + carry;
        // c1 and c2 cannot both be 1: that would need a + b = 2^65 - 1.
        const uint64_t c2 = (s2 < s1) ? 1ull : 0ull;
        t[i] = s2;
        carry = c1 + c2;
    }
    t[4] = carry;   // fold() turns it into +19, which is exactly right
    fold (t, out);
}

void subF (const F& a, const F& b, F& out)
{
    uint64_t r[4];
    uint64_t borrow = 0;
    for (int i = 0; i < 4; ++i)
    {
        const uint64_t s = a.v[i] - b.v[i];
        const uint64_t b1 = (s > a.v[i]) ? 1ull : 0ull;
        const uint64_t s2 = s - borrow;
        const uint64_t b2 = (s2 > s) ? 1ull : 0ull;
        r[i] = s2;
        borrow = b1 + b2;   // never 2: b1 == 1 means s <= a.v[i] - 1 < 2^64 - 1
    }
    for (int i = 0; i < 4; ++i) out.v[i] = r[i];
    if (borrow)
    {
        uint64_t carry = 0;
        for (int i = 0; i < 4; ++i)
        {
            const uint64_t s = out.v[i] + kP[i];
            const uint64_t c = (s < out.v[i]) ? 1ull : 0ull;
            const uint64_t s2 = s + carry;
            carry = c + ((s2 < s) ? 1ull : 0ull);
            out.v[i] = s2;
        }
    }
    reduce (out);
}

void mulF (const F& a, const F& b, F& out)
{
    uint64_t t[8];
    mul512 (a.v, b.v, t);
    fold (t, out);
}

void sqrF (const F& a, F& out) { mulF (a, a, out); }

// out = base^e, e as 4 little-endian limbs.
void powF (const F& base, const uint64_t e[4], F& out)
{
    F r; setOne (r);
    F b = base;
    for (int k = 0; k < 256; ++k)
    {
        if ((e[k >> 6] >> (k & 63)) & 1) mulF (r, b, r);
        sqrF (b, b);
    }
    out = r;
}

void invF (const F& x, F& out)
{
    uint64_t e[4];   // p - 2
    uint64_t borrow = 2;
    for (int i = 0; i < 4; ++i)
    {
        const uint64_t s = kP[i] - borrow;
        borrow = (s > kP[i]) ? 1ull : 0ull;
        e[i] = s;
    }
    powF (x, e, out);
}

// (kP + 3) >> 3: the exponent of the square root for p ≡ 5 (mod 8).
// A right shift takes the low bits of the NEXT (more significant) limb.
void shrLimbs (const uint64_t t[4], int n, uint64_t e[4])
{
    const int other = 64 - n;
    for (int i = 0; i < 4; ++i)
    {
        e[i] = t[i] >> n;
        if (i + 1 < 4) e[i] |= t[i + 1] << other;
    }
}

void expSqrt (uint64_t e[4])
{
    uint64_t t[4];
    uint64_t carry = 3;
    for (int i = 0; i < 4; ++i)
    {
        const uint64_t s = kP[i] + carry;
        carry = (s < kP[i]) ? 1ull : 0ull;
        t[i] = s;
    }
    shrLimbs (t, 3, e);
}

// (kP - 1) >> 2: exponent for sqrt(-1) = 2^((p-1)/4).
void expQuarter (uint64_t e[4])
{
    uint64_t t[4];
    uint64_t borrow = 1;
    for (int i = 0; i < 4; ++i)
    {
        const uint64_t s = kP[i] - borrow;
        borrow = (s > kP[i]) ? 1ull : 0ull;
        t[i] = s;
    }
    shrLimbs (t, 2, e);
}

// --------------------------------------------------------------------------
// Curve constants (computed, not hardcoded: nothing to mistype).
// --------------------------------------------------------------------------
struct Curve
{
    F d;        // -121665 / 121666
    F sqrtM1;   // 2^((p-1)/4)
    uint64_t sqrtExp[4];
    bool ready = false;

    void init ()
    {
        if (ready) return;

        F num, den, denInv;
        setZero (num); num.v[0] = 121665ull;
        setZero (den); den.v[0] = 121666ull;
        invF (den, denInv);
        F negNum; setZero (negNum);
        subF (negNum, num, negNum);   // p - 121665
        mulF (negNum, denInv, d);

        F two; setZero (two); two.v[0] = 2;
        expQuarter (sqrtExp);
        uint64_t e[4];
        for (int i = 0; i < 4; ++i) e[i] = sqrtExp[i];
        powF (two, e, sqrtM1);

        expSqrt (sqrtExp);
        ready = true;
    }
};

Curve& curve()
{
    static Curve c;
    c.init ();
    return c;
}

// --------------------------------------------------------------------------
// Points: extended coordinates (RFC 8032 Appendix A).
// --------------------------------------------------------------------------
struct Point { F X, Y, Z, T; };

void identity (Point& P)
{
    setZero (P.X); setOne (P.Y); setOne (P.Z); setZero (P.T);
}

void pointAdd (const Point& P, const Point& Q, Point& R)
{
    const Curve& C = curve();
    F A, B, Cc, D, E, Ff, G, H, t1, t2;

    subF (P.Y, P.X, t1); subF (Q.Y, Q.X, t2); mulF (t1, t2, A);
    addF (P.Y, P.X, t1); addF (Q.Y, Q.X, t2); mulF (t1, t2, B);
    mulF (P.T, Q.T, Cc); addF (Cc, Cc, Cc); mulF (Cc, C.d, Cc);
    mulF (P.Z, Q.Z, D); addF (D, D, D);

    subF (B, A, E);
    subF (D, Cc, Ff);
    addF (D, Cc, G);
    addF (B, A, H);

    Point out;
    mulF (E, Ff, out.X);
    mulF (G, H, out.Y);
    mulF (Ff, G, out.Z);
    mulF (E, H, out.T);
    R = out;
}

void pointDbl (const Point& P, Point& R) { pointAdd (P, P, R); }

// Double-and-add, most significant bit first. The scalar is NOT reduced: the
// group has order L, so an unreduced scalar acts identically and no mod-L
// arithmetic is needed on the verify path.
void pointMul (const uint64_t* e, int nLimbs, const Point& P, Point& R)
{
    identity (R);
    int top = 64 * nLimbs - 1;
    while (top >= 0 && ((e[top >> 6] >> (top & 63)) & 1) == 0) --top;
    for (int k = top; k >= 0; --k)
    {
        pointDbl (R, R);
        if ((e[k >> 6] >> (k & 63)) & 1) pointAdd (R, P, R);
    }
}

bool pointsEqual (const Point& P, const Point& Q)
{
    // x1/z1 == x2/z2  <=>  x1*z2 == x2*z1 (same for y)
    F a, b;
    mulF (P.X, Q.Z, a); mulF (Q.X, P.Z, b); if (!equalF (a, b)) return false;
    mulF (P.Y, Q.Z, a); mulF (Q.Y, P.Z, b); if (!equalF (a, b)) return false;
    return true;
}

void pointCompress (const Point& P, uint8_t out[32])
{
    F zi, x, y;
    invF (P.Z, zi);
    mulF (P.X, zi, x);
    mulF (P.Y, zi, y);
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 8; ++j)
            out[8 * i + j] = uint8_t ((y.v[i] >> (8 * j)) & 0xFF);
    out[31] = uint8_t (out[31] | ((x.v[0] & 1) << 7));
}

void loadLE (const uint8_t* bytes, uint64_t* limbs, int nLimbs)
{
    for (int i = 0; i < nLimbs; ++i)
    {
        uint64_t v = 0;
        for (int j = 0; j < 8; ++j)
            v |= uint64_t (bytes[8 * i + j]) << (8 * j);
        limbs[i] = v;
    }
}

bool pointDecompress (const uint8_t in[32], Point& P)
{
    const Curve& C = curve();

    uint64_t yw[4];
    loadLE (in, yw, 4);
    const int sign = int ((yw[3] >> 63) & 1);
    yw[3] &= 0x7FFFFFFFFFFFFFFFull;
    if (cmp4 (yw, kP) >= 0) return false;   // y >= p

    F y; for (int i = 0; i < 4; ++i) y.v[i] = yw[i];

    F y2, one, u, v;
    sqrF (y, y2);
    setOne (one);
    subF (y2, one, u);              // u = y^2 - 1
    mulF (C.d, y2, v); addF (v, one, v);   // v = d y^2 + 1
    if (isZero (v)) return false;

    F vInv, x2;
    invF (v, vInv);
    mulF (u, vInv, x2);

    if (isZero (x2))
    {
        if (sign) return false;
        setZero (x2);
    }

    F x;
    powF (x2, C.sqrtExp, x);       // p ≡ 5 (mod 8)
    F chk;
    sqrF (x, chk);
    if (!equalF (chk, x2))
    {
        mulF (x, C.sqrtM1, x);
        sqrF (x, chk);
        if (!equalF (chk, x2)) return false;
    }

    if (int (x.v[0] & 1) != sign)
    {
        F zero; setZero (zero);
        subF (zero, x, x);         // x = p - x
    }

    P.X = x; P.Y = y; setOne (P.Z); mulF (x, y, P.T);
    return true;
}

const Point& basepoint()
{
    static Point B;
    static bool built = false;
    if (!built)
    {
        // y = 4/5, x the even root.
        F four, five, inv5;
        setZero (four); four.v[0] = 4;
        setZero (five); five.v[0] = 5;
        invF (five, inv5);
        F y; mulF (four, inv5, y);

        uint8_t bytes[32];
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 8; ++j)
                bytes[8 * i + j] = uint8_t ((y.v[i] >> (8 * j)) & 0xFF);
        pointDecompress (bytes, B);   // sign 0: the even x
        built = true;
    }
    return B;
}

// --------------------------------------------------------------------------
// Reduction modulo L (only for signing): binary long division, subtract L
// shifted left by k bits whenever the value is at least that large.
// --------------------------------------------------------------------------
int topBit (const uint64_t* w, int nLimbs)
{
    for (int i = nLimbs - 1; i >= 0; --i)
        if (w[i])
        {
            int b = 63;
            while (b > 0 && ((w[i] >> b) & 1) == 0) --b;
            return 64 * i + b;
        }
    return -1;
}

void shlBits (const uint64_t* in, int nIn, int shift, uint64_t* out, int nOut)
{
    const int limbShift = shift / 64, bitShift = shift % 64;
    for (int i = 0; i < nOut; ++i)
    {
        uint64_t v = 0;
        const int s = i - limbShift;
        if (s >= 0 && s < nIn) v |= in[s] << bitShift;
        if (bitShift != 0 && s - 1 >= 0 && s - 1 < nIn) v |= in[s - 1] >> (64 - bitShift);
        out[i] = v;
    }
}

void cmpSubL (uint64_t w[8], int k)
{
    uint64_t s[8];
    shlBits (kL, 4, k, s, 8);

    int cmp = -1;
    for (int i = 7; i >= 0; --i)
        if (w[i] != s[i]) { cmp = w[i] < s[i] ? -1 : 1; break; }
    if (cmp < 0) return;

    uint64_t borrow = 0;
    for (int i = 0; i < 8; ++i)
    {
        const uint64_t r = w[i] - s[i];
        const uint64_t b1 = (r > w[i]) ? 1ull : 0ull;
        const uint64_t r2 = r - borrow;
        const uint64_t b2 = (r2 > r) ? 1ull : 0ull;
        w[i] = r2;
        borrow = b1 + b2;
    }
}

void modL (uint64_t w[8])
{
    const int top = topBit (w, 8);
    for (int k = top - 252; k >= 0; --k)   // L has its top bit at 252
        cmpSubL (w, k);
}

} // namespace

// ==========================================================================
// Public Ed25519 entry points
// ==========================================================================

bool ed25519Verify (const uint8_t pub[32], const uint8_t* msg, size_t msgLen,
                    const uint8_t sig[64])
{
    Point A, R;
    if (!pointDecompress (pub, A)) return false;
    if (!pointDecompress (sig, R)) return false;

    uint64_t s[4];
    loadLE (sig + 32, s, 4);
    if (cmp4 (s, kL) >= 0) return false;   // s must be < L

    uint8_t hb[64];
    Sha512 h;
    h.update (sig, 32);
    h.update (pub, 32);
    h.update (msg, msgLen);
    h.rawDigest (hb);
    uint64_t hh[8];
    loadLE (hb, hh, 8);

    Point sB, hA, sum;
    pointMul (s, 4, basepoint(), sB);
    pointMul (hh, 8, A, hA);
    pointAdd (R, hA, sum);

    return pointsEqual (sB, sum);
}

bool ed25519PublicFromSeed (const uint8_t seed[32], uint8_t pubOut[32])
{
    uint8_t h[64];
    Sha512 hs;
    hs.update (seed, 32);
    hs.rawDigest (h);

    uint8_t ab[32];
    std::memcpy (ab, h, 32);
    ab[0] &= 0xF8; ab[31] &= 0x7F; ab[31] |= 0x40;   // RFC 8032 clamping

    uint64_t a[4];
    loadLE (ab, a, 4);
    Point A;
    pointMul (a, 4, basepoint(), A);
    pointCompress (A, pubOut);
    return true;
}

bool ed25519Sign (const uint8_t seed[32], const uint8_t* msg, size_t msgLen,
                  uint8_t sigOut[64])
{
    uint8_t h[64];
    {
        Sha512 hs;
        hs.update (seed, 32);
        hs.rawDigest (h);
    }

    uint8_t ab[32];
    std::memcpy (ab, h, 32);
    ab[0] &= 0xF8; ab[31] &= 0x7F; ab[31] |= 0x40;
    uint64_t a8[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    loadLE (ab, a8, 4);
    modL (a8);

    Point A;
    uint64_t a4[4];
    for (int i = 0; i < 4; ++i) a4[i] = a8[i];
    pointMul (a4, 4, basepoint(), A);
    uint8_t pk[32];
    pointCompress (A, pk);

    uint8_t rb[64];
    {
        Sha512 rs;
        rs.update (h + 32, 32);
        rs.update (msg, msgLen);
        rs.rawDigest (rb);
    }
    uint64_t r[8];
    loadLE (rb, r, 8);
    modL (r);

    Point R;
    pointMul (r, 4, basepoint(), R);
    uint8_t rEnc[32];
    pointCompress (R, rEnc);

    uint8_t hb[64];
    {
        Sha512 hs2;
        hs2.update (rEnc, 32);
        hs2.update (pk, 32);
        hs2.update (msg, msgLen);
        hs2.rawDigest (hb);
    }
    uint64_t hh[8];
    loadLE (hb, hh, 8);
    modL (hh);

    // s = (r + h*a) mod L: h*a is at most 506 bits.
    uint64_t prod[8];
    mul512 (hh, a4, prod);   // a4 = a mod L fits in 4 limbs
    uint64_t sum[8];
    uint64_t carry = 0;
    for (int i = 0; i < 8; ++i)
    {
        const uint64_t s = r[i] + prod[i];
        const uint64_t c = (s < r[i]) ? 1ull : 0ull;
        const uint64_t s2 = s + carry;
        carry = c + ((s2 < s) ? 1ull : 0ull);
        sum[i] = s2;
    }
    modL (sum);

    std::memcpy (sigOut, rEnc, 32);
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 8; ++j)
            sigOut[32 + 8 * i + j] = uint8_t ((sum[i] >> (8 * j)) & 0xFF);
    return true;
}

// ==========================================================================
// Hex helpers
// ==========================================================================

namespace {
int hexNibble (char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
} // namespace

std::string toHex (const uint8_t* data, size_t len)
{
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve (len * 2);
    for (size_t i = 0; i < len; ++i)
    {
        out.push_back (kHex[data[i] >> 4]);
        out.push_back (kHex[data[i] & 15]);
    }
    return out;
}

bool keyFromHex (const std::string& hex, uint8_t out[32])
{
    if (hex.size() != 64) return false;
    for (int i = 0; i < 32; ++i)
    {
        const int hi = hexNibble (hex[size_t (2 * i)]);
        const int lo = hexNibble (hex[size_t (2 * i + 1)]);
        if (hi < 0 || lo < 0) return false;
        out[i] = uint8_t ((hi << 4) | lo);
    }
    return true;
}

bool sigFromHex (const std::string& text, uint8_t out[64])
{
    std::string s;
    for (char c : text)
        if (! (c == ' ' || c == '\t' || c == '\r' || c == '\n')) s.push_back (c);
    if (s.size() != 128) return false;
    for (int i = 0; i < 64; ++i)
    {
        const int hi = hexNibble (s[size_t (2 * i)]);
        const int lo = hexNibble (s[size_t (2 * i + 1)]);
        if (hi < 0 || lo < 0) return false;
        out[i] = uint8_t ((hi << 4) | lo);
    }
    return true;
}

}} // namespace sc::crypto
