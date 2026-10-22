#pragma once

// Per-channel event rates and dropped events of the live stream, measured on
// the FPGA's marker clock over the last second, plus totals since the last
// reset (the rate bar).  A tap on the raw records, so it sees every run
// whichever measurements are running.

#include "record_tap.hpp"

#include <array>
#include <cstdint>
#include <deque>
#include <mutex>

namespace app {

class RateMonitor final : public dt::RecordTap {
public:
    struct Snapshot {
        bool active{false};
        std::array<double, dt::kNumChannels> rate_hz{};
        double dropped_hz{0};
        std::array<std::uint64_t, dt::kNumChannels> events{};
        std::uint64_t dropped{0};
        double fpga_seconds{0};          // FPGA time covered by runs since the reset
        std::uint64_t runs{0};
    };
    Snapshot snapshot();
    void reset_totals();

    void run_started(const dt::RunInfo& info) override;
    void records(const std::uint64_t* raw, std::size_t n) override;
    void run_ended(const dt::RunEnd& end) override;

private:
    struct Sample {
        std::uint64_t coarse;
        std::array<std::uint64_t, dt::kNumChannels> events;
        std::uint64_t dropped;
    };
    std::mutex mutex_;
    Snapshot snap_;
    bool have_time_{false};
    std::uint64_t coarse_{0};
    std::uint64_t run_start_coarse_{0};
    std::uint64_t marked_this_run_{0};
    std::array<std::uint64_t, dt::kNumChannels> run_events_{};
    std::uint64_t run_dropped_{0};
    std::deque<Sample> samples_;
    double seconds_before_run_{0};
    double tick_ns_{10.0};
};

}  // namespace app
