#pragma once

// A source of time tags: the TDC board (or its simulated FPGA), the signal
// simulator, or a replay of given tags.  The Tagger's device thread calls
// run() while measurements are running; settings are applied only between
// runs (the firmware refuses comparator commands while streaming).

#include "common.hpp"

#include <functional>
#include <string>

namespace dt {

class Engine;

class Backend {
public:
    virtual ~Backend() = default;
    virtual std::string serial() const = 0;
    virtual std::string model() const = 0;

    // Streams one run into `engine`: the first chunk has run_begin, the last
    // run_end.  Returns when should_stop() becomes true (polled every few ms).
    // Throws on a device failure (after closing the run with a run_end chunk
    // if it had begun).
    virtual void run(Engine& engine, const std::function<bool()>& should_stop) = 0;

    // ---- between runs only ----------------------------------------------------
    virtual double set_trigger(int channel, double volts) = 0;        // returns the applied level
    virtual double set_hysteresis(int channel, double ohms) = 0;      // returns the applied value
    virtual void set_resolution(int factor) = 0;                      // 1, 2 or 4
    // What the comparator is set to now (read back from the board where it
    // has one).  Nothing is applied on open (user, 2026-10-22).
    virtual double current_trigger(int channel) = 0;
    virtual double current_hysteresis(int channel) = 0;
    virtual int current_resolution() { return 1; }
    // Shortest pulse-to-pulse spacing the hardware resolves (ps).
    virtual timestamp_t min_deadtime() const = 0;
    // Readout limit per channel in events/s (0: none), for the saturation warning.
    virtual double readout_limit() const = 0;
    // Multi-line description (configuration, calibration, link).
    virtual std::string info() = 0;
    virtual void close() {}
};

}  // namespace dt
