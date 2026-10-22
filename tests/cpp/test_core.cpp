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

#include "tdcctl/fake_fpga.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <filesystem>
#include <functional>
#include <thread>
#include <map>
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

// ref_bits 0: the frame policy of the release (release 2: 6 + 18 bits DDR at
// 250 MHz, an 8 us reference-index wrap; release 1: 24 + 20 bits SDR).
static void hw_case(const char* name, int ref_bits, double ppm, std::uint64_t jitter_ps, double rate_hz,
                    timestamp_t silence_ps, std::uint64_t drop_every, bool release1 = false) {
    const auto dir = std::filesystem::temp_directory_path() / ("dantagger_test_state_" + std::to_string(getpid()));
    TdcBackend::simulate_unplug();
    auto& fake = TdcBackend::simulated_fpga();
    if (release1) fake.release1();
    fake.gpx_ppm = ppm;
    fake.latency_ps = 600000;
    fake.latency_jitter_ps = jitter_ps;
    fake.drop_every = drop_every;
    Truth truth;
    std::mt19937_64 rng(7);
    std::uint64_t silent_from = 0;
    bool silenced = false;
    fake.event_source = [&](std::uint64_t t0, std::uint64_t t1, std::vector<std::pair<std::uint64_t, int>>& out) {
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
    Tagger tagger(std::make_unique<TdcBackend>(o));
    {
        auto& b = static_cast<TdcBackend&>(tagger.backend());
        const auto st = b.last_status();
        const int want_ref = ref_bits != 0 ? ref_bits : release1 ? 24 : 6;
        CHECK(st.lvds.ref_bits == want_ref);
        CHECK(st.lvds.ddr == !release1);
        CHECK(st.design_version == (release1 ? tdcctl::kDesignVersionR1 : tdcctl::kDesignVersionR2));
    }
    const timestamp_t duration = 6'000'000'000 + silence_ps;
    std::vector<std::int32_t> chans;
    std::size_t specials = 0;
    const auto got = run_stream(tagger, duration, &chans, &specials);
    // Truth: per channel the events seen (some may be dropped / in flight at the run end).
    std::sort(truth.events.begin(), truth.events.end());
    std::printf("  %-34s %zu events (truth %zu), %zu markers\n", name, got.size(), truth.events.size(), specials);
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
        CHECK(channel_ok == n);
        CHECK(n == got.size());
        const bool short_wrap = ref_bits == 0 && !release1;   // 8 us: a long silence may re-anchor once
        if (silence_ps == 0 || (!short_wrap && (ref_bits == 24 || ref_bits == 0 || silence_ps < 10'000'000'000'000)))
            CHECK(changes == 0);
        else
            CHECK(changes <= 1);
    } else {
        CHECK(specials > 0);
        CHECK(tagger.log().dropped() > 0);
    }
    tagger.close();
    std::filesystem::remove_all(dir);
}

static void test_hardware_path() {
    hw_case("24-bit index, no drift", 24, 0.0, 0, 50000, 0, 0);
    hw_case("16-bit index, +80 ppm, 3 us jitter", 16, 80.0, 3'000'000, 50000, 0, 0);
    hw_case("16-bit index, -95 ppm, 1 MHz/ch", 16, -95.0, 2'000'000, 1e6, 0, 0);
    hw_case("24-bit index, 2 s silence, +60 ppm", 24, 60.0, 1'000'000, 20000, 2'000'000'000'000, 0);
    hw_case("16-bit index, 2 s silence, -60 ppm", 16, -60.0, 1'000'000, 20000, 2'000'000'000'000, 0);
    hw_case("16-bit index, 30 s silence, +100 ppm", 16, 100.0, 1'000'000, 20000, 30'000'000'000'000, 0);
    hw_case("FPGA drops every 9th event", 16, 0.0, 0, 200000, 0, 9);
    // Release 2 frame policy: 6-bit reference index (8 us wrap), 250 MHz DDR, 8 ns coarse ticks.
    hw_case("R2 policy (6+18), no drift", 0, 0.0, 0, 50000, 0, 0);
    hw_case("R2 policy, +95 ppm, 1.2 us jitter", 0, 95.0, 1'200'000, 50000, 0, 0);
    hw_case("R2 policy, -95 ppm, 1 MHz/ch, 1 us jitter", 0, -95.0, 1'000'000, 1e6, 0, 0);
    hw_case("R2 policy, 2 kHz/ch, +60 ppm", 0, 60.0, 500'000, 2000, 0, 0);
    hw_case("R2 policy, 2 s silence, -60 ppm", 0, -60.0, 1'000'000, 20000, 2'000'000'000'000, 0);
    hw_case("R2 policy, FPGA drops every 7th", 0, 0.0, 0, 200000, 0, 7);
    // Release 1 frame policy: 24 + 20 bits SDR, 10 ns coarse ticks.
    hw_case("R1 policy (24+20), +80 ppm", 0, 80.0, 2'000'000, 50000, 0, 0, true);
    hw_case("R1 policy, 2 s silence, -60 ppm", 0, -60.0, 1'000'000, 20000, 2'000'000'000'000, 0, true);
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
        CHECK(info.have_sidecar && info.ref_bits == 6 && info.ddr && info.coarse_tick_ns == 8.0);
        CHECK(prog.lost == 0 && prog.error.empty());
        CHECK(same);
        if (mode == 0) CHECK(matched == live.size());   // the file holds the whole live window (and the run's end)
        if (mode == 1) CHECK(matched < live.size() && matched > live.size() / 4);   // from the join to the window end
        if (mode == 2) CHECK(prog.limit_reached && prog.seconds >= 0.002 && prog.seconds < 0.0021);
        CHECK(static_cast<FileBackend&>(replay.backend()).error().empty());
    }
    fake.latency_jitter_ps = 0;
    fake.event_rate_hz = 20000;
    std::filesystem::remove_all(dir);
}

int main() {
    std::printf("saved tag streams (recorder -> file backend)\n");
    test_saved_streams();
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
