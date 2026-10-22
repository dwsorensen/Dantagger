#pragma once

// Run files written by `tdcctl stream` and the TDC Control app:
//   FILE       every record of the run, 8 bytes little-endian (PROTOCOL.md 3.3)
//   FILE.json  the settings needed to interpret it, counts and the result
// plus loading a run for analysis (counts, rates) and Swabian-style
// histograms of time differences between two channels.

#include "tdcctl/tdc_protocol.hpp"
#include "tdcctl/tdc_session.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace tdcctl {

std::string hex_bytes(const std::uint8_t* bytes, std::size_t n);
std::string iso_now();

// Writes FILE.json for a finished run.  `before` is the STATUS taken before
// START (GPX2 configuration and capture format of the run).
void write_run_sidecar(const std::string& run_path, const Response& before, const StreamResult& result,
                       double refclk_hz);

// Numbers from FILE.json (defaults when the sidecar is missing).
struct RunSettings {
    bool have_sidecar{false};
    double refclk_hz{8.0e6};
    std::uint32_t divisions{125000};
    int ref_bits{24};
    int stop_bits{20};
    double coarse_tick_ns{kCoarseTickNsR1};   // release 1 runs carry no entry
    std::uint32_t design_version{kDesignVersionR1};
    double lvds_phase{-1};
    std::uint64_t fpga_dropped{0};
    bool sidecar_pass{false};
    std::string start;                  // ISO time the run started (host clock)
};
RunSettings read_run_settings(const std::string& run_path);

struct Measurement {
    std::string path;
    RunSettings settings;
    std::uint64_t records{0};
    std::uint64_t markers{0};
    std::uint64_t time_markers{0};
    std::uint64_t overflow_markers{0};
    std::uint64_t dropped_marked{0};    // sum of OVERFLOW counts (each saturates at 4095)
    bool start_seen{false};
    bool end_seen{false};
    bool ended_by_window{false};
    double run_s{0.0};                  // START marker to END (or last) marker, FPGA coarse clock
    std::array<std::vector<std::int64_t>, kChannels> times_ps;   // per channel, relative to the first event
    std::string warning;                // the decoding stopped early (data up to there kept)

    std::uint64_t events(int channel) const { return times_ps[static_cast<std::size_t>(channel)].size(); }
    double rate_hz(int channel) const { return run_s > 0 ? static_cast<double>(events(channel)) / run_s : 0.0; }
};

// Loads FILE (and FILE.json).  `progress` (0..1) and `cancel` may be null.
// Throws std::runtime_error if the file cannot be read or is not a run file.
Measurement load_measurement(const std::string& run_path, std::atomic<float>* progress = nullptr,
                             const std::atomic<bool>* cancel = nullptr);

// Swabian Time Tagger semantics (START and STOP may be the same channel; a
// click is used as STOP before it becomes a START, so it never pairs with
// itself):
//   Histogram: multiple start, multiple stop - every STOP click is binned
//              against every earlier START click within the histogram range.
//   StartStop: each STOP click is binned against the latest START click and
//              that START is then used up (consecutive START clicks: only the
//              last one counts).
// START and STOP delays belong to the histogram, not to the channels: every
// delay is shown as STOP - START + (stop_delay - start_delay), i.e. the
// histogram slides sideways.  In Histogram mode pairs whose shifted delay
// falls in the range are counted, including a STOP before its START; in
// StartStop mode the pairing is unchanged and only the delays move.
enum class HistogramMode { histogram, start_stop };

struct HistogramRequest {
    HistogramMode mode{HistogramMode::histogram};
    int start_channel{0};               // 0..3
    int stop_channel{1};
    std::int64_t binwidth_ps{1000};
    int n_bins{1000};
    std::int64_t start_delay_ps{0};
    std::int64_t stop_delay_ps{0};
};

struct HistogramResult {
    std::vector<std::uint64_t> counts;
    std::uint64_t starts{0};
    std::uint64_t stops{0};
    std::uint64_t in_range{0};          // pairs counted in a bin
    std::uint64_t outside{0};           // StartStop pairs shifted outside the range
    bool cancelled{false};
    bool too_many_pairs{false};         // stopped at the pair budget (range too wide for the rate)
};

constexpr std::uint64_t kHistogramPairBudget = 2'000'000'000ULL;

HistogramResult compute_histogram(const Measurement& m, const HistogramRequest& request,
                                  std::atomic<float>* progress = nullptr, const std::atomic<bool>* cancel = nullptr);

}  // namespace tdcctl
