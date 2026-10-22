#pragma once

// The time tagger device as the Python layer sees it: settings, the engine,
// and the device thread that streams only while a measurement is running.
//
// Settings rule (SettingsPolicy::strict, the Python library; user,
// 2026-10-21): no setting may change while a measurement the user started
// (start/startFor) is running -> SettingsBusyError.  A measurement that is
// only running because it was created without SynchronizedMeasurements
// (Swabian autostart) does not block a change: the run is stopped, the change
// applied, and streaming resumes (with a warning; the measurement's capture
// time excludes the gap).  SettingsPolicy::restart (the app; user,
// 2026-10-22) treats every running measurement that way, quietly.
//
// Opening applies nothing (user, 2026-10-22): the comparator values are read
// back from the backend; reset() applies the defaults.

#include "backend.hpp"
#include "engine.hpp"
#include "warnings.hpp"

#include <array>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <functional>
#include <thread>

namespace dt {

enum class SettingsPolicy { strict, restart };

class Tagger {
public:
    explicit Tagger(std::unique_ptr<Backend> backend, SettingsPolicy policy = SettingsPolicy::strict);
    ~Tagger();
    Tagger(const Tagger&) = delete;
    Tagger& operator=(const Tagger&) = delete;

    Engine& engine() { return *engine_; }
    WarningLog& log() { return log_; }
    Backend& backend() { return *backend_; }

    void set_trigger_level(int channel, double volts);
    double trigger_level(int channel) const;
    double set_hysteresis(int channel, double ohms);   // clamps to the hardware range; returns the applied value
    double hysteresis(int channel) const;
    timestamp_t set_deadtime(int channel, timestamp_t ps);   // returns the effective dead time
    timestamp_t deadtime(int channel) const;
    void set_delay(int channel, timestamp_t ps);
    timestamp_t delay(int channel) const;
    void set_resolution(int factor);
    int resolution() const { return resolution_; }
    void reset();   // every setting back to its default
    SettingsPolicy policy() const { return policy_; }

    // Runs `fn` with the backend idle (no run streaming), under the settings
    // policy: for device commands of the app (comparator, LEDs, GPX2, LVDS).
    // With `allow_while_measuring` false it refuses (SettingsBusyError) while
    // any measurement runs; otherwise the run pauses and resumes around it.
    void exclusive(const char* what, const std::function<void()>& fn, bool allow_while_measuring);
    // Reads the comparator values back from the backend (after commands that
    // bypassed set_trigger_level / set_hysteresis).
    void sync_from_backend();

    bool streaming();
    void close();
    bool closed() const { return closed_; }

private:
    template <class F>
    void with_idle(const char* what, F&& apply);
    void device_loop();
    void push_software_settings();

    WarningLog log_;
    std::unique_ptr<Backend> backend_;
    std::unique_ptr<Engine> engine_;
    SettingsPolicy policy_;

    std::array<double, kNumChannels> trigger_{};
    std::array<double, kNumChannels> hysteresis_{};
    std::array<timestamp_t, kNumChannels> deadtime_{};
    std::array<timestamp_t, kNumChannels> delay_{};
    int resolution_{1};

    std::mutex dev_mu_;
    std::condition_variable dev_cv_;
    bool streaming_{false};
    bool pause_{false};
    bool quit_{false};
    std::atomic<bool> stop_flag_{false};
    bool closed_{false};
    std::thread device_thread_;
};

}  // namespace dt
