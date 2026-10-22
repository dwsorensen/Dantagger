#include "engine.hpp"

#include <algorithm>
#include <cstdio>

namespace dt {

namespace {
constexpr std::size_t kQueueBudgetBytes = 256u << 20;   // then the backend waits (USB back-pressure)

std::string seconds_text(timestamp_t ps) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.6f s", static_cast<double>(ps) * 1e-12);
    return buf;
}
}  // namespace

Engine::Engine(WarningLog& log) : log_(log) { thread_ = std::thread([this] { loop(); }); }

Engine::~Engine() {
    {
        std::lock_guard<std::mutex> lock(qmu_);
        quit_ = true;
    }
    q_cv_.notify_all();
    space_cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void Engine::set_demand_callback(std::function<void()> cb) {
    std::lock_guard<std::mutex> lock(cb_mu_);
    demand_cb_ = std::move(cb);
}

void Engine::recount_locked() {
    int n = 0;
    for (const auto& m : measurements_)
        if (m->running) ++n;
    n_running_.store(n);
}

void Engine::notify_demand() {
    std::function<void()> cb;
    {
        std::lock_guard<std::mutex> lock(cb_mu_);
        cb = demand_cb_;
    }
    if (cb) cb();
}

void Engine::push(RawChunk&& chunk) {
    const std::size_t bytes = chunk.bytes();
    std::unique_lock<std::mutex> lock(qmu_);
    space_cv_.wait(lock, [&] { return quit_ || queued_bytes_ < kQueueBudgetBytes || queue_.empty(); });
    if (quit_) return;
    queued_bytes_ += bytes;
    queue_.push_back(std::move(chunk));
    q_cv_.notify_one();
}

void Engine::wait_drained() {
    std::unique_lock<std::mutex> lock(qmu_);
    drained_cv_.wait(lock, [&] { return quit_ || (queue_.empty() && !processing_); });
}

bool Engine::drained() {
    std::lock_guard<std::mutex> lock(qmu_);
    return queue_.empty() && !processing_;
}

void Engine::configure(const std::array<timestamp_t, kNumChannels>& delays,
                       const std::array<timestamp_t, kNumChannels>& deadtimes) {
    wait_drained();
    std::lock_guard<std::mutex> lock(qmu_);   // the processing thread is idle and stays so while we hold qmu_
    merger_.configure(delays, deadtimes);
}

void Engine::loop() {
    set_thread_name("dt-engine");
    while (true) {
        RawChunk chunk;
        {
            std::unique_lock<std::mutex> lock(qmu_);
            if (queue_.empty()) {
                processing_ = false;
                drained_cv_.notify_all();
            }
            q_cv_.wait(lock, [&] { return quit_ || !queue_.empty(); });
            if (quit_) {
                processing_ = false;
                drained_cv_.notify_all();
                return;
            }
            chunk = std::move(queue_.front());
            queue_.pop_front();
            queued_bytes_ -= std::min(queued_bytes_, chunk.bytes());
            processing_ = true;
        }
        space_cv_.notify_all();
        bool demand_changed = false;
        if (merger_.push(chunk, block_)) {
            watch(block_);
            std::lock_guard<std::mutex> lock(mu_);
            dispatch(block_, demand_changed);
        }
        position_.store(merger_.position());
        tags_processed_.fetch_add(block_.time.size());
        deadtime_dropped_.store(merger_.deadtime_dropped());
        if (demand_changed) notify_demand();
    }
}

void Engine::watch(const TagBlock& block) {
    // Overflow: MissedEvents markers (FPGA FIFO full).  Aggregated so the log
    // shows one banner per second at most, and always at least one.
    for (const auto& s : block.specials) {
        if (s.type != TagType::MissedEvents) continue;
        if (pending_dropped_ == 0) pending_drop_first_ = s.time;
        pending_dropped_ += s.missed;
        pending_drop_last_ = s.time;
    }
    const auto now = std::chrono::steady_clock::now();
    bool queue_empty;
    {
        std::lock_guard<std::mutex> lock(qmu_);
        queue_empty = queue_.empty();
    }
    if (pending_dropped_ > 0 && (now - last_drop_warning_ > std::chrono::seconds(1) || queue_empty)) {
        log_.add_dropped(pending_dropped_);
        log_.push("overflow", std::to_string(pending_dropped_) +
                                  " EVENTS WERE LOST (the FPGA event FIFO overflowed: the USB link or this computer "
                                  "could not keep up). Tag time " + seconds_text(pending_drop_first_) + " to " +
                                  seconds_text(pending_drop_last_) +
                                  ". Data in that interval is incomplete; count rates are LOWER BOUNDS.");
        pending_dropped_ = 0;
        last_drop_warning_ = now;
    }

    // Readout saturation of the GPX2: its 16-deep per-channel FIFO loses
    // events silently when a channel stays near the LVDS frame rate.
    const double limit = saturation_limit_.load();
    if (limit <= 0 || block.end <= block.begin) return;
    if (block.run_begin || sat_since_ == kUnset) {
        sat_counts_.fill(0);
        sat_since_ = block.begin;
    }
    for (auto c : block.channel) ++sat_counts_[static_cast<std::size_t>(c)];
    const timestamp_t span = block.end - sat_since_;
    if (span < 10'000'000'000) return;   // judge over >= 10 ms of tag time
    for (int c = 1; c <= kNumChannels; ++c) {
        const double rate = static_cast<double>(sat_counts_[static_cast<std::size_t>(c)]) / (static_cast<double>(span) * 1e-12);
        if (rate > 0.9 * limit && now - last_sat_warning_ > std::chrono::seconds(2)) {
            char buf[512];
            std::snprintf(buf, sizeof buf,
                          "CHANNEL %d RUNS AT %.3f M events/s, AT THE TDC CHIP READOUT LIMIT (%.3f M/s per channel). "
                          "The TDC-GPX2 drops events inside the chip without any count: rates and histograms on "
                          "channel %d are probably INCOMPLETE.",
                          c, rate * 1e-6, limit * 1e-6, c);
            log_.push("saturation", buf);
            last_sat_warning_ = now;
        }
    }
    sat_counts_.fill(0);
    sat_since_ = block.end;
}

void Engine::dispatch(const TagBlock& block, bool& demand_changed) {
    const std::size_t n = block.time.size();
    const timestamp_t* const t = block.time.data();
    const std::int8_t* const ch = block.channel.data();
    bool finished = false;
    for (const auto& mp : measurements_) {
        Measurement& m = *mp;
        if (!m.running) continue;
        if (m.pending) {
            m.pending = false;
            m.window_start = block.begin;
            m.stop_time = m.duration >= 0 ? block.begin + m.duration : kNever;
            m.window_seen = false;
            m.on_window_start(block.begin);
        }
        if (block.run_begin && m.seen_data) m.on_gap();
        // Time between runs is not captured: a timed window covers `duration`
        // of captured time, so its end moves out by the gap (a run that was
        // still ending when the window began, or a settings change).
        if (m.window_seen && block.begin > m.last_time && m.stop_time != kNever) m.stop_time += block.begin - m.last_time;
        const timestamp_t ws = std::max(block.begin, m.window_start);
        const timestamp_t we = std::min(block.end, m.stop_time);
        if (we > ws) {
            const std::size_t lo = ws <= block.begin ? 0 : static_cast<std::size_t>(std::lower_bound(t, t + n, ws) - t);
            const std::size_t hi = we >= block.end ? n : static_cast<std::size_t>(std::lower_bound(t, t + n, we) - t);
            std::size_t pos = lo;
            timestamp_t seg_begin = ws;
            for (const auto& s : block.specials) {
                if (s.time < ws) continue;
                if (s.time >= we) break;
                const std::size_t cut = static_cast<std::size_t>(std::lower_bound(t + pos, t + hi, s.time) - t);
                m.process(t + pos, ch + pos, cut - pos, seg_begin, s.time);
                m.capture_duration += s.time - seg_begin;
                m.on_special(s);
                pos = cut;
                seg_begin = s.time;
            }
            m.process(t + pos, ch + pos, hi - pos, seg_begin, we);
            m.capture_duration += we - seg_begin;
            m.last_time = we;
            m.seen_data = true;
            m.window_seen = true;
        }
        if (m.stop_time <= block.end) {
            m.running = false;
            finished = true;
        }
    }
    if (finished) {
        recount_locked();
        finished_cv_.notify_all();
        demand_changed = true;
    }
}

void Engine::add(const std::shared_ptr<Measurement>& m, bool autostart) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        m->engine = this;
        if (autostart) {
            m->running = true;
            m->pending = true;
            m->duration = -1;
            m->explicit_start = false;
        }
        measurements_.push_back(m);
        recount_locked();
    }
    if (autostart) notify_demand();
}

void Engine::remove(Measurement* m) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        measurements_.erase(std::remove_if(measurements_.begin(), measurements_.end(),
                                           [&](const std::shared_ptr<Measurement>& p) { return p.get() == m; }),
                            measurements_.end());
        recount_locked();
        finished_cv_.notify_all();
    }
    notify_demand();
}

void Engine::start(const std::vector<Measurement*>& ms, timestamp_t duration, bool clear) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!error_.empty()) throw DeviceError(error_);
        for (Measurement* m : ms) {
            if (clear) {
                m->clear_data();
                m->capture_duration = 0;
                m->seen_data = false;
            }
            m->running = true;
            m->pending = true;
            m->duration = duration;
            m->explicit_start = true;
        }
        recount_locked();
    }
    notify_demand();
}

void Engine::stop(const std::vector<Measurement*>& ms) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        for (Measurement* m : ms) {
            m->running = false;
            m->pending = false;
        }
        recount_locked();
        finished_cv_.notify_all();
    }
    notify_demand();
}

void Engine::clear(const std::vector<Measurement*>& ms) {
    std::lock_guard<std::mutex> lock(mu_);
    for (Measurement* m : ms) {
        m->clear_data();
        m->capture_duration = 0;
        m->seen_data = false;
    }
}

bool Engine::wait(const std::vector<Measurement*>& ms, int timeout_ms) {
    std::unique_lock<std::mutex> lock(mu_);
    auto done = [&] {
        if (!error_.empty()) return true;
        for (Measurement* m : ms)
            if (m->running) return false;
        return true;
    };
    bool ok;
    if (timeout_ms < 0) {
        finished_cv_.wait(lock, done);
        ok = true;
    } else {
        ok = finished_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), done);
    }
    if (!error_.empty()) throw DeviceError(error_);
    return ok;
}

bool Engine::any_running(const std::vector<Measurement*>& ms) {
    std::lock_guard<std::mutex> lock(mu_);
    for (Measurement* m : ms)
        if (m->running) return true;
    return false;
}

bool Engine::demand() {
    std::lock_guard<std::mutex> lock(mu_);
    if (!error_.empty()) return false;
    for (const auto& m : measurements_)
        if (m->running) return true;
    return false;
}

bool Engine::explicit_running() {
    std::lock_guard<std::mutex> lock(mu_);
    for (const auto& m : measurements_)
        if (m->running && m->explicit_start) return true;
    return false;
}

void Engine::fail(const std::string& message) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (error_.empty()) error_ = message;
        for (const auto& m : measurements_) {
            m->running = false;
            m->pending = false;
        }
        recount_locked();
        failed_.store(true);
        finished_cv_.notify_all();
    }
    log_.push("device", "THE TDC DEVICE FAILED: " + message + ". Every running measurement was stopped.");
    notify_demand();
}

std::string Engine::error() {
    std::lock_guard<std::mutex> lock(mu_);
    return error_;
}

void Engine::clear_error() {
    std::lock_guard<std::mutex> lock(mu_);
    error_.clear();
    failed_.store(false);
}

}  // namespace dt
