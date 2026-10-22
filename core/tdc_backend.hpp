#pragma once

// The TDC board (TDC_Firmware release 1, design 0x54440001, or release 2,
// 0x54440002; told apart by its responses) as a Dantagger backend, over the
// FT601 (D3XX) or tdcctl's simulated FPGA.
//
// Open lifecycle (user, 2026-10-21; SetupMode::automatic, the Python
// library): the first open after the board is plugged in configures the
// TDC-GPX2 and calibrates the LVDS sampling phase; later opens recognise a
// board that is still configured and calibrated and skip both.
// "Calibrated" = the FPGA's LVDS state (MMCM locked, IDELAYCTRL ready,
// divider, bit phase, taps and sampling-phase step) equals what was recorded
// in the state file after the last successful calibration of this board (by
// Dantagger or the app).  A power cycle resets the FPGA (phase step 0, GPX2
// unconfigured), so the check fails and the setup runs again.  "Configured"
// = the GPX2 is verified and measuring with exactly the wanted registers.
// SetupMode::none (the app) changes nothing on open; the app offers the same
// setup as a button.  Neither mode touches the comparator (user, 2026-10-22).
//
// Frame policy (user, 2026-10-22): release 2 = DDR, the shortest stop width
// for a 1 ps LSB and the shortest reference index that keeps times
// unambiguous at full LCLKIN (tdcctl's automatic choice: 6 + 18 bits at
// 250 MHz); release 1 = SDR, 24 + 20 bits (the format proven on the board).
//
// Each run decodes with the GPX2 configuration and clocking the board
// reports at its start, so a configuration made by hand in the app decodes
// correctly too.

#include "backend.hpp"
#include "record_decoder.hpp"
#include "record_tap.hpp"

#include "tdcctl/tdc_protocol.hpp"

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace au2ft601 {
class D3xxTransport;
}
namespace tdcctl {
class Session;
class FakeFpga;
}  // namespace tdcctl

namespace dt {

enum class SetupMode { automatic, none };

struct TdcOptions {
    std::string serial;            // empty: the only FT601 attached
    bool simulate_fpga{false};     // tdcctl's FakeFpga instead of hardware (persists across opens in a process)
    bool recalibrate{false};       // force configure + calibrate (automatic mode)
    SetupMode setup{SetupMode::automatic};
    int ref_index_bits{0};         // GPX2 LVDS frame override (0: the frame policy for the loaded release)
    int stop_bits{0};
    double refclk_hz{8.0e6};       // the board's REFCLK (SW1 "Int")
    std::string state_dir;         // empty: $XDG_STATE_HOME/dantagger or ~/.local/state/dantagger
};

// The GPX2 configuration of the frame policy above, for an image with
// `timing`; non-zero overrides replace the widths.  Throws tdcctl::ArgumentError.
tdcctl::GpxConfig policy_gpx_config(const tdcctl::LinkTiming& timing, double refclk_hz, int high_resolution,
                                    int ref_bits_override = 0, int stop_bits_override = 0);

// "Release 2 (design 0x54440002)" or "Release 1 (...)" or "unknown design ...".
std::string release_name(std::uint32_t design_version);
// One line: frame widths, DDR/SDR, LCLKIN and the per-channel readout limit of a STATUS.
std::string describe_capture(const tdcctl::Response& status);
// Events per second per channel the GPX2 can read out with this capture format (0 if not configured).
double readout_limit_of(const tdcctl::Response& status);

class TdcBackend final : public Backend {
public:
    explicit TdcBackend(const TdcOptions& options);
    ~TdcBackend() override;

    std::string serial() const override { return serial_; }
    std::string model() const override;
    void run(Engine& engine, const std::function<bool()>& should_stop) override;
    double set_trigger(int channel, double volts) override;
    double set_hysteresis(int channel, double ohms) override;
    void set_resolution(int factor) override;
    double current_trigger(int channel) override;
    double current_hysteresis(int channel) override;
    int current_resolution() override;
    timestamp_t min_deadtime() const override;
    double readout_limit() const override;
    std::string info() override;
    void close() override;

    // What the open did: "skipped" (already configured and calibrated),
    // "configured" (GPX2 configured, calibration kept), "calibrated" (full
    // setup), "none" (SetupMode::none: nothing done).
    const std::string& setup_action() const { return setup_action_; }
    const std::string& setup_message() const { return setup_message_; }
    const std::string& usb_speed() const { return usb_speed_; }
    bool simulated() const { return fake_ != nullptr; }

    // ---- the app (only while no run streams: Tagger::exclusive) ------------------
    tdcctl::Session& session() { return *session_; }
    // The last response seen (any command, or a run's start/final response).
    tdcctl::Response last_status() const;
    // Records a response the caller got from session() as the latest state.
    void note_response(const tdcctl::Response& r);
    // Whether the board is configured and calibrated as the state file says
    // (what the automatic setup would skip).
    bool set_up();
    // The automatic setup: GPX2 with the frame policy and calibration (or
    // only the configuration if still calibrated, unless `force`); returns
    // a description.  Throws DeviceError.
    std::string run_setup(bool force_calibrate);
    // After a calibration made by hand: record it in the state file.
    void record_calibration(const std::string& note);
    double refclk_hz() const { return refclk_hz_; }
    // The GPX2 configuration the automatic setup applies to this board.
    tdcctl::GpxConfig policy_config();

    // Raw-record taps (any thread; a tap added during a run joins at once with
    // a synthetic START marker so its records stand alone).
    void add_tap(const std::shared_ptr<RecordTap>& tap);
    void remove_tap(const RecordTap* tap);

    // simulate_fpga only: forget the simulated board (as if unplugged).
    static void simulate_unplug();
    // simulate_fpga only: test controls of the shared simulated FPGA.
    static tdcctl::FakeFpga& simulated_fpga();

private:
    void lifecycle(const TdcOptions& options);
    void configure_gpx(const tdcctl::GpxConfig& config);
    void lclkin_off();
    std::string state_path() const;
    tdcctl::Response refresh();   // STATUS, recorded
    RunInfo make_run_info(const tdcctl::Response& st) const;
    void flush_taps(bool final_flush);

    std::unique_ptr<au2ft601::D3xxTransport> d3xx_;
    std::shared_ptr<tdcctl::FakeFpga> fake_owner_;
    tdcctl::FakeFpga* fake_{nullptr};
    std::unique_ptr<tdcctl::Session> session_;
    RecordDecoder decoder_;
    std::string serial_;
    std::string usb_speed_;
    std::string state_dir_;
    std::string setup_action_;
    std::string setup_message_;
    double refclk_hz_{8.0e6};
    int ref_override_{0}, stop_override_{0};
    int high_resolution_{0};       // wanted HIGH_RESOLUTION code (setHighResolution)
    bool open_{false};

    mutable std::mutex state_mu_;
    tdcctl::Response last_;

    struct TapEntry {
        std::shared_ptr<RecordTap> tap;
        bool joined{false};        // has had run_started for the current run
    };
    std::mutex tap_mu_;
    std::vector<TapEntry> taps_;
    bool in_run_{false};           // tap_mu_
    RunInfo run_info_;             // tap_mu_
    std::vector<std::uint64_t> tap_buf_;   // reader thread
    std::uint64_t tap_buf_coarse_{0};      // decoder coarse time before the buffer's first record
};

}  // namespace dt
