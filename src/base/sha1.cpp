#include "base/sha1.h"

#include <cstring>
#include <string>

namespace manta {

namespace {

constexpr std::uint32_t rotl(std::uint32_t v, int n) noexcept {
    return (v << n) | (v >> (32 - n));
}

// One 512-bit block, FIPS 180-4 section 6.1.2.
void compress(std::uint32_t h[5], const std::uint8_t block[64]) noexcept {
    std::uint32_t w[80];
    for (int i = 0; i < 16; ++i) {
        w[i] = static_cast<std::uint32_t>(block[i * 4]) << 24 |
               static_cast<std::uint32_t>(block[i * 4 + 1]) << 16 |
               static_cast<std::uint32_t>(block[i * 4 + 2]) << 8 |
               static_cast<std::uint32_t>(block[i * 4 + 3]);
    }
    for (int i = 16; i < 80; ++i) {
        w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }

    std::uint32_t a = h[0];
    std::uint32_t b = h[1];
    std::uint32_t c = h[2];
    std::uint32_t d = h[3];
    std::uint32_t e = h[4];

    for (int i = 0; i < 80; ++i) {
        std::uint32_t f = 0;
        std::uint32_t k = 0;
        if (i < 20) {
            f = (b & c) | (~b & d);
            k = 0x5a827999u;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ed9eba1u;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8f1bbcdcu;
        } else {
            f = b ^ c ^ d;
            k = 0xca62c1d6u;
        }
        std::uint32_t t = rotl(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rotl(b, 30);
        b = a;
        a = t;
    }

    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
}

}  // namespace

Sha1Digest sha1(std::string_view data) noexcept {
    std::uint32_t h[5] = {0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u, 0xc3d2e1f0u};

    const auto* bytes = reinterpret_cast<const std::uint8_t*>(data.data());
    std::size_t whole = data.size() / 64;
    for (std::size_t i = 0; i < whole; ++i) compress(h, bytes + i * 64);

    // The tail, its 0x80 terminator and the 64-bit big-endian bit length. Both
    // fit in one block unless the remainder leaves fewer than nine bytes.
    std::uint8_t tail[128] = {};
    std::size_t rest = data.size() - whole * 64;
    if (rest != 0) std::memcpy(tail, bytes + whole * 64, rest);
    tail[rest] = 0x80;
    std::size_t blocks = rest < 56 ? 1 : 2;

    std::uint64_t bits = static_cast<std::uint64_t>(data.size()) * 8;
    for (int i = 0; i < 8; ++i) {
        tail[blocks * 64 - 1 - static_cast<std::size_t>(i)] =
            static_cast<std::uint8_t>(bits >> (8 * i));
    }
    for (std::size_t i = 0; i < blocks; ++i) compress(h, tail + i * 64);

    Sha1Digest out{};
    for (int i = 0; i < 5; ++i) {
        out[static_cast<std::size_t>(i) * 4] = static_cast<std::uint8_t>(h[i] >> 24);
        out[static_cast<std::size_t>(i) * 4 + 1] = static_cast<std::uint8_t>(h[i] >> 16);
        out[static_cast<std::size_t>(i) * 4 + 2] = static_cast<std::uint8_t>(h[i] >> 8);
        out[static_cast<std::size_t>(i) * 4 + 3] = static_cast<std::uint8_t>(h[i]);
    }
    return out;
}

std::string sha1Hex(std::string_view data) {
    static constexpr char kHex[] = "0123456789abcdef";
    Sha1Digest digest = sha1(data);
    std::string out;
    out.reserve(40);
    for (std::uint8_t b : digest) {
        out += kHex[b >> 4];
        out += kHex[b & 0x0f];
    }
    return out;
}

}  // namespace manta
