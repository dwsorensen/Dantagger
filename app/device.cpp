#include "device.hpp"

#include "config.hpp"

#include "tdcctl/fake_fpga.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <random>

namespace app {

using tdcctl::Response;
using tdcctl::Session;
using tdcctl::Status;

namespace {

std::string clock_now() {
    char buf[16];
    const std::time_t t = std::time(nullptr);
    std::strftime(buf, sizeof(buf), "%H:%M:%S", std::localtime(&t));
    return buf;
}

// Signals of the simulated board (--simulate): a 1 MHz pulsed laser on
// channel 4, two detectors clicking on 10 % and 8 % of its pulses 31.55 and
// 31.75 ns later (25 / 35 ps jitter, 300 /s dark counts) on channels 1 and 2,
// 50 kHz Poisson on channel 3.  A channel's comparator matters: no pulses
// above the trigger once it exceeds the pulse height, and noise chatter when
// the trigger sits within a few mV of the baseline (so a trigger scan looks
// like one on the bench).
struct SimSignals {
    tdcctl::FakeFpga* fake;
    std::shared_ptr<std::mt19937_64> rng{std::make_shared<std::mt19937_64>(2026)};

    void operator()(std::uint64_t t0, std::uint64_t t1, std::vector<std::pair<std::uint64_t, int>>& out) const {
        static const double amplitude[4] = {0.6, 0.6, 1.0, 0.8};
        double trig[4];
        for (int c = 0; c < 4; ++c) trig[c] = tdcctl::threshold_volts(fake->dac[static_cast<std::size_t>(c)]);
        auto armed = [&](int c) { return trig[c] < amplitude[c]; };
        std::normal_distribution<double> gauss(0.0, 1.0);
        std::uniform_real_distribution<double> uni(0.0, 1.0);
        auto& r = *rng;
        const std::uint64_t period = 1'000'000;
        for (std::uint64_t k = (t0 + period - 1) / period; k * period < t1; ++k) {
            const double laser = static_cast<double>(k * period) + 3.0 * gauss(r);
            if (armed(3)) out.emplace_back(static_cast<std::uint64_t>(laser), 3);
            if (armed(0) && uni(r) < 0.10) out.emplace_back(static_cast<std::uint64_t>(laser + 31'550 + 25.0 * gauss(r)), 0);
            if (armed(1) && uni(r) < 0.08) out.emplace_back(static_cast<std::uint64_t>(laser + 31'750 + 35.0 * gauss(r)), 1);
        }
        auto poisson = [&](int c, double hz) {
            std::exponential_distribution<double> gap(hz * 1e-12);
            for (double t = static_cast<double>(t0) + gap(r); t < static_cast<double>(t1); t += gap(r))
                out.emplace_back(static_cast<std::uint64_t>(t), c);
        };
        if (armed(2)) poisson(2, 50'000);
        for (int c = 0; c < 2; ++c)
            if (armed(c)) poisson(c, 300);
        for (int c = 0; c < 4; ++c)
            if (std::fabs(trig[c]) < 0.004) poisson(c, 200'000);   // chatter on the baseline noise
    }
};

}  // namespace

Device::Device(Options options) : options_(std::move(options)), rates_(std::make_shared<RateMonitor>()) {
    channels_path_ = config_dir() + "/channels.ini";
    load_channels();
    thread_ = std::thread([this] { worker(); });
}

Device::~Device() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        quit_ = true;
        jobs_.clear();
    }
    abort_ = true;
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    do_disconnect("");
}

std::shared_ptr<dt::Tagger> Device::tagger() {
    std::lock_guard<std::mutex> lock(tagger_mu_);
    return tagger_;
}

dt::TdcBackend* Device::backend() {
    std::lock_guard<std::mutex> lock(tagger_mu_);
    return backend_;
}

void Device::log(Level level, const std::string& text) {
    std::lock_guard<std::mutex> lock(mutex_);
    log_.push_back({clock_now(), text, level});
}

std::vector<LogLine> Device::take_log() {
    if (auto t = tagger()) {
        for (auto& [kind, text] : t->log().take()) log(kind == "overflow" || kind == "device" ? Level::error : Level::warn, text);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<LogLine> out;
    out.swap(log_);
    return out;
}

Device::View Device::view() {
    std::lock_guard<std::mutex> lock(mutex_);
    return view_;
}

bool Device::idle() {
    std::lock_guard<std::mutex> lock(mutex_);
    return jobs_.empty() && !view_.busy;
}

bool Device::submit(const std::string& label, Job job, bool needs_connection) {
    if (needs_connection && !tagger()) {
        log(Level::error, label + ": not connected");
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        jobs_.emplace_back(label, std::move(job));
    }
    cv_.notify_all();
    return true;
}

void Device::worker() {
    auto last_poll = std::chrono::steady_clock::now();
    while (true) {
        std::pair<std::string, Job> job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait_for(lock, std::chrono::milliseconds(250), [&] { return quit_ || !jobs_.empty(); });
            if (quit_) return;
            if (!jobs_.empty()) {
                job = std::move(jobs_.front());
                jobs_.pop_front();
                view_.busy = true;
                view_.busy_label = job.first;
            }
        }
        if (job.second) {
            try {
                job.second();
            } catch (const std::exception& e) {
                log(Level::error, job.first + ": " + e.what());
            }
            post_state();
            std::lock_guard<std::mutex> lock(mutex_);
            view_.busy = false;
            view_.busy_label.clear();
            continue;
        }
        // Idle: keep the shown state fresh (temperature, run state), only while nothing streams.
        auto t = tagger();
        if (!t) continue;
        const std::string error = t->engine().error();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            view_.error = error;
            view_.streaming = t->streaming();
        }
        if (std::chrono::steady_clock::now() - last_poll > std::chrono::milliseconds(1500)) {
            last_poll = std::chrono::steady_clock::now();
            if (!t->engine().demand() && error.empty()) {
                try {
                    t->exclusive("status", [&] { backend_->note_response(backend_->session().status()); }, false);
                } catch (const std::exception&) {
                    // a measurement started meanwhile: next time
                }
                post_state();
            }
        }
    }
}

void Device::post_state() {
    dt::TdcBackend* b = backend();
    std::lock_guard<std::mutex> lock(mutex_);
    if (!b) return;
    const auto st = b->last_status();
    view_.state = st;
    ++view_.state_seq;
    view_.set_up = b->set_up();
    view_.release = dt::release_name(st.design_version);
    view_.capture = dt::describe_capture(st);
    try {
        const auto policy = b->policy_config().bytes();
        view_.policy_frame = st.gpx.written == policy;
        view_.policy_text = tdcctl::describe_gpx_config(policy);
    } catch (const std::exception& e) {
        view_.policy_frame = false;
        view_.policy_text = e.what();
    }
    bool differs = false;
    for (int c = 0; c < dt::kNumChannels; ++c) {
        const auto i = static_cast<std::size_t>(c);
        differs |= std::fabs(tdcctl::threshold_volts(st.board.dac_codes[i]) - channels_.trigger_v[i]) > 0.0005;
        differs |= std::fabs(tdcctl::wiper_ohms(st.board.wiper(c)) - channels_.hysteresis_ohm[i]) > 25.0;
    }
    view_.comparator_differs = differs;
}

// ---------------------------------------------------------------------------
// Connection
// ---------------------------------------------------------------------------
void Device::connect() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        view_.connecting = true;
    }
    submit("connecting", [this] { do_connect(); }, false);
}

void Device::disconnect() {
    submit("disconnecting", [this] { do_disconnect("disconnected"); }, false);
}

void Device::apply_software_settings(dt::Tagger& t) {
    ChannelSettings ch;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ch = channels_;
    }
    for (int c = 1; c <= dt::kNumChannels; ++c) {
        t.set_delay(c, ch.delay_ps[static_cast<std::size_t>(c - 1)]);
        t.set_deadtime(c, ch.deadtime_ps[static_cast<std::size_t>(c - 1)]);
    }
}

void Device::do_connect() {
    if (tagger()) do_disconnect("");
    try {
        if (options_.simulate) {
            auto& f = dt::TdcBackend::simulated_fpga();
            f.realtime = true;
            f.latency_jitter_ps = 300'000;
            f.event_source = SimSignals{&f};
        }
        dt::TdcOptions o;
        o.serial = options_.serial;
        o.simulate_fpga = options_.simulate;
        o.setup = dt::SetupMode::none;   // connecting changes nothing on the board (user, 2026-10-22)
        o.state_dir = options_.state_dir;
        auto backend = std::make_unique<dt::TdcBackend>(o);
        dt::TdcBackend* raw = backend.get();
        auto t = std::make_shared<dt::Tagger>(std::move(backend), dt::SettingsPolicy::restart);
        apply_software_settings(*t);
        raw->add_tap(rates_);
        {
            std::lock_guard<std::mutex> lock(tagger_mu_);
            tagger_ = t;
            backend_ = raw;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            view_.connected = true;
            view_.connecting = false;
            ++view_.generation;
            view_.serial = raw->serial();
            view_.link_text = (options_.simulate ? std::string("simulated FPGA") : "FT601 " + raw->serial()) +
                              " · USB " + raw->usb_speed();
            view_.error.clear();
        }
        post_state();
        const auto v = view();
        log(Level::ok, "connected to " + v.link_text + ", " + v.release + "; " + raw->setup_message());
        if (!raw->usb_note().empty())
            log(raw->usb_note().find("refused") == std::string::npos ? Level::info : Level::warn, "USB: " + raw->usb_note());
        if (!v.set_up)
            log(Level::warn, "the board is not set up: use \"Set up the board\" (configures the GPX2 and calibrates the LVDS link)");
        if (v.comparator_differs)
            log(Level::warn, "the board's trigger levels / hysteresis differ from your saved ones (e.g. after a power cycle); "
                             "nothing was changed: apply the saved values or keep the board's (Channels)");
    } catch (const std::exception& e) {
        std::lock_guard<std::mutex> lock(mutex_);
        view_.connecting = false;
        log_.push_back({clock_now(), std::string("connect failed: ") + e.what(), Level::error});
    }
}

void Device::do_disconnect(const std::string& why) {
    std::shared_ptr<dt::Tagger> t;
    dt::TdcBackend* b = nullptr;
    {
        std::lock_guard<std::mutex> lock(tagger_mu_);
        t = std::move(tagger_);
        b = backend_;
        tagger_.reset();
        backend_ = nullptr;
    }
    if (t) {
        if (b) b->remove_tap(rates_.get());
        try {
            t->close();
        } catch (const std::exception&) {
        }
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (view_.connected) ++view_.generation;
    view_.connected = false;
    view_.streaming = false;
    view_.state.reset();
    view_.link_text = "not connected";
    if (!why.empty()) log_.push_back({clock_now(), why, Level::info});
}

// ---------------------------------------------------------------------------
// Device commands
// ---------------------------------------------------------------------------
bool Device::command(const std::string& label, std::function<Response(Session&)> job, bool while_measuring) {
    return submit(label, [this, label, job, while_measuring] {
        auto t = tagger();
        if (!t) throw std::runtime_error("not connected");
        Response r;
        t->exclusive(label.c_str(), [&] {
            r = job(backend_->session());
            backend_->note_response(r);
        }, while_measuring);
        t->sync_from_backend();
        if (r.status == Status::ok) log(Level::ok, label);
        else log(Level::error, label + ": " + tdcctl::status_name(r.status));
    });
}

bool Device::setup(bool force_calibrate) {
    return submit(force_calibrate ? "calibrating the board" : "setting up the board", [this, force_calibrate] {
        auto t = tagger();
        std::string message;
        t->exclusive("board setup", [&] { message = backend_->run_setup(force_calibrate); }, false);
        log(Level::ok, "board set up: " + message);
    });
}

bool Device::calibrate(const tdcctl::GpxConfig& config, const tdcctl::CalibrationOptions& options) {
    return submit("calibrating", [this, config, options] {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            view_.calibrating = true;
            view_.cal_points.clear();
            view_.last_cal.reset();
        }
        auto t = tagger();
        tdcctl::CalibrationResult result;
        try {
            t->exclusive("calibration", [&] {
                auto& se = backend_->session();
                tdcctl::LclkinScope lclk(se, false);   // calibrate() switches LCLKIN on itself; off afterwards
                result = se.calibrate(config, options, [this](const tdcctl::CalibrationPoint& p) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    view_.cal_points.push_back(p);
                });
                lclk.off();
                backend_->note_response(se.status());
                if (result.success) backend_->record_calibration("calibrated in the Dantagger app");
            }, false);
        } catch (...) {
            std::lock_guard<std::mutex> lock(mutex_);
            view_.calibrating = false;
            throw;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            view_.calibrating = false;
            view_.last_cal = result;
        }
        log(result.success ? Level::ok : Level::error,
            std::string(result.success ? "calibration OK: " : "calibration FAILED: ") + result.message);
    });
}

bool Device::link_test(std::uint32_t window_cycles) {
    return submit("link test", [this, window_cycles] {
        auto t = tagger();
        t->exclusive("link test", [&] {
            auto& se = backend_->session();
            tdcctl::LclkinScope lclk(se, true);
            backend_->note_response(se.link_test(window_cycles));
            lclk.off();
            // keep the link-test counts of the response (the STATUS after would clear nothing, but keep it simple)
        }, false);
        log(Level::ok, "link test (" + std::to_string(window_cycles) + " cycles)");
    });
}

void Device::abort() { abort_ = true; }

bool Device::quick_count(double seconds) {
    abort_ = false;
    return submit("counting", [this, seconds] {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            view_.counting = true;
        }
        CountResult c;
        c.window_s = seconds;
        try {
            tagger()->exclusive("count", [&] {
                auto& se = backend_->session();
                const auto before = se.status();
                backend_->note_response(before);
                if (!before.lvds.format_valid || !before.gpx.verified())
                    throw std::runtime_error("the GPX2 is not configured and verified (set up the board first)");
                if (const auto why = tdcctl::capture_problem(before, backend_->refclk_hz()); !why.empty())
                    throw std::runtime_error(why);
                tdcctl::StreamRequest req;
                req.window_us = static_cast<std::uint32_t>(std::llround(seconds * 1e6));
                req.duration_s = seconds + 5.0;
                tdcctl::LclkinScope lclk(se, true);
                const auto res = se.stream(req, [](std::uint64_t) {}, {}, &abort_);
                lclk.off();
                backend_->note_response(se.status());
                c.pass = res.pass && res.by_window;
                c.events = res.events;
                for (std::size_t i = 0; i < c.events.size(); ++i) c.rates_hz[i] = static_cast<double>(c.events[i]) / seconds;
                c.dropped = res.final_response ? res.final_response->stream.dropped : 0;
                c.failure = c.pass ? "" : (res.failure.empty() ? "the window did not end the count (stopped?)" : res.failure);
            }, false);
        } catch (const std::exception& e) {
            c.pass = false;
            c.failure = e.what();
        }
        std::lock_guard<std::mutex> lock(mutex_);
        view_.counting = false;
        view_.last_count = c;
    });
}

bool Device::threshold_scan(const ScanRequest& request) {
    int channels = 0;
    for (bool c : request.channels) channels += c ? 1 : 0;
    if (channels == 0 || request.points < 1 || request.seconds <= 0) {
        log(Level::error, "trigger scan: choose at least one channel, one point and a measurement time");
        return false;
    }
    try {   // the whole range (and the hysteresis) before touching the board
        (void)tdcctl::threshold_code(request.from_v);
        (void)tdcctl::threshold_code(request.to_v);
        if (request.set_hysteresis) (void)tdcctl::wiper_code(request.hysteresis_ohm);
    } catch (const std::exception& bad) {
        log(Level::error, std::string("trigger scan: ") + bad.what());
        return false;
    }
    abort_ = false;
    return submit("trigger scan", [this, request, channels] {
        auto t = tagger();
        std::string outcome = "complete";
        {
            std::lock_guard<std::mutex> lock(mutex_);
            view_.scanning = true;
            view_.scan_request = request;
            view_.scan_points.clear();
            view_.scan_total = channels * request.points;
            view_.scan_message = "scanning";
        }
        try {
            t->exclusive("trigger scan", [&] {
                auto& se = backend_->session();
                const auto before = se.status();
                backend_->note_response(before);
                if (!before.lvds.format_valid || !before.gpx.verified())
                    throw std::runtime_error("the GPX2 is not configured and verified (set up the board first)");
                if (const auto why = tdcctl::capture_problem(before, backend_->refclk_hz()); !why.empty())
                    throw std::runtime_error(why);
                const auto original = before.board.dac_codes;
                unsigned scanned = 0;
                for (int ch = 0; ch < dt::kNumChannels; ++ch)
                    if (request.channels[static_cast<std::size_t>(ch)]) scanned |= 1U << static_cast<unsigned>(ch);
                // Put the scanned triggers back (also when the scan fails part-way).  A power-up
                // code 0 (-0.1004 V) is below the host's window: it becomes the lowest allowed code.
                auto restore = [&] {
                    auto codes = original;
                    for (int ch = 0; ch < dt::kNumChannels; ++ch) {
                        auto& code = codes[static_cast<std::size_t>(ch)];
                        if (((scanned >> ch) & 1U) && (code < tdcctl::kDacCodeMin || code > tdcctl::kDacCodeMax))
                            code = static_cast<std::uint16_t>(std::clamp<int>(code, tdcctl::kDacCodeMin, tdcctl::kDacCodeMax));
                    }
                    backend_->note_response(se.thresholds(scanned, codes));
                };
                if (request.set_hysteresis) {
                    const auto w = tdcctl::wiper_code(request.hysteresis_ohm);
                    const auto h = se.hysteresis(scanned, {w, w, w, w});
                    backend_->note_response(h);
                    if (h.status != Status::ok) throw std::runtime_error("setting the hysteresis failed (" + tdcctl::status_name(h.status) + ")");
                }
                {
                    const auto now = se.status();
                    std::lock_guard<std::mutex> lock(mutex_);
                    for (int ch = 0; ch < dt::kNumChannels; ++ch)
                        view_.scan_hysteresis_ohm[static_cast<std::size_t>(ch)] = tdcctl::wiper_ohms(now.board.wiper(ch));
                }
                tdcctl::StreamRequest req;
                req.window_us = static_cast<std::uint32_t>(std::llround(request.seconds * 1e6));
                req.duration_s = request.seconds + 5.0;   // host STOP only if the window never ends a point
                tdcctl::LclkinScope lclk(se, true);
                try {
                    for (int ch = 0; ch < dt::kNumChannels && outcome == "complete"; ++ch) {
                        if (!request.channels[static_cast<std::size_t>(ch)]) continue;
                        const unsigned mask = 1U << static_cast<unsigned>(ch);
                        for (int i = 0; i < request.points; ++i) {
                            if (abort_) {
                                outcome = "stopped";
                                break;
                            }
                            const double v = request.points == 1
                                                 ? request.from_v
                                                 : request.from_v + (request.to_v - request.from_v) * i / (request.points - 1);
                            const auto code = tdcctl::threshold_code(v);
                            const auto set = se.thresholds(mask, {code, code, code, code});
                            backend_->note_response(set);
                            if (set.status != Status::ok) {
                                outcome = "failed: SET_THRESHOLDS " + tdcctl::status_name(set.status);
                                break;
                            }
                            const auto res = se.stream(req, [](std::uint64_t) {}, {}, &abort_);
                            if (!res.pass || !res.by_window) {
                                outcome = abort_ ? "stopped"
                                                 : "failed: " + (res.failure.empty() ? std::string("the window did not end the count")
                                                                                     : res.failure);
                                break;
                            }
                            ScanPoint p;
                            p.channel = ch;
                            p.volts = tdcctl::threshold_volts(set.board.dac_codes[static_cast<std::size_t>(ch)]);
                            p.events = res.events[static_cast<std::size_t>(ch)];
                            p.rate_hz = static_cast<double>(p.events) / request.seconds;
                            p.dropped = res.final_response ? res.final_response->stream.dropped : 0;
                            std::lock_guard<std::mutex> lock(mutex_);
                            view_.scan_points.push_back(p);
                        }
                    }
                } catch (...) {
                    try {
                        lclk.off();
                        restore();
                    } catch (const std::exception&) {
                    }
                    throw;
                }
                lclk.off();
                restore();
                backend_->note_response(se.status());
            }, false);
        } catch (...) {
            std::lock_guard<std::mutex> lock(mutex_);
            view_.scanning = false;
            view_.scan_message = "failed";
            throw;
        }
        t->sync_from_backend();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            view_.scanning = false;
            view_.scan_message = outcome;
        }
        log(outcome == "complete" ? Level::ok : (outcome == "stopped" ? Level::warn : Level::error),
            "trigger scan " + outcome + "; triggers restored");
    });
}

// ---------------------------------------------------------------------------
// Shared channel settings
// ---------------------------------------------------------------------------
void Device::load_channels() {
    const auto kv = read_kv(channels_path_);
    for (int c = 1; c <= dt::kNumChannels; ++c) {
        const auto i = static_cast<std::size_t>(c - 1);
        const std::string n = std::to_string(c);
        channels_.trigger_v[i] = kv_num(kv, "trigger_v." + n, channels_.trigger_v[i]);
        channels_.hysteresis_ohm[i] = kv_num(kv, "hysteresis_ohm." + n, channels_.hysteresis_ohm[i]);
        channels_.delay_ps[i] = static_cast<std::int64_t>(kv_num(kv, "delay_ps." + n, 0));
        channels_.deadtime_ps[i] = static_cast<std::int64_t>(kv_num(kv, "deadtime_ps." + n, 0));
    }
}

void Device::save_channels() {
    KeyValues kv;
    char buf[64];
    for (int c = 1; c <= dt::kNumChannels; ++c) {
        const auto i = static_cast<std::size_t>(c - 1);
        const std::string n = std::to_string(c);
        std::snprintf(buf, sizeof buf, "%.5f", channels_.trigger_v[i]);
        kv.emplace_back("trigger_v." + n, buf);
        std::snprintf(buf, sizeof buf, "%.1f", channels_.hysteresis_ohm[i]);
        kv.emplace_back("hysteresis_ohm." + n, buf);
        kv.emplace_back("delay_ps." + n, std::to_string(channels_.delay_ps[i]));
        kv.emplace_back("deadtime_ps." + n, std::to_string(channels_.deadtime_ps[i]));
    }
    if (!write_kv(channels_path_, kv)) log(Level::error, "cannot save the channel settings to " + channels_path_);
}

void Device::set_trigger(int channel, double volts) {
    if (!std::isfinite(volts) || volts < dt::kTriggerMinVolts - 1e-9 || volts > dt::kTriggerMaxVolts + 1e-9) {
        char buf[160];
        std::snprintf(buf, sizeof buf, "trigger %.4f V on channel %d is outside the safe range %.3f ... %+.3f V: not applied",
                      volts, channel, dt::kTriggerMinVolts, dt::kTriggerMaxVolts);
        log(Level::error, buf);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        channels_.trigger_v[static_cast<std::size_t>(channel - 1)] = volts;
    }
    save_channels();
    if (!tagger()) return;
    char label[64];
    std::snprintf(label, sizeof label, "trigger %d = %.4f V", channel, volts);
    submit(label, [this, channel, volts, l = std::string(label)] {
        tagger()->set_trigger_level(channel, volts);
        log(Level::ok, l);
    });
}

void Device::set_hysteresis(int channel, double ohms) {
    ohms = std::clamp(ohms, dt::kHysteresisMinOhms, dt::kHysteresisMaxOhms);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        channels_.hysteresis_ohm[static_cast<std::size_t>(channel - 1)] = ohms;
    }
    save_channels();
    if (!tagger()) return;
    char label[64];
    std::snprintf(label, sizeof label, "hysteresis %d = %.0f ohm", channel, ohms);
    submit(label, [this, channel, ohms, l = std::string(label)] {
        const double applied = tagger()->set_hysteresis(channel, ohms);
        char buf[96];
        std::snprintf(buf, sizeof buf, "%s (applied %.0f ohm)", l.c_str(), applied);
        log(Level::ok, buf);
    });
}

void Device::set_delay(int channel, std::int64_t ps) {
    ps = std::clamp<std::int64_t>(ps, -1'000'000'000'000, 1'000'000'000'000);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        channels_.delay_ps[static_cast<std::size_t>(channel - 1)] = ps;
    }
    save_channels();
    if (!tagger()) return;
    submit("input delay " + std::to_string(channel), [this, channel, ps] {
        tagger()->set_delay(channel, ps);
        log(Level::ok, "input delay " + std::to_string(channel) + " = " + std::to_string(ps) + " ps");
    });
}

void Device::set_deadtime(int channel, std::int64_t ps) {
    ps = std::clamp<std::int64_t>(ps, 0, 1'000'000'000'000);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        channels_.deadtime_ps[static_cast<std::size_t>(channel - 1)] = ps;
    }
    save_channels();
    if (!tagger()) return;
    submit("dead time " + std::to_string(channel), [this, channel, ps] {
        const auto applied = tagger()->set_deadtime(channel, ps);
        log(Level::ok, "dead time " + std::to_string(channel) + " = " + std::to_string(ps) + " ps (effective " +
                           std::to_string(applied) + " ps)");
    });
}

void Device::apply_saved_comparator() {
    ChannelSettings ch;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ch = channels_;
    }
    submit("applying the saved comparator settings", [this, ch] {
        auto t = tagger();
        for (int c = 1; c <= dt::kNumChannels; ++c) {
            t->set_trigger_level(c, ch.trigger_v[static_cast<std::size_t>(c - 1)]);
            t->set_hysteresis(c, ch.hysteresis_ohm[static_cast<std::size_t>(c - 1)]);
        }
        log(Level::ok, "saved trigger levels and hysteresis applied to the board");
    });
}

void Device::adopt_board_comparator() {
    const auto v = view();
    if (!v.state) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (int c = 0; c < dt::kNumChannels; ++c) {
            const auto i = static_cast<std::size_t>(c);
            channels_.trigger_v[i] = tdcctl::threshold_volts(v.state->board.dac_codes[i]);
            channels_.hysteresis_ohm[i] = tdcctl::wiper_ohms(v.state->board.wiper(c));
        }
        view_.comparator_differs = false;
    }
    save_channels();
    log(Level::info, "kept the board's trigger levels and hysteresis (now the saved values)");
}

}  // namespace app
