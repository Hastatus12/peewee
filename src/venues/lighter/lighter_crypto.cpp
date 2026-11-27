#include "lighter_crypto.hpp"

#include <openssl/rand.h>

#include <stdexcept>

namespace peewee::lighter {

namespace {

// ---------------------------------------------------------------------------
// Goldilocks field: p = 2^64 - 2^32 + 1. Values are kept canonical (< p).
// ---------------------------------------------------------------------------

__extension__ typedef unsigned __int128 u128;  // GCC and Clang.

constexpr uint64_t kP = 0xffffffff00000001ull;
constexpr uint64_t kEps = 0xffffffffull;  // 2^64 mod p.

// Any 128-bit value modulo p, using 2^64 = 2^32 - 1 and 2^96 = -1 (mod p).
uint64_t reduce(u128 x) {
    const uint64_t lo = static_cast<uint64_t>(x);
    const uint64_t hi = static_cast<uint64_t>(x >> 64);
    const uint64_t hi_hi = hi >> 32;
    const uint64_t hi_lo = hi & kEps;

    uint64_t t0 = lo - hi_hi;  // lo - hi_hi * 2^96
    if (lo < hi_hi) t0 -= kEps;
    const uint64_t t1 = hi_lo * kEps;  // hi_lo * 2^64
    uint64_t r = t0 + t1;
    if (r < t1) r += kEps;
    return r >= kP ? r - kP : r;
}

uint64_t add(uint64_t a, uint64_t b) {
    uint64_t s = a + b;
    if (s < a) s += kEps;  // Wrapped past 2^64: add 2^64 mod p back.
    return s >= kP ? s - kP : s;
}

uint64_t sub(uint64_t a, uint64_t b) { return a >= b ? a - b : a - b + kP; }

uint64_t mul(uint64_t a, uint64_t b) { return reduce(static_cast<u128>(a) * b); }

uint64_t pow(uint64_t x, uint64_t e) {
    uint64_t r = 1;
    while (e) {
        if (e & 1) r = mul(r, x);
        x = mul(x, x);
        e >>= 1;
    }
    return r;
}

uint64_t inverse(uint64_t x) { return pow(x, kP - 2); }  // 0 for 0.

// ---------------------------------------------------------------------------
// Poseidon2, width 12, as poseidon2_goldilocks_plonky2 (constants: config.go).
// ---------------------------------------------------------------------------

#include "lighter_poseidon2_constants.inc"

constexpr int kWidth = 12;
constexpr int kRate = 8;
using State = std::array<uint64_t, kWidth>;

void external_linear_layer(State& s) {
    // 4x4 MDS block on each group of 4, then add the sum of the groups.
    std::array<u128, kWidth> x{};
    for (int i = 0; i < kWidth; i += 4) {
        const u128 a = s[i], b = s[i + 1], c = s[i + 2], d = s[i + 3];
        const u128 t01 = a + b;
        const u128 t23 = c + d;
        const u128 t0123 = t01 + t23;
        x[i] = t0123 + t01 + b;
        x[i + 1] = t0123 + b + 2 * c;
        x[i + 2] = t0123 + t23 + d;
        x[i + 3] = t0123 + d + 2 * a;
    }
    for (int i = 0; i < 4; ++i) {
        const u128 sum = x[i] + x[i + 4] + x[i + 8];
        for (int j = i; j < kWidth; j += 4) s[j] = reduce(x[j] + sum);
    }
}

void internal_linear_layer(State& s) {
    u128 total = 0;
    for (uint64_t v : s) total += v;
    const uint64_t sum = reduce(total);
    for (int i = 0; i < kWidth; ++i) s[i] = add(sum, mul(s[i], kDiag[i]));
}

uint64_t sbox(uint64_t x) {  // x^7
    const uint64_t x2 = mul(x, x);
    const uint64_t x3 = mul(x2, x);
    return mul(mul(x3, x3), x);
}

void full_rounds(State& s, int first) {
    for (int r = first; r < first + 4; ++r) {
        for (int i = 0; i < kWidth; ++i) s[i] = sbox(add(s[i], kExternal[r][i]));
        external_linear_layer(s);
    }
}

void permute(State& s) {
    external_linear_layer(s);
    full_rounds(s, 0);
    for (uint64_t c : kInternal) {
        s[0] = sbox(add(s[0], c));
        internal_linear_layer(s);
    }
    full_rounds(s, 4);
}

// ---------------------------------------------------------------------------
// GF(p^5) = GF(p)[X] / (X^5 - 3).
// ---------------------------------------------------------------------------

constexpr uint64_t kW = 3;
constexpr uint64_t kDthRoot = 1041288259238279555ull;  // Frobenius: X^p = kDthRoot * X.

Fp5 operator+(const Fp5& a, const Fp5& b) {
    return {add(a[0], b[0]), add(a[1], b[1]), add(a[2], b[2]), add(a[3], b[3]), add(a[4], b[4])};
}

Fp5 operator-(const Fp5& a, const Fp5& b) {
    return {sub(a[0], b[0]), sub(a[1], b[1]), sub(a[2], b[2]), sub(a[3], b[3]), sub(a[4], b[4])};
}

Fp5 operator*(const Fp5& a, const Fp5& b) { return fp5_mul(a, b); }

Fp5 square(const Fp5& a) { return a * a; }
Fp5 twice(const Fp5& a) { return a + a; }

Fp5 scale(const Fp5& a, uint64_t k) { return {mul(a[0], k), mul(a[1], k), mul(a[2], k), mul(a[3], k), mul(a[4], k)}; }

bool is_zero(const Fp5& a) { return (a[0] | a[1] | a[2] | a[3] | a[4]) == 0; }

// x^(p^count).
Fp5 frobenius(const Fp5& x, int count) {
    const uint64_t z0 = pow(kDthRoot, static_cast<uint64_t>(count));
    Fp5 r{};
    uint64_t z = 1;
    for (int i = 0; i < 5; ++i) {
        r[i] = mul(x[i], z);
        z = mul(z, z0);
    }
    return r;
}

// ---------------------------------------------------------------------------
// Scalars modulo the curve order n.
// ---------------------------------------------------------------------------

constexpr Scalar kN = {0xE80FD996948BFFE1ull, 0xE8885C39D724A09Cull, 0x7FFFFFE6CFB80639ull, 0x7FFFFFF100000016ull,
                       0x7FFFFFFD80000007ull};
constexpr uint64_t kN0I = 0xD78BEF72057B7BDFull;  // -1/n mod 2^64.
constexpr Scalar kR2 = {0xA01001DCE33DC739ull, 0x6C3228D33F62ACCFull, 0xD1D796CC91CF8525ull, 0xAADFFF5D1574C1D8ull,
                        0x4ACA13B28CA251F5ull};  // 2^640 mod n.

// a - b over 320 bits; returns the borrow (1 if a < b).
uint64_t sub_borrow(const Scalar& a, const Scalar& b, Scalar& out) {
    uint64_t borrow = 0;
    for (int i = 0; i < 5; ++i) {
        const u128 d = static_cast<u128>(a[i]) - b[i] - borrow;
        out[i] = static_cast<uint64_t>(d);
        borrow = static_cast<uint64_t>(d >> 64) & 1;
    }
    return borrow;
}

Scalar add_no_carry(const Scalar& a, const Scalar& b) {
    Scalar r{};
    uint64_t carry = 0;
    for (int i = 0; i < 5; ++i) {
        const u128 s = static_cast<u128>(a[i]) + b[i] + carry;
        r[i] = static_cast<uint64_t>(s);
        carry = static_cast<uint64_t>(s >> 64);
    }
    return r;
}

// Any 320-bit value modulo n (2^320 < 3n: at most two subtractions).
Scalar reduce_scalar(Scalar x) {
    for (int i = 0; i < 2; ++i) {
        Scalar d{};
        if (!sub_borrow(x, kN, d)) x = d;
    }
    return x;
}

// Montgomery product a * b / 2^320 mod n (as Go's MontyMul).
Scalar monty_mul(const Scalar& a, const Scalar& b) {
    Scalar r{};
    for (int i = 0; i < 5; ++i) {
        const uint64_t m = b[i];
        const uint64_t f = (a[0] * m + r[0]) * kN0I;
        uint64_t cc1 = 0, cc2 = 0;
        for (int j = 0; j < 5; ++j) {
            const u128 z = static_cast<u128>(a[j]) * m + r[j] + cc1;
            cc1 = static_cast<uint64_t>(z >> 64);
            const u128 z2 = static_cast<u128>(f) * kN[j] + static_cast<uint64_t>(z) + cc2;
            cc2 = static_cast<uint64_t>(z2 >> 64);
            if (j > 0) r[j - 1] = static_cast<uint64_t>(z2);
        }
        r[4] = cc1 + cc2;
    }
    Scalar d{};
    return sub_borrow(r, kN, d) ? r : d;
}

// ---------------------------------------------------------------------------
// ECgFp5 points, in (x, z, u, t) coordinates (Pornin's double-odd curve
// y^2 = x(x^2 + 2x + 263X)). The formulas are complete: no special cases.
// ---------------------------------------------------------------------------

struct Point {
    Fp5 x, z, u, t;
};

constexpr Fp5 kB = {0, 263, 0, 0, 0};
constexpr Fp5 kB2 = {0, 2 * 263, 0, 0, 0};
constexpr Fp5 kB4 = {0, 4 * 263, 0, 0, 0};
constexpr Fp5 kFour = {4, 0, 0, 0, 0};

constexpr Point kNeutral = {{0, 0, 0, 0, 0}, {1, 0, 0, 0, 0}, {0, 0, 0, 0, 0}, {1, 0, 0, 0, 0}};
constexpr Point kGenerator = {
    {12883135586176881569ull, 4356519642755055268ull, 5248930565894896907ull, 2165973894480315022ull,
     2448410071095648785ull},
    {1, 0, 0, 0, 0},
    {1, 0, 0, 0, 0},
    {4, 0, 0, 0, 0}};

Point point_add(const Point& p, const Point& q) {
    const Fp5 t1 = p.x * q.x;
    const Fp5 t2 = p.z * q.z;
    const Fp5 t3 = p.u * q.u;
    const Fp5 t4 = p.t * q.t;
    const Fp5 t5 = (p.x + p.z) * (q.x + q.z) - (t1 + t2);
    const Fp5 t6 = (p.u + p.t) * (q.u + q.t) - (t3 + t4);
    const Fp5 t7 = t1 + t2 * kB;
    const Fp5 t8 = t4 * t7;
    const Fp5 t9 = t3 * (t5 * kB2 + twice(t7));
    const Fp5 t10 = (t4 + twice(t3)) * (t5 + t7);
    return {(t10 - t8) * kB, t8 - t9, t6 * (t2 * kB - t1), t8 + t9};
}

Point point_double(const Point& p) {
    const Fp5 t1 = p.z * p.t;
    const Fp5 t2 = t1 * p.t;
    const Fp5 x1 = square(t2);
    const Fp5 z1 = t1 * p.u;
    const Fp5 t3 = square(p.u);
    const Fp5 w1 = t2 - t3 * twice(p.x + p.z);
    const Fp5 t4 = square(z1);
    const Fp5 z = square(w1);
    return {t4 * kB4, z, square(w1 + z1) - (t4 + z), twice(x1) - (t4 * kFour + z)};
}

// Constant-time select: a if mask == 0, b if mask == ~0.
Fp5 select(uint64_t mask, const Fp5& a, const Fp5& b) {
    Fp5 r{};
    for (int i = 0; i < 5; ++i) r[i] = a[i] ^ (mask & (a[i] ^ b[i]));
    return r;
}

// k*G, always doing the same operations whatever the bits of k (k is secret).
Point mul_generator(const Scalar& k) {
    Point r = kNeutral;
    for (int bit = 318; bit >= 0; --bit) {  // k < n < 2^319.
        r = point_double(r);
        const Point sum = point_add(r, kGenerator);
        const uint64_t mask = 0 - ((k[bit / 64] >> (bit % 64)) & 1);
        r = {select(mask, r.x, sum.x), select(mask, r.z, sum.z), select(mask, r.u, sum.u), select(mask, r.t, sum.t)};
    }
    return r;
}

Fp5 encode(const Point& p) { return p.t * fp5_inverse(p.u); }

// A hash output (5 canonical limbs, < 2^320) as a scalar.
Scalar scalar_from_fp5(const Fp5& x) { return reduce_scalar(x); }

}  // namespace

Fp5 hash_to_fp5(std::span<const uint64_t> elements) {
    State s{};
    for (size_t i = 0; i < elements.size(); i += kRate) {
        // The rate part is OVERWRITTEN, not added to (as in Go).
        for (size_t j = 0; j < kRate && i + j < elements.size(); ++j) s[j] = elements[i + j];
        permute(s);
    }
    return {s[0], s[1], s[2], s[3], s[4]};
}

Fp5 fp5_mul(const Fp5& a, const Fp5& b) {
    // X^5 = 3: the terms of degree 5..8 fold back multiplied by 3.
    std::array<u128, 5> c{};
    for (int i = 0; i < 5; ++i) {
        for (int j = 0; j < 5; ++j) {
            const uint64_t prod = mul(a[i], b[j]);
            const int k = i + j;
            if (k < 5) c[k] += prod;
            else c[k - 5] += static_cast<u128>(prod) * kW;
        }
    }
    return {reduce(c[0]), reduce(c[1]), reduce(c[2]), reduce(c[3]), reduce(c[4])};
}

Fp5 fp5_inverse(const Fp5& a) {
    if (is_zero(a)) return {};
    // a^(r-1) with r = 1 + p + ... + p^4 is f below; a * f lies in GF(p).
    const Fp5 d = frobenius(a, 1);
    const Fp5 e = d * frobenius(d, 1);
    const Fp5 f = e * frobenius(e, 2);
    const uint64_t g = add(mul(a[0], f[0]),
                           mul(kW, add(add(mul(a[1], f[4]), mul(a[2], f[3])), add(mul(a[3], f[2]), mul(a[4], f[1])))));
    return scale(f, inverse(g));
}

Bytes40 to_bytes(const Fp5& x) {
    Bytes40 out{};
    for (int i = 0; i < 5; ++i) {
        for (int b = 0; b < 8; ++b) out[i * 8 + b] = static_cast<uint8_t>(x[i] >> (8 * b));
    }
    return out;
}

Bytes40 to_bytes_scalar(const Scalar& x) { return to_bytes(x); }

Scalar scalar_from_bytes(std::span<const uint8_t, 40> bytes) {
    Scalar x{};
    for (int i = 0; i < 5; ++i) {
        for (int b = 7; b >= 0; --b) x[i] = (x[i] << 8) | bytes[i * 8 + b];
    }
    return reduce_scalar(x);
}

Scalar scalar_mul(const Scalar& a, const Scalar& b) { return monty_mul(monty_mul(a, kR2), b); }

Scalar scalar_sub(const Scalar& a, const Scalar& b) {
    Scalar d{};
    return sub_borrow(a, b, d) ? add_no_carry(d, kN) : d;
}

Scalar random_scalar() {
    for (;;) {
        std::array<uint8_t, 40> bytes{};
        if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) {
            throw std::runtime_error("lighter: system random generator failed");
        }
        bytes[39] &= 0x7f;  // < 2^319: n is just below, so this rarely retries.
        Scalar k{};
        for (int i = 0; i < 5; ++i) {
            for (int b = 7; b >= 0; --b) k[i] = (k[i] << 8) | bytes[i * 8 + b];
        }
        Scalar unused{};
        if (sub_borrow(k, kN, unused) && (k[0] | k[1] | k[2] | k[3] | k[4]) != 0) return k;
    }
}

Fp5 public_key(const Scalar& sk) { return encode(mul_generator(sk)); }

Signature schnorr_sign(const Fp5& hashed_message, const Scalar& sk, const Scalar& k) {
    const Fp5 r = encode(mul_generator(k));
    const std::array<uint64_t, 10> pre = {r[0], r[1], r[2], r[3], r[4], hashed_message[0], hashed_message[1],
                                          hashed_message[2], hashed_message[3], hashed_message[4]};
    const Scalar e = scalar_from_fp5(hash_to_fp5(pre));
    const Scalar s = scalar_sub(k, scalar_mul(e, sk));

    Signature sig{};
    const Bytes40 s_bytes = to_bytes_scalar(s);
    const Bytes40 e_bytes = to_bytes_scalar(e);
    std::copy(s_bytes.begin(), s_bytes.end(), sig.begin());
    std::copy(e_bytes.begin(), e_bytes.end(), sig.begin() + 40);
    return sig;
}

}  // namespace peewee::lighter
