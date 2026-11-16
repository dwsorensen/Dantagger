// C++ tests of the Dantagger core: FastDiv, the merger, and the whole hardware
// path (simulated FPGA -> FT601 protocol -> record decoder -> merger ->
// engine) against known event times.
//   .venv/bin/python Dantagger/build.py --tests      (or --asan)

#include "engine.hpp"
#include "file_backend.hpp"
#include "measurements.hpp"
#include "recorder.hpp"
#include "merger.hpp"
#include "record_decoder.hpp"
#include "tagger.hpp"
#include "tdc_backend.hpp"
#include "virtual_channels.hpp"

#include "tdcctl/fake_fpga.hpp"
#include "tdcctl/tdc_session.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <filesystem>
#include <functional>
#include <thread>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <vector>

#include <unistd.h>

using namespace dt;

static int g_checks = 0, g_failures = 0;
#define CHECK(cond)                                                                   \
    do {                                                                              \
        ++g_checks;                                                                   \
        if (!(cond)) {                                                                \
            ++g_failures;                                                             \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
        }                                                                             \
    } while (0)

// ---------------------------------------------------------------------------
static void test_fastdiv() {
    std::mt19937_64 rng(1);
    for (std::int64_t d : {1LL, 2LL, 3LL, 7LL, 100LL, 997LL, 4504LL, 1000000LL, 123456789LL, (1LL << 40) + 3}) {
        FastDiv f(d);
        for (int i = 0; i < 200000; ++i) {
            const std::int64_t x = static_cast<std::int64_t>(rng() % (std::uint64_t{1} << 52));
            if (f(x) != x / d) {
                CHECK(f(x) == x / d);
                break;
            }
            const std::int64_t y = (x / d) * d;   // exact multiples and their neighbours
            CHECK(f(y) == y / d);
            if (y > 0) CHECK(f(y - 1) == (y - 1) / d);
        }
    }
}

// ---------------------------------------------------------------------------
static void test_merger() {
    std::mt19937_64 rng(2);
    for (int trial = 0; trial < 40; ++trial) {
        std::array<timestamp_t, kNumChannels> delays{}, dead{};
        for (auto& d : delays) d = static_cast<timestamp_t>(rng() % 40000) - 20000;
        if (trial % 3 == 0) dead[1] = 5000;
        Merger m;
        m.configure(delays, dead);
        // Per-channel raw streams with ties.
        std::vector<std::vector<timestamp_t>> raw(kNumChannels);
        for (int c = 0; c < kNumChannels; ++c) {
            timestamp_t t = 100000;
            for (int i = 0; i < 3000; ++i) {
                t += static_cast<timestamp_t>(rng() % 3000);
                raw[static_cast<std::size_t>(c)].push_back(rng() % 4 == 0 ? (t / 1000) * 1000 : t);   // ties
            }
            std::sort(raw[static_cast<std::size_t>(c)].begin(), raw[static_cast<std::size_t>(c)].end());
        }
        // Expected: dead time, delay, drop before start, order by (time, channel).
        const timestamp_t base = 50000;
        std::vector<std::pair<timestamp_t, int>> want;
        for (int c = 0; c < kNumChannels; ++c) {
            timestamp_t last = kUnset;
            for (timestamp_t t : raw[static_cast<std::size_t>(c)]) {
                if (dead[static_cast<std::size_t>(c)] > 0 && last != kUnset && t - last < dead[static_cast<std::size_t>(c)]) continue;
                last = t;
                const timestamp_t td = t + delays[static_cast<std::size_t>(c)];
                if (td >= base) want.emplace_back(td, c + 1);
            }
        }
        std::stable_sort(want.begin(), want.end());
        // Feed in random chunks with a correct watermark.
        std::vector<std::pair<timestamp_t, int>> got;
        TagBlock blk;
        RawChunk first;
        first.run_begin = true;
        first.run_base = base;
        first.watermark = base;
        m.push(first, blk);
        std::array<std::size_t, kNumChannels> pos{};
        timestamp_t w = base;
        timestamp_t prev_end = blk.end;
        CHECK(blk.begin == base);
        for (int step = 0;; ++step) {
            w += static_cast<timestamp_t>(rng() % 200000);
            RawChunk c;
            bool any = false;
            for (int ch = 0; ch < kNumChannels; ++ch) {
                auto& r = raw[static_cast<std::size_t>(ch)];
                while (pos[static_cast<std::size_t>(ch)] < r.size() && r[pos[static_cast<std::size_t>(ch)]] < w)
                    c.events[ch].push_back(r[pos[static_cast<std::size_t>(ch)]++]);
                any |= pos[static_cast<std::size_t>(ch)] < r.size();
            }
            c.watermark = w;
            c.run_end = !any;
            m.push(c, blk);
            CHECK(blk.begin == prev_end);
            prev_end = blk.end;
            for (std::size_t i = 0; i < blk.size(); ++i) {
                CHECK(blk.time[i] >= blk.begin && blk.time[i] < blk.end);
                got.emplace_back(blk.time[i], blk.channel[i]);
            }
            if (!any) break;
        }
        CHECK(got == want);
        // The next run continues the timeline (never back).
        RawChunk next;
        next.run_begin = true;
        next.run_base = 0;
        next.watermark = 0;
        m.push(next, blk);
        CHECK(blk.begin >= prev_end);
    }
}

// ---------------------------------------------------------------------------
// Hardware path through the simulated FPGA.
struct Truth {
    std::vector<std::pair<std::uint64_t, int>> events;   // true GPX2 ps, channel 0..3
};

static std::vector<timestamp_t> run_stream(Tagger& tagger, timestamp_t duration, std::vector<std::int32_t>* channels,
                                           std::size_t* specials) {
    auto stream = std::make_shared<TimeTagStream>(50'000'000, std::vector<int>{1, 2, 3, 4});
    tagger.engine().add(stream, false);
    tagger.engine().start({stream.get()}, duration, true);
    tagger.engine().wait({stream.get()}, -1);
    StreamBuffer b;
    {
        auto g = stream->guard();
        b = stream->take();
    }
    tagger.engine().remove(stream.get());
    std::vector<timestamp_t> out;
    std::size_t sp = 0;
    for (std::size_t i = 0; i < b.timestamps.size(); ++i) {
        if (b.types[i] != 0) {
            ++sp;
            continue;
        }
        out.push_back(b.timestamps[i]);
        if (channels) channels->push_back(b.channels[i]);
    }
    if (specials) *specials = sp;
    return out;
}

// ref_bits 0: the frame policy of the release (releases 2 and 3: 24 + 18
// bits DDR; release 1: 24 + 20 bits SDR).  ref_bits 6 on release 2/3: the
// shortest frame at 250 MHz DDR (6 + 18, an 8 us reference-index wrap).
// Release 3 (the simulated FPGA's default) runs with compact records where
// the frame allows them (12-24 index bits, <= 18 stop bits) unless `compact`
// is 0.
static void hw_case(const char* name, int ref_bits, double ppm, std::uint64_t jitter_ps, double rate_hz,
                    timestamp_t silence_ps, std::uint64_t drop_every, int release = 3, int compact = -1) {
    const bool release1 = release == 1;
    const auto dir = std::filesystem::temp_directory_path() / ("dantagger_test_state_" + std::to_string(getpid()));
    TdcBackend::simulate_unplug();
    auto& fake = TdcBackend::simulated_fpga();
    if (release == 1) fake.release1();
    if (release == 2) fake.release2();
    fake.gpx_ppm = ppm;
    fake.latency_ps = 600000;
    fake.latency_jitter_ps = jitter_ps;
    fake.drop_every = drop_every;
    Truth truth;
    std::mutex truth_mu;   // the source runs on the receiving thread
    std::mt19937_64 rng(7);
    std::uint64_t silent_from = 0;
    bool silenced = false;
    fake.event_source = [&](std::uint64_t t0, std::uint64_t t1, std::vector<std::pair<std::uint64_t, int>>& out) {
        std::lock_guard<std::mutex> lock(truth_mu);
        if (silent_from == 0) silent_from = t0 + 3'000'000'000;   // 3 ms of events, then (optionally) silence
        for (int c = 0; c < kNumChannels; ++c) {
            std::exponential_distribution<double> gap(rate_hz * 1e-12);
            double t = static_cast<double>(t0) + gap(rng);
            while (t < static_cast<double>(t1)) {
                const auto ti = static_cast<std::uint64_t>(t);
                const bool in_silence = silence_ps > 0 && ti >= silent_from && ti < silent_from + static_cast<std::uint64_t>(silence_ps);
                if (!in_silence) {
                    out.emplace_back(ti, c);
                    truth.events.emplace_back(ti, c);
                }
                silenced |= in_silence;
                t += gap(rng);
            }
        }
    };
    TdcOptions o;
    o.simulate_fpga = true;
    o.ref_index_bits = ref_bits;
    o.stop_bits = ref_bits == 0 ? 0 : ref_bits == 24 ? 20 : 18;
    o.state_dir = dir.string();
    o.compact = compact;
    const int want_ref = ref_bits != 0 ? ref_bits : 24;
    const int want_stop = ref_bits == 0 ? (release1 ? 20 : 18) : o.stop_bits;
    const bool want_compact = release == 3 && compact != 0 && want_ref >= 12 && want_stop <= 18;
    Tagger tagger(std::make_unique<TdcBackend>(o));
    {
        auto& b = static_cast<TdcBackend&>(tagger.backend());
        const auto st = b.last_status();
        CHECK(st.lvds.ref_bits == want_ref);
        CHECK(st.lvds.stop_bits == want_stop);
        CHECK(st.lvds.ddr == !release1);
        CHECK(st.design_version == (release == 1   ? tdcctl::kDesignVersionR1
                                    : release == 2 ? tdcctl::kDesignVersionR2
                                                   : tdcctl::kDesignVersionR3));
    }
    const timestamp_t duration = 6'000'000'000 + silence_ps;
    std::vector<std::int32_t> chans;
    std::size_t specials = 0;
    const auto got = run_stream(tagger, duration, &chans, &specials);
    {   // Truth: per channel the events seen (some may be dropped / in flight at the run end).
        std::lock_guard<std::mutex> lock(truth_mu);
        Truth copy = truth;
        truth = std::move(copy);
    }
    auto& backend = static_cast<TdcBackend&>(tagger.backend());
    tagger.close();   // the source stops with the run
    // The run used the records it should have (its final response, field 112 on release 3).
    CHECK(backend.last_status().stream.record_format == (want_compact ? 2 : 1));
    std::sort(truth.events.begin(), truth.events.end());
    std::printf("  %-44s %zu events (truth %zu), %zu markers%s\n", name, got.size(), truth.events.size(), specials,
                want_compact ? ", compact" : "");
    CHECK(!got.empty());
    CHECK(std::is_sorted(got.begin(), got.end()));
    if (drop_every == 0) {
        // Every truth event before the window end appears with the exact spacing.
        // Map: decoded = truth + K (one constant per run, except after a re-anchor).
        std::vector<timestamp_t> tr;
        std::vector<int> trc;
        for (const auto& [t, c] : truth.events) {
            tr.push_back(static_cast<timestamp_t>(t));
            trc.push_back(c + 1);
        }
        // decoded = truth + K, one K per stretch between re-anchors (a re-anchor
        // places an event by the coarse clock after a long silence).
        std::size_t n = std::min(got.size(), tr.size()), changes = 0, channel_ok = 0;
        for (std::size_t i = 0; i < n; ++i) {
            if (chans[i] == trc[i]) ++channel_ok;
            if (i > 0 && got[i] - tr[i] != got[i - 1] - tr[i - 1]) ++changes;
        }
        std::printf("    offset changes %zu, channels matching %zu of %zu\n", changes, channel_ok, n);
        for (std::size_t i = 1; (channel_ok != n || changes > 1) && i < n; ++i)   // diagnostic: where it departs from the truth
            if (chans[i] != trc[i] || got[i] - tr[i] != got[0] - tr[0]) {
                for (std::size_t j = i > 3 ? i - 3 : 0; j < std::min(n, i + 4); ++j)
                    std::printf("      #%zu got %lld ch%d  truth %lld ch%d  (offset %lld)\n", j, static_cast<long long>(got[j]),
                                chans[j], static_cast<long long>(tr[j]), trc[j], static_cast<long long>(got[j] - tr[j]));
                break;
            }
        CHECK(channel_ok == n);
        CHECK(n == got.size());
        const bool short_wrap = ref_bits == 6;   // 8 us: a long silence may re-anchor once
        if (silence_ps == 0 || (!short_wrap && (ref_bits == 24 || ref_bits == 0 || silence_ps < 10'000'000'000'000)))
            CHECK(changes == 0);
        else
            CHECK(changes <= 1);
    } else {
        CHECK(specials > 0);
        CHECK(tagger.log().dropped() > 0);
    }
    std::filesystem::remove_all(dir);
}

static void test_hardware_path() {
    // 24 + 20 bits: 64-bit records (compact needs <= 18 stop bits); 16 + 18: compact.
    hw_case("24-bit index, no drift", 24, 0.0, 0, 50000, 0, 0);
    hw_case("16-bit index, +80 ppm, 3 us jitter", 16, 80.0, 3'000'000, 50000, 0, 0);
    hw_case("16-bit index, -95 ppm, 1 MHz/ch", 16, -95.0, 2'000'000, 1e6, 0, 0);
    hw_case("24-bit index, 2 s silence, +60 ppm", 24, 60.0, 1'000'000, 20000, 2'000'000'000'000, 0);
    hw_case("16-bit index, 2 s silence, -60 ppm", 16, -60.0, 1'000'000, 20000, 2'000'000'000'000, 0);
    hw_case("16-bit index, 30 s silence, +100 ppm", 16, 100.0, 1'000'000, 20000, 30'000'000'000'000, 0);
    hw_case("FPGA drops every 9th event", 16, 0.0, 0, 200000, 0, 9);
    // Release 3 frame policy: 24 + 18 bits, 250 MHz DDR, 8 ns coarse ticks, compact records.
    hw_case("R3 policy (24+18), no drift", 0, 0.0, 0, 50000, 0, 0);
    hw_case("R3 policy, -95 ppm, 1 MHz/ch, 1 us jitter", 0, -95.0, 1'000'000, 1e6, 0, 0);
    hw_case("R3 policy, 30 s silence, +100 ppm", 0, 100.0, 1'000'000, 20000, 30'000'000'000'000, 0);
    hw_case("R3 policy, FPGA drops every 7th", 0, 0.0, 0, 200000, 0, 7);
    hw_case("R3 policy, 4 MHz/ch, +50 ppm, 1 us jitter", 0, 50.0, 1'000'000, 4e6, 0, 0);
    hw_case("R3 policy, compact off, -95 ppm, 1 MHz/ch", 0, -95.0, 1'000'000, 1e6, 0, 0, 3, 0);
    // Release 2 (64-bit records only), same policy.
    hw_case("R2 policy (24+18), no drift", 0, 0.0, 0, 50000, 0, 0, 2);
    hw_case("R2 policy, 30 s silence, +100 ppm", 0, 100.0, 1'000'000, 20000, 30'000'000'000'000, 0, 2);
    hw_case("R2 policy, FPGA drops every 7th", 0, 0.0, 0, 200000, 0, 7, 2);
    // Release 2 shortest frame (6 + 18, an 8 us wrap; still allowed as an override).
    hw_case("6+18 (64-bit records), no drift", 6, 0.0, 0, 50000, 0, 0);
    hw_case("6+18 (64-bit records), +95 ppm, 1.2 us jitter", 6, 95.0, 1'200'000, 50000, 0, 0);
    hw_case("6+18 (64-bit records), -95 ppm, 1 MHz/ch, 1 us jitter", 6, -95.0, 1'000'000, 1e6, 0, 0);
    hw_case("6+18 (64-bit records), 2 kHz/ch, +60 ppm", 6, 60.0, 500'000, 2000, 0, 0);
    hw_case("6+18 (64-bit records), 2 s silence, -60 ppm", 6, -60.0, 1'000'000, 20000, 2'000'000'000'000, 0);
    // Release 1 frame policy: 24 + 20 bits SDR, 10 ns coarse ticks.
    hw_case("R1 policy (24+20), +80 ppm", 0, 80.0, 2'000'000, 50000, 0, 0, 1);
    hw_case("R1 policy, 2 s silence, -60 ppm", 0, -60.0, 1'000'000, 20000, 2'000'000'000'000, 0, 1);
}

// ---------------------------------------------------------------------------
static void test_lifecycle() {
    const auto dir = std::filesystem::temp_directory_path() / ("dantagger_test_life_" + std::to_string(getpid()));
    TdcBackend::simulate_unplug();
    TdcBackend::simulated_fpga().event_source = nullptr;
    TdcOptions o;
    o.simulate_fpga = true;
    o.state_dir = dir.string();
    {
        TdcBackend b(o);
        CHECK(b.setup_action() == "calibrated");
    }
    {
        TdcBackend b(o);
        CHECK(b.setup_action() == "skipped");
        b.set_resolution(4);
    }
    {
        TdcBackend b(o);
        CHECK(b.setup_action() == "configured");   // still calibrated, GPX2 back to the default configuration
    }
    {
        TdcBackend b(o);
        CHECK(b.setup_action() == "skipped");
        CHECK(!TdcBackend::simulated_fpga().lclk_en);
    }
    TdcBackend::simulate_unplug();   // power cycle
    {
        TdcBackend b(o);
        CHECK(b.setup_action() == "calibrated");
    }
    o.recalibrate = true;
    {
        TdcBackend b(o);
        CHECK(b.setup_action() == "calibrated");
        const auto st = b.last_status();
        CHECK(st.lvds.ref_bits == 24 && st.lvds.stop_bits == 18 && st.lvds.ddr && st.lvds.divider_code() == 0);
    }
    o.recalibrate = false;
    // A fallback calibration (125 MHz SDR, made by hand as in the app's TDC page) stays the setup.
    {
        TdcBackend b(o);
        auto config = policy_gpx_config(b.last_status().lvds.timing, 8e6, 0, 0, 0, false);
        tdcctl::CalibrationOptions co;
        co.divider_code = 1;
        const auto r = b.session().calibrate(config, co);
        CHECK(r.success);
        b.record_calibration("test fallback");
    }
    {
        TdcBackend b(o);
        CHECK(b.setup_action() == "skipped");   // the policy now is SDR (from the state file): nothing to redo
        CHECK(!b.last_status().lvds.ddr && b.last_status().lvds.divider_code() == 1);
    }
    TdcBackend::simulate_unplug();   // power cycle: the setup calibrates at the fallback again
    {
        TdcBackend b(o);
        CHECK(b.setup_action() == "calibrated");
        const auto st = b.last_status();
        CHECK(!st.lvds.ddr && st.lvds.divider_code() == 1 && st.lvds.ref_bits == 24 && st.lvds.stop_bits == 18);
        CHECK(b.set_up());
    }
    // A release-1 session in between (power cycle: release 1 from flash) keeps the release-2 fallback.
    TdcBackend::simulate_unplug();
    TdcBackend::simulated_fpga().release1();
    {
        TdcBackend b(o);
        CHECK(b.setup_action() == "calibrated");
        const auto st = b.last_status();
        CHECK(st.design_version == tdcctl::kDesignVersionR1 && !st.lvds.ddr && st.lvds.ref_bits == 24 && st.lvds.stop_bits == 20);
    }
    CHECK(std::filesystem::exists(dir / "SIMULATED-FPGA.json") && std::filesystem::exists(dir / "SIMULATED-FPGA.release2.json"));
    TdcBackend::simulate_unplug();   // release 3 loaded again (it shares release 2's state file)
    {
        TdcBackend b(o);
        CHECK(b.setup_action() == "calibrated");
        const auto st = b.last_status();
        CHECK(st.design_version == tdcctl::kDesignVersionR3 && !st.lvds.ddr && st.lvds.divider_code() == 1);
    }
    std::filesystem::remove_all(dir);
}

// A timed window that spans the gap between two runs covers `duration` of
// captured time (the gap does not count), so a Counter bin of the window
// length completes (regression: SDE tomography flow, 2026-10-22).
static void test_window_across_runs() {
    WarningLog log;
    Engine engine(log);
    const timestamp_t d = 1'000'000;
    auto counter = std::make_shared<Counter>(std::vector<int>{1}, d, 1);
    engine.add(counter, false);
    engine.start({counter.get()}, d, true);
    auto run = [&](timestamp_t base, timestamp_t len, int n) {
        RawChunk a;
        a.run_begin = true;
        a.run_base = base;
        a.watermark = base;
        for (int i = 0; i < n; ++i) a.events[0].push_back(base + (i + 1) * len / (n + 1));
        a.watermark = base + len;
        a.run_end = true;
        engine.push(std::move(a));
    };
    run(0, 400'000, 4);              // 0.4 of the window, then the run ends
    run(5'000'000, 2'000'000, 10);   // the next run after a 4.6 us gap
    CHECK(engine.wait({counter.get()}, 5000));
    auto g = counter->guard();
    CHECK(counter->capture_duration == d);
    const auto tot = counter->total_counts();
    CHECK(tot.size() == 1 && tot[0] == 4 + 3);   // 4 in the first run, 3 in the first 0.6 us of the second
}

// ---------------------------------------------------------------------------
// Saved tag streams: the recorder writes the raw records of a live run, the
// file backend decodes them to exactly the same tags.
static std::vector<std::pair<timestamp_t, int>> stream_tags(Tagger& tagger, timestamp_t duration,
                                                            const std::function<void()>& during = {}) {
    auto stream = std::make_shared<TimeTagStream>(50'000'000, std::vector<int>{1, 2, 3, 4});
    tagger.engine().add(stream, false);
    tagger.engine().start({stream.get()}, duration, true);
    if (during) during();
    if (duration >= 0) {
        tagger.engine().wait({stream.get()}, -1);
    } else {   // a file: until it is read
        auto& fb = static_cast<FileBackend&>(tagger.backend());
        while (!fb.finished()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        tagger.engine().wait_drained();
        tagger.engine().stop({stream.get()});
    }
    StreamBuffer b;
    {
        auto g = stream->guard();
        b = stream->take();
    }
    tagger.engine().remove(stream.get());
    std::vector<std::pair<timestamp_t, int>> out;
    for (std::size_t i = 0; i < b.timestamps.size(); ++i)
        if (b.types[i] == 0) out.emplace_back(b.timestamps[i], b.channels[i]);
    return out;
}

// `replayed` must equal a contiguous part of `live` up to one time offset.
static bool same_tags(const std::vector<std::pair<timestamp_t, int>>& live,
                      const std::vector<std::pair<timestamp_t, int>>& replayed, std::size_t& matched) {
    matched = 0;
    if (replayed.empty()) return false;
    for (std::size_t s = 0; s < live.size(); ++s) {   // first live tag the replay starts at
        if (live[s].second != replayed[0].second) continue;
        const timestamp_t k = live[s].first - replayed[0].first;
        std::size_t i = 0;
        while (i < replayed.size() && s + i < live.size() && live[s + i].second == replayed[i].second &&
               live[s + i].first - replayed[i].first == k)
            ++i;
        if (i == replayed.size() || s + i == live.size()) {   // every overlapping tag equal
            matched = i;
            return i > 0;
        }
    }
    return false;
}

static void test_saved_streams() {
    const auto dir = std::filesystem::temp_directory_path() / ("dantagger_test_rec_" + std::to_string(getpid()));
    std::filesystem::create_directories(dir);
    TdcBackend::simulate_unplug();
    auto& fake = TdcBackend::simulated_fpga();
    fake.event_source = nullptr;
    fake.event_rate_hz = 200000;
    fake.latency_jitter_ps = 500'000;
    TdcOptions o;
    o.simulate_fpga = true;
    o.state_dir = (dir / "state").string();
    for (int mode = 0; mode < 3; ++mode) {   // 0: from the run start, 1: joins mid-run, 2: time limit
        Tagger tagger(std::make_unique<TdcBackend>(o), SettingsPolicy::restart);
        auto& backend = static_cast<TdcBackend&>(tagger.backend());
        StreamRecorder::Options ro;
        ro.path = (dir / ("rec" + std::to_string(mode) + ".bin")).string();
        ro.max_seconds = mode == 2 ? 0.002 : 0;
        auto rec = std::make_shared<StreamRecorder>(ro);
        if (mode != 1) backend.add_tap(rec);
        const auto live = stream_tags(tagger, 6'000'000'000, [&] {
            if (mode == 1) {   // once the run has streamed a while
                while (tagger.engine().tags_processed() < 1000) std::this_thread::sleep_for(std::chrono::microseconds(200));
                backend.add_tap(rec);
            }
        });
        backend.remove_tap(rec.get());
        rec->finish();
        const auto prog = rec->progress();
        tagger.close();
        Tagger replay(std::make_unique<FileBackend>(ro.path));
        const auto info = static_cast<FileBackend&>(replay.backend()).stream_info();
        const auto got = stream_tags(replay, -1);
        std::size_t matched = 0;
        const bool same = same_tags(live, got, matched);
        std::printf("  %-26s live %zu tags, file %zu tags (%llu records, %.4f s), matched %zu\n",
                    mode == 0 ? "recorded from run start" : mode == 1 ? "recorder joined mid-run" : "2 ms time limit",
                    live.size(), got.size(), static_cast<unsigned long long>(prog.records), prog.seconds, matched);
        CHECK(info.have_sidecar && info.ref_bits == 24 && info.stop_bits == 18 && info.ddr && info.coarse_tick_ns == 8.0);
        // Release 3 sent compact records; the file holds 64-bit ones and says so.
        CHECK(info.design_version == tdcctl::kDesignVersionR3 && info.record_format == 2 &&
              info.coarse_uncertainty_ticks == tdcctl::kTimeMarkTicks + 16);
        CHECK(prog.lost == 0 && prog.error.empty());
        CHECK(same);
        if (mode == 0) CHECK(matched == live.size());   // the file holds the whole live window (and the run's end)
        if (mode == 1) CHECK(matched < live.size() && matched > live.size() / 4);   // from the join to the window end
        // Compact events carry no coarse time: the limit is seen at the next marker (TIME every 131 us).
        if (mode == 2) CHECK(prog.limit_reached && prog.seconds >= 0.002 && prog.seconds < 0.002 + 0.000135);
        CHECK(static_cast<FileBackend&>(replay.backend()).error().empty());
    }
    fake.latency_jitter_ps = 0;
    fake.event_rate_hz = 20000;
    std::filesystem::remove_all(dir);
}

// ---------------------------------------------------------------------------
// Virtual channels (Coincidence): the same tags however the stream is cut into
// chunks; a new run and an overflow forget the past; numbers and inputs.
struct CoincRun {
    std::vector<std::pair<timestamp_t, int>> tags;   // every channel incl. the virtual ones, stream order
    std::vector<int> vch;
};

// runs[r][c - 1]: absolute raw times; run r + 1 starts `gap` after run r ends.
static CoincRun coincidence_run(const std::vector<std::vector<std::vector<timestamp_t>>>& runs, std::size_t pieces,
                                const std::vector<Special>& overflow = {}, timestamp_t gap = 1'000'000) {
    WarningLog log;
    Engine engine(log);
    auto c12 = std::make_shared<Coincidence>(std::vector<int>{1, 2}, 2000);
    auto c123 = std::make_shared<Coincidence>(std::vector<int>{1, 2, 3}, 5000);
    CoincRun out;
    out.vch.push_back(engine.add_virtual(c12));
    out.vch.push_back(engine.add_virtual(c123));
    auto chain = std::make_shared<Coincidence>(std::vector<int>{out.vch[0], 4}, 3000);
    out.vch.push_back(engine.add_virtual(chain));
    std::vector<int> all{1, 2, 3, 4};
    all.insert(all.end(), out.vch.begin(), out.vch.end());
    auto stream = std::make_shared<TimeTagStream>(10'000'000, all);
    engine.add(stream, false);
    engine.start({stream.get()}, -1, true);
    timestamp_t base = 0;
    for (const auto& run : runs) {
        timestamp_t end = base;
        for (const auto& lane : run)
            if (!lane.empty()) end = std::max(end, lane.back() + 1);
        end += 1000;
        for (std::size_t k = 0; k < pieces; ++k) {   // `pieces` chunks with increasing watermarks
            RawChunk ch;
            const timestamp_t lo = base + (end - base) * static_cast<timestamp_t>(k) / static_cast<timestamp_t>(pieces);
            const timestamp_t hi = base + (end - base) * static_cast<timestamp_t>(k + 1) / static_cast<timestamp_t>(pieces);
            ch.run_begin = k == 0;
            ch.run_base = base;
            ch.watermark = hi;
            ch.run_end = k + 1 == pieces;
            for (int c = 0; c < kNumChannels; ++c)
                for (timestamp_t t : run[static_cast<std::size_t>(c)])
                    if (t >= lo && t < hi) ch.events[c].push_back(t);
            for (const auto& s : overflow)
                if (s.time >= lo && s.time < hi) ch.specials.push_back(s);
            engine.push(std::move(ch));
        }
        base = end + gap;
    }
    engine.wait_drained();
    StreamBuffer b;
    {
        auto g = stream->guard();
        b = stream->take();
    }
    for (std::size_t i = 0; i < b.timestamps.size(); ++i)
        if (b.types[i] == 0) out.tags.emplace_back(b.timestamps[i], b.channels[i]);
    return out;
}

static void test_virtual_channels() {
    std::mt19937_64 rng(9);
    std::vector<std::vector<std::vector<timestamp_t>>> runs(2, std::vector<std::vector<timestamp_t>>(kNumChannels));
    timestamp_t base = 0;
    for (auto& run : runs) {
        for (int c = 0; c < kNumChannels; ++c) {
            auto& lane = run[static_cast<std::size_t>(c)];
            timestamp_t t = base + 100;
            for (int i = 0; i < 4000; ++i) {
                t += 1 + static_cast<timestamp_t>(rng() % 6000);
                lane.push_back(c == 3 ? (t / 500) * 500 : t);   // channel 4 on a grid: ties with the others
            }
            std::sort(lane.begin(), lane.end());
            lane.erase(std::unique(lane.begin(), lane.end()), lane.end());
        }
        base = 100'000'000'000;   // the second run far later
    }
    const CoincRun one = coincidence_run(runs, 1);
    CHECK((one.vch == std::vector<int>{5, 6, 7}));
    std::size_t n_virtual = 0;
    for (const auto& [t, c] : one.tags) n_virtual += c > kNumChannels ? 1 : 0;
    CHECK(n_virtual > 1000);
    for (std::size_t pieces : {2, 7, 64, 500}) {
        const CoincRun many = coincidence_run(runs, pieces);
        CHECK(many.tags == one.tags);   // block edges change nothing
    }
    // Brute force for {1, 2} within 2000 ps, each run on its own (a new run forgets the past).
    std::size_t want = 0;
    for (const auto& run : runs) {
        std::vector<std::pair<timestamp_t, int>> m;
        for (int c = 1; c <= 2; ++c)
            for (timestamp_t t : run[static_cast<std::size_t>(c - 1)]) m.emplace_back(t, c);
        std::sort(m.begin(), m.end());
        timestamp_t last[3] = {kUnset, kUnset, kUnset};
        for (const auto& [t, c] : m) {
            last[c] = t;
            const timestamp_t other = last[3 - c];
            if (other != kUnset && t - other < 2000) ++want;
        }
    }
    std::size_t got = 0;
    for (const auto& [t, c] : one.tags) got += c == 5 ? 1 : 0;
    CHECK(got == want);
    // Each virtual tag right after a tag of its group at the same time.
    for (std::size_t i = 0; i < one.tags.size(); ++i) {
        if (one.tags[i].second <= kNumChannels) continue;
        std::size_t j = i;   // skip later virtual channels' tags made by the same tag
        while (j > 0 && one.tags[j - 1].second > one.tags[i].second) --j;
        CHECK(j > 0 && one.tags[j - 1].first == one.tags[i].first);
    }

    // Across a run gap and across an overflow: no coincidence of tags on either side.
    {
        std::vector<std::vector<std::vector<timestamp_t>>> two(2, std::vector<std::vector<timestamp_t>>(kNumChannels));
        two[0][0] = {1000};
        two[1][1] = {2101};   // the first run ends at 2001, the second begins there: 1101 ps apart
        const CoincRun r = coincidence_run(two, 1, {}, 0);
        CHECK(r.tags.size() == 2);
        two[1][1].clear();
        two[0][1] = {2101};   // the same tags in one run: a coincidence
        CHECK(coincidence_run(two, 1, {}, 0).tags.size() == 3);
        std::vector<std::vector<std::vector<timestamp_t>>> ov(1, std::vector<std::vector<timestamp_t>>(kNumChannels));
        ov[0][0] = {1000, 5000};
        ov[0][1] = {1600, 5100};
        Special s;
        s.time = 1500;
        s.type = TagType::OverflowBegin;
        const CoincRun with = coincidence_run(ov, 3, {s});
        const CoincRun without = coincidence_run(ov, 3);
        std::size_t a = 0, b = 0;
        for (const auto& [t, c] : with.tags) a += c == 5 ? 1 : 0;
        for (const auto& [t, c] : without.tags) b += c == 5 ? 1 : 0;
        CHECK(a == 1 && b == 2);
    }

    // Registration: numbers after the inputs, inputs must exist, freed numbers come back.
    {
        WarningLog log;
        Engine engine(log);
        auto a = std::make_shared<Coincidence>(std::vector<int>{1, 2}, 1000);
        CHECK(engine.add_virtual(a) == 5);
        CHECK(engine.channel_exists(4) && engine.channel_exists(5) && !engine.channel_exists(6));
        bool threw = false;
        try {
            engine.add_virtual(std::make_shared<Coincidence>(std::vector<int>{1, 9}, 1000));
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        CHECK(threw);
        threw = false;
        try {
            engine.add_virtual(a);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        CHECK(threw);
        auto b = std::make_shared<Coincidence>(std::vector<int>{5, 3}, 1000);
        CHECK(engine.add_virtual(b) == 6);
        engine.remove_virtual(a.get());
        CHECK(!engine.channel_exists(5));
        CHECK(engine.add_virtual(std::make_shared<Coincidence>(std::vector<int>{1}, 1000)) == 7);   // 5 is still read by b
        engine.remove_virtual(b.get());
        CHECK(engine.add_virtual(std::make_shared<Coincidence>(std::vector<int>{1}, 1000)) == 5);
        threw = false;
        try {
            Coincidence bad(std::vector<int>{1, 2}, -1);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        CHECK(threw);
    }
}

int main() {
    std::printf("saved tag streams (recorder -> file backend)\n");
    test_saved_streams();
    std::printf("virtual channels (Coincidence)\n");
    test_virtual_channels();
    std::printf("window across runs\n");
    test_window_across_runs();
    std::printf("fastdiv\n");
    test_fastdiv();
    std::printf("merger\n");
    test_merger();
    std::printf("lifecycle\n");
    test_lifecycle();
    std::printf("hardware path (simulated FPGA)\n");
    test_hardware_path();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
