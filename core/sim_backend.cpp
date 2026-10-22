#include "sim_backend.hpp"

#include "engine.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <thread>

namespace dt {

namespace {

using Clock = std::chrono::steady_clock;

// Paces a run to the wall clock: tag time advances `speed` times as fast as
// real time (speed <= 0: no pacing).
class Pacer {
public:
    Pacer(double speed, timestamp_t base) : speed_(speed), base_(base), wall0_(Clock::now()) {}
    void wait_until(timestamp_t t) const {
        if (speed_ <= 0) return;
        const double seconds = static_cast<double>(t - base_) * 1e-12 / speed_;
        std::this_thread::sleep_until(wall0_ + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(seconds)));
    }
    bool behind(timestamp_t t) const {
        if (speed_ <= 0) return true;
        const double seconds = static_cast<double>(t - base_) * 1e-12 / speed_;
        return Clock::now() >= wall0_ + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(seconds));
    }

private:
    double speed_;
    timestamp_t base_;
    Clock::time_point wall0_;
};

}  // namespace

// ---------------------------------------------------------------------------
// SimBackend
// ---------------------------------------------------------------------------
SimBackend::SimBackend(const std::array<SimSignal, kNumChannels>& signals, double speed, std::uint64_t seed)
    : signals_(signals), speed_(speed), rng_(seed), opened_(Clock::now()) {
    double max_jitter = 0;
    for (int c = 0; c < kNumChannels; ++c) {
        const auto& s = signals_[static_cast<std::size_t>(c)];
        if (s.kind == SimSignal::Kind::detector) {
            if (s.ref < 1 || s.ref > kNumChannels || signals_[static_cast<std::size_t>(s.ref - 1)].kind != SimSignal::Kind::periodic)
                throw std::invalid_argument("simulated detector on channel " + std::to_string(c + 1) +
                                            ": `ref` must name a periodic channel");
            if (s.efficiency < 0 || s.efficiency > 1) throw std::invalid_argument("detector efficiency must be 0..1");
        }
        if ((s.kind == SimSignal::Kind::periodic || s.kind == SimSignal::Kind::poisson) && !(s.rate_hz > 0))
            throw std::invalid_argument("simulated channel " + std::to_string(c + 1) + ": rate must be > 0");
        max_jitter = std::max(max_jitter, s.jitter_ps);
    }
    margin_ = static_cast<timestamp_t>(std::ceil(8.0 * max_jitter)) + 1;
}

double SimBackend::set_trigger(int channel, double volts) {
    trigger_[static_cast<std::size_t>(channel - 1)] = volts;
    return volts;
}

std::string SimBackend::info() {
    std::ostringstream o;
    o << "simulated TDC (speed " << speed_ << ")\n";
    const char* kinds[] = {"none", "poisson", "periodic", "detector"};
    for (int c = 0; c < kNumChannels; ++c) {
        const auto& s = signals_[static_cast<std::size_t>(c)];
        o << "  channel " << c + 1 << ": " << kinds[static_cast<int>(s.kind)];
        if (s.kind == SimSignal::Kind::poisson || s.kind == SimSignal::Kind::periodic) o << " " << s.rate_hz << " Hz";
        if (s.kind == SimSignal::Kind::detector)
            o << " on channel " << s.ref << ", efficiency " << s.efficiency << ", delay " << s.delay_ps << " ps";
        o << "\n";
    }
    return o.str();
}

void SimBackend::generate(timestamp_t t0, timestamp_t t1, RawChunk& chunk) {
    std::normal_distribution<double> gauss(0.0, 1.0);
    auto jitter = [&](double sigma) -> timestamp_t {
        if (sigma <= 0) return 0;
        const double g = std::max(-8.0, std::min(8.0, gauss(rng_)));
        return static_cast<timestamp_t>(std::llround(g * sigma));
    };
    auto poisson_times = [&](double rate, std::vector<timestamp_t>& out) {
        if (rate <= 0) return;
        std::exponential_distribution<double> gap(rate * 1e-12);
        double t = static_cast<double>(t0) + gap(rng_);
        while (t < static_cast<double>(t1)) {
            out.push_back(static_cast<timestamp_t>(t));
            t += gap(rng_);
        }
    };
    for (int c = 0; c < kNumChannels; ++c) {
        const auto& s = signals_[static_cast<std::size_t>(c)];
        auto& out = pending_[static_cast<std::size_t>(c)];
        const bool armed = trigger_[static_cast<std::size_t>(c)] < s.amplitude_v;
        if (!armed || s.kind == SimSignal::Kind::none) continue;
        if (s.kind == SimSignal::Kind::poisson) {
            poisson_times(s.rate_hz, out);
        } else if (s.kind == SimSignal::Kind::periodic) {
            const double period = 1e12 / s.rate_hz;
            auto k = static_cast<std::int64_t>(std::ceil((static_cast<double>(t0) - s.phase_ps) / period));
            for (;; ++k) {
                const double nominal = s.phase_ps + static_cast<double>(k) * period;
                if (nominal >= static_cast<double>(t1)) break;
                if (nominal < static_cast<double>(t0)) continue;
                out.push_back(static_cast<timestamp_t>(std::llround(nominal)) + jitter(s.jitter_ps));
            }
        } else {   // detector
            const auto& r = signals_[static_cast<std::size_t>(s.ref - 1)];
            const double period = 1e12 / r.rate_hz;
            const double first = static_cast<double>(t0) - s.delay_ps;
            auto k = static_cast<std::int64_t>(std::ceil((first - r.phase_ps) / period));
            if (s.efficiency > 0) {
                // Geometric skipping over the reference pulses.
                std::geometric_distribution<std::int64_t> skip(s.efficiency);
                for (k += s.efficiency < 1 ? skip(rng_) : 0;; k += 1 + (s.efficiency < 1 ? skip(rng_) : 0)) {
                    const double nominal = r.phase_ps + static_cast<double>(k) * period + s.delay_ps;
                    if (nominal >= static_cast<double>(t1)) break;
                    out.push_back(static_cast<timestamp_t>(std::llround(nominal)) + jitter(s.jitter_ps));
                }
            }
            poisson_times(s.dark_hz, out);
        }
        std::sort(out.begin(), out.end());
    }
    // Release everything that can no longer be preceded by a later event.
    const timestamp_t release = t1 - margin_;
    for (int c = 0; c < kNumChannels; ++c) {
        auto& p = pending_[static_cast<std::size_t>(c)];
        const auto cut = std::lower_bound(p.begin(), p.end(), release);
        chunk.events[c].assign(p.begin(), cut);
        p.erase(p.begin(), cut);
    }
    chunk.watermark = release;
}

void SimBackend::run(Engine& engine, const std::function<bool()>& should_stop) {
    // Tag time follows the wall clock since the backend was created (so idle
    // time between runs is a real gap), and never goes back.
    const auto since_open =
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - opened_).count() * 1000LL;
    const timestamp_t base = std::max<timestamp_t>(last_end_ + margin_ + 1'000'000, speed_ > 0 ? static_cast<timestamp_t>(since_open * speed_) : 0);
    for (auto& p : pending_) p.clear();
    {
        RawChunk c;
        c.run_begin = true;
        c.run_base = base;
        c.watermark = base;
        engine.push(std::move(c));
    }
    double total_rate = 0;
    for (int c = 0; c < kNumChannels; ++c) {
        const auto& s = signals_[static_cast<std::size_t>(c)];
        if (s.kind == SimSignal::Kind::poisson || s.kind == SimSignal::Kind::periodic) total_rate += s.rate_hz;
        if (s.kind == SimSignal::Kind::detector)
            total_rate += signals_[static_cast<std::size_t>(s.ref - 1)].rate_hz * s.efficiency + s.dark_hz;
    }
    // About 65k tags per chunk, between 20 us and 2 ms of tag time.
    const double step_s = std::min(2e-3, std::max(20e-6, total_rate > 0 ? 65536.0 / total_rate : 2e-3));
    const auto step = static_cast<timestamp_t>(step_s * 1e12);
    Pacer pacer(speed_, base);
    timestamp_t t = base;
    while (!should_stop()) {
        RawChunk c;
        generate(t, t + step, c);
        t += step;
        engine.push(std::move(c));
        pacer.wait_until(t);
    }
    RawChunk end;
    end.run_end = true;
    // In-flight jittered events are released with the final chunk.
    for (int c = 0; c < kNumChannels; ++c) {
        auto& p = pending_[static_cast<std::size_t>(c)];
        const auto cut = std::lower_bound(p.begin(), p.end(), t);
        end.events[c].assign(p.begin(), cut);
        p.clear();
    }
    end.watermark = t;
    engine.push(std::move(end));
    last_end_ = t;
}

// ---------------------------------------------------------------------------
// ReplayBackend
// ---------------------------------------------------------------------------
ReplayBackend::ReplayBackend(std::vector<timestamp_t> times, std::vector<std::int8_t> channels, double speed)
    : times_(std::move(times)), channels_(std::move(channels)), speed_(speed) {
    if (times_.size() != channels_.size()) throw std::invalid_argument("replay: times and channels differ in length");
    for (std::size_t i = 0; i < times_.size(); ++i) {
        if (channels_[i] < 1 || channels_[i] > kNumChannels) throw std::invalid_argument("replay: channels must be 1..4");
        if (i > 0 && times_[i] < times_[i - 1]) throw std::invalid_argument("replay: times must be sorted");
    }
    if (!times_.empty() && times_.front() < 0) throw std::invalid_argument("replay: times must be >= 0");
}

void ReplayBackend::run(Engine& engine, const std::function<bool()>& should_stop) {
    const timestamp_t base = first_run_ ? 0 : position_;
    first_run_ = false;
    {
        RawChunk c;
        c.run_begin = true;
        c.run_base = base;
        c.watermark = base;
        engine.push(std::move(c));
    }
    Pacer pacer(speed_, base);
    timestamp_t t = base;
    constexpr std::size_t kChunk = 65536;
    constexpr timestamp_t kEmptyStep = 10'000'000'000;   // 10 ms of empty timeline per chunk after the data
    while (!should_stop()) {
        RawChunk c;
        if (cursor_ < times_.size()) {
            const std::size_t end = std::min(times_.size(), cursor_ + kChunk);
            for (std::size_t i = cursor_; i < end; ++i) c.events[channels_[i] - 1].push_back(times_[i]);
            cursor_ = end;
            // Tags equal to the last one may still follow.
            t = cursor_ < times_.size() ? times_[cursor_] : times_[cursor_ - 1] + 1;
        } else {
            t += kEmptyStep;
        }
        c.watermark = t;
        engine.push(std::move(c));
        pacer.wait_until(t);
    }
    RawChunk end;
    end.run_end = true;
    end.watermark = t;
    engine.push(std::move(end));
    position_ = t + 1;
}

}  // namespace dt
