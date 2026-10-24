#pragma once

// A saved tag stream as a backend: the raw FPGA records of FILE (with its
// FILE.json sidecar: `tdcctl stream` files and the app's recordings) go
// through the same record decoder as live data, as fast as the engine takes
// them.  Every START marker in the file begins a run (a recording spans
// several runs when settings changed during it).  After the end of the file
// (or a stop) run() only idles; finished() tells the owner.
//
// The comparator settings are those of the recording (read from the
// sidecar); setting them has no effect.  Software input delays and dead
// times apply as usual.

#include "backend.hpp"
#include "record_decoder.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

namespace dt {

struct SavedStreamInfo {
    std::string path;
    bool have_sidecar{false};
    std::uint32_t design_version{0};
    double refclk_hz{8.0e6};
    std::uint32_t divisions{125000};
    int ref_bits{24};
    int stop_bits{20};
    bool ddr{false};
    int high_resolution{0};
    double coarse_tick_ns{10.0};
    double lclkin_mhz{100.0};
    int record_format{1};              // how the board sent it (2: compact, release 3); the file holds 64-bit records
    std::uint64_t coarse_uncertainty_ticks{0};   // compact: events carry the last marker's coarse time
    std::uint64_t records{0};          // from the file size
    std::array<double, kNumChannels> trigger_v{};
    std::array<double, kNumChannels> hysteresis_ohm{};
    bool have_comparator{false};
    std::string start;                 // ISO time the recording started (host clock)
    std::string note;
    double seconds{0};                 // FPGA time the recording covers (sidecar)
};
// Reads FILE.json (defaults where it is missing) and checks FILE.  Throws
// std::runtime_error if FILE is not a record file.
SavedStreamInfo read_saved_stream(const std::string& path);

class FileBackend final : public Backend {
public:
    explicit FileBackend(const std::string& path);

    std::string serial() const override { return "FILE"; }
    std::string model() const override { return "Dantagger saved tag stream"; }
    void run(Engine& engine, const std::function<bool()>& should_stop) override;
    double set_trigger(int channel, double) override { return current_trigger(channel); }
    double set_hysteresis(int channel, double) override { return current_hysteresis(channel); }
    void set_resolution(int) override {}
    double current_trigger(int channel) override;
    double current_hysteresis(int channel) override;
    timestamp_t min_deadtime() const override;
    double readout_limit() const override { return 0; }
    std::string info() override;

    const SavedStreamInfo& stream_info() const { return info_; }
    double progress() const;           // 0 .. 1 of the records
    bool finished() const { return finished_.load(); }
    std::string error() const;
    const DecoderStats& decoder_stats() const { return decoder_.stats(); }

private:
    SavedStreamInfo info_;
    RecordDecoder decoder_;
    std::atomic<std::uint64_t> done_{0};
    std::atomic<bool> finished_{false};
    mutable std::mutex mu_;
    std::string error_;
};

}  // namespace dt
