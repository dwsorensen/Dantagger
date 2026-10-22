#pragma once

// Host session for the TDC firmware: one command outstanding at a time over an
// SDK-neutral au2ft601::Transport (D3XX or the simulated FPGA), streaming runs
// with strict DATA-frame validation, and the LVDS calibration procedure.
//
// Read discipline (Test_project hardware findings, 2026-09-23): a synchronous
// D3XX read that times out part-filled loses the pipe, so outside streaming
// reads never ask for more than the rest of one frame; during a run the
// transport's pipelined reads are used when available.  A command written
// while the FPGA streams can block until the host reads, so commands are
// written on their own thread while this one keeps reading.

#include "au2ft601/transport.hpp"
#include "tdcctl/tdc_protocol.hpp"

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace tdcctl {

class SessionError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct SessionOptions {
    std::uint32_t read_timeout_ms{100};
    std::uint32_t write_timeout_ms{1000};
    std::uint32_t response_timeout_ms{3000};   // board operations take up to tens of ms; LINK_TEST up to 1 s
    unsigned sync_attempts{3};
    std::uint32_t drain_after_ms{5};
    std::uint32_t drain_read_timeout_ms{10};
    // Streaming: pipelined reads (transports that support them).
    unsigned pipeline_depth{8};
    std::size_t pipeline_read_bytes{64 * 4096};
    // Longest silence allowed during a run (the FPGA sends a frame at least
    // every few ms while streaming: TIME markers every 164 us, flush after 1 ms).
    std::uint32_t stream_silence_ms{2000};
    std::uint32_t final_timeout_ms{5000};      // STOP written -> final response
};

class FrameScanner {
public:
    std::vector<Frame> feed(const std::uint8_t* data, std::size_t size);
    std::size_t discarded_bytes() const noexcept { return discarded_; }
    std::size_t buffered_bytes() const noexcept { return buffer_.size(); }

private:
    std::vector<std::uint8_t> buffer_;
    std::size_t discarded_{0};
};

struct SyncReport {
    Response status;
    unsigned attempts{0};
    std::size_t stale_frames{0};
    std::size_t discarded_bytes{0};
};

// ---- Streaming --------------------------------------------------------------
struct StreamRequest {
    std::uint32_t window_us{0};        // 0: until STOP (abort flag or duration)
    double duration_s{0.0};            // host-timed STOP after this long (0: none)
};

struct StreamProgress {
    double elapsed_s{0.0};
    std::uint64_t frames{0};
    std::uint64_t records{0};
    std::array<std::uint64_t, kChannels> events{};
    std::uint64_t dropped_marked{0};
    bool stopping{false};
};

struct StreamResult {
    bool pass{false};
    bool by_window{false};             // ended by the FPGA window (RUN_COMPLETE)
    std::uint64_t run_id{0};
    std::uint64_t frames{0};
    std::uint64_t records{0};
    std::array<std::uint64_t, kChannels> events{};
    std::uint64_t dropped_marked{0};   // sum of OVERFLOW marker counts (each saturates at 4095)
    double seconds{0.0};
    std::optional<Response> start_ack;
    std::optional<Response> final_response;
    std::string failure;
    bool recovered{false};
    unsigned pipeline_depth{0};
};

// Called with every record of the run, in order (markers included).
using RecordSink = std::function<void(std::uint64_t raw)>;
// The same, a validated DATA frame's records at a time (one call per frame
// instead of one per record: the receiving thread stays cheap).
using RecordBatchSink = std::function<void(const std::uint64_t* raw, std::size_t n)>;
using StreamProgressCallback = std::function<void(const StreamProgress&)>;

// ---- Calibration ----------------------------------------------------------------
// The sampling point is a bit phase (which of the samples per GPX2 bit the
// FPGA keeps) plus a fine phase (0 .. steps_per_sample - 1 steps of
// 17.857 ps); together they cover one bit period.  Calibration sweeps that
// whole period with LINK_TEST and picks the centre of the widest error-free
// window (circular).
struct CalibrationOptions {
    int divider_code{0};
    int step{0};                       // fine-phase steps between points (0: about 100 points per bit)
    std::uint32_t window_us{1000};     // LINK_TEST time per point
    std::uint32_t min_frames{100};     // a point is good only with this many frames on every channel
    unsigned channel_mask{0xF};        // channels that must decode
};

struct CalibrationPoint {
    int bit_phase{0};
    int phase{0};
    double offset_ps{0.0};             // sampling point within the bit period
    std::array<LinkCounts, kChannels> link{};
    std::array<std::uint16_t, 8> lclkout_hist{};
    bool good{false};
};

struct CalibrationResult {
    bool success{false};
    int chosen_bit_phase{-1};
    int chosen_phase{-1};
    double chosen_offset_ps{0.0};
    double good_width_ps{0.0};         // width of the chosen error-free window
    double bit_ps{0.0};                // the bit period swept
    std::vector<CalibrationPoint> points;
    std::optional<Response> final_test;
    std::string message;
};

class Session {
public:
    Session(au2ft601::Transport& transport, SessionOptions options, std::uint64_t first_transaction = 0);

    // STOP (ends a run another host left behind), then STATUS; discards stale
    // frames.  Throws SessionError.
    SyncReport sync();

    Response status();
    Response set_leds(std::uint8_t mask);
    Response thresholds(unsigned mask, const std::array<std::uint16_t, kChannels>& codes);
    Response hysteresis(unsigned mask, const std::array<std::uint16_t, kChannels>& wipers);
    Response led1(std::uint8_t red, std::uint8_t green, std::uint8_t blue, int current_setting);
    Response led2(unsigned on_mask);
    Response probe();
    Response readback();
    Response gpx_configure(const std::array<std::uint8_t, kGpxRegisters>& regs);
    Response gpx_read_config();
    Response gpx_reset();
    Response gpx_init();
    Response gpx_read_results();
    Response lvds_set(bool enable, int divider_code, int bit_phase,
                      const std::array<std::uint8_t, kChannels>& sdo_taps = {},
                      const std::array<std::uint8_t, kChannels>& frame_taps = {});
    Response lvds_phase(int step);
    Response link_test(std::uint32_t window_cycles);
    Response stop();

    // One streaming run; never throws for protocol or data failures (they are
    // in the result).  `abort` may be set from another thread to send STOP.
    StreamResult stream(const StreamRequest& request, const RecordSink& sink,
                        const StreamProgressCallback& progress = {}, const std::atomic<bool>* abort = nullptr);
    StreamResult stream_batches(const StreamRequest& request, const RecordBatchSink& sink,
                                const StreamProgressCallback& progress = {}, const std::atomic<bool>* abort = nullptr);

    // Calibration (PROTOCOL.md 2.3): configures the GPX2 with `config` plus
    // the test pattern, enables LCLKIN, sweeps the sampling phase with
    // LINK_TEST, chooses the centre of the widest error-free window, sets it,
    // checks it and reconfigures the GPX2 with `config` as given.  Throws
    // SessionError on transport failures.
    CalibrationResult calibrate(const GpxConfig& config, const CalibrationOptions& options,
                                const std::function<void(const CalibrationPoint&)>& point_done = {});

    Response transact(const Frame& command, std::uint64_t transaction);
    std::uint64_t next_transaction() { return next_transaction_++; }
    std::size_t stale_frames() const noexcept { return stale_frames_; }
    // Clocking of the loaded image, from the last response (nullopt before
    // the first).  Commands are checked against it before they are sent.
    const std::optional<LinkTiming>& timing() const noexcept { return timing_; }

private:
    void write_frame(const Frame& frame);
    std::vector<Frame> read_frames(std::uint32_t timeout_ms, std::size_t max_bytes);
    std::vector<Frame> read_transport(std::uint32_t timeout_ms, std::size_t max_bytes);
    std::optional<Response> await_response(std::uint64_t transaction, std::uint32_t timeout_ms);
    Response simple(Opcode opcode);

    au2ft601::Transport& transport_;
    SessionOptions options_;
    std::uint64_t next_transaction_;
    FrameScanner scanner_;
    std::vector<std::uint8_t> read_buffer_;
    std::deque<Frame> early_frames_;
    std::size_t stale_frames_{0};
    std::optional<LinkTiming> timing_;
};

// LCLKIN only while the FPGA reads the GPX2 over LVDS (link test,
// calibration, runs): the GPX2 runs hot with LCLKIN toggling (first board,
// 2026-10-20).  Create one around such an operation; off() or the destructor
// tri-states LCLKIN again, keeping the FPGA's divider, bit phase and IDELAY
// taps (the sampling phase is not touched by LVDS_SET).  With turn_on, LCLKIN
// is enabled with those settings if it is off, followed by a short wait so
// that results the GPX2 held while LCLKIN was stopped drain before capture.
class LclkinScope {
public:
    LclkinScope(Session& session, bool turn_on);
    ~LclkinScope();
    LclkinScope(const LclkinScope&) = delete;
    LclkinScope& operator=(const LclkinScope&) = delete;
    // Switches LCLKIN off now; returns the LVDS_SET response (nullopt if it
    // was already done).  Throws SessionError if the FPGA refuses.
    std::optional<Response> off();

private:
    Session& session_;
    bool active_{true};
};

}  // namespace tdcctl
