#pragma once

// The TDC board for the Dantagger app.  A worker thread connects, runs the
// device commands of the setup pages (comparator, LEDs, GPX2, LVDS, the
// trigger scan) and applies the shared channel settings; the measurements
// talk to the live dt::Tagger directly (its engine), so they never wait for
// the worker.
//
// The live tagger uses SettingsPolicy::restart (user, 2026-10-22): a trigger,
// hysteresis, delay or dead-time change while measurements run stops the
// run, applies the change and streams on.  Connecting changes nothing on the
// board (SetupMode::none); "Set up the board" runs the same configure +
// calibrate as the Python library's first open, and writes the same state
// file, so a script started later skips its setup.
//
// Channel settings (trigger level, hysteresis, input delay, dead time) are
// shared by every measurement and persistent (user, 2026-10-22): they are
// kept in the config folder and the app's own values.  The board keeps its
// comparator values until it loses power; if they differ from the saved ones
// when the app connects, the app says so and offers to apply them (it never
// does that by itself).  Delay and dead time are software and are applied to
// the tagger on every connect (and to file replays).

#include "rate_monitor.hpp"

#include "tagger.hpp"
#include "tdc_backend.hpp"
#include "tdcctl/tdc_protocol.hpp"
#include "tdcctl/tdc_session.hpp"

#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace app {

enum class Level { info, ok, warn, error };

struct LogLine {
    std::string time;
    std::string text;
    Level level{Level::info};
};

struct ChannelSettings {
    std::array<double, dt::kNumChannels> trigger_v{{0.05, 0.05, 0.05, 0.05}};
    std::array<double, dt::kNumChannels> hysteresis_ohm{{1000, 1000, 1000, 1000}};
    std::array<std::int64_t, dt::kNumChannels> delay_ps{};
    std::array<std::int64_t, dt::kNumChannels> deadtime_ps{};
};

// Count rate against the comparator trigger (the old app's scan): for each
// selected channel in turn, the trigger steps through [from, to] and a count
// timed by the FPGA window is taken at each point; triggers restored after.
struct ScanRequest {
    std::array<bool, dt::kNumChannels> channels{{true, true, false, false}};
    double from_v{tdcctl::kThresholdMinVolts};
    double to_v{tdcctl::kThresholdMaxVolts};
    int points{30};
    double seconds{0.1};
    bool set_hysteresis{false};
    double hysteresis_ohm{1000.0};
};

struct CountResult {
    bool pass{false};
    double window_s{0};
    std::array<std::uint64_t, dt::kNumChannels> events{};
    std::array<double, dt::kNumChannels> rates_hz{};
    std::uint64_t dropped{0};
    std::string failure;
};

struct ScanPoint {
    int channel{0};                     // 0..3
    double volts{0};
    double rate_hz{0};
    std::uint64_t events{0};
    std::uint64_t dropped{0};
};

class Device {
public:
    struct Options {
        bool simulate{false};
        std::string serial;
        std::string state_dir;          // calibration state (empty: the Python library's default)
    };

    struct View {
        bool connected{false};
        bool connecting{false};
        bool busy{false};
        std::string busy_label;
        std::string link_text{"not connected"};
        std::string serial;
        std::optional<tdcctl::Response> state;
        std::uint64_t state_seq{0};
        std::uint64_t generation{0};    // +1 on every connect / disconnect
        bool set_up{false};             // configured + calibrated (state file)
        std::string release;            // "Release 2 (design 0x54440002)"
        std::string capture;            // frames, DDR/SDR, LCLKIN, readout limit
        bool policy_frame{false};       // the GPX2 has the frame policy's configuration
        std::string policy_text;        // what the policy configuration is
        bool streaming{false};
        std::string error;              // device failure (the engine stopped every measurement)
        bool comparator_differs{false}; // board comparator != saved values
        // quick count
        bool counting{false};
        std::optional<CountResult> last_count;
        // calibration
        bool calibrating{false};
        std::vector<tdcctl::CalibrationPoint> cal_points;
        std::optional<tdcctl::CalibrationResult> last_cal;
        // trigger scan
        bool scanning{false};
        ScanRequest scan_request;
        std::vector<ScanPoint> scan_points;
        std::array<double, dt::kNumChannels> scan_hysteresis_ohm{};
        int scan_total{0};
        std::string scan_message;
    };

    explicit Device(Options options);
    ~Device();
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    void connect();
    void disconnect();
    // The live tagger (null when not connected).  Any thread.
    std::shared_ptr<dt::Tagger> tagger();
    dt::TdcBackend* backend();          // of tagger(); valid while that tagger lives

    // Device commands (worker thread).  Without `while_measuring` they are
    // refused while measurements run; with it the run pauses around them.
    bool command(const std::string& label, std::function<tdcctl::Response(tdcctl::Session&)> job,
                 bool while_measuring = false);
    bool setup(bool force_calibrate);
    bool calibrate(const tdcctl::GpxConfig& config, const tdcctl::CalibrationOptions& options);
    bool link_test(std::uint32_t window_cycles);
    bool threshold_scan(const ScanRequest& request);
    bool quick_count(double seconds);   // a count timed by the FPGA window, nothing kept but the totals
    void abort();                       // stops a scan between points

    // Shared, persistent channel settings (UI thread).
    const ChannelSettings& channels() const { return channels_; }
    void set_trigger(int channel, double volts);
    void set_hysteresis(int channel, double ohms);
    void set_delay(int channel, std::int64_t ps);
    void set_deadtime(int channel, std::int64_t ps);
    void apply_saved_comparator();      // the saved trigger/hysteresis to the board
    void adopt_board_comparator();      // the board's values become the saved ones

    View view();
    std::vector<LogLine> take_log();
    void log(Level level, const std::string& text);
    RateMonitor& rates() { return *rates_; }
    bool simulated() const { return options_.simulate; }
    bool idle();                        // no job queued or running

private:
    using Job = std::function<void()>;
    bool submit(const std::string& label, Job job, bool needs_connection = true);
    void worker();
    void post_state();
    void do_connect();
    void do_disconnect(const std::string& why);
    void save_channels();
    void load_channels();
    void apply_software_settings(dt::Tagger& t);

    Options options_;
    std::shared_ptr<RateMonitor> rates_;
    ChannelSettings channels_;          // UI thread
    std::string channels_path_;

    std::mutex tagger_mu_;
    std::shared_ptr<dt::Tagger> tagger_;
    dt::TdcBackend* backend_{nullptr};

    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::pair<std::string, Job>> jobs_;
    bool quit_{false};
    View view_;
    std::vector<LogLine> log_;
    std::atomic<bool> abort_{false};
    std::thread thread_;
};

}  // namespace app
