#include "au2ft601/config_workflow.hpp"

#include "au2ft601/sha256.hpp"

#include <algorithm>
#include <cstdio>
#include <memory>
#include <optional>
#include <ostream>
#include <sstream>

namespace au2ft601 {
namespace {

namespace fs = std::filesystem;

// Collects evidence files for one command. Files are created exclusively so a
// run can never overwrite earlier evidence; every name is checked before any
// device write so post-write evidence cannot fail on a name collision.
class Evidence {
public:
    Evidence(WorkflowContext& ctx, fs::path dir, std::string command)
        : ctx_(ctx), dir_(std::move(dir)), command_(std::move(command)) {}

    bool prepare(const std::vector<std::string>& names) {
        std::error_code error;
        fs::create_directories(dir_, error);
        if (error) {
            say("ERROR: cannot create evidence directory " + dir_.string() + ": " + error.message());
            return false;
        }
        std::vector<std::string> all = names;
        all.push_back(log_name());
        for (const auto& name : all) {
            if (fs::exists(dir_ / name)) {
                say("ERROR: evidence file already exists, refusing to overwrite: " + (dir_ / name).string());
                return false;
            }
        }
        prepared_ = true;
        return true;
    }

    void say(const std::string& line) {
        ctx_.out << line << '\n';
        log_ << line << '\n';
    }

    bool write(const std::string& name, const std::uint8_t* data, std::size_t size) {
        const auto path = dir_ / name;
        std::unique_ptr<std::FILE, int (*)(std::FILE*)> file(std::fopen(path.c_str(), "wbx"), &std::fclose);
        if (!file || std::fwrite(data, 1, size, file.get()) != size || std::fflush(file.get()) != 0) {
            say("ERROR: cannot write evidence file " + path.string());
            return false;
        }
        return true;
    }

    bool write_text(const std::string& name, const std::string& text) {
        return write(name, reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
    }

    bool write_image(const std::string& stem, const ConfigImage& image) {
        const bool raw = write(stem + ".bin", image.data(), image.size());
        const bool text = write_text(stem + ".txt", describe_config(image));
        if (raw && text) {
            say("saved " + (dir_ / (stem + ".bin")).string() + " sha256=" + config_sha256(image));
        }
        return raw && text;
    }

    int finish(int code) {
        say(command_ + " exit code " + std::to_string(code));
        if (prepared_) {
            const auto text = log_.str();
            (void)write(log_name(), reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
        }
        return code;
    }

    const fs::path& dir() const { return dir_; }

private:
    std::string log_name() const { return command_ + "-log.txt"; }

    WorkflowContext& ctx_;
    fs::path dir_;
    std::string command_;
    std::ostringstream log_;
    bool prepared_{false};
};

std::vector<std::string> image_files(const std::vector<std::string>& stems) {
    std::vector<std::string> names;
    for (const auto& stem : stems) {
        names.push_back(stem + ".bin");
        names.push_back(stem + ".txt");
    }
    return names;
}

bool find_unique_serial(WorkflowContext& ctx, Evidence& ev, const std::string& serial) {
    if (serial.empty()) {
        ev.say("ERROR: an exact, non-empty serial is required");
        return false;
    }
    std::vector<std::string> serials;
    try {
        serials = ctx.device.list_serials();
    } catch (const DeviceError& error) {
        ev.say(std::string("ERROR: device enumeration failed: ") + error.what());
        return false;
    }
    const auto count = std::count(serials.begin(), serials.end(), serial);
    if (count != 1) {
        ev.say("ERROR: expected exactly one D3XX device with serial " + serial + ", found " +
               std::to_string(count) + " among " + std::to_string(serials.size()) + " device(s)");
        return false;
    }
    return true;
}

std::optional<ConfigImage> read_current(WorkflowContext& ctx, Evidence& ev, const std::string& serial) {
    try {
        return ctx.device.read_config(serial);
    } catch (const DeviceError& error) {
        ev.say(std::string("ERROR: configuration read failed: ") + error.what());
        return std::nullopt;
    }
}

void log_environment(WorkflowContext& ctx, Evidence& ev, const std::string& serial) {
    try {
        ev.say("environment: " + ctx.device.environment(serial));
    } catch (const DeviceError& error) {
        ev.say(std::string("environment: unavailable (") + error.what() + ')');
    }
}

bool identity_matches(const ConfigImage& image, const std::string& serial) {
    const auto decoded = decode_config(image);
    return decoded.strings_valid && decoded.serial == serial;
}

// Called only after one write attempt. Polls read-only operations until the
// device re-enumerates and a configuration read succeeds, or the timeout expires.
std::optional<ConfigImage> wait_for_readback(WorkflowContext& ctx, Evidence& ev, const std::string& serial) {
    ev.say("waiting " + std::to_string(ctx.settle.count()) + " ms for the FT601 to restart, then polling up to " +
           std::to_string(ctx.reenumeration_timeout.count()) + " ms for re-enumeration");
    ctx.sleep(ctx.settle);
    const auto start = ctx.now();
    const auto deadline = start + ctx.reenumeration_timeout;
    bool seen_absent = false;
    unsigned attempts = 0;
    unsigned logged_errors = 0;
    while (true) {
        ++attempts;
        try {
            const auto serials = ctx.device.list_serials();
            const auto count = std::count(serials.begin(), serials.end(), serial);
            if (count == 1) {
                auto image = ctx.device.read_config(serial);
                const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(ctx.now() - start);
                ev.say("device readable again after " + std::to_string(attempts) + " poll(s), " +
                       std::to_string(waited.count()) + " ms after settle; absence observed during polling: " +
                       (seen_absent ? "yes" : "no"));
                return image;
            }
            if (count > 1) {
                ev.say("ERROR: more than one device reports serial " + serial + " after the write");
                return std::nullopt;
            }
            seen_absent = true;
        } catch (const DeviceError& error) {
            if (logged_errors++ < 5) {
                ev.say(std::string("poll ") + std::to_string(attempts) + ": " + error.what());
            }
        }
        if (ctx.now() >= deadline) {
            ev.say("ERROR: device did not become readable within the re-enumeration timeout (" +
                   std::to_string(attempts) + " polls)");
            return std::nullopt;
        }
        ctx.sleep(ctx.poll_interval);
    }
}

// Performs the single write, waits, reads back and compares with `expected`.
int write_once_and_verify(WorkflowContext& ctx, Evidence& ev, const std::string& serial,
                          const ConfigImage& before, const ConfigImage& expected, const std::string& readback_stem,
                          const std::string& recovery_hint) {
    ev.say("writing configuration ONCE (no automatic retry)");
    bool write_reported_error = false;
    try {
        ctx.device.write_config(serial, expected);
        ev.say("write call returned success");
    } catch (const DeviceError& error) {
        write_reported_error = true;
        ev.say(std::string("ERROR: write call failed: ") + error.what());
        ev.say("NOT retrying. Reading back to record the resulting state.");
    }
    const auto readback = wait_for_readback(ctx, ev, serial);
    if (!readback) {
        ev.say("RESULT: device state UNKNOWN. Do not retry the write blindly; reconnect/inspect first.");
        ev.say(recovery_hint);
        return config_exit::write_unverified;
    }
    const bool saved = ev.write_image(readback_stem, *readback);
    const auto diffs = diff_config(expected, *readback);
    if (!diffs.empty()) {
        ev.say("RESULT: readback does NOT match the intended image. Differences (intended -> readback):");
        ev.say(format_diff(diffs));
        ev.say(*readback == before ? "readback equals the pre-write image: configuration appears unchanged"
                                   : "readback differs from both the pre-write and intended images");
        ev.say(recovery_hint);
        return config_exit::write_unverified;
    }
    if (write_reported_error) {
        ev.say("RESULT: readback matches the intended image, but the write call reported an error; review before use.");
        return config_exit::write_unverified;
    }
    if (!saved) {
        ev.say("RESULT: readback matches, but readback evidence could not be saved.");
        return config_exit::write_unverified;
    }
    ev.say("RESULT: PASS - full 152-byte readback matches the intended image");
    return config_exit::ok;
}

std::string restore_command(const WorkflowContext& ctx, const std::string& serial, const fs::path& backup,
                            const std::string& sha, const fs::path& evidence_dir) {
    return ctx.program + " restore --serial " + serial + " --backup " + backup.string() + " --expect-backup-sha256 " +
           sha + " --evidence-dir " + (evidence_dir / "restore").string() + " --confirm-write";
}

}  // namespace

int run_config_preview(WorkflowContext& ctx, const std::string& serial, FifoClock target,
                       const fs::path& evidence_dir) {
    Evidence ev(ctx, evidence_dir, "preview");
    ev.say("FT601 configuration PREVIEW (read-only; no configuration write is performed)");
    auto names = image_files({"preview-before", "preview-proposed"});
    names.push_back("preview-diff.txt");
    if (!ev.prepare(names) || !find_unique_serial(ctx, ev, serial)) {
        return ev.finish(config_exit::failure);
    }
    log_environment(ctx, ev, serial);
    const auto current = read_current(ctx, ev, serial);
    if (!current || !ev.write_image("preview-before", *current)) {
        return ev.finish(config_exit::failure);
    }
    ev.say("current configuration:\n" + describe_config(*current));
    if (!identity_matches(*current, serial)) {
        ev.say("REFUSED: configuration string descriptors do not carry serial " + serial);
        return ev.finish(config_exit::refused);
    }
    ConfigImage proposed{};
    std::string reason;
    const auto change = make_clock_change(*current, target, proposed, reason);
    if (change == ClockChange::refused) {
        ev.say("REFUSED: " + reason);
        return ev.finish(config_exit::refused);
    }
    if (change == ClockChange::already_at_target) {
        ev.say("NO CHANGE NEEDED: " + reason);
        return ev.finish(config_exit::ok);
    }
    const auto diff_text = format_diff(diff_config(*current, proposed));
    if (!ev.write_image("preview-proposed", proposed) || !ev.write_text("preview-diff.txt", diff_text)) {
        return ev.finish(config_exit::failure);
    }
    const auto sha = config_sha256(*current);
    const auto backup = ev.dir() / "preview-before.bin";
    ev.say("proposed change (current -> proposed):\n" + diff_text);
    ev.say("To apply after review (writes once, then reads back):\n  " + ctx.program + " apply --serial " + serial +
           " --fifo-clock " + (target == FifoClock::mhz66 ? "66" : "100") + " --expect-current-sha256 " + sha +
           " --evidence-dir " + (evidence_dir / "apply").string() + " --confirm-write");
    ev.say("To restore this exact pre-change image afterwards:\n  " +
           restore_command(ctx, serial, backup, sha, evidence_dir));
    return ev.finish(config_exit::ok);
}

int run_config_apply(WorkflowContext& ctx, const std::string& serial, FifoClock target,
                     const std::string& expected_current_sha256, const fs::path& evidence_dir, bool confirm_write) {
    Evidence ev(ctx, evidence_dir, "apply");
    ev.say("FT601 configuration APPLY (FIFOClock only)");
    if (!confirm_write) {
        ev.say("REFUSED: --confirm-write was not given; nothing was written. Run preview to review the change.");
        return ev.finish(config_exit::refused);
    }
    auto names = image_files({"apply-before", "apply-expected", "apply-readback"});
    names.push_back("apply-diff.txt");
    if (!ev.prepare(names) || !find_unique_serial(ctx, ev, serial)) {
        return ev.finish(config_exit::failure);
    }
    log_environment(ctx, ev, serial);
    const auto current = read_current(ctx, ev, serial);
    if (!current || !ev.write_image("apply-before", *current)) {
        return ev.finish(config_exit::failure);
    }
    const auto sha = config_sha256(*current);
    if (sha != expected_current_sha256) {
        ev.say("REFUSED: current configuration sha256 " + sha + " does not match the reviewed " +
               expected_current_sha256 + "; re-run preview and review again. Nothing was written.");
        return ev.finish(config_exit::refused);
    }
    if (!identity_matches(*current, serial)) {
        ev.say("REFUSED: configuration string descriptors do not carry serial " + serial);
        return ev.finish(config_exit::refused);
    }
    ConfigImage expected{};
    std::string reason;
    const auto change = make_clock_change(*current, target, expected, reason);
    if (change == ClockChange::refused) {
        ev.say("REFUSED: " + reason);
        return ev.finish(config_exit::refused);
    }
    if (change == ClockChange::already_at_target) {
        ev.say("NO CHANGE NEEDED: " + reason + ". Nothing was written.");
        return ev.finish(config_exit::ok);
    }
    const auto diffs = diff_config(*current, expected);
    if (diffs.size() != 1 || diffs[0].field != "FIFOClock") {
        ev.say("REFUSED: internal check failed; proposed change is not FIFOClock-only");
        return ev.finish(config_exit::refused);
    }
    const auto diff_text = format_diff(diffs);
    if (!ev.write_image("apply-expected", expected) || !ev.write_text("apply-diff.txt", diff_text)) {
        return ev.finish(config_exit::failure);
    }
    ev.say("change: " + diff_text);
    const auto hint = "To restore the pre-change image:\n  " +
                      restore_command(ctx, serial, ev.dir() / "apply-before.bin", sha, evidence_dir);
    const int code = write_once_and_verify(ctx, ev, serial, *current, expected, "apply-readback", hint);
    if (code == config_exit::ok) {
        ev.say(hint);
    }
    return ev.finish(code);
}

int run_config_restore(WorkflowContext& ctx, const std::string& serial, const fs::path& backup,
                       const std::string& expected_backup_sha256, const fs::path& evidence_dir, bool confirm_write) {
    Evidence ev(ctx, evidence_dir, "restore");
    ev.say("FT601 configuration RESTORE from " + backup.string());
    ConfigImage image{};
    std::string error;
    if (!load_config_image(backup, image, error)) {
        ev.say("ERROR: " + error);
        return ev.finish(config_exit::failure);
    }
    const auto backup_sha = config_sha256(image);
    if (backup_sha != expected_backup_sha256) {
        ev.say("REFUSED: backup sha256 " + backup_sha + " does not match the stated " + expected_backup_sha256);
        return ev.finish(config_exit::refused);
    }
    const auto backup_decoded = decode_config(image);
    if (backup_decoded.vendor_id != kFtdiVendorId || backup_decoded.product_id != kFt601ProductId ||
        !identity_matches(image, serial)) {
        ev.say("REFUSED: backup is not an FT601 image carrying serial " + serial);
        return ev.finish(config_exit::refused);
    }
    auto names = image_files({"restore-before", "restore-readback"});
    names.push_back("restore-diff.txt");
    if (!ev.prepare(names) || !find_unique_serial(ctx, ev, serial)) {
        return ev.finish(config_exit::failure);
    }
    log_environment(ctx, ev, serial);
    const auto current = read_current(ctx, ev, serial);
    if (!current || !ev.write_image("restore-before", *current)) {
        return ev.finish(config_exit::failure);
    }
    if (!identity_matches(*current, serial)) {
        ev.say("REFUSED: device configuration does not carry serial " + serial);
        return ev.finish(config_exit::refused);
    }
    const auto diffs = diff_config(*current, image);
    const auto diff_text = format_diff(diffs);
    if (!ev.write_text("restore-diff.txt", diff_text)) {
        return ev.finish(config_exit::failure);
    }
    if (diffs.empty()) {
        ev.say("NO CHANGE NEEDED: device already matches the backup. Nothing was written.");
        return ev.finish(config_exit::ok);
    }
    ev.say("restore change (current -> backup):\n" + diff_text);
    if (!confirm_write) {
        ev.say("REFUSED: --confirm-write was not given; nothing was written (dry run).");
        return ev.finish(config_exit::refused);
    }
    const auto hint = std::string("Restore was not verified. Reconnect and inspect before any further write.");
    return ev.finish(write_once_and_verify(ctx, ev, serial, *current, image, "restore-readback", hint));
}

int run_config_verify(WorkflowContext& ctx, const std::string& serial, const fs::path& expected,
                      const fs::path& evidence_dir) {
    Evidence ev(ctx, evidence_dir, "verify");
    ev.say("FT601 configuration VERIFY (read-only) against " + expected.string());
    ConfigImage image{};
    std::string error;
    if (!load_config_image(expected, image, error)) {
        ev.say("ERROR: " + error);
        return ev.finish(config_exit::failure);
    }
    if (!ev.prepare(image_files({"verify-readback"})) || !find_unique_serial(ctx, ev, serial)) {
        return ev.finish(config_exit::failure);
    }
    log_environment(ctx, ev, serial);
    const auto current = read_current(ctx, ev, serial);
    if (!current || !ev.write_image("verify-readback", *current)) {
        return ev.finish(config_exit::failure);
    }
    const auto diffs = diff_config(image, *current);
    ev.say("expected sha256=" + config_sha256(image) + "\ndifferences (expected -> device):\n" + format_diff(diffs));
    return ev.finish(diffs.empty() ? config_exit::ok : config_exit::mismatch);
}

}  // namespace au2ft601
