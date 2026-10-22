#include "rate_monitor.hpp"

#include "tdcctl/tdc_protocol.hpp"

namespace app {

using namespace tdcctl;

namespace {
constexpr double kSampleEveryNs = 50e6;   // 50 ms between rate samples
constexpr double kRateWindowNs = 1e9;     // 1 s
}  // namespace

RateMonitor::Snapshot RateMonitor::snapshot() {
    std::lock_guard<std::mutex> lock(mutex_);
    return snap_;
}

void RateMonitor::reset_totals() {
    std::lock_guard<std::mutex> lock(mutex_);
    snap_.events = {};
    snap_.dropped = 0;
    snap_.fpga_seconds = 0;
    snap_.runs = 0;
    seconds_before_run_ = 0;
    run_start_coarse_ = coarse_;
    run_events_ = {};
    run_dropped_ = 0;
    samples_.clear();
    have_time_ = false;
}

void RateMonitor::run_started(const dt::RunInfo& info) {
    std::lock_guard<std::mutex> lock(mutex_);
    tick_ns_ = info.coarse_tick_ns > 0 ? info.coarse_tick_ns : kCoarseTickNsR1;
    snap_.active = true;
    snap_.rate_hz = {};
    snap_.dropped_hz = 0;
    ++snap_.runs;
    have_time_ = false;
    samples_.clear();
    marked_this_run_ = 0;
    run_events_ = {};
    run_dropped_ = 0;
    seconds_before_run_ = snap_.fpga_seconds;
}

void RateMonitor::records(const std::uint64_t* raw, std::size_t n) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (std::size_t i = 0; i < n; ++i) {
        const auto r = decode_record(raw[i]);
        if (!r.marker) {
            ++snap_.events[static_cast<std::size_t>(r.channel)];
            ++run_events_[static_cast<std::size_t>(r.channel)];
            continue;
        }
        if (r.marker_type == MarkerType::overflow) {
            snap_.dropped += r.info;
            run_dropped_ += r.info;
            marked_this_run_ += r.info;
        }
        coarse_ = r.coarse;
        if (!have_time_) {
            have_time_ = true;
            run_start_coarse_ = coarse_;
            samples_.push_back({coarse_, run_events_, run_dropped_});
            continue;
        }
        const auto sample_every = static_cast<std::uint64_t>(kSampleEveryNs / tick_ns_);
        const auto rate_window = static_cast<std::uint64_t>(kRateWindowNs / tick_ns_);
        if (coarse_ - samples_.back().coarse >= sample_every) {
            samples_.push_back({coarse_, run_events_, run_dropped_});
            while (samples_.size() > 2 && coarse_ - samples_[1].coarse >= rate_window) samples_.pop_front();
            const auto& old = samples_.front();
            const double dt = static_cast<double>(coarse_ - old.coarse) * tick_ns_ * 1e-9;
            if (dt > 0) {
                for (std::size_t c = 0; c < static_cast<std::size_t>(kChannels); ++c)
                    snap_.rate_hz[c] = static_cast<double>(run_events_[c] - old.events[c]) / dt;
                snap_.dropped_hz = static_cast<double>(run_dropped_ - old.dropped) / dt;
            }
        }
        snap_.fpga_seconds = seconds_before_run_ + static_cast<double>(coarse_ - run_start_coarse_) * tick_ns_ * 1e-9;
    }
}

void RateMonitor::run_ended(const dt::RunEnd& end) {
    std::lock_guard<std::mutex> lock(mutex_);
    snap_.active = false;
    // OVERFLOW markers saturate at 4095 each; the FPGA's own count is exact.
    if (end.fpga_dropped > marked_this_run_) snap_.dropped += end.fpga_dropped - marked_this_run_;
}

}  // namespace app
