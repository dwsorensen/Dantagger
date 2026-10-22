#include "au2ft601/ft601_config.hpp"

#include "au2ft601/sha256.hpp"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>

namespace au2ft601 {
namespace {

std::uint64_t get_le(const ConfigImage& image, std::size_t offset, std::size_t size) {
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < size; ++index) {
        value |= static_cast<std::uint64_t>(image[offset + index]) << (8U * index);
    }
    return value;
}

std::string hex(std::uint64_t value, int width) {
    std::ostringstream out;
    out << "0x" << std::hex << std::setfill('0') << std::setw(width) << value;
    return out.str();
}

// Decodes one USB string descriptor (bLength, 0x03, UTF-16LE) holding ASCII.
bool read_string_descriptor(const ConfigImage& image, std::size_t& offset, std::size_t end,
                            std::string& text) {
    if (offset + 2 > end) {
        return false;
    }
    const std::size_t length = image[offset];
    if (length < 2 || length % 2 != 0 || image[offset + 1] != 0x03 || offset + length > end) {
        return false;
    }
    text.clear();
    for (std::size_t index = offset + 2; index < offset + length; index += 2) {
        if (image[index + 1] != 0 || image[index] < 0x20 || image[index] > 0x7e) {
            return false;
        }
        text.push_back(static_cast<char>(image[index]));
    }
    offset += length;
    return true;
}

std::string fifo_mode_name(std::uint8_t raw) {
    switch (raw) {
        case 0: return "245 synchronous FIFO";
        case 1: return "600 FIFO";
        default: return "unknown";
    }
}

std::string channel_config_name(std::uint8_t raw) {
    switch (raw) {
        case 0: return "4 bidirectional channels";
        case 1: return "2 bidirectional channels";
        case 2: return "1 bidirectional channel";
        case 3: return "1 OUT-only channel";
        case 4: return "1 IN-only channel";
        default: return "unknown";
    }
}

std::string bytes_hex(const std::vector<std::uint8_t>& bytes) {
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        out << (index == 0 ? "" : " ") << std::setw(2) << static_cast<unsigned>(bytes[index]);
    }
    return out.str();
}

}  // namespace

const std::vector<ConfigField>& config_fields() {
    namespace o = config_offset;
    static const std::vector<ConfigField> fields{
        {"VendorID", o::vendor_id, 2},
        {"ProductID", o::product_id, 2},
        {"StringDescriptors", o::string_descriptors, o::string_descriptors_size},
        {"bInterval", o::b_interval, 1},
        {"PowerAttributes", o::power_attributes, 1},
        {"PowerConsumption", o::power_consumption, 2},
        {"Reserved2", o::reserved2, 1},
        {"FIFOClock", o::fifo_clock, 1},
        {"FIFOMode", o::fifo_mode, 1},
        {"ChannelConfig", o::channel_config, 1},
        {"OptionalFeatureSupport", o::optional_features, 2},
        {"BatteryChargingGPIOConfig", o::battery_charging, 1},
        {"FlashEEPROMDetection", o::flash_detection, 1},
        {"MSIO_Control", o::msio_control, 4},
        {"GPIO_Control", o::gpio_control, 4},
    };
    return fields;
}

DecodedConfig decode_config(const ConfigImage& image) {
    namespace o = config_offset;
    DecodedConfig decoded;
    decoded.vendor_id = static_cast<std::uint16_t>(get_le(image, o::vendor_id, 2));
    decoded.product_id = static_cast<std::uint16_t>(get_le(image, o::product_id, 2));
    decoded.fifo_clock = image[o::fifo_clock];
    decoded.fifo_mode = image[o::fifo_mode];
    decoded.channel_config = image[o::channel_config];
    decoded.optional_features = static_cast<std::uint16_t>(get_le(image, o::optional_features, 2));
    std::size_t offset = o::string_descriptors;
    const std::size_t end = o::string_descriptors + o::string_descriptors_size;
    decoded.strings_valid = read_string_descriptor(image, offset, end, decoded.manufacturer) &&
                            read_string_descriptor(image, offset, end, decoded.description) &&
                            read_string_descriptor(image, offset, end, decoded.serial);
    return decoded;
}

std::string fifo_clock_name(std::uint8_t raw) {
    switch (raw) {
        case 0: return "100 MHz";
        case 1: return "66.67 MHz";
        case 2: return "50 MHz";
        case 3: return "40 MHz";
        default: return "unknown";
    }
}

std::string describe_config(const ConfigImage& image) {
    namespace o = config_offset;
    const auto d = decode_config(image);
    std::ostringstream out;
    out << "sha256: " << config_sha256(image) << '\n'
        << "VID:PID: " << hex(d.vendor_id, 4) << ':' << hex(d.product_id, 4) << '\n';
    if (d.strings_valid) {
        out << "manufacturer: \"" << d.manufacturer << "\"\ndescription: \"" << d.description
            << "\"\nserial: \"" << d.serial << "\"\n";
    } else {
        out << "string descriptors: NOT DECODABLE\n";
    }
    out << "bInterval: " << static_cast<unsigned>(image[o::b_interval]) << '\n'
        << "PowerAttributes: " << hex(image[o::power_attributes], 2) << '\n'
        << "PowerConsumption: " << get_le(image, o::power_consumption, 2) << '\n'
        << "Reserved2: " << hex(image[o::reserved2], 2) << '\n'
        << "FIFOClock: " << fifo_clock_name(d.fifo_clock) << " [raw=" << static_cast<unsigned>(d.fifo_clock) << "]\n"
        << "FIFOMode: " << fifo_mode_name(d.fifo_mode) << " [raw=" << static_cast<unsigned>(d.fifo_mode) << "]\n"
        << "ChannelConfig: " << channel_config_name(d.channel_config) << " [raw="
        << static_cast<unsigned>(d.channel_config) << "]\n"
        << "OptionalFeatureSupport: " << hex(d.optional_features, 4) << '\n'
        << "BatteryChargingGPIOConfig: " << hex(image[o::battery_charging], 2) << '\n'
        << "FlashEEPROMDetection (read-only): " << hex(image[o::flash_detection], 2) << '\n'
        << "MSIO_Control: " << hex(get_le(image, o::msio_control, 4), 8) << '\n'
        << "GPIO_Control: " << hex(get_le(image, o::gpio_control, 4), 8) << '\n';
    return out.str();
}

std::vector<FieldDiff> diff_config(const ConfigImage& before, const ConfigImage& after) {
    std::vector<FieldDiff> diffs;
    for (const auto& field : config_fields()) {
        const auto first = static_cast<std::ptrdiff_t>(field.offset);
        const auto last = static_cast<std::ptrdiff_t>(field.offset + field.size);
        if (!std::equal(before.begin() + first, before.begin() + last, after.begin() + first)) {
            diffs.push_back(FieldDiff{field.name, field.offset, field.size,
                                      {before.begin() + first, before.begin() + last},
                                      {after.begin() + first, after.begin() + last}});
        }
    }
    return diffs;
}

std::string format_diff(const std::vector<FieldDiff>& diffs) {
    if (diffs.empty()) {
        return "no differences (all 152 bytes identical)\n";
    }
    std::ostringstream out;
    for (const auto& diff : diffs) {
        out << diff.field << " @" << diff.offset << " (" << diff.size << " byte" << (diff.size == 1 ? "" : "s")
            << "): " << bytes_hex(diff.before) << " -> " << bytes_hex(diff.after);
        if (diff.field == "FIFOClock") {
            out << "  (" << fifo_clock_name(diff.before[0]) << " -> " << fifo_clock_name(diff.after[0]) << ')';
        }
        out << '\n';
    }
    return out.str();
}

ClockChange make_clock_change(const ConfigImage& current, FifoClock target, ConfigImage& proposed,
                              std::string& reason) {
    const auto d = decode_config(current);
    const auto target_raw = static_cast<std::uint8_t>(target);
    if (target != FifoClock::mhz66 && target != FifoClock::mhz100) {
        reason = "only 66.67 MHz and 100 MHz targets are permitted";
        return ClockChange::refused;
    }
    if (d.vendor_id != kFtdiVendorId || d.product_id != kFt601ProductId) {
        reason = "configuration is not an FT601 with FTDI default VID:PID 0403:601f";
        return ClockChange::refused;
    }
    if (!d.strings_valid) {
        reason = "string descriptor block is not decodable; refusing to rewrite identity";
        return ClockChange::refused;
    }
    if (d.fifo_mode != kFifoMode245) {
        reason = "FIFOMode is not 245 synchronous FIFO";
        return ClockChange::refused;
    }
    if (d.channel_config != kChannelConfig1Bidirectional) {
        reason = "ChannelConfig is not 1 bidirectional channel";
        return ClockChange::refused;
    }
    if (d.fifo_clock > static_cast<std::uint8_t>(FifoClock::mhz40)) {
        reason = "current FIFOClock value is not a recognized enum";
        return ClockChange::refused;
    }
    proposed = current;
    if (d.fifo_clock == target_raw) {
        reason = "FIFOClock is already " + fifo_clock_name(target_raw);
        return ClockChange::already_at_target;
    }
    proposed[config_offset::fifo_clock] = target_raw;
    reason.clear();
    return ClockChange::changed;
}

bool load_config_image(const std::filesystem::path& path, ConfigImage& image, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "cannot open " + path.string();
        return false;
    }
    const std::vector<char> bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    if (bytes.size() != kFt601ConfigSize) {
        error = path.string() + " is " + std::to_string(bytes.size()) + " bytes, expected " +
                std::to_string(kFt601ConfigSize);
        return false;
    }
    std::copy(bytes.begin(), bytes.end(), reinterpret_cast<char*>(image.data()));
    return true;
}

std::string config_sha256(const ConfigImage& image) {
    return sha256_hex(image.data(), image.size());
}

}  // namespace au2ft601
