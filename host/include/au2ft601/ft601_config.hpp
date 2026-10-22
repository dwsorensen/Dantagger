#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace au2ft601 {

// Raw FT_60XCONFIGURATION image exactly as returned by FT_GetChipConfiguration
// on little-endian x86-64 (D3XX 1.1.8). The D3XX adapter static_asserts that
// the SDK structure matches these offsets, so this code never needs FTDI headers.
constexpr std::size_t kFt601ConfigSize = 152;
using ConfigImage = std::array<std::uint8_t, kFt601ConfigSize>;

namespace config_offset {
constexpr std::size_t vendor_id = 0;
constexpr std::size_t product_id = 2;
constexpr std::size_t string_descriptors = 4;
constexpr std::size_t string_descriptors_size = 128;
constexpr std::size_t b_interval = 132;
constexpr std::size_t power_attributes = 133;
constexpr std::size_t power_consumption = 134;
constexpr std::size_t reserved2 = 136;
constexpr std::size_t fifo_clock = 137;
constexpr std::size_t fifo_mode = 138;
constexpr std::size_t channel_config = 139;
constexpr std::size_t optional_features = 140;
constexpr std::size_t battery_charging = 142;
constexpr std::size_t flash_detection = 143;
constexpr std::size_t msio_control = 144;
constexpr std::size_t gpio_control = 148;
}  // namespace config_offset

// Mirrors the vendored CONFIGURATION_FIFO_CLK enum (checked in the D3XX adapter).
enum class FifoClock : std::uint8_t { mhz100 = 0, mhz66 = 1, mhz50 = 2, mhz40 = 3 };
constexpr std::uint8_t kFifoMode245 = 0;
constexpr std::uint8_t kChannelConfig1Bidirectional = 2;
constexpr std::uint16_t kFtdiVendorId = 0x0403;
constexpr std::uint16_t kFt601ProductId = 0x601f;

struct ConfigField {
    const char* name;
    std::size_t offset;
    std::size_t size;
};

// Every byte of the image belongs to exactly one field, in offset order.
const std::vector<ConfigField>& config_fields();

struct DecodedConfig {
    std::uint16_t vendor_id{};
    std::uint16_t product_id{};
    std::string manufacturer;
    std::string description;
    std::string serial;
    bool strings_valid{false};
    std::uint8_t fifo_clock{};
    std::uint8_t fifo_mode{};
    std::uint8_t channel_config{};
    std::uint16_t optional_features{};
};

DecodedConfig decode_config(const ConfigImage& image);
std::string fifo_clock_name(std::uint8_t raw);
std::string describe_config(const ConfigImage& image);

struct FieldDiff {
    std::string field;
    std::size_t offset{};
    std::size_t size{};
    std::vector<std::uint8_t> before;
    std::vector<std::uint8_t> after;
};

std::vector<FieldDiff> diff_config(const ConfigImage& before, const ConfigImage& after);
std::string format_diff(const std::vector<FieldDiff>& diffs);

enum class ClockChange { changed, already_at_target, refused };

// Produces `proposed` from `current` with only FIFOClock altered. Refuses unless
// the image is an FT601 in 245 mode with one bidirectional channel, a valid
// string-descriptor block and a recognized current clock; only 66.67 MHz and
// 100 MHz are accepted targets.
ClockChange make_clock_change(const ConfigImage& current, FifoClock target, ConfigImage& proposed,
                              std::string& reason);

bool load_config_image(const std::filesystem::path& path, ConfigImage& image, std::string& error);
std::string config_sha256(const ConfigImage& image);

}  // namespace au2ft601
