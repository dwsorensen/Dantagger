#pragma once

// Backends without hardware.
//
// SimBackend: synthetic signals per channel (Poisson, periodic laser,
// detector clicks correlated with a periodic channel), generated in time
// order and paced to the wall clock (`speed` = 1 real time; <= 0 as fast as
// possible).  A channel only produces tags while its trigger level is below
// the signal amplitude, so trigger scans behave plausibly.
//
// ReplayBackend: given tags (time-ordered), replayed into the engine; after
// the last tag the timeline keeps advancing (empty) so windows can finish.
// The first run starts at time 0, so the timeline equals the given times.

#include "backend.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <random>
#include <vector>

namespace dt {

struct SimSignal {
    enum class Kind { none, poisson, periodic, detector };
    Kind kind{Kind::none};
    double rate_hz{0};          // poisson; periodic: repetition rate
    double jitter_ps{0};        // gaussian sigma (periodic, detector), clamped to +-8 sigma
    double phase_ps{0};         // periodic: time of pulse 0
    int ref{0};                 // detector: periodic reference channel
    double efficiency{0};       // detector: click probability per reference pulse
    double delay_ps{0};         // detector: delay from the reference pulse
    double dark_hz{0};          // detector: extra Poisson clicks
    double amplitude_v{1.0};    // tags only while the trigger level is below this
};

class SimBackend final : public Backend {
public:
    SimBackend(const std::array<SimSignal, kNumChannels>& signals, double speed, std::uint64_t seed);
    std::string serial() const override { return "SIMULATED"; }
    std::string model() const override { return "Dantagger simulated TDC"; }
    void run(Engine& engine, const std::function<bool()>& should_stop) override;
    double set_trigger(int channel, double volts) override;
    double set_hysteresis(int channel, double ohms) override {
        hysteresis_[static_cast<std::size_t>(channel - 1)] = ohms;
        return ohms;
    }
    void set_resolution(int factor) override { resolution_ = factor; }
    double current_trigger(int channel) override { return trigger_[static_cast<std::size_t>(channel - 1)]; }
    double current_hysteresis(int channel) override { return hysteresis_[static_cast<std::size_t>(channel - 1)]; }
    int current_resolution() override { return resolution_; }
    timestamp_t min_deadtime() const override { return 0; }
    double readout_limit() const override { return 0; }
    std::string info() override;

private:
    void generate(timestamp_t t0, timestamp_t t1, RawChunk& chunk);
    std::array<SimSignal, kNumChannels> signals_;
    // A new simulated board starts at the values reset() applies.
    std::array<double, kNumChannels> trigger_{{kDefaultTriggerVolts, kDefaultTriggerVolts, kDefaultTriggerVolts,
                                               kDefaultTriggerVolts}};
    std::array<double, kNumChannels> hysteresis_{{kDefaultHysteresisOhms, kDefaultHysteresisOhms, kDefaultHysteresisOhms,
                                                  kDefaultHysteresisOhms}};
    double speed_;
    std::mt19937_64 rng_;
    std::array<std::vector<timestamp_t>, kNumChannels> pending_;
    timestamp_t margin_{0};
    timestamp_t last_end_{0};
    std::chrono::steady_clock::time_point opened_;
    int resolution_{1};
};

class ReplayBackend final : public Backend {
public:
    ReplayBackend(std::vector<timestamp_t> times, std::vector<std::int8_t> channels, double speed);
    std::string serial() const override { return "REPLAY"; }
    std::string model() const override { return "Dantagger replay"; }
    void run(Engine& engine, const std::function<bool()>& should_stop) override;
    double set_trigger(int, double volts) override { return volts; }
    double set_hysteresis(int, double ohms) override { return ohms; }
    void set_resolution(int) override {}
    double current_trigger(int) override { return kDefaultTriggerVolts; }
    double current_hysteresis(int) override { return kDefaultHysteresisOhms; }
    timestamp_t min_deadtime() const override { return 0; }
    double readout_limit() const override { return 0; }
    std::string info() override { return "replay of " + std::to_string(times_.size()) + " tags"; }

private:
    std::vector<timestamp_t> times_;
    std::vector<std::int8_t> channels_;
    double speed_;
    std::size_t cursor_{0};
    timestamp_t position_{0};
    bool first_run_{true};
};

}  // namespace dt
