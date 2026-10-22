#include "d3xx_common.hpp"

#include "ftd3xx.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace au2ft601::d3xx {
namespace {

std::string fixed_string(const char* data, std::size_t size) {
    return std::string(data, std::find(data, data + size, '\0'));
}

std::string read_line(const std::filesystem::path& path) {
    std::ifstream in(path);
    std::string value;
    std::getline(in, value);
    return value;
}

void check(FT_STATUS status, const char* operation) {
    if (status != FT_OK) {
        throw DeviceError(std::string(operation) + " failed: " + status_name(status));
    }
}

}  // namespace

std::string status_name(std::uint32_t status) {
    switch (status) {
        case FT_OK: return "FT_OK";
        case FT_INVALID_HANDLE: return "FT_INVALID_HANDLE";
        case FT_DEVICE_NOT_FOUND: return "FT_DEVICE_NOT_FOUND";
        case FT_DEVICE_NOT_OPENED: return "FT_DEVICE_NOT_OPENED";
        case FT_IO_ERROR: return "FT_IO_ERROR";
        case FT_INSUFFICIENT_RESOURCES: return "FT_INSUFFICIENT_RESOURCES";
        case FT_INVALID_PARAMETER: return "FT_INVALID_PARAMETER";
        case FT_FAILED_TO_WRITE_DEVICE: return "FT_FAILED_TO_WRITE_DEVICE";
        case FT_INVALID_ARGS: return "FT_INVALID_ARGS";
        case FT_NOT_SUPPORTED: return "FT_NOT_SUPPORTED";
        case FT_TIMEOUT: return "FT_TIMEOUT";
        case FT_OPERATION_ABORTED: return "FT_OPERATION_ABORTED";
        case FT_BUSY: return "FT_BUSY";
        case FT_DEVICE_NOT_CONNECTED: return "FT_DEVICE_NOT_CONNECTED";
        case FT_OTHER_ERROR: return "FT_OTHER_ERROR";
        default: return "FT_STATUS_" + std::to_string(status);
    }
}

std::string version_string(std::uint32_t version) {
    std::ostringstream out;
    out << ((version >> 24U) & 0xffU) << '.' << ((version >> 16U) & 0xffU) << '.' << (version & 0xffffU)
        << " (0x" << std::hex << std::setfill('0') << std::setw(8) << version << ')';
    return out.str();
}

std::vector<ListedDevice> enumerate_devices() {
    DWORD count = 0;
    check(FT_CreateDeviceInfoList(&count), "FT_CreateDeviceInfoList");
    std::vector<ListedDevice> devices;
    for (DWORD index = 0; index < count; ++index) {
        std::array<char, 16> serial{};
        std::array<char, 32> description{};
        DWORD flags = 0, type = 0, id = 0, location = 0;
        check(FT_GetDeviceInfoDetail(index, &flags, &type, &id, &location, serial.data(), description.data(), nullptr),
              "FT_GetDeviceInfoDetail");
        devices.push_back(ListedDevice{index, flags, type, id, location, fixed_string(serial.data(), serial.size()),
                                       fixed_string(description.data(), description.size())});
    }
    return devices;
}

std::optional<std::string> usb_speed_for_serial(const std::string& serial) {
    std::error_code error;
    const std::filesystem::path root{"/sys/bus/usb/devices"};
    std::optional<std::string> speed;
    for (std::filesystem::directory_iterator it(root, error), end; !error && it != end; it.increment(error)) {
        if (read_line(it->path() / "serial") == serial) {
            if (speed) {
                return std::nullopt;  // ambiguous: more than one USB device with this serial
            }
            speed = read_line(it->path() / "speed");
        }
    }
    return speed;
}

ListedDevice require_unique(const std::vector<ListedDevice>& devices, const std::string& serial) {
    const auto count = std::count_if(devices.begin(), devices.end(),
                                     [&](const ListedDevice& d) { return d.serial == serial; });
    if (serial.empty() || count != 1) {
        throw DeviceError("expected exactly one D3XX device with serial '" + serial + "', found " +
                          std::to_string(count));
    }
    return *std::find_if(devices.begin(), devices.end(), [&](const ListedDevice& d) { return d.serial == serial; });
}

std::string describe_environment(const std::string& serial) {
    const auto device = require_unique(enumerate_devices(), serial);
    FT_HANDLE handle = nullptr;
    check(FT_Create(const_cast<char*>(serial.c_str()), FT_OPEN_BY_SERIAL_NUMBER, &handle), "FT_Create");
    DWORD library = 0, driver = 0;
    ULONG firmware = 0;
    const FT_STATUS s1 = FT_GetLibraryVersion(&library);
    const FT_STATUS s2 = FT_GetDriverVersion(handle, &driver);
    const FT_STATUS s3 = FT_GetFirmwareVersion(handle, &firmware);
    (void)FT_Close(handle);
    check(s1, "FT_GetLibraryVersion");
    check(s2, "FT_GetDriverVersion");
    check(s3, "FT_GetFirmwareVersion");
    const auto speed = usb_speed_for_serial(serial);
    std::ostringstream out;
    out << "SDK package 1.1.8; library " << version_string(library) << "; driver " << version_string(driver)
        << "; firmware " << version_string(firmware) << "; type " << device.type << "; description \""
        << device.description << "\"; USB speed " << (speed ? *speed + " Mb/s" : std::string("unknown"));
    return out.str();
}

}  // namespace au2ft601::d3xx
