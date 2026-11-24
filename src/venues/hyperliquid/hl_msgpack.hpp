#pragma once
// Minimal msgpack writer: only what Hyperliquid actions need (maps, arrays,
// strings, unsigned integers, booleans).
//
// Hyperliquid signs the msgpack bytes of the action, so they must match
// Python's msgpack.packb() exactly: always the SMALLEST encoding for each
// value, and keys in the same order the action JSON uses.
// tests/test_hl_signer.cpp compares the output with the Python SDK.

#include <cstdint>
#include <string>
#include <string_view>

namespace peewee::hyperliquid {

class MsgpackWriter {
public:
    // A map header: write `n` key/value pairs right after it.
    void map(uint32_t n) {
        if (n < 16) {
            out_ += static_cast<char>(0x80 | n);
        } else if (n < 0x10000) {
            out_ += static_cast<char>(0xde);
            big_endian(n, 2);
        } else {
            out_ += static_cast<char>(0xdf);
            big_endian(n, 4);
        }
    }

    // An array header: write `n` values right after it.
    void array(uint32_t n) {
        if (n < 16) {
            out_ += static_cast<char>(0x90 | n);
        } else if (n < 0x10000) {
            out_ += static_cast<char>(0xdc);
            big_endian(n, 2);
        } else {
            out_ += static_cast<char>(0xdd);
            big_endian(n, 4);
        }
    }

    void str(std::string_view s) {
        const auto n = s.size();
        if (n < 32) {
            out_ += static_cast<char>(0xa0 | n);
        } else if (n < 0x100) {
            out_ += static_cast<char>(0xd9);
            big_endian(n, 1);
        } else if (n < 0x10000) {
            out_ += static_cast<char>(0xda);
            big_endian(n, 2);
        } else {
            out_ += static_cast<char>(0xdb);
            big_endian(n, 4);
        }
        out_ += s;
    }

    void uint(uint64_t v) {
        if (v < 0x80) {
            out_ += static_cast<char>(v);  // "positive fixint": the value itself.
        } else if (v < 0x100) {
            out_ += static_cast<char>(0xcc);
            big_endian(v, 1);
        } else if (v < 0x10000) {
            out_ += static_cast<char>(0xcd);
            big_endian(v, 2);
        } else if (v < 0x100000000ULL) {
            out_ += static_cast<char>(0xce);
            big_endian(v, 4);
        } else {
            out_ += static_cast<char>(0xcf);
            big_endian(v, 8);
        }
    }

    void boolean(bool b) { out_ += static_cast<char>(b ? 0xc3 : 0xc2); }

    const std::string& bytes() const { return out_; }

private:
    // Writes the lowest `n` bytes of v, most significant first.
    void big_endian(uint64_t v, int n) {
        for (int i = n - 1; i >= 0; --i) {
            out_ += static_cast<char>((v >> (8 * i)) & 0xff);
        }
    }

    std::string out_;
};

}  // namespace peewee::hyperliquid
