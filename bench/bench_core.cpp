// Benchmarks of the Dantagger core on simulated data (single thread unless
// noted).  .venv/bin/python Dantagger/build.py --bench && Dantagger/build/bench
//
// Stream "tomography": ch4 laser 2.733 MHz (2 ps jitter), ch1/ch2 detector
// clicks at 5 % of the pulses 31.5/31.6 ns later (20 ps jitter), ch3 100 kHz
// Poisson -- the SDE tomography setup at the highest planned rates.

#include "engine.hpp"
#include "measurements.hpp"
#include "virtual_channels.hpp"
#include "merger.hpp"
#include "record_decoder.hpp"
#include "sim_backend.hpp"
#include "tagger.hpp"

#include "au2ft601/protocol.hpp"
#include "tdcctl/tdc_session.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <vector>

using namespace dt;
using Clock = std::chrono::steady_clock;

namespace {

struct Stream {
    std::vector<timestamp_t> t;
    std::vector<std::int8_t> ch;
};

Stream tomography_stream(std::size_t n_target, double laser_hz = 2.733e6, double eff = 0.05) {
    std::mt19937_64 rng(42);
    std::normal_distribution<double> g(0, 1);
    std::bernoulli_distribution click(eff);
    std::exponential_distribution<double> dark(100e3 * 1e-12);
    const double period = 1e12 / laser_hz;
    std::vector<std::pair<timestamp_t, std::int8_t>> ev;
    double next_dark = dark(rng);
    for (std::int64_t k = 0; ev.size() < n_target; ++k) {
        const double p = static_cast<double>(k) * period;
        ev.emplace_back(static_cast<timestamp_t>(p + 2 * g(rng)), 4);
        if (click(rng)) ev.emplace_back(static_cast<timestamp_t>(p + 31500 + 20 * g(rng)), 1);
        if (click(rng)) ev.emplace_back(static_cast<timestamp_t>(p + 31600 + 20 * g(rng)), 2);
        while (next_dark < p) {
            ev.emplace_back(static_cast<timestamp_t>(next_dark), 3);
            next_dark += dark(rng);
        }
    }
    std::sort(ev.begin(), ev.end());
    Stream s;
    for (auto& [t, c] : ev) {
        s.t.push_back(t);
        s.ch.push_back(c);
    }
    return s;
}

double seconds_since(Clock::time_point t0) { return std::chrono::duration<double>(Clock::now() - t0).count(); }

// Runs m over the stream in blocks of 65536 tags, `reps` times.
void bench_measurement(const char* name, Measurement& m, const Stream& s, int reps = 3) {
    const std::size_t block = 65536;
    double best = 1e9;
    for (int r = 0; r < reps; ++r) {
        m.clear_data();
        m.on_gap();
        m.capture_duration = 0;
        const auto t0 = Clock::now();
        for (std::size_t i = 0; i < s.t.size(); i += block) {
            const std::size_t n = std::min(block, s.t.size() - i);
            const timestamp_t b = s.t[i], e = (i + n < s.t.size()) ? s.t[i + n] : s.t.back() + 1;
            m.process(s.t.data() + i, s.ch.data() + i, n, b, e);
            m.capture_duration += e - b;
        }
        best = std::min(best, seconds_since(t0));
    }
    std::printf("  %-44s %8.1f M tags/s  (%5.2f ns/tag)\n", name, static_cast<double>(s.t.size()) / best * 1e-6,
                best / static_cast<double>(s.t.size()) * 1e9);
}

void bench_merger(const Stream& s) {
    // Split back into channels and feed in chunks of ~65k (the backend's chunking).
    std::array<std::vector<timestamp_t>, kNumChannels> per;
    for (std::size_t i = 0; i < s.t.size(); ++i) per[static_cast<std::size_t>(s.ch[i] - 1)].push_back(s.t[i]);
    for (bool with_delay : {false, true}) {
        Merger m;
        std::array<timestamp_t, kNumChannels> d{}, dead{};
        if (with_delay) d = {31'552'016, 31'550'711, 0, 25'000};
        m.configure(d, dead);
        TagBlock blk;
        const auto t0 = Clock::now();
        RawChunk first;
        first.run_begin = true;
        first.run_base = 0;
        m.push(first, blk);
        std::array<std::size_t, kNumChannels> pos{};
        const timestamp_t step = s.t.back() / static_cast<timestamp_t>(s.t.size() / 65536 + 1);
        std::size_t total = 0;
        for (timestamp_t w = step;; w += step) {
            RawChunk c;
            bool any = false;
            for (int k = 0; k < kNumChannels; ++k) {
                auto& v = per[static_cast<std::size_t>(k)];
                auto& p = pos[static_cast<std::size_t>(k)];
                const auto e = std::lower_bound(v.begin() + static_cast<std::ptrdiff_t>(p), v.end(), w) - v.begin();
                c.events[k].assign(v.begin() + static_cast<std::ptrdiff_t>(p), v.begin() + e);
                p = static_cast<std::size_t>(e);
                any |= p < v.size();
            }
            c.watermark = w;
            c.run_end = !any;
            m.push(c, blk);
            total += blk.size();
            if (!any) break;
        }
        const double dt = seconds_since(t0);
        std::printf("  %-44s %8.1f M tags/s  (%5.2f ns/tag)%s\n", with_delay ? "merger, 4 ch, tomography delays" : "merger, 4 ch, no delays",
                    static_cast<double>(total) / dt * 1e-6, dt / static_cast<double>(total) * 1e9,
                    total == s.t.size() ? "" : "  COUNT MISMATCH");
    }
}

void bench_decoder(const Stream& s) {
    // Encode the stream as firmware records (16-bit index, TIME markers every 2^14 coarse cycles).
    std::vector<std::uint64_t> rec;
    const std::uint64_t base_coarse = 1000;
    rec.push_back((1ULL << 63) | base_coarse);   // START
    std::uint64_t next_mark = (base_coarse | 0x3FFFU) + 1;
    for (std::size_t i = 0; i < s.t.size(); ++i) {
        const std::uint64_t t = static_cast<std::uint64_t>(s.t[i]) + base_coarse * 10000 + 600000;
        const std::uint64_t coarse = (t + 600000) / 10000;
        while (next_mark <= coarse) {
            rec.push_back((1ULL << 63) | (1ULL << 60) | next_mark);
            next_mark += 16384;
        }
        const std::uint64_t ref = (t / 125000) & 0xFFFF, stop = t % 125000;
        rec.push_back((static_cast<std::uint64_t>(s.ch[i] - 1) << 61) | ((coarse & 0xFFFF) << 44) | (ref << 20) | stop);
    }
    RecordDecoder d({125000, 1, 16, 18});
    const auto t0 = Clock::now();
    d.begin_run();
    RawChunk c;
    std::size_t events = 0;
    for (std::size_t i = 0; i < rec.size(); ++i) {
        d.feed(rec[i], c);
        if ((i & 32767) == 32767) {
            for (auto& e : c.events) {
                events += e.size();
                e.clear();
            }
        }
    }
    const double dt = seconds_since(t0);
    std::printf("  %-44s %8.1f M records/s (%5.2f ns/record)\n", "record decoder (unwrap, watermark)",
                static_cast<double>(rec.size()) / dt * 1e-6, dt / static_cast<double>(rec.size()) * 1e9);

    // The host side of the USB path as Session::stream runs it: frame scan
    // (magic, CRC), DATA frame checks, one std::function call per record, decode.
    std::vector<std::uint8_t> bytes;
    for (std::size_t i = 0, seq = 0; i < rec.size(); i += tdcctl::kMaxRecordsPerFrame, ++seq) {
        const std::size_t n = std::min(tdcctl::kMaxRecordsPerFrame, rec.size() - i);
        std::vector<std::uint8_t> payload(8 * n);
        for (std::size_t r = 0; r < n; ++r)
            for (std::size_t b = 0; b < 8; ++b) payload[8 * r + b] = static_cast<std::uint8_t>(rec[i + r] >> (8U * b));
        au2ft601::FrameHeader h;
        h.type = au2ft601::FrameType::data;
        h.run_id = 5;
        h.sequence_or_transaction = seq;
        h.payload_offset = i;
        h.pattern_or_opcode = 1;
        h.valid_payload_length = static_cast<std::uint32_t>(payload.size());
        const auto f = au2ft601::encode_frame(h, payload.data(), payload.size());
        bytes.insert(bytes.end(), f.begin(), f.end());
    }
    RecordDecoder d2({125000, 1, 16, 18});
    d2.begin_run();
    RawChunk c2;
    std::function<void(std::uint64_t)> sink = [&](std::uint64_t raw) { d2.feed(raw, c2); };
    tdcctl::FrameScanner scanner;
    const auto t1 = Clock::now();
    std::size_t nrec = 0;
    for (std::size_t off = 0; off < bytes.size(); off += 64 * 4096) {
        const std::size_t len = std::min<std::size_t>(64 * 4096, bytes.size() - off);
        for (const auto& frame : scanner.feed(bytes.data() + off, len)) {
            std::string error;
            auto data = tdcctl::decode_data_frame(frame, error);
            if (!data) continue;
            for (auto raw : data->records) {
                sink(raw);
                ++nrec;
            }
        }
        for (auto& e : c2.events) e.clear();
    }
    const double dt1 = seconds_since(t1);
    std::printf("  %-44s %8.1f M records/s (%5.2f ns/record, %.0f MB/s of USB data)\n",
                "USB frames -> records -> decoder (host side)", static_cast<double>(nrec) / dt1 * 1e-6,
                dt1 / static_cast<double>(nrec) * 1e9, static_cast<double>(bytes.size()) / dt1 * 1e-6);
}

// End to end: ReplayBackend -> engine queue -> merger -> dispatch, as fast as possible.
// `coincidences`: Coincidence virtual channels (1 ns) registered first, as channels 5, 6, ...
void bench_end_to_end(const char* name, const Stream& s,
                      const std::function<std::vector<std::shared_ptr<Measurement>>()>& make,
                      std::array<timestamp_t, kNumChannels> delays = {},
                      const std::vector<std::vector<int>>& coincidences = {}) {
    Tagger tagger(std::make_unique<ReplayBackend>(s.t, s.ch, -1.0));
    for (int c = 0; c < kNumChannels; ++c) tagger.set_delay(c + 1, delays[static_cast<std::size_t>(c)]);
    std::vector<std::shared_ptr<Coincidence>> virtuals;
    for (const auto& g : coincidences) {
        virtuals.push_back(std::make_shared<Coincidence>(g, 1000));
        tagger.engine().add_virtual(virtuals.back());
    }
    auto ms = make();
    std::vector<Measurement*> raw;
    for (auto& m : ms) {
        tagger.engine().add(m, false);
        raw.push_back(m.get());
    }
    const auto before = tagger.engine().tags_processed();
    const auto t0 = Clock::now();
    tagger.engine().start(raw, s.t.back() + 1, true);
    tagger.engine().wait(raw, -1);
    const double dt = seconds_since(t0);
    const auto tags = tagger.engine().tags_processed() - before;
    std::printf("  %-44s %8.1f M tags/s  (%5.2f ns/tag, %zu tags)\n", name, static_cast<double>(s.t.size()) / dt * 1e-6,
                dt / static_cast<double>(s.t.size()) * 1e9, static_cast<std::size_t>(tags));
    for (auto* m : raw) tagger.engine().remove(m);
    tagger.close();
}

}  // namespace

int main() {
    std::printf("Generating the tomography stream ...\n");
    const Stream s = tomography_stream(20'000'000);
    std::printf("  %zu tags, %.3f s of tag time (%.2f M tags/s)\n\n", s.t.size(), static_cast<double>(s.t.back()) * 1e-12,
                static_cast<double>(s.t.size()) / (static_cast<double>(s.t.back()) * 1e-12) * 1e-6);

    std::printf("Measurements alone (process() over 64k-tag blocks):\n");
    {
        Countrate m({1, 2, 3, 4});
        bench_measurement("Countrate [1,2,3,4]", m, s);
    }
    {
        Counter m({1, 2}, 1'000'000'000, 1000);
        bench_measurement("Counter [1,2], 1 ms bins", m, s);
    }
    {
        Counter m({3}, 1'000'000, 10000);
        bench_measurement("Counter [3], 1 us bins (many bins)", m, s);
    }
    {
        Histogram m(1, 4, 100, 1000);   // 100 ns range: ~0.3 starts per click
        bench_measurement("Histogram click 1 / start 4, 100 ps x 1000", m, s);
    }
    {
        Histogram m(1, 4, 1000, 10000);   // 10 us range: ~27 starts per click
        bench_measurement("Histogram 1/4, 1 ns x 10000 (27 starts/click)", m, s);
    }
    {
        Correlation m(1, 4, 100, 1000);
        bench_measurement("Correlation 1-4, 100 ps x 1000", m, s);
    }
    {
        Correlation m(1, 2, 10, 2000);
        bench_measurement("Correlation 1-2, 10 ps x 2000", m, s);
    }
    {
        Correlation m(4, 4, 1000, 2000);   // 2 us range at 2.7 MHz: ~5 partners per tag
        bench_measurement("Correlation auto 4, 1 ns x 2000", m, s);
    }
    {
        HistogramND m(4, {2, 1}, {4, 4}, {500, 500});
        bench_measurement("HistogramND 4 -> [2,1], 4 ps x 500 x 500", m, s);
    }
    {
        HistogramND m(4, {2, 1}, {100, 100}, {500, 500});
        bench_measurement("HistogramND 4 -> [2,1], 100 ps x 500 x 500", m, s);
    }
    {
        TimeTagStream m(30'000'000, {1, 2, 3, 4});
        bench_measurement("TimeTagStream (all channels)", m, s, 1);
    }

    std::printf("\nPipeline stages:\n");
    bench_merger(s);
    bench_decoder(s);

    std::printf("\nEnd to end (replay backend -> queue -> merger -> engine), as fast as possible:\n");
    bench_end_to_end("tomography set: Countrate+HistogramND+2xCounter", s, [] {
        return std::vector<std::shared_ptr<Measurement>>{
            std::make_shared<Countrate>(std::vector<int>{3}),
            std::make_shared<HistogramND>(4, std::vector<int>{2, 1}, std::vector<timestamp_t>{4, 4}, std::vector<int>{500, 500}),
            std::make_shared<Counter>(std::vector<int>{3}, 1'000'000'000'000, 1),
            std::make_shared<Counter>(std::vector<int>{4, 2, 1}, 1'000'000'000'000, 1)};
    }, {0, 0, 0, 0});
    bench_end_to_end("tomography set with 31.5 us delays", s, [] {
        return std::vector<std::shared_ptr<Measurement>>{
            std::make_shared<Countrate>(std::vector<int>{3}),
            std::make_shared<HistogramND>(4, std::vector<int>{2, 1}, std::vector<timestamp_t>{4, 4}, std::vector<int>{500, 500}),
            std::make_shared<Counter>(std::vector<int>{3}, 1'000'000'000'000, 1),
            std::make_shared<Counter>(std::vector<int>{4, 2, 1}, 1'000'000'000'000, 1)};
    }, {31'552'016, 31'550'711, 0, 25'000});
    bench_end_to_end("1-D tomography: Correlation + Countrate", s, [] {
        return std::vector<std::shared_ptr<Measurement>>{
            std::make_shared<Correlation>(4, 1, 10, 3000),
            std::make_shared<Countrate>(std::vector<int>{4, 1, 3})};
    });
    bench_end_to_end("ttCountRate: Countrate only", s, [] {
        return std::vector<std::shared_ptr<Measurement>>{std::make_shared<Countrate>(std::vector<int>{1})};
    });
    bench_end_to_end("Countrate 1-4 + coincidences 1+2, 1+4, 1+2+4", s, [] {
        return std::vector<std::shared_ptr<Measurement>>{std::make_shared<Countrate>(std::vector<int>{1, 2, 3, 4, 5, 6, 7})};
    }, {0, 0, 0, 0}, {{1, 2}, {1, 4}, {1, 2, 4}});
    return 0;
}
