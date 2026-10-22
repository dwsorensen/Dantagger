#pragma once

// The measurement engine: one processing thread takes raw chunks from the
// backend, merges them into time-ordered tag blocks and runs every running
// measurement over its window of each block.
//
// Timing model (Swabian semantics): a measurement's window is measured on the
// tag timeline.  start()/startFor() take effect at the beginning of the next
// block the engine processes (the stream position at that moment, or the
// start of the next FPGA run when the board is idle); startFor(d) then covers
// exactly d ps of captured time (time between runs does not count).  Synchronized groups are changed atomically
// (one mutex covers block dispatch and every control call), so they share
// their window edges exactly.

#include "common.hpp"
#include "measurement.hpp"
#include "merger.hpp"
#include "warnings.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace dt {

class Engine {
public:
    explicit Engine(WarningLog& log);
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    std::mutex& mutex() { return mu_; }

    // Called (without engine locks held) whenever the number of running
    // measurements may have changed.
    void set_demand_callback(std::function<void()> cb);

    // ---- backend thread ----------------------------------------------------------
    // Queues a chunk; blocks while more than the queue budget is waiting.
    void push(RawChunk&& chunk);

    // ---- control (any thread) ----------------------------------------------------
    // Waits until every queued chunk has been processed.
    void wait_drained();
    // Without waiting: whether every queued chunk has been processed.
    bool drained();
    // Only while no run is streaming (the caller guarantees it).
    void configure(const std::array<timestamp_t, kNumChannels>& delays,
                   const std::array<timestamp_t, kNumChannels>& deadtimes);
    // Readout limit per channel (events/s) for the saturation warning; 0 = off.
    void set_saturation_limit(double events_per_s) { saturation_limit_ = events_per_s; }

    void add(const std::shared_ptr<Measurement>& m, bool autostart);
    void remove(Measurement* m);
    void start(const std::vector<Measurement*>& ms, timestamp_t duration, bool clear);
    void stop(const std::vector<Measurement*>& ms);
    void clear(const std::vector<Measurement*>& ms);
    // True when none of `ms` is running any more (false on timeout).
    // timeout_ms < 0 waits for ever.  Throws DeviceError after a device failure.
    bool wait(const std::vector<Measurement*>& ms, int timeout_ms);
    bool any_running(const std::vector<Measurement*>& ms);

    bool demand();             // any measurement running
    // Lock-free version (atomic count kept up to date under the mutex).
    bool demand_fast() const { return n_running_.load() > 0 && !failed_.load(); }
    bool explicit_running();   // any running that the user started explicitly

    void fail(const std::string& message);
    std::string error();
    void clear_error();

    timestamp_t position() const { return position_.load(); }
    std::uint64_t tags_processed() const { return tags_processed_.load(); }
    std::uint64_t deadtime_dropped() const { return deadtime_dropped_.load(); }
    WarningLog& log() { return log_; }

private:
    void loop();
    void dispatch(const TagBlock& block, bool& demand_changed);
    void watch(const TagBlock& block);
    void notify_demand();
    void recount_locked();   // updates n_running_ (mu_ held)

    WarningLog& log_;
    std::mutex mu_;                       // measurements + their data
    std::condition_variable finished_cv_;
    std::vector<std::shared_ptr<Measurement>> measurements_;
    std::string error_;

    std::mutex qmu_;
    std::condition_variable q_cv_;
    std::condition_variable space_cv_;
    std::condition_variable drained_cv_;
    std::deque<RawChunk> queue_;
    std::size_t queued_bytes_{0};
    bool processing_{false};
    bool quit_{false};

    Merger merger_;
    TagBlock block_;
    std::function<void()> demand_cb_;
    std::mutex cb_mu_;

    std::atomic<int> n_running_{0};
    std::atomic<bool> failed_{false};
    std::atomic<timestamp_t> position_{0};
    std::atomic<std::uint64_t> tags_processed_{0};
    std::atomic<std::uint64_t> deadtime_dropped_{0};
    std::atomic<double> saturation_limit_{0.0};

    // warning aggregation
    std::uint64_t pending_dropped_{0};
    timestamp_t pending_drop_first_{0}, pending_drop_last_{0};
    std::chrono::steady_clock::time_point last_drop_warning_{};
    std::array<std::int64_t, kNumChannels + 1> sat_counts_{};
    timestamp_t sat_since_{kUnset};
    std::chrono::steady_clock::time_point last_sat_warning_{};

    std::thread thread_;
};

}  // namespace dt
