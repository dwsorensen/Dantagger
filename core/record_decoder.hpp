#pragma once

// TDC firmware records (PROTOCOL.md 3.3) -> raw per-channel picosecond times.
//
// Raw time domain: FPGA coarse time x the coarse tick ("coarse ps"; 10 ns on
// release 1, 8 ns on the 250 MHz release 2).  Inside a run an
// event's time is exact on the GPX2 time base: raw = t_gpx + K, where t_gpx =
// (unwrapped reference index x REFCLK_DIVISIONS + stop result) x LSB, and K is
// fixed when the run's first event places it at its coarse time minus the
// nominal readout latency.  The reference index is unwrapped with the coarse
// time elapsed since the previous event, scaled by the measured GPX2/coarse
// clock ratio (kept for the whole session).  When that prediction is too
// uncertain for the index width (a long silence early in a session), the
// event is re-anchored to its coarse time instead of risking a wrong wrap.
//
// The prediction error is at most the spread of readout latencies between two
// events (the GPX2 FIFO full at the LVDS frame rate, `latency_spread_ps`) plus
// the clock-ratio uncertainty over the gap; the wrap is chosen only while that
// stays below 0.45 of a wrap, else the event re-anchors.  With release 2's
// short reference index (6 bits = 8 us at 250 MHz DDR) this is what keeps
// times exact across gaps of up to seconds once the ratio is measured.
//
// The watermark (every later event is at or after it) follows the last
// event through the coarse clock, minus a margin for the GPX2 readout latency
// (its 16-deep FIFO at the LVDS frame rate) and the clock-ratio uncertainty.

#include "common.hpp"

#include <cstdint>

namespace dt {

struct DecoderConfig {
    std::uint32_t divisions{125000};   // REFCLK_DIVISIONS
    timestamp_t lsb_ps{1};             // 1e12 / (refclk * divisions)
    int ref_bits{16};
    int stop_bits{18};
    timestamp_t coarse_ps{10'000};       // FPGA coarse tick (LinkTiming::coarse_tick_ns)
    timestamp_t latency_spread_ps{2'000'000};   // worst-case readout latency difference (tdcctl::max_result_latency_us)
};

struct DecoderStats {
    std::uint64_t events{0};
    std::uint64_t markers{0};
    std::uint64_t dropped{0};          // sum of OVERFLOW marker counts
    std::uint64_t reanchors{0};
    std::uint64_t corrupt{0};          // stop result >= divisions
    std::uint64_t order_fixes{0};      // an event earlier than the previous one of its channel
};

class RecordDecoder {
public:
    explicit RecordDecoder(DecoderConfig config = {});
    void set_config(const DecoderConfig& config);

    // Starts a run (before its START record).
    void begin_run();
    // Feeds one record; adds events/specials to `chunk`.  The first record of
    // a run must be START (sets chunk.run_begin / run_base).
    void feed(std::uint64_t raw, RawChunk& chunk);
    // Current watermark (raw domain).
    timestamp_t watermark() const;
    std::uint64_t coarse() const { return coarse_; }
    bool run_started() const { return started_; }
    bool run_ended() const { return ended_; }
    const DecoderStats& stats() const { return stats_; }
    double ratio() const { return ratio_; }
    double ratio_uncertainty() const { return ratio_unc_; }
    // Latency assumed for the first event of a run (ps between stop edge and frame).
    static constexpr timestamp_t kNominalLatencyPs = 600'000;
    static constexpr timestamp_t kMarginPs = 20'000'000;   // 20 us

private:
    void update_watermark();
    timestamp_t marker_time() const;   // raw time of "now" (latest coarse), no margin

    DecoderConfig cfg_;
    timestamp_t coarse_ps_{10'000};
    double spread_ps_{2e6};
    double ref_period_ps_{125000.0};
    double wrap_refs_{65536.0};
    bool started_{false};
    bool ended_{false};
    std::uint64_t coarse_{0};
    timestamp_t run_base_{0};
    bool have_event_{false};
    timestamp_t K_{0};
    timestamp_t last_tgpx_{0};
    std::uint64_t last_event_coarse_{0};
    timestamp_t last_raw_{0};
    timestamp_t last_raw_ch_[kNumChannels]{};
    bool have_ch_[kNumChannels]{};
    mutable timestamp_t watermark_{0};
    std::int64_t wrap_ps_{0};
    double inv_wrap_ps_{0};
    std::int64_t fast_dc_limit_{1};
    // GPX2 ps per coarse ps, measured within runs (kept across runs).
    double ratio_{1.0};
    double ratio_unc_{2e-4};
    std::uint64_t anchor_coarse_{0};
    timestamp_t anchor_tgpx_{0};
    DecoderStats stats_;
};

}  // namespace dt
