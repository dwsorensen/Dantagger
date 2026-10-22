#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace au2ft601 {

using Sha256Digest = std::array<std::uint8_t, 32>;

// Dependency-free SHA-256 used to fingerprint configuration backups and
// captured evidence. Not intended for high-volume hashing.
Sha256Digest sha256(const std::uint8_t* data, std::size_t size);
std::string sha256_hex(const std::uint8_t* data, std::size_t size);

}  // namespace au2ft601
