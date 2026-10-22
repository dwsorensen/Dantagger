#include "tagger.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cmath>
#include <stdexcept>

namespace dt {

Tagger::Tagger(std::unique_ptr<Backend> backend, SettingsPolicy policy) : backend_(std::move(backend)), policy_(policy) {
    engine_ = std::make_unique<Engine>(log_);
    engine_->set_saturation_limit(backend_->readout_limit());
    engine_->set_demand_callback([this] {
        if (!engine_->demand_fast()) stop_flag_.store(true);
        dev_cv_.notify_all();   // without dev_mu_: the device thread also polls every 10 ms
    });
    sync_from_backend();
    push_software_settings();
    device_thread_ = std::thread([this] { device_loop(); });
}

Tagger::~Tagger() {
    try {
        close();
    } catch (...) {
    }
}

void Tagger::close() {
    if (closed_) return;
    {
        std::lock_guard<std::mutex> lock(dev_mu_);
        quit_ = true;
        stop_flag_.store(true);
    }
    dev_cv_.notify_all();
    if (device_thread_.joinable()) device_thread_.join();
    engine_->wait_drained();
    backend_->close();
    closed_ = true;
}

void Tagger::device_loop() {
    std::unique_lock<std::mutex> lock(dev_mu_);
    while (!quit_) {
        if (pause_ || !engine_->demand_fast()) {
            dev_cv_.wait_for(lock, std::chrono::milliseconds(10));
            continue;
        }
        streaming_ = true;
        stop_flag_.store(false);
        lock.unlock();
        try {
            backend_->run(*engine_, [this] { return stop_flag_.load() || !engine_->demand_fast(); });
        } catch (const std::exception& failure) {
            engine_->fail(failure.what());
        }
        lock.lock();
        streaming_ = false;
        dev_cv_.notify_all();
    }
}

bool Tagger::streaming() {
    std::lock_guard<std::mutex> lock(dev_mu_);
    return streaming_;
}

void Tagger::sync_from_backend() {
    for (int c = 1; c <= kNumChannels; ++c) {
        trigger_[static_cast<std::size_t>(c - 1)] = backend_->current_trigger(c);
        hysteresis_[static_cast<std::size_t>(c - 1)] = backend_->current_hysteresis(c);
    }
    resolution_ = backend_->current_resolution();
}

void Tagger::exclusive(const char* what, const std::function<void()>& fn, bool allow_while_measuring) {
    if (!allow_while_measuring && engine_->demand())
        throw SettingsBusyError(std::string("stop the running measurements before: ") + what);
    with_idle(what, fn);
}

template <class F>
void Tagger::with_idle(const char* what, F&& apply) {
    if (closed_) throw std::runtime_error("the time tagger has been freed");
    std::unique_lock<std::mutex> lock(dev_mu_);
    if (policy_ == SettingsPolicy::strict && engine_->explicit_running())
        throw SettingsBusyError(std::string("cannot change the ") + what +
                                " while a measurement is running: stop it (or wait for startFor to finish) first");
    pause_ = true;
    if (streaming_) {
        // Still streaming: either a finished window's run is ending (normal),
        // or an autostarted measurement keeps it going (tell the user).
        if (policy_ == SettingsPolicy::strict && engine_->demand_fast())
            log_.push("restart", std::string("The ") + what +
                                 " changed while a measurement created without SynchronizedMeasurements was "
                                 "accumulating: streaming paused for the change and resumed; that measurement "
                                 "now holds data from before and after the change.");
        stop_flag_.store(true);
        dev_cv_.wait(lock, [&] { return !streaming_; });
    }
    try {
        engine_->wait_drained();
        apply();
    } catch (...) {
        pause_ = false;
        dev_cv_.notify_all();
        throw;
    }
    pause_ = false;
    dev_cv_.notify_all();
}

void Tagger::set_trigger_level(int channel, double volts) {
    check_channel(channel);
    if (!std::isfinite(volts) || volts < kTriggerMinVolts - 1e-9 || volts > kTriggerMaxVolts + 1e-9) {
        char buf[200];
        std::snprintf(buf, sizeof buf,
                      "trigger level %.4f V on channel %d is outside the TDC board's safe range %.3f .. %+.3f V",
                      volts, channel, kTriggerMinVolts, kTriggerMaxVolts);
        throw std::invalid_argument(buf);
    }
    with_idle("trigger level", [&] { trigger_[static_cast<std::size_t>(channel - 1)] = backend_->set_trigger(channel, volts); });
}

double Tagger::trigger_level(int channel) const {
    check_channel(channel);
    return trigger_[static_cast<std::size_t>(channel - 1)];
}

double Tagger::set_hysteresis(int channel, double ohms) {
    check_channel(channel);
    if (!std::isfinite(ohms)) throw std::invalid_argument("hysteresis must be a finite resistance in ohms");
    const double clamped = std::min(kHysteresisMaxOhms, std::max(kHysteresisMinOhms, ohms));
    double applied = clamped;
    with_idle("hysteresis", [&] {
        applied = backend_->set_hysteresis(channel, clamped);
        hysteresis_[static_cast<std::size_t>(channel - 1)] = applied;
    });
    return applied;
}

double Tagger::hysteresis(int channel) const {
    check_channel(channel);
    return hysteresis_[static_cast<std::size_t>(channel - 1)];
}

timestamp_t Tagger::set_deadtime(int channel, timestamp_t ps) {
    check_channel(channel);
    if (ps < 0) throw std::invalid_argument("dead time must be >= 0 ps");
    with_idle("dead time", [&] {
        deadtime_[static_cast<std::size_t>(channel - 1)] = ps;
        push_software_settings();
    });
    return deadtime(channel);
}

timestamp_t Tagger::deadtime(int channel) const {
    check_channel(channel);
    return std::max(deadtime_[static_cast<std::size_t>(channel - 1)], backend_->min_deadtime());
}

void Tagger::set_delay(int channel, timestamp_t ps) {
    check_channel(channel);
    if (ps > 1'000'000'000'000 || ps < -1'000'000'000'000)
        throw std::invalid_argument("input delay must be within +-1 s");
    with_idle("input delay", [&] {
        delay_[static_cast<std::size_t>(channel - 1)] = ps;
        push_software_settings();
    });
}

timestamp_t Tagger::delay(int channel) const {
    check_channel(channel);
    return delay_[static_cast<std::size_t>(channel - 1)];
}

void Tagger::set_resolution(int factor) {
    if (factor != 1 && factor != 2 && factor != 4)
        throw std::invalid_argument("resolution factor must be 1 (standard), 2 or 4 (GPX2 HIGH_RESOLUTION)");
    with_idle("resolution", [&] {
        backend_->set_resolution(factor);
        resolution_ = factor;
    });
}

void Tagger::push_software_settings() {
    std::array<timestamp_t, kNumChannels> dead{};
    for (int c = 0; c < kNumChannels; ++c) {
        const timestamp_t d = deadtime_[static_cast<std::size_t>(c)];
        dead[static_cast<std::size_t>(c)] = d > backend_->min_deadtime() ? d : 0;   // the hardware does the rest
    }
    engine_->configure(delay_, dead);
}

void Tagger::reset() {
    with_idle("settings (reset)", [&] {
        for (int c = 1; c <= kNumChannels; ++c) {
            trigger_[static_cast<std::size_t>(c - 1)] = backend_->set_trigger(c, kDefaultTriggerVolts);
            hysteresis_[static_cast<std::size_t>(c - 1)] = backend_->set_hysteresis(c, kDefaultHysteresisOhms);
        }
        deadtime_.fill(0);
        delay_.fill(0);
        if (resolution_ != 1) {
            backend_->set_resolution(1);
            resolution_ = 1;
        }
        push_software_settings();
    });
}

}  // namespace dt
