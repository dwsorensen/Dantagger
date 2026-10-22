#pragma once

// Internal helpers shared by the D3XX-backed executables. This header exposes
// no FTDI types; only the .cpp files include ftd3xx.h.

#include "au2ft601/config_workflow.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace au2ft601::d3xx {

struct ListedDevice {
    std::uint32_t index{};
    std::uint32_t flags{};
    std::uint32_t type{};
    std::uint32_t id{};
    std::uint32_t location{};
    std::string serial;
    std::string description;
};

std::string status_name(std::uint32_t status);
std::string version_string(std::uint32_t version);

// Throws DeviceError on SDK failure.
std::vector<ListedDevice> enumerate_devices();

// Negotiated USB speed in Mb/s from sysfs ("480", "5000", ...), if found.
std::optional<std::string> usb_speed_for_serial(const std::string& serial);

// Throws DeviceError unless exactly one listed device carries `serial`.
ListedDevice require_unique(const std::vector<ListedDevice>& devices, const std::string& serial);

// Library/driver/firmware versions and USB speed via a short read-only open.
std::string describe_environment(const std::string& serial);

}  // namespace au2ft601::d3xx
