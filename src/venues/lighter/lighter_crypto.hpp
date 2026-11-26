#pragma once
// The cryptography behind Lighter signatures. Pure math, no I/O.
//
// Lighter does not use Ethereum signatures for orders. Its API keys are
// Schnorr keys on ECgFp5, an elliptic curve over GF(p^5) where
// p = 2^64 - 2^32 + 1 ("Goldilocks"), and messages are hashed with Poseidon2
// over the same field:
//   1. The transaction's fields, as field elements -> Poseidon2 -> one Fp5
//      element (the "tx hash", 40 bytes).
//   2. Schnorr: r = k*G (encoded as an Fp5 element),
//      e = Poseidon2(r || hash) as a scalar, s = k - e*sk.
//      Signature = s || e, 40 + 40 bytes little-endian.
//
// Ported from github.com/elliottech/poseidon_crypto v0.0.15 (the version
// lighter-go uses) and checked byte for byte against it in
// tests/test_lighter_signer.cpp.

#include <array>
#include <cstdint>
#include <span>

namespace peewee::lighter {

// An element of GF(p^5): five Goldilocks limbs, always canonical (< p).
using Fp5 = std::array<uint64_t, 5>;

// A scalar modulo the curve order n (about 2^319): five 64-bit limbs,
// little-endian, always < n.
using Scalar = std::array<uint64_t, 5>;

using Bytes40 = std::array<uint8_t, 40>;
using Signature = std::array<uint8_t, 80>;  // s || e, both little-endian.

// Poseidon2 (width 12, rate 8) of field elements, squeezed to 5 elements.
// Inputs must be canonical (< p). An empty input hashes to zero, as in Go.
Fp5 hash_to_fp5(std::span<const uint64_t> elements);

// Fp5 arithmetic, exposed for tests.
Fp5 fp5_mul(const Fp5& a, const Fp5& b);
Fp5 fp5_inverse(const Fp5& a);  // 0 for 0.

Bytes40 to_bytes(const Fp5& x);                 // 5 limbs, little-endian.
Bytes40 to_bytes_scalar(const Scalar& x);
// 40 little-endian bytes, reduced modulo n (as Go's ScalarElementFromLittleEndianBytes).
Scalar scalar_from_bytes(std::span<const uint8_t, 40> bytes);
Scalar scalar_mul(const Scalar& a, const Scalar& b);
Scalar scalar_sub(const Scalar& a, const Scalar& b);

// A uniformly random nonzero scalar (OpenSSL RAND_bytes). Throws if the
// system RNG fails.
Scalar random_scalar();

// The public key of `sk`: (sk*G) encoded as one Fp5 element.
Fp5 public_key(const Scalar& sk);

// Schnorr signature of an already hashed message, with the nonce `k`.
// k must be secret, random and never reused: production code passes
// random_scalar(); tests pass a fixed k to compare with Go's output.
Signature schnorr_sign(const Fp5& hashed_message, const Scalar& sk, const Scalar& k);

}  // namespace peewee::lighter
