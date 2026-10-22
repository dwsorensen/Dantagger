#include "au2ft601/protocol.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace au2ft601 {
namespace {

template <typename T>
void put_le(Frame& frame, std::size_t offset, T value) {
    for (std::size_t index = 0; index < sizeof(T); ++index) {
        frame[offset + index] = static_cast<std::uint8_t>(value & static_cast<T>(0xff));
        value = static_cast<T>(value >> 8U);
    }
}

template <typename T>
T get_le(const Frame& frame, std::size_t offset) {
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < sizeof(T); ++index) {
        value |= static_cast<std::uint64_t>(frame[offset + index]) << (8U * index);
    }
    return static_cast<T>(value);
}

bool known_type(FrameType type) {
    switch (type) {
        case FrameType::data:
        case FrameType::command:
        case FrameType::response:
        case FrameType::echo_response:
            return true;
    }
    return false;
}

std::uint32_t xorshift(std::uint32_t value) {
    value ^= value << 13U;
    value ^= value >> 17U;
    value ^= value << 5U;
    return value;
}

// Slicing-by-8 tables for the reflected polynomial 0xedb88320: table[0] is
// the usual byte-at-a-time table and table[k][i] advances table[k-1][i] by one
// more zero byte.  The host checks every received frame's CRC, twice, at the
// USB rate.
using CrcTables = std::array<std::array<std::uint32_t, 256>, 8>;

const CrcTables& crc_tables() {
    static const CrcTables tables = [] {
        CrcTables entries{};
        for (std::uint32_t index = 0; index < 256; ++index) {
            std::uint32_t crc = index;
            for (unsigned bit = 0; bit < 8; ++bit) {
                const std::uint32_t mask = 0U - (crc & 1U);
                crc = (crc >> 1U) ^ (0xedb88320U & mask);
            }
            entries[0][index] = crc;
        }
        for (std::size_t k = 1; k < entries.size(); ++k) {
            for (std::size_t index = 0; index < 256; ++index) {
                const std::uint32_t prior = entries[k - 1][index];
                entries[k][index] = (prior >> 8U) ^ entries[0][prior & 0xffU];
            }
        }
        return entries;
    }();
    return tables;
}

}  // namespace

std::uint32_t crc32_iso_hdlc(const std::uint8_t* data, std::size_t size) {
    const auto& t = crc_tables();
    std::uint32_t crc = 0xffffffffU;
    std::size_t index = 0;
    for (; index + 8 <= size; index += 8) {
        const std::uint8_t* b = data + index;
        const std::uint32_t low = crc ^ (static_cast<std::uint32_t>(b[0]) | (static_cast<std::uint32_t>(b[1]) << 8U) |
                                         (static_cast<std::uint32_t>(b[2]) << 16U) |
                                         (static_cast<std::uint32_t>(b[3]) << 24U));
        crc = t[7][low & 0xffU] ^ t[6][(low >> 8U) & 0xffU] ^ t[5][(low >> 16U) & 0xffU] ^ t[4][low >> 24U] ^
              t[3][b[4]] ^ t[2][b[5]] ^ t[1][b[6]] ^ t[0][b[7]];
    }
    for (; index < size; ++index) {
        crc = (crc >> 8U) ^ t[0][(crc ^ data[index]) & 0xffU];
    }
    return crc ^ 0xffffffffU;
}

std::uint32_t derive_frame_seed(std::uint32_t run_seed, std::uint64_t sequence) {
    const auto low = static_cast<std::uint32_t>(sequence);
    const auto high = static_cast<std::uint32_t>(sequence >> 32U);
    std::uint32_t seed = run_seed ^ low ^ high;
    return seed == 0 ? 1U : seed;
}

std::uint32_t pattern_word(Pattern pattern, std::uint64_t sequence,
                           std::size_t word_index, std::uint32_t run_seed) {
    if (word_index >= kPayloadWords) {
        throw std::out_of_range("payload word index");
    }
    const std::uint64_t global = sequence * kPayloadWords + word_index;
    switch (pattern) {
        case Pattern::counter:
            return static_cast<std::uint32_t>(global);
        case Pattern::walk1:
            return 1U << static_cast<unsigned>(global % 32U);
        case Pattern::walk0:
            return ~(1U << static_cast<unsigned>(global % 32U));
        case Pattern::xorshift32: {
            std::uint32_t value = derive_frame_seed(run_seed, sequence);
            for (std::size_t index = 0; index <= word_index; ++index) {
                value = xorshift(value);
            }
            return value;
        }
    }
    throw std::invalid_argument("unknown pattern");
}

Frame encode_frame(const FrameHeader& header, const std::uint8_t* payload,
                   std::size_t payload_size) {
    if (payload_size > kPayloadSize || header.valid_payload_length > kPayloadSize ||
        payload_size != header.valid_payload_length) {
        throw std::invalid_argument("invalid payload size");
    }
    if (payload_size != 0 && payload == nullptr) {
        throw std::invalid_argument("null payload");
    }

    Frame frame{};
    std::copy(kMagic.begin(), kMagic.end(), frame.begin());
    put_le<std::uint16_t>(frame, 8, kProtocolVersion);
    put_le<std::uint16_t>(frame, 10, static_cast<std::uint16_t>(header.type));
    put_le<std::uint32_t>(frame, 12, static_cast<std::uint32_t>(kFrameSize));
    put_le<std::uint64_t>(frame, 16, header.run_id);
    put_le<std::uint64_t>(frame, 24, header.sequence_or_transaction);
    put_le<std::uint64_t>(frame, 32, header.payload_offset);
    put_le<std::uint32_t>(frame, 40, header.pattern_or_opcode);
    put_le<std::uint32_t>(frame, 44, header.seed);
    put_le<std::uint32_t>(frame, 48, header.valid_payload_length);
    put_le<std::uint32_t>(frame, 52, header.flags_or_status);
    put_le<std::uint64_t>(frame, 56, header.timestamp);
    if (payload_size != 0) {
        std::copy(payload, payload + payload_size, frame.begin() + kHeaderSize);
    }
    put_le<std::uint32_t>(frame, kFrameSize - kTrailerSize,
                          crc32_iso_hdlc(frame.data(), kFrameSize - kTrailerSize));
    return frame;
}

bool decode_frame(const Frame& frame, FrameHeader& header, std::string& error) {
    if (!std::equal(kMagic.begin(), kMagic.end(), frame.begin())) {
        error = "bad magic";
        return false;
    }
    if (get_le<std::uint16_t>(frame, 8) != kProtocolVersion) {
        error = "unsupported protocol version";
        return false;
    }
    const auto type = static_cast<FrameType>(get_le<std::uint16_t>(frame, 10));
    if (!known_type(type)) {
        error = "unknown frame type";
        return false;
    }
    if (get_le<std::uint32_t>(frame, 12) != kFrameSize) {
        error = "bad total frame length";
        return false;
    }
    const auto payload_length = get_le<std::uint32_t>(frame, 48);
    if (payload_length > kPayloadSize) {
        error = "invalid payload length";
        return false;
    }
    const auto expected_crc = get_le<std::uint32_t>(frame, kFrameSize - kTrailerSize);
    const auto actual_crc = crc32_iso_hdlc(frame.data(), kFrameSize - kTrailerSize);
    if (actual_crc != expected_crc) {
        error = "CRC mismatch";
        return false;
    }

    header.type = type;
    header.run_id = get_le<std::uint64_t>(frame, 16);
    header.sequence_or_transaction = get_le<std::uint64_t>(frame, 24);
    header.payload_offset = get_le<std::uint64_t>(frame, 32);
    header.pattern_or_opcode = get_le<std::uint32_t>(frame, 40);
    header.seed = get_le<std::uint32_t>(frame, 44);
    header.valid_payload_length = payload_length;
    header.flags_or_status = get_le<std::uint32_t>(frame, 52);
    header.timestamp = get_le<std::uint64_t>(frame, 56);
    error.clear();
    return true;
}

Frame make_data_frame(std::uint64_t run_id, std::uint64_t sequence,
                      Pattern pattern, std::uint32_t run_seed,
                      std::uint64_t timestamp) {
    std::array<std::uint8_t, kPayloadSize> payload{};
    std::uint32_t prng_state = derive_frame_seed(run_seed, sequence);
    for (std::size_t word = 0; word < kPayloadWords; ++word) {
        const auto value = pattern == Pattern::xorshift32
                               ? (prng_state = xorshift(prng_state))
                               : pattern_word(pattern, sequence, word, run_seed);
        for (std::size_t byte = 0; byte < 4; ++byte) {
            payload[word * 4 + byte] =
                static_cast<std::uint8_t>((value >> (8U * byte)) & 0xffU);
        }
    }

    FrameHeader header;
    header.type = FrameType::data;
    header.run_id = run_id;
    header.sequence_or_transaction = sequence;
    header.payload_offset = sequence * kPayloadSize;
    header.pattern_or_opcode = static_cast<std::uint32_t>(pattern);
    header.seed = pattern == Pattern::xorshift32
                      ? derive_frame_seed(run_seed, sequence)
                      : 0U;
    header.valid_payload_length = static_cast<std::uint32_t>(kPayloadSize);
    header.timestamp = timestamp;
    return encode_frame(header, payload.data(), payload.size());
}

bool validate_data_frame(const Frame& frame, std::uint64_t expected_run_id,
                         std::uint64_t expected_sequence, Pattern expected_pattern,
                         std::uint32_t run_seed, std::string& error) {
    FrameHeader header;
    if (!decode_frame(frame, header, error)) {
        return false;
    }
    if (header.type != FrameType::data) {
        error = "expected DATA frame";
        return false;
    }
    if (header.run_id != expected_run_id) {
        error = "run ID mismatch";
        return false;
    }
    if (header.sequence_or_transaction != expected_sequence) {
        error = "sequence mismatch";
        return false;
    }
    if (header.payload_offset != expected_sequence * kPayloadSize) {
        error = "payload offset mismatch";
        return false;
    }
    if (header.pattern_or_opcode != static_cast<std::uint32_t>(expected_pattern)) {
        error = "pattern ID mismatch";
        return false;
    }
    if (header.valid_payload_length != kPayloadSize) {
        error = "DATA payload length mismatch";
        return false;
    }
    if (header.flags_or_status != 0) {
        error = "unexpected DATA flags";
        return false;
    }
    const auto expected_seed = expected_pattern == Pattern::xorshift32
                                   ? derive_frame_seed(run_seed, expected_sequence)
                                   : 0U;
    if (header.seed != expected_seed) {
        error = "frame seed mismatch";
        return false;
    }

    std::uint32_t prng_state = derive_frame_seed(run_seed, expected_sequence);
    for (std::size_t word = 0; word < kPayloadWords; ++word) {
        const auto expected = expected_pattern == Pattern::xorshift32
                                  ? (prng_state = xorshift(prng_state))
                                  : pattern_word(expected_pattern, expected_sequence, word, run_seed);
        const std::size_t offset = kHeaderSize + word * 4;
        const std::uint32_t actual =
            static_cast<std::uint32_t>(frame[offset]) |
            (static_cast<std::uint32_t>(frame[offset + 1]) << 8U) |
            (static_cast<std::uint32_t>(frame[offset + 2]) << 16U) |
            (static_cast<std::uint32_t>(frame[offset + 3]) << 24U);
        if (actual != expected) {
            error = "payload mismatch at word " + std::to_string(word);
            return false;
        }
    }
    error.clear();
    return true;
}

}  // namespace au2ft601
