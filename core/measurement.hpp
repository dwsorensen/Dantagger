#pragma once

// Base class of every measurement.  The Engine owns the timing state (running,
// window, capture duration) and calls process() on its processing thread with
// the engine mutex held; data getters take the same mutex through guard().

#include "common.hpp"

#include <memory>
#include <mutex>
#include <string>

namespace dt {

class Engine;

class Measurement {
public:
    virtual ~Measurement() = default;

    // ---- processing thread, engine mutex held ----------------------------------
    // Tags t[0..n) (time-ordered) all lie in this measurement's window
    // [begin, end); the window may be empty of tags (n == 0) but time still
    // passed.  capture_duration does not yet include this window.
    virtual void process(const timestamp_t* t, const std::int8_t* ch, std::size_t n, timestamp_t begin,
                         timestamp_t end) = 0;
    // Overflow and missed-event markers inside the window, in time order
    // relative to the tags (process() is split around them).
    virtual void on_special(const Special& s) {
        if (s.type == TagType::OverflowBegin) on_gap();
    }
    // A discontinuity (new FPGA run, overflow): forget start/stop pairing.
    virtual void on_gap() {}
    // Swabian clear(): discard the accumulated data.
    virtual void clear_data() = 0;
    // Called when a window starts (after the start time is known).
    virtual void on_window_start(timestamp_t /*t*/) {}
    virtual const char* name() const = 0;

    // ---- engine-managed state (engine mutex) -----------------------------------
    bool running{false};
    bool explicit_start{false};      // start()/startFor() by the user (not the creation autostart)
    bool pending{false};             // window begins at the begin of the next block
    timestamp_t duration{-1};        // requested length of the current window, -1 = until stop()
    timestamp_t window_start{0};
    timestamp_t stop_time{kNever};
    timestamp_t capture_duration{0};
    timestamp_t last_time{0};        // end of the last processed window
    bool seen_data{false};           // processed at least one window since clear (for gaps)
    bool window_seen{false};         // processed data in the current window (for run gaps inside it)
    Engine* engine{nullptr};

    std::unique_lock<std::mutex> guard() const;
};

}  // namespace dt
