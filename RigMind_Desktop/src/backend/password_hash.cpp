#include "password_hash.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#else
#include <cstdio>
#endif

namespace rigmind {

namespace {

constexpr std::array<std::uint32_t, 64> kK = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
    0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
    0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
    0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
    0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

inline std::uint32_t rotr(std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

struct Sha256 {
    std::array<std::uint32_t, 8> h{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::array<std::uint8_t, 64> buf{};
    std::size_t used = 0;
    std::uint64_t total = 0;

    void block(const std::uint8_t* p) {
        std::uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = (std::uint32_t(p[i * 4]) << 24) | (std::uint32_t(p[i * 4 + 1]) << 16) | (std::uint32_t(p[i * 4 + 2]) << 8) | p[i * 4 + 3];
        for (int i = 16; i < 64; ++i) {
            const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            const std::uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + kK[i] + w[i];
            const std::uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }

    void update(const std::uint8_t* data, std::size_t len) {
        total += len;
        while (len > 0) {
            const std::size_t take = std::min<std::size_t>(64 - used, len);
            std::memcpy(buf.data() + used, data, take);
            used += take; data += take; len -= take;
            if (used == 64) { block(buf.data()); used = 0; }
        }
    }

    Bytes finish() {
        const std::uint64_t bits = total * 8;
        std::uint8_t pad = 0x80;
        update(&pad, 1);
        const std::uint8_t zero = 0;
        while (used != 56) update(&zero, 1);
        std::uint8_t len[8];
        for (int i = 0; i < 8; ++i) len[i] = static_cast<std::uint8_t>(bits >> (56 - 8 * i));
        update(len, 8);
        Bytes out(32);
        for (int i = 0; i < 8; ++i) {
            out[i * 4] = static_cast<std::uint8_t>(h[i] >> 24);
            out[i * 4 + 1] = static_cast<std::uint8_t>(h[i] >> 16);
            out[i * 4 + 2] = static_cast<std::uint8_t>(h[i] >> 8);
            out[i * 4 + 3] = static_cast<std::uint8_t>(h[i]);
        }
        return out;
    }
};

// HMAC with the key pads already absorbed, so PBKDF2 pays two compressions per iteration instead of four.
struct HmacKey {
    Sha256 inner, outer;
    explicit HmacKey(const Bytes& key_in) {
        Bytes key = key_in.size() > 64 ? sha256(key_in) : key_in;
        key.resize(64, 0);
        std::uint8_t ipad[64], opad[64];
        for (int i = 0; i < 64; ++i) { ipad[i] = key[i] ^ 0x36; opad[i] = key[i] ^ 0x5c; }
        inner.update(ipad, 64);
        outer.update(opad, 64);
    }
    Bytes mac(const std::uint8_t* data, std::size_t len) const {
        Sha256 i = inner;
        i.update(data, len);
        const Bytes ih = i.finish();
        Sha256 o = outer;
        o.update(ih.data(), ih.size());
        return o.finish();
    }
};

} // namespace

Bytes sha256(const Bytes& data) {
    Sha256 s;
    s.update(data.data(), data.size());
    return s.finish();
}

Bytes hmac_sha256(const Bytes& key, const Bytes& data) { return HmacKey(key).mac(data.data(), data.size()); }

Bytes pbkdf2_sha256(const std::string& password, const Bytes& salt, std::uint32_t iterations, std::size_t dk_len) {
    if (iterations == 0) throw std::invalid_argument("iterations must be at least 1");
    const HmacKey key(Bytes(password.begin(), password.end()));
    Bytes out;
    for (std::uint32_t block = 1; out.size() < dk_len; ++block) {
        Bytes msg = salt;
        msg.push_back(static_cast<std::uint8_t>(block >> 24));
        msg.push_back(static_cast<std::uint8_t>(block >> 16));
        msg.push_back(static_cast<std::uint8_t>(block >> 8));
        msg.push_back(static_cast<std::uint8_t>(block));
        Bytes u = key.mac(msg.data(), msg.size());
        Bytes t = u;
        for (std::uint32_t i = 1; i < iterations; ++i) {
            u = key.mac(u.data(), u.size());
            for (std::size_t k = 0; k < t.size(); ++k) t[k] ^= u[k];
        }
        out.insert(out.end(), t.begin(), t.end());
    }
    out.resize(dk_len);
    return out;
}

Bytes random_bytes(std::size_t count) {
    Bytes out(count);
#ifdef _WIN32
    if (BCryptGenRandom(nullptr, out.data(), static_cast<ULONG>(count), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) throw std::runtime_error("no system random source");
#else
    std::FILE* f = std::fopen("/dev/urandom", "rb");
    if (!f || std::fread(out.data(), 1, count, f) != count) {
        if (f) std::fclose(f);
        throw std::runtime_error("no system random source");
    }
    std::fclose(f);
#endif
    return out;
}

std::string to_hex(const Bytes& bytes) {
    static const char* kDigits = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (std::uint8_t b : bytes) { out += kDigits[b >> 4]; out += kDigits[b & 15]; }
    return out;
}

Bytes from_hex(const std::string& hex) {
    if (hex.size() % 2 != 0) throw std::invalid_argument("odd hex length");
    auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        throw std::invalid_argument("bad hex digit");
    };
    Bytes out;
    for (std::size_t i = 0; i < hex.size(); i += 2) out.push_back(static_cast<std::uint8_t>(digit(hex[i]) * 16 + digit(hex[i + 1])));
    return out;
}

bool constant_time_equal(const Bytes& a, const Bytes& b) {
    unsigned diff = static_cast<unsigned>(a.size() ^ b.size());
    for (std::size_t i = 0; i < (a.size() < b.size() ? a.size() : b.size()); ++i) diff |= static_cast<unsigned>(a[i] ^ b[i]);
    return diff == 0;
}

} // namespace rigmind
