#pragma once

#include "au2ft601/ft601_config.hpp"

#include <chrono>
#include <filesystem>
#include <functional>
#include <iosfwd>
#include <stdexcept>
#include <string>
#include <vector>

namespace au2ft601 {

struct DeviceError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// SDK-neutral access to FT601 persistent configuration. Each call opens the
// device by exact serial, performs one operation, and closes the handle.
// Implementations must never retry a configuration write internally.
class ConfigDevice {
public:
    virtual ~ConfigDevice() = default;
    virtual std::vector<std::string> list_serials() = 0;
    virtual ConfigImage read_config(const std::string& serial) = 0;
    virtual void write_config(const std::string& serial, const ConfigImage& image) = 0;
    // Best-effort library/driver/firmware/USB-speed text for evidence logs.
    virtual std::string environment(const std::string& serial) = 0;
};

struct WorkflowContext {
    ConfigDevice& device;
    std::ostream& out;
    std::string program;  // executable name echoed in printed follow-up commands
    std::function<void(std::chrono::milliseconds)> sleep;
    std::function<std::chrono::steady_clock::time_point()> now;
    std::chrono::milliseconds settle{1500};
    std::chrono::milliseconds reenumeration_timeout{20000};
    std::chrono::milliseconds poll_interval{250};
};

// Process exit codes shared by the configuration commands.
namespace config_exit {
constexpr int ok = 0;
constexpr int failure = 1;        // device/file error before any write
constexpr int usage = 2;
constexpr int refused = 3;        // a safety precondition failed; nothing was written
constexpr int write_unverified = 4;  // a write was attempted and readback did not prove the intended result
constexpr int mismatch = 5;          // verify: device image differs from the expected file
}  // namespace config_exit

// Read-only: saves the current image, proposes a FIFOClock-only change and
// prints the exact apply/restore commands. Never writes the device.
int run_config_preview(WorkflowContext& ctx, const std::string& serial, FifoClock target,
                       const std::filesystem::path& evidence_dir);

// Writes the FIFOClock-only change exactly once, then waits for re-enumeration
// and compares a full readback. Requires the SHA-256 of the current image as
// recorded by preview and explicit confirmation.
int run_config_apply(WorkflowContext& ctx, const std::string& serial, FifoClock target,
                     const std::string& expected_current_sha256, const std::filesystem::path& evidence_dir,
                     bool confirm_write);

// Writes a previously saved raw image back exactly once. Requires the backup's
// SHA-256, matching identity, and explicit confirmation.
int run_config_restore(WorkflowContext& ctx, const std::string& serial, const std::filesystem::path& backup,
                       const std::string& expected_backup_sha256, const std::filesystem::path& evidence_dir,
                       bool confirm_write);

// Read-only: compares the device image to a saved raw image.
int run_config_verify(WorkflowContext& ctx, const std::string& serial, const std::filesystem::path& expected,
                      const std::filesystem::path& evidence_dir);

}  // namespace au2ft601
