#include "file_backend.hpp"

#include "engine.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace dt {

namespace {

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

bool json_bool(const std::string& text, const std::string& key, bool fallback) {
    if (text.find("\"" + key + "\": true") != std::string::npos) return true;
    if (text.find("\"" + key + "\": false") != std::string::npos) return false;
    return fallback;
}

// "[a, b, c, d]" after the key.
bool json_array4(const std::string& text, const std::string& key, std::array<double, kNumChannels>& out) {
    const auto at = text.find("\"" + key + "\": [");
    if (at == std::string::npos) return false;
    std::istringstream in(text.substr(at + key.size() + 5));
    for (int i = 0; i < kNumChannels; ++i) {
        double v;
        char sep;
        if (!(in >> v)) return false;
        out[static_cast<std::size_t>(i)] = v;
        if (i + 1 < kNumChannels && !(in >> sep)) return false;
    }
    return true;
}

}  // namespace

SavedStreamInfo read_saved_stream(const std::string& path) {
    SavedStreamInfo s;
    s.path = path;
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error("cannot read " + path);
    const auto size = static_cast<std::uint64_t>(file.tellg());
    if (size == 0 || size % 8 != 0)
        throw std::runtime_error(path + " is not a tag-stream file (" + std::to_string(size) + " bytes, not a multiple of 8)");
    s.records = size / 8;
    file.seekg(0);
    unsigned char first[8];
    file.read(reinterpret_cast<char*>(first), 8);
    std::uint64_t raw = 0;
    for (int i = 0; i < 8; ++i) raw |= static_cast<std::uint64_t>(first[i]) << (8 * i);
    if ((raw >> 63) == 0 || ((raw >> 60) & 7U) != 0)
        throw std::runtime_error(path + " does not begin with a START marker: not a tag-stream file");

    std::ifstream side(path + ".json");
    if (!side) return s;
    const std::string text((std::istreambuf_iterator<char>(side)), std::istreambuf_iterator<char>());
    s.have_sidecar = true;
    s.design_version = static_cast<std::uint32_t>(json_number(text, "design_version").value_or(0));
    s.refclk_hz = json_number(text, "refclk_hz").value_or(s.refclk_hz);
    s.divisions = static_cast<std::uint32_t>(json_number(text, "divisions").value_or(s.divisions));
    s.ref_bits = static_cast<int>(json_number(text, "ref_bits").value_or(s.ref_bits));
    s.stop_bits = static_cast<int>(json_number(text, "stop_bits").value_or(s.stop_bits));
    s.coarse_tick_ns = json_number(text, "coarse_tick_ns").value_or(s.coarse_tick_ns);
    if (!(s.coarse_tick_ns > 0)) s.coarse_tick_ns = 10.0;
    s.lclkin_mhz = json_number(text, "lclkin_mhz").value_or(s.lclkin_mhz);
    if (!(s.lclkin_mhz > 0)) s.lclkin_mhz = 100.0;
    s.ddr = json_bool(text, "ddr", false);
    s.high_resolution = static_cast<int>(json_number(text, "high_resolution").value_or(0));
    s.record_format = static_cast<int>(json_number(text, "record_format").value_or(1));
    s.coarse_uncertainty_ticks = static_cast<std::uint64_t>(json_number(text, "coarse_uncertainty_ticks").value_or(0));
    s.seconds = json_number(text, "seconds").value_or(0);
    s.have_comparator = json_array4(text, "trigger_v", s.trigger_v) && json_array4(text, "hysteresis_ohm", s.hysteresis_ohm);
    s.start = json_string(text, "start");
    s.note = json_string(text, "note");
    return s;
}

FileBackend::FileBackend(const std::string& path) : info_(read_saved_stream(path)) {
    DecoderConfig dc;
    dc.divisions = info_.divisions;
    const double lsb = 1e12 / (info_.refclk_hz * static_cast<double>(info_.divisions));
    if (std::fabs(lsb - std::round(lsb)) > 1e-6 || lsb < 0.5)
        throw std::runtime_error(path + ": the GPX2 LSB is not a whole number of picoseconds");
    dc.lsb_ps = static_cast<timestamp_t>(std::llround(lsb));
    dc.ref_bits = info_.ref_bits;
    dc.stop_bits = info_.stop_bits;
    dc.coarse_ps = static_cast<timestamp_t>(std::llround(info_.coarse_tick_ns * 1000.0));
    // tdcctl::max_result_latency_us: the GPX2 FIFO full at the frame rate, plus 0.5 us.
    const int fifo = info_.high_resolution == 0 ? 16 : info_.high_resolution == 1 ? 8 : 4;
    const double bit_ns = 1000.0 / info_.lclkin_mhz / (info_.ddr ? 2.0 : 1.0);
    dc.latency_spread_ps =
        static_cast<timestamp_t>(((fifo + 1) * (info_.ref_bits + info_.stop_bits) * bit_ns * 1e-3 + 0.5) * 1e6);
    dc.coarse_uncertainty_ps = static_cast<timestamp_t>(info_.coarse_uncertainty_ticks) * dc.coarse_ps;
    decoder_.set_config(dc);
}

double FileBackend::current_trigger(int channel) {
    return info_.have_comparator ? info_.trigger_v[static_cast<std::size_t>(channel - 1)] : 0.0;
}

double FileBackend::current_hysteresis(int channel) {
    return info_.have_comparator ? info_.hysteresis_ohm[static_cast<std::size_t>(channel - 1)] : 0.0;
}

timestamp_t FileBackend::min_deadtime() const {
    return info_.high_resolution == 0 ? 20'000 : info_.high_resolution == 1 ? 50'000 : 100'000;
}

double FileBackend::progress() const {
    return info_.records ? static_cast<double>(done_.load()) / static_cast<double>(info_.records) : 1.0;
}

std::string FileBackend::error() const {
    std::lock_guard<std::mutex> lock(mu_);
    return error_;
}

std::string FileBackend::info() {
    std::ostringstream o;
    o << "saved tag stream " << info_.path << " (" << info_.records << " records";
    if (info_.have_sidecar)
        o << ", " << info_.ref_bits << " + " << info_.stop_bits << "-bit frames, coarse tick " << info_.coarse_tick_ns << " ns" << (info_.record_format == 2 ? ", sent as compact records" : "");
    o << ")";
    return o.str();
}

void FileBackend::run(Engine& engine, const std::function<bool()>& should_stop) {
    if (finished_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        return;
    }
    std::ifstream file(info_.path, std::ios::binary);
    if (!file) {
        std::lock_guard<std::mutex> lock(mu_);
        error_ = "cannot read " + info_.path;
        finished_.store(true);
        return;
    }
    RawChunk chunk;
    std::size_t chunk_events = 0;
    auto push = [&] {
        chunk.watermark = decoder_.watermark();
        engine.push(std::move(chunk));
        chunk = RawChunk{};
        chunk.watermark = decoder_.watermark();
        chunk_events = 0;
    };
    std::vector<unsigned char> buf(8 * 65536);
    std::uint64_t pos = 0;
    try {
        while (pos < info_.records && !should_stop()) {
            const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(65536, info_.records - pos));
            if (!file.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(n * 8)))
                throw std::runtime_error("read error in " + info_.path);
            for (std::size_t i = 0; i < n; ++i) {
                std::uint64_t raw = 0;
                for (int b = 0; b < 8; ++b) raw |= static_cast<std::uint64_t>(buf[i * 8 + static_cast<std::size_t>(b)]) << (8 * b);
                const bool start = (raw >> 63) != 0 && ((raw >> 60) & 7U) == 0;
                if (start) {
                    if (decoder_.run_started()) {   // the previous run of the recording ends here
                        chunk.run_end = true;
                        push();
                    }
                    decoder_.begin_run();
                }
                const auto before = decoder_.stats().events;
                decoder_.feed(raw, chunk);
                chunk_events += decoder_.stats().events - before;
                if (chunk.run_begin || chunk_events >= 32768) push();
            }
            pos += n;
            done_.store(pos);
        }
        if (decoder_.run_started()) {
            chunk.run_end = true;
            push();
        }
    } catch (const std::exception& e) {
        std::lock_guard<std::mutex> lock(mu_);
        error_ = std::string("bad tag-stream file: ") + e.what() + " (at record " + std::to_string(pos) + ")";
        if (decoder_.run_started()) {
            chunk.run_end = true;
            engine.push(std::move(chunk));
        }
    }
    finished_.store(true);
}

}  // namespace dt
