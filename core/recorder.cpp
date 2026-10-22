#include "recorder.hpp"

#include <cstdio>
#include <ctime>
#include <filesystem>
#include <stdexcept>

namespace dt {

namespace {

constexpr std::uint64_t kMask48 = (std::uint64_t{1} << 48) - 1;

std::uint64_t end_marker(std::uint64_t coarse) { return (std::uint64_t{1} << 63) | (std::uint64_t{3} << 60) | (coarse & kMask48); }

std::string iso_now() {
    char buf[32];
    const std::time_t t = std::time(nullptr);
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", std::localtime(&t));
    return buf;
}

std::string hex(const std::array<std::uint8_t, 17>& b) {
    std::string s;
    char x[4];
    for (std::size_t i = 0; i < b.size(); ++i) {
        std::snprintf(x, sizeof x, "%02X", b[i]);
        if (i) s += ' ';
        s += x;
    }
    return s;
}

std::string escape(const std::string& in) {
    std::string out;
    for (char c : in) {
        if (c == '"' || c == '\\') out += '\\';
        out += static_cast<unsigned char>(c) < 0x20 ? ' ' : c;
    }
    return out;
}

template <class T>
std::string array4(const std::array<T, kNumChannels>& v, const char* fmt) {
    std::string s = "[";
    char buf[48];
    for (int i = 0; i < kNumChannels; ++i) {
        std::snprintf(buf, sizeof buf, fmt, v[static_cast<std::size_t>(i)]);
        s += (i ? ", " : "") + std::string(buf);
    }
    return s + "]";
}

}  // namespace

StreamRecorder::StreamRecorder(Options options) : opt_(std::move(options)) {
    const auto dir = std::filesystem::path(opt_.path).parent_path();
    if (!dir.empty()) std::filesystem::create_directories(dir);
    file_.open(opt_.path, std::ios::binary | std::ios::trunc);
    if (!file_) throw std::runtime_error("cannot create " + opt_.path);
    p_.path = opt_.path;
    started_at_ = iso_now();
    thread_ = std::thread([this] { writer(); });
}

StreamRecorder::~StreamRecorder() {
    try {
        finish();
    } catch (...) {
    }
}

void StreamRecorder::append_locked(std::uint64_t raw) {
    if (pending_.size() >= kMaxBuffered) {
        ++p_.lost;
        return;
    }
    pending_.push_back(raw);
    ++p_.records;
}

void StreamRecorder::run_started(const RunInfo& info) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!accepting_) return;
    runs_.push_back(info);
    ++p_.runs;
    in_run_ = true;
    end_written_ = false;
    tick_ns_ = info.coarse_tick_ns;
    run_start_coarse_ = 0;   // set by the run's START marker
}

void StreamRecorder::records(const std::uint64_t* raw, std::size_t n) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!accepting_ || !in_run_) return;
    const double limit = opt_.max_seconds;
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint64_t r = raw[i];
        if ((r >> 63) != 0) {
            const unsigned type = static_cast<unsigned>((r >> 60) & 7U);
            const std::uint64_t c48 = r & kMask48;
            if (type == 0) {
                coarse_ = c48;
                run_start_coarse_ = c48;
            } else if (c48 + 64 >= coarse_) {
                coarse_ = c48;
            }
            if (type == 2) dropped_marked_ += (r >> 48) & 0xFFFU;
            if (type == 3) end_written_ = true;
        } else {
            const auto low = static_cast<std::uint16_t>((r >> 44) & 0xFFFFU);
            const auto diff = static_cast<std::int16_t>(static_cast<std::uint16_t>(low - static_cast<std::uint16_t>(coarse_ & 0xFFFFU)));
            coarse_ = static_cast<std::uint64_t>(static_cast<std::int64_t>(coarse_) + diff);
        }
        p_.seconds = seconds_before_ + static_cast<double>(coarse_ - run_start_coarse_) * tick_ns_ * 1e-9;
        if (limit > 0 && p_.seconds >= limit) {
            if (!end_written_) append_locked(end_marker(coarse_));
            end_written_ = true;
            p_.limit_reached = true;
            accepting_ = false;
            break;
        }
        if ((r >> 63) == 0) {
            const int c = static_cast<int>((r >> 61) & 3U);
            if (((opt_.channel_mask >> c) & 1U) == 0) continue;
            ++p_.events[static_cast<std::size_t>(c)];
        }
        append_locked(r);
    }
    cv_.notify_one();
}

void StreamRecorder::run_ended(const RunEnd& end) {
    std::lock_guard<std::mutex> lock(mu_);
    p_.fpga_dropped += end.fpga_dropped;
    if (!in_run_) return;
    if (accepting_ && !end_written_) append_locked(end_marker(coarse_));
    end_written_ = true;
    in_run_ = false;
    seconds_before_ = p_.seconds;
    if (!end.pass && !end.failure.empty() && p_.error.empty()) p_.error = "run failed: " + end.failure;
}

void StreamRecorder::writer() {
    std::vector<std::uint64_t> batch;
    std::vector<char> bytes;
    while (true) {
        {
            std::unique_lock<std::mutex> lock(mu_);
            cv_.wait(lock, [&] { return quit_ || !pending_.empty(); });
            if (pending_.empty() && quit_) return;
            batch.swap(pending_);
        }
        bytes.resize(batch.size() * 8);
        for (std::size_t i = 0; i < batch.size(); ++i)
            for (int b = 0; b < 8; ++b) bytes[i * 8 + static_cast<std::size_t>(b)] = static_cast<char>((batch[i] >> (8 * b)) & 0xFFU);
        file_.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        const bool ok = static_cast<bool>(file_);
        {
            std::lock_guard<std::mutex> lock(mu_);
            p_.bytes_written += ok ? bytes.size() : 0;
            if (!ok && p_.error.empty()) p_.error = "write error on " + opt_.path + " (disk full?)";
        }
        batch.clear();
    }
}

void StreamRecorder::finish() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (p_.finished) return;
        if (in_run_ && accepting_ && !end_written_) append_locked(end_marker(coarse_));
        accepting_ = false;
        quit_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    file_.close();
    {
        std::lock_guard<std::mutex> lock(mu_);
        p_.finished = true;
    }
    write_sidecar();
}

StreamRecorder::Progress StreamRecorder::progress() const {
    std::lock_guard<std::mutex> lock(mu_);
    return p_;
}

bool StreamRecorder::limit_reached() const {
    std::lock_guard<std::mutex> lock(mu_);
    return p_.limit_reached;
}

void StreamRecorder::write_sidecar() {
    std::lock_guard<std::mutex> lock(mu_);
    std::ofstream side(opt_.path + ".json");
    const RunInfo first = runs_.empty() ? RunInfo{} : runs_.front();
    std::array<unsigned long long, kNumChannels> events{};
    for (int c = 0; c < kNumChannels; ++c) events[static_cast<std::size_t>(c)] = p_.events[static_cast<std::size_t>(c)];
    char num[64];
    auto f = [&](double v) {
        std::snprintf(num, sizeof num, "%.10g", v);
        return std::string(num);
    };
    side << "{\n  \"tool\": \"dantagger\",\n  \"design_version\": " << first.design_version << ",\n  \"start\": \"" << started_at_
         << "\",\n  \"note\": \"" << escape(opt_.note) << "\",\n  \"refclk_hz\": " << f(first.refclk_hz)
         << ",\n  \"divisions\": " << first.divisions << ",\n  \"ref_bits\": " << first.ref_bits
         << ",\n  \"stop_bits\": " << first.stop_bits << ",\n  \"coarse_tick_ns\": " << f(first.coarse_tick_ns)
         << ",\n  \"lclkin_mhz\": " << f(first.lclkin_mhz) << ",\n  \"ddr\": " << (first.ddr ? "true" : "false")
         << ",\n  \"high_resolution\": " << first.high_resolution << ",\n  \"bit_phase\": " << first.bit_phase
         << ",\n  \"gpx_registers\": \"" << hex(first.gpx_registers) << "\",\n  \"lvds_phase\": " << first.lvds_phase
         << ",\n  \"trigger_v\": " << array4(first.trigger_v, "%.5f") << ",\n  \"hysteresis_ohm\": "
         << array4(first.hysteresis_ohm, "%.1f") << ",\n  \"channel_mask\": " << opt_.channel_mask
         << ",\n  \"records\": " << p_.records << ",\n  \"events\": " << array4(events, "%llu")
         << ",\n  \"dropped_marked\": " << dropped_marked_ << ",\n  \"fpga_dropped\": " << p_.fpga_dropped
         << ",\n  \"lost_on_host\": " << p_.lost << ",\n  \"seconds\": " << f(p_.seconds)
         << ",\n  \"limit_reached\": " << (p_.limit_reached ? "true" : "false") << ",\n  \"runs\": [";
    for (std::size_t i = 0; i < runs_.size(); ++i)
        side << (i ? "," : "") << "\n    {\"run_id\": " << runs_[i].run_id << ", \"trigger_v\": " << array4(runs_[i].trigger_v, "%.5f")
             << ", \"hysteresis_ohm\": " << array4(runs_[i].hysteresis_ohm, "%.1f") << "}";
    side << "\n  ],\n  \"pass\": " << (p_.error.empty() && p_.lost == 0 ? "true" : "false") << ",\n  \"failure\": \""
         << escape(p_.error) << "\"\n}\n";
}

}  // namespace dt
