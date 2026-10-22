#pragma once

// Raw FPGA records as they arrive from the board, for consumers that need
// the unprocessed stream (the app's tag-stream recorder and its rate bar).
// The TDC backend calls every registered tap on its reader thread, so a tap
// must be quick (queue the work elsewhere).  The Python layer never registers
// one: Dantagger itself never saves tags (user, 2026-10-21).

#include "common.hpp"

#include <array>
#include <cstdint>
#include <string>

namespace dt {

// What a run's records need to be interpreted (also written to a saved
// stream's .json sidecar).
struct RunInfo {
    std::uint32_t design_version{0};
    double refclk_hz{8.0e6};
    std::uint32_t divisions{125000};
    int ref_bits{24};
    int stop_bits{20};
    bool ddr{false};
    int high_resolution{0};            // GPX2 HIGH_RESOLUTION code (0 off, 1 2x, 2 4x)
    double coarse_tick_ns{10.0};
    double lclkin_mhz{100.0};
    int bit_phase{0};
    int lvds_phase{0};
    std::array<std::uint8_t, 17> gpx_registers{};
    std::array<double, kNumChannels> trigger_v{};
    std::array<double, kNumChannels> hysteresis_ohm{};
    std::uint64_t run_id{0};
};

struct RunEnd {
    bool pass{false};
    std::string failure;
    std::uint64_t records{0};
    std::uint64_t fpga_dropped{0};     // from the final response (0 if none)
};

class RecordTap {
public:
    virtual ~RecordTap() = default;
    virtual void run_started(const RunInfo& info) = 0;
    virtual void records(const std::uint64_t* raw, std::size_t n) = 0;
    virtual void run_ended(const RunEnd& end) = 0;
};

}  // namespace dt
