#include "tdcctl/run_file.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace tdcctl {

namespace {

std::string json_escape(const std::string& in) {
    std::string out;
    for (char c : in) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (static_cast<unsigned char>(c) < 0x20) {
            out += ' ';
        } else {
            out += c;
        }
    }
    return out;
}

std::string json_u64_array(const std::array<std::uint64_t, kChannels>& v) {
    std::string s = "[";
    for (std::size_t i = 0; i < v.size(); ++i) s += (i ? "," : "") + std::to_string(v[i]);
    return s + "]";
}

// Minimal sidecar reading (the sidecar is ours; numbers and one string).
std::optional<double> json_number(const std::string& text, const std::string& key) {
    const auto at = text.find("\"" + key + "\":");
    if (at == std::string::npos) return std::nullopt;
    try {
        return std::stod(text.substr(at + key.size() + 3));
    } catch (...) {
        return std::nullopt;
    }
}

std::string json_string(const std::string& text, const std::string& key) {
    const auto at = text.find("\"" + key + "\": \"");
    if (at == std::string::npos) return {};
    const auto from = at + key.size() + 5;
    const auto to = text.find('"', from);
    return to == std::string::npos ? std::string() : text.substr(from, to - from);
}

}  // namespace

std::string hex_bytes(const std::uint8_t* bytes, std::size_t n) {
    std::string s;
    char buf[4];
    for (std::size_t i = 0; i < n; ++i) {
        std::snprintf(buf, sizeof(buf), "%02X", bytes[i]);
        if (i) s += ' ';
        s += buf;
    }
    return s;
}

std::string iso_now() {
    char buf[32];
    const std::time_t t = std::time(nullptr);
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", std::localtime(&t));
    return buf;
}

void write_run_sidecar(const std::string& run_path, const Response& before, const StreamResult& result,
                       double refclk_hz) {
    const std::uint32_t divisions = gpx_divisions(before.gpx.written);
    std::ofstream side(run_path + ".json");
    const auto& t = before.lvds.timing;
    side << "{\n  \"tool\": \"tdcctl\",\n  \"design_version\": " << before.design_version << ",\n  \"start\": \"" << iso_now()
         << "\",\n  \"run_id\": " << result.run_id << ",\n  \"refclk_hz\": " << refclk_hz
         << ",\n  \"divisions\": " << divisions << ",\n  \"ref_bits\": " << before.lvds.ref_bits
         << ",\n  \"stop_bits\": " << before.lvds.stop_bits << ",\n  \"coarse_tick_ns\": " << t.coarse_tick_ns
         << ",\n  \"lclkin_mhz\": " << before.lvds.lclkin_mhz() << ",\n  \"ddr\": " << (before.lvds.ddr ? "true" : "false")
         << ",\n  \"bit_phase\": " << before.lvds.bit_phase() << ",\n  \"gpx_registers\": \""
         << hex_bytes(before.gpx.written.data(), kGpxRegisters) << "\",\n  \"lvds_phase\": " << before.lvds.phase_step()
         << ",\n  \"records\": " << result.records << ",\n  \"frames\": " << result.frames << ",\n  \"events\": "
         << json_u64_array(result.events) << ",\n  \"dropped_marked\": " << result.dropped_marked
         << ",\n  \"fpga_dropped\": " << (result.final_response ? result.final_response->stream.dropped : 0)
         << ",\n  \"fpga_captured\": " << (result.final_response ? result.final_response->stream.captured : 0)
         << ",\n  \"by_window\": " << (result.by_window ? "true" : "false") << ",\n  \"seconds\": " << result.seconds
         << ",\n  \"pass\": " << (result.pass ? "true" : "false") << ",\n  \"failure\": \"" << json_escape(result.failure)
         << "\"\n}\n";
}

RunSettings read_run_settings(const std::string& run_path) {
    RunSettings s;
    std::ifstream side(run_path + ".json");
    if (!side) return s;
    const std::string text((std::istreambuf_iterator<char>(side)), std::istreambuf_iterator<char>());
    s.have_sidecar = true;
    s.refclk_hz = json_number(text, "refclk_hz").value_or(s.refclk_hz);
    s.divisions = static_cast<std::uint32_t>(json_number(text, "divisions").value_or(s.divisions));
    s.ref_bits = static_cast<int>(json_number(text, "ref_bits").value_or(s.ref_bits));
    s.stop_bits = static_cast<int>(json_number(text, "stop_bits").value_or(s.stop_bits));
    s.design_version = static_cast<std::uint32_t>(json_number(text, "design_version").value_or(s.design_version));
    s.coarse_tick_ns = json_number(text, "coarse_tick_ns").value_or(s.coarse_tick_ns);
    if (!(s.coarse_tick_ns > 0)) s.coarse_tick_ns = kCoarseTickNsR1;
    s.lvds_phase = json_number(text, "lvds_phase").value_or(-1);
    s.fpga_dropped = static_cast<std::uint64_t>(json_number(text, "fpga_dropped").value_or(0));
    s.sidecar_pass = text.find("\"pass\": true") != std::string::npos;
    s.start = json_string(text, "start");
    return s;
}

Measurement load_measurement(const std::string& run_path, std::atomic<float>* progress, const std::atomic<bool>* cancel) {
    Measurement m;
    m.path = run_path;
    m.settings = read_run_settings(run_path);
    std::ifstream file(run_path, std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error("cannot read " + run_path);
    const auto size = static_cast<std::uint64_t>(file.tellg());
    if (size == 0 || size % 8 != 0)
        throw std::runtime_error(run_path + " is not a run file (" + std::to_string(size) + " bytes, not a multiple of 8)");
    file.seekg(0);
    const std::uint64_t total = size / 8;
    TimeUnwrapper unwrap(m.settings.refclk_hz, m.settings.divisions, m.settings.ref_bits, m.settings.coarse_tick_ns);
    std::uint64_t start_coarse = 0, last_marker_coarse = 0;
    std::vector<char> buf(8 * 65536);
    bool stopped = false;
    while (!stopped && m.records < total) {
        const auto want = static_cast<std::streamsize>(std::min<std::uint64_t>(buf.size(), (total - m.records) * 8));
        if (!file.read(buf.data(), want)) throw std::runtime_error("read error in " + run_path);
        for (std::streamsize off = 0; off < want; off += 8) {
            std::uint64_t raw = 0;
            for (int i = 0; i < 8; ++i)
                raw |= static_cast<std::uint64_t>(static_cast<unsigned char>(buf[static_cast<std::size_t>(off + i)])) << (8 * i);
            const auto rec = decode_record(raw);
            if (m.records == 0 && !(rec.marker && rec.marker_type == MarkerType::start))
                throw std::runtime_error(run_path + " does not begin with a START marker: not a run file");
            ++m.records;
            if (rec.marker) {
                ++m.markers;
                if (rec.marker_type == MarkerType::start) {
                    m.start_seen = true;
                    start_coarse = rec.coarse;
                } else if (rec.marker_type == MarkerType::time) {
                    ++m.time_markers;
                } else if (rec.marker_type == MarkerType::overflow) {
                    ++m.overflow_markers;
                    m.dropped_marked += rec.info;
                } else if (rec.marker_type == MarkerType::end) {
                    m.end_seen = true;
                    m.ended_by_window = (rec.info & 1U) != 0;
                }
                last_marker_coarse = rec.coarse;
            }
            try {
                const auto e = unwrap.feed(rec);
                if (e) m.times_ps[static_cast<std::size_t>(e->channel)].push_back(std::llround(e->time_ps));
            } catch (const std::exception& bad) {
                m.warning = "decoding stopped at record " + std::to_string(m.records) + ": " + bad.what();
                stopped = true;
                break;
            }
        }
        if (progress) progress->store(static_cast<float>(static_cast<double>(m.records) / static_cast<double>(total)));
        if (cancel && cancel->load()) throw std::runtime_error("loading cancelled");
    }
    if (last_marker_coarse > start_coarse)
        m.run_s = static_cast<double>(last_marker_coarse - start_coarse) * m.settings.coarse_tick_ns * 1e-9;
    if (!m.end_seen && m.warning.empty()) m.warning = "no END marker: the run was cut short (link loss or reset)";
    return m;
}

HistogramResult compute_histogram(const Measurement& m, const HistogramRequest& q, std::atomic<float>* progress,
                                  const std::atomic<bool>* cancel) {
    if (q.start_channel < 0 || q.start_channel >= kChannels || q.stop_channel < 0 || q.stop_channel >= kChannels)
        throw ArgumentError("channels must be 1..4");
    if (q.binwidth_ps < 1 || q.binwidth_ps > 1'000'000'000'000LL) throw ArgumentError("bin width must be 1 ps ... 1 s");
    if (q.n_bins < 1 || q.n_bins > 10'000'000) throw ArgumentError("number of bins must be 1 ... 10,000,000");
    if (q.binwidth_ps > 4'000'000'000'000'000'000LL / q.n_bins) throw ArgumentError("histogram range too large");
    for (auto d : {q.start_delay_ps, q.stop_delay_ps})
        if (d < -1'000'000'000'000LL || d > 1'000'000'000'000LL) throw ArgumentError("START/STOP delays must be within +-1 s");
    const std::int64_t range = q.binwidth_ps * q.n_bins;
    const std::int64_t offset = q.stop_delay_ps - q.start_delay_ps;   // shown delay = STOP - START + offset
    const bool same = q.start_channel == q.stop_channel;

    auto sorted = [](std::vector<std::int64_t> v) {
        std::sort(v.begin(), v.end());
        return v;
    };
    const auto starts = sorted(m.times_ps[static_cast<std::size_t>(q.start_channel)]);
    const auto stops = same ? starts : sorted(m.times_ps[static_cast<std::size_t>(q.stop_channel)]);

    HistogramResult r;
    r.counts.assign(static_cast<std::size_t>(q.n_bins), 0);
    r.starts = starts.size();
    r.stops = stops.size();
    auto tick = [&](std::size_t i) {
        if ((i & 0xFFFFU) != 0) return false;
        if (progress) progress->store(static_cast<float>(static_cast<double>(i) / static_cast<double>(stops.size())));
        if (cancel && cancel->load()) {
            r.cancelled = true;
            return true;
        }
        return false;
    };

    if (q.mode == HistogramMode::histogram) {
        // Every (START, STOP) pair whose shifted delay p - s + offset lies in
        // [0, range): START times in (p + offset - range, p + offset].  Both
        // window edges only move forward as p increases.
        std::size_t lo = 0, hi = 0;
        std::uint64_t work = 0;
        for (std::size_t i = 0; i < stops.size(); ++i) {
            if (tick(i)) break;
            const std::int64_t p = stops[i];
            while (lo < starts.size() && starts[lo] <= p + offset - range) ++lo;
            while (hi < starts.size() && starts[hi] <= p + offset) ++hi;
            for (std::size_t k = lo; k < hi; ++k) {
                if (same && k == i) continue;   // a click never pairs with itself
                ++r.counts[static_cast<std::size_t>((p - starts[k] + offset) / q.binwidth_ps)];
                ++r.in_range;
            }
            work += hi > lo ? hi - lo : 0;
            if (work > kHistogramPairBudget) {
                r.too_many_pairs = true;
                break;
            }
        }
    } else {
        // StartStop: pairs made in time order (a STOP takes the latest START
        // before it, which is then used up; on one channel each click closes
        // the previous interval), then shifted by the offset.
        std::size_t k = 0;
        bool have = false;
        std::int64_t last = 0;
        for (std::size_t i = 0; i < stops.size(); ++i) {
            if (tick(i)) break;
            const std::int64_t p = stops[i];
            if (same) {
                if (i > 0) {
                    last = stops[i - 1];
                    have = true;
                }
            } else {
                while (k < starts.size() && starts[k] <= p) {
                    last = starts[k++];
                    have = true;
                }
            }
            if (!have) continue;
            const std::int64_t shifted = p - last + offset;
            if (shifted >= 0 && shifted < range) {
                ++r.counts[static_cast<std::size_t>(shifted / q.binwidth_ps)];
                ++r.in_range;
            } else {
                ++r.outside;
            }
            have = false;
        }
    }
    if (progress) progress->store(1.0F);
    return r;
}

}  // namespace tdcctl
