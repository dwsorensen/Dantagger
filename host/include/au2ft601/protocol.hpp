#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace au2ft601 {

constexpr std::size_t kFrameSize = 4096;
constexpr std::size_t kHeaderSize = 64;
constexpr std::size_t kPayloadSize = 4028;
constexpr std::size_t kTrailerSize = 4;
constexpr std::size_t kPayloadWords = kPayloadSize / 4;
constexpr std::uint16_t kProtocolVersion = 1;
constexpr std::array<std::uint8_t, 8> kMagic{{'A', 'U', '2', 'F', 'T', '6', '0', '1'}};

enum class FrameType : std::uint16_t {
    data = 1,
    command = 2,
    response = 3,
    echo_response = 4,
};

enum class Pattern : std::uint32_t {
    counter = 0,
    walk1 = 1,
    walk0 = 2,
    xorshift32 = 3,
};

struct FrameHeader {
    FrameType type{FrameType::data};
    std::uint64_t run_id{0};
    std::uint64_t sequence_or_transaction{0};
    std::uint64_t payload_offset{0};
    std::uint32_t pattern_or_opcode{0};
    std::uint32_t seed{0};
    std::uint32_t valid_payload_length{0};
    std::uint32_t flags_or_status{0};
    std::uint64_t timestamp{0};
};

using Frame = std::array<std::uint8_t, kFrameSize>;

std::uint32_t crc32_iso_hdlc(const std::uint8_t* data, std::size_t size);
std::uint32_t derive_frame_seed(std::uint32_t run_seed, std::uint64_t sequence);
std::uint32_t pattern_word(Pattern pattern, std::uint64_t sequence,
                           std::size_t word_index, std::uint32_t run_seed);

Frame encode_frame(const FrameHeader& header, const std::uint8_t* payload,
                   std::size_t payload_size);
bool decode_frame(const Frame& frame, FrameHeader& header, std::string& error);

Frame make_data_frame(std::uint64_t run_id, std::uint64_t sequence,
                      Pattern pattern, std::uint32_t run_seed,
                      std::uint64_t timestamp = 0);

bool validate_data_frame(const Frame& frame, std::uint64_t expected_run_id,
                         std::uint64_t expected_sequence, Pattern expected_pattern,
                         std::uint32_t run_seed, std::string& error);

}  // namespace au2ft601

