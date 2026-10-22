#include "au2ft601/sha256.hpp"

#include <cstring>

namespace au2ft601 {
namespace {

constexpr std::array<std::uint32_t, 64> kRound{{
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
}};

std::uint32_t rotr(std::uint32_t value, unsigned bits) {
    return (value >> bits) | (value << (32U - bits));
}

void compress(std::array<std::uint32_t, 8>& state, const std::uint8_t* block) {
    std::array<std::uint32_t, 64> w{};
    for (std::size_t i = 0; i < 16; ++i) {
        w[i] = (static_cast<std::uint32_t>(block[4 * i]) << 24U) |
               (static_cast<std::uint32_t>(block[4 * i + 1]) << 16U) |
               (static_cast<std::uint32_t>(block[4 * i + 2]) << 8U) |
               static_cast<std::uint32_t>(block[4 * i + 3]);
    }
    for (std::size_t i = 16; i < 64; ++i) {
        const auto s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3U);
        const auto s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10U);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    auto a = state[0], b = state[1], c = state[2], d = state[3];
    auto e = state[4], f = state[5], g = state[6], h = state[7];
    for (std::size_t i = 0; i < 64; ++i) {
        const auto t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + kRound[i] + w[i];
        const auto t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

}  // namespace

Sha256Digest sha256(const std::uint8_t* data, std::size_t size) {
    std::array<std::uint32_t, 8> state{{0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
                                        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U}};
    std::size_t offset = 0;
    for (; size - offset >= 64; offset += 64) {
        compress(state, data + offset);
    }
    std::array<std::uint8_t, 128> tail{};
    const std::size_t remaining = size - offset;
    if (remaining != 0) {
        std::memcpy(tail.data(), data + offset, remaining);
    }
    tail[remaining] = 0x80;
    const std::size_t tail_size = remaining < 56 ? 64 : 128;
    const std::uint64_t bits = static_cast<std::uint64_t>(size) * 8U;
    for (std::size_t i = 0; i < 8; ++i) {
        tail[tail_size - 1 - i] = static_cast<std::uint8_t>(bits >> (8U * i));
    }
    compress(state, tail.data());
    if (tail_size == 128) {
        compress(state, tail.data() + 64);
    }
    Sha256Digest digest{};
    for (std::size_t i = 0; i < 8; ++i) {
        digest[4 * i] = static_cast<std::uint8_t>(state[i] >> 24U);
        digest[4 * i + 1] = static_cast<std::uint8_t>(state[i] >> 16U);
        digest[4 * i + 2] = static_cast<std::uint8_t>(state[i] >> 8U);
        digest[4 * i + 3] = static_cast<std::uint8_t>(state[i]);
    }
    return digest;
}

std::string sha256_hex(const std::uint8_t* data, std::size_t size) {
    static constexpr char kHex[] = "0123456789abcdef";
    const auto digest = sha256(data, size);
    std::string text;
    text.reserve(64);
    for (const auto byte : digest) {
        text.push_back(kHex[byte >> 4U]);
        text.push_back(kHex[byte & 0x0fU]);
    }
    return text;
}

}  // namespace au2ft601
