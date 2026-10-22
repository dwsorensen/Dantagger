#include "tdc_backend.hpp"

#include "engine.hpp"

#include "d3xx_common.hpp"
#include "d3xx_transport.hpp"
#include "tdcctl/fake_fpga.hpp"
#include "tdcctl/tdc_session.hpp"

#include <algorithm>
#include <atomic>
#include <pthread.h>
#include <thread>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>

namespace dt {

namespace {

using tdcctl::Status;

constexpr std::uint32_t kFtFlagsOpened = 1;   // FT_FLAGS_OPENED: another process has the device open

// The simulated board of this process; open backends share its ownership, so
// it outlives them also at process exit.
std::shared_ptr<tdcctl::FakeFpga>& shared_fake() {
    static std::shared_ptr<tdcctl::FakeFpga> fake;
    return fake;
}

struct LvdsCache {
    bool valid{false};
    int phase{0}, divider{0}, bit_phase{0};
    bool ddr{true};                    // capture format of that calibration (absent in older files: DDR)
    std::array<int, kNumChannels> sdo{}, frame{};
};

std::string join(const std::array<int, kNumChannels>& v) {
    std::ostringstream o;
    for (int i = 0; i < kNumChannels; ++i) o << (i ? ", " : "") << v[static_cast<std::size_t>(i)];
    return o.str();
}

LvdsCache read_cache(const std::string& path) {
    LvdsCache c;
    std::ifstream in(path);
    if (!in) return c;
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();
    auto get_int = [&](const char* key, int& out) {
        std::smatch m;
        if (!std::regex_search(text, m, std::regex(std::string("\"") + key + "\"\\s*:\\s*(-?\\d+)"))) return false;
        out = std::stoi(m[1]);
        return true;
    };
    auto get_arr = [&](const char* key, std::array<int, kNumChannels>& out) {
        std::smatch m;
        if (!std::regex_search(text, m, std::regex(std::string("\"") + key + "\"\\s*:\\s*\\[([^\\]]*)\\]"))) return false;
        std::stringstream items(m[1].str());
        std::string item;
        int i = 0;
        while (std::getline(items, item, ',') && i < kNumChannels) out[static_cast<std::size_t>(i++)] = std::stoi(item);
        return i == kNumChannels;
    };
    c.valid = get_int("phase_step", c.phase) && get_int("divider_code", c.divider) && get_int("bit_phase", c.bit_phase) &&
              get_arr("sdo_taps", c.sdo) && get_arr("frame_taps", c.frame);
    int ddr = 1;
    if (get_int("ddr", ddr)) c.ddr = ddr != 0;
    return c;
}

std::array<int, kNumChannels> taps(const std::array<std::uint8_t, kNumChannels>& t) {
    std::array<int, kNumChannels> out{};
    for (int i = 0; i < kNumChannels; ++i) out[static_cast<std::size_t>(i)] = t[static_cast<std::size_t>(i)];
    return out;
}

void write_cache(const std::string& path, const tdcctl::LvdsState& lv, const std::string& note) {
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp);
        out << "{\n  \"phase_step\": " << lv.phase_step() << ",\n  \"divider_code\": " << lv.divider_code()
            << ",\n  \"bit_phase\": " << lv.bit_phase() << ",\n  \"sdo_taps\": [" << join(taps(lv.sdo_taps))
            << "],\n  \"frame_taps\": [" << join(taps(lv.frame_taps)) << "],\n  \"ddr\": " << (lv.ddr ? 1 : 0)
            << ",\n  \"design_version\": " << lv.timing.design
            << ",\n  \"note\": \"" << note << "\"\n}\n";
    }
    std::filesystem::rename(tmp, path);
}

std::string now_text() {
    const auto t = std::time(nullptr);
    char buf[64];
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", std::localtime(&t));
    return buf;
}

void require_ok(const tdcctl::Response& r, const std::string& what) {
    if (r.status != Status::ok) throw DeviceError(what + " failed: " + tdcctl::status_name(r.status));
}

bool calibrated_per_cache(const tdcctl::Response& st, const LvdsCache& cache) {
    const auto& lv = st.lvds;
    return cache.valid && lv.mmcm_locked() && lv.idelay_ready() && lv.phase_step() == cache.phase &&
           lv.divider_code() == cache.divider && lv.bit_phase() == cache.bit_phase && taps(lv.sdo_taps) == cache.sdo &&
           taps(lv.frame_taps) == cache.frame && (cache.phase != 0 || st.gpx.configured());
}

int resolution_factor(int code) { return code == 1 ? 2 : code == 2 ? 4 : 1; }

}  // namespace

// ---------------------------------------------------------------------------
// The receive -> decode queue: blocks of raw records, bounded in bytes.
// ---------------------------------------------------------------------------
RecordQueue::RecordQueue(std::size_t budget_bytes) : budget_(budget_bytes) {}

void RecordQueue::push(std::vector<std::uint64_t>&& block) {
    const std::size_t bytes = block.size() * sizeof(std::uint64_t);
    std::unique_lock<std::mutex> lock(mu_);
    space_.wait(lock, [&] { return closed_ || bytes_ < budget_ || blocks_.empty(); });
    if (closed_) return;
    bytes_ += bytes;
    peak_ = std::max(peak_, bytes_);
    blocks_.push_back(std::move(block));
    ready_.notify_one();
}

bool RecordQueue::pop(std::vector<std::uint64_t>& block) {
    std::unique_lock<std::mutex> lock(mu_);
    ready_.wait(lock, [&] { return closed_ || !blocks_.empty(); });
    if (blocks_.empty()) return false;   // closed and drained
    block = std::move(blocks_.front());
    blocks_.pop_front();
    bytes_ -= block.size() * sizeof(std::uint64_t);
    space_.notify_one();
    return true;
}

void RecordQueue::close() {
    std::lock_guard<std::mutex> lock(mu_);
    closed_ = true;
    ready_.notify_all();
    space_.notify_all();
}

// ---------------------------------------------------------------------------
// Frame policy and descriptions
// ---------------------------------------------------------------------------
tdcctl::GpxConfig policy_gpx_config(const tdcctl::LinkTiming& timing, double refclk_hz, int high_resolution,
                                    int ref_bits_override, int stop_bits_override, bool ddr) {
    tdcctl::GpxConfig c;
    c.channel_mask = 0xF;
    c.lvds_out = true;
    c.high_resolution = high_resolution;
    c.refclk_divisions = tdcctl::GpxConfig::divisions_for(refclk_hz, 1.0);
    if (timing.release2()) {
        c.ddr = ddr;
        c.stop_bits = stop_bits_override ? stop_bits_override : tdcctl::min_stop_bits(c.refclk_divisions);
        c.ref_index_bits = ref_bits_override ? ref_bits_override : 24;
        if (c.stop_bits == 0) throw tdcctl::ArgumentError("the LSB is too fine for 20 stop bits");
    } else {
        c.ddr = false;
        c.ref_index_bits = ref_bits_override ? ref_bits_override : 24;
        c.stop_bits = stop_bits_override ? stop_bits_override : 20;
    }
    (void)c.bytes();   // validates
    return c;
}

std::string release_name(std::uint32_t design_version) {
    char buf[96];
    if (design_version == tdcctl::kDesignVersionR2) std::snprintf(buf, sizeof buf, "Release 2 (design 0x%08X)", design_version);
    else if (design_version == tdcctl::kDesignVersionR1) std::snprintf(buf, sizeof buf, "Release 1 (design 0x%08X)", design_version);
    else std::snprintf(buf, sizeof buf, "unknown design 0x%08X", design_version);
    return buf;
}

double readout_limit_of(const tdcctl::Response& st) {
    const auto& l = st.lvds;
    if (!l.format_valid || l.ref_bits + l.stop_bits <= 0) return 0.0;
    return 1e9 / ((l.ref_bits + l.stop_bits) * l.timing.bit_ns(l.divider_code(), l.ddr));
}

std::string describe_capture(const tdcctl::Response& st) {
    const auto& l = st.lvds;
    if (!l.format_valid) return "GPX2 not configured (no capture format)";
    char buf[200];
    std::snprintf(buf, sizeof buf, "%d + %d-bit frames (%d bits), %s at LCLKIN %.4g MHz: at most %.2f M events/s per channel",
                  l.ref_bits, l.stop_bits, l.ref_bits + l.stop_bits, l.ddr ? "DDR" : "SDR", l.lclkin_mhz(),
                  readout_limit_of(st) * 1e-6);
    return buf;
}

// ---------------------------------------------------------------------------
// Open / close
// ---------------------------------------------------------------------------
void TdcBackend::simulate_unplug() { shared_fake().reset(); }

tdcctl::FakeFpga& TdcBackend::simulated_fpga() {
    auto& f = shared_fake();
    if (!f) f = std::make_shared<tdcctl::FakeFpga>();
    return *f;
}

TdcBackend::TdcBackend(const TdcOptions& options)
    : refclk_hz_(options.refclk_hz), ref_override_(options.ref_index_bits), stop_override_(options.stop_bits) {
    if (!options.state_dir.empty()) {
        state_dir_ = options.state_dir;
    } else if (const char* x = std::getenv("XDG_STATE_HOME"); x && *x) {
        state_dir_ = std::string(x) + "/dantagger";
    } else {
        const char* home = std::getenv("HOME");
        state_dir_ = std::string(home ? home : ".") + "/.local/state/dantagger";
    }

    const auto first_txn = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                                          std::chrono::system_clock::now().time_since_epoch())
                                                          .count())
                           << 8U;
    if (options.simulate_fpga) {
        simulated_fpga();
        fake_owner_ = shared_fake();
        fake_ = fake_owner_.get();
        serial_ = "SIMULATED-FPGA";
        usb_speed_ = "simulated";
        session_ = std::make_unique<tdcctl::Session>(*fake_, tdcctl::SessionOptions{}, first_txn);
    } else {
        const auto devices = au2ft601::d3xx::enumerate_devices();
        if (options.serial.empty()) {
            if (devices.empty()) throw DeviceError("no TDC board found (no FT601 on USB); is the Ft+ USB cable connected?");
            if (devices.size() > 1) throw DeviceError("more than one FT601 found; pass the serial of the TDC board");
            serial_ = devices[0].serial;
        } else {
            serial_ = options.serial;
        }
        const std::string busy_hint =
            ". Only one program can use the TDC board at a time: close the other one (the Dantagger app, or a Python "
            "script using Dantagger) and try again.";
        for (const auto& d : devices)
            if (d.serial == serial_ && (d.flags & kFtFlagsOpened) != 0)
                throw DeviceError("the TDC board (FT601 " + serial_ + ") is busy: another program has it open" + busy_hint);
        int urbs = options.usb_urb_count, queue = options.usb_queue_buffers;
        if (const char* env = std::getenv("DANTAGGER_USB"); env && *env) {
            if (std::sscanf(env, "%d,%d", &urbs, &queue) != 2)
                throw DeviceError(std::string("DANTAGGER_USB must be URBS,QUEUE (e.g. 64,4096; 0,0 = the library defaults), not ") + env);
        }
        auto open_with = [&](int u, int q) {
            au2ft601::D3xxTransportOptions transport_options;
            // Refuses an FT601 that is not in the recorded 245-mode, one-channel,
            // 66.67 MHz configuration before any byte is sent (as tdcctl).
            transport_options.expected_fifo_clock = au2ft601::FifoClock::mhz66;
            transport_options.in_urb_buffer_size = static_cast<std::uint32_t>(au2ft601::kFrameSize);
            transport_options.in_urb_count = static_cast<std::uint8_t>(std::clamp(u, 0, 255));
            transport_options.in_urb_buffer_count = static_cast<std::uint16_t>(std::clamp(q, 0, 65535));
            d3xx_ = std::make_unique<au2ft601::D3xxTransport>(transport_options);
            d3xx_->open_by_serial(serial_);
        };
        try {
            try {
                open_with(urbs, queue);
            } catch (const std::exception& e) {
                const std::string why = e.what();
                if ((urbs == 0 && queue == 0) || (why.find("URB") == std::string::npos && why.find("TransferParams") == std::string::npos))
                    throw;
                d3xx_.reset();
                usb_note_ = "the driver refused " + std::to_string(urbs) + " URBs / " + std::to_string(queue) +
                            " queue buffers (" + why + "); using the library defaults";
                open_with(0, 0);
            }
        } catch (const std::exception& e) {
            d3xx_.reset();
            throw DeviceError(std::string("cannot open the TDC board (FT601 ") + serial_ + "): " + e.what() +
                              ". If another program has it open, that is the reason" + busy_hint);
        }
        {   // what is in effect, from the transport's report
            const std::string& info = d3xx_->session_info();
            std::smatch m;
            std::string effect = "USB buffering unknown";
            if (std::regex_search(info, m, std::regex(R"(URB buffer (\d+) B, URB count (\d+), URB buffer count (\d+))")))
                effect = m[2].str() + " x " + std::to_string(std::stoi(m[1].str()) / 1024) + " KiB in flight, " + m[3].str() +
                         " x " + std::to_string(std::stoi(m[1].str()) / 1024) + " KiB library queue";
            usb_note_ = usb_note_.empty() ? effect : effect + " (" + usb_note_ + ")";
        }
        const auto speed = au2ft601::d3xx::usb_speed_for_serial(serial_);
        usb_speed_ = speed ? *speed + " Mb/s" : "unknown speed";
        session_ = std::make_unique<tdcctl::Session>(*d3xx_, tdcctl::SessionOptions{}, first_txn);
    }
    try {
        note_response(session_->sync().status);   // ends a run an earlier process left behind
        lifecycle(options);
    } catch (...) {
        session_.reset();
        if (d3xx_) d3xx_->close();
        throw;
    }
    open_ = true;
}

TdcBackend::~TdcBackend() { close(); }

void TdcBackend::close() {
    if (!open_) return;
    open_ = false;
    try {
        lclkin_off();
    } catch (...) {
    }
    session_.reset();
    if (d3xx_) d3xx_->close();
    d3xx_.reset();
}

std::string TdcBackend::model() const {
    return "Dantagger TDC (TDC-GPX2, " + release_name(last_status().design_version) + ")";
}

// One state file per firmware release (power cycles bring back release 1 from
// flash while release 2 is tested from RAM: neither may overwrite the other's
// calibration).  Release 1 keeps SDE/Dantagger's name.
std::string TdcBackend::state_path() const {
    const bool r2 = last_status().design_version == tdcctl::kDesignVersionR2;
    return state_dir_ + "/" + serial_ + (r2 ? ".release2.json" : ".json");
}

tdcctl::Response TdcBackend::last_status() const {
    std::lock_guard<std::mutex> lock(state_mu_);
    return last_;
}

void TdcBackend::note_response(const tdcctl::Response& r) {
    std::lock_guard<std::mutex> lock(state_mu_);
    last_ = r;
}

tdcctl::Response TdcBackend::refresh() {
    const auto r = session_->status();
    note_response(r);
    return r;
}

tdcctl::GpxConfig TdcBackend::policy_config() {
    const auto st = last_status();
    const auto cache = read_cache(state_path());   // release 2: SDR or DDR as the last calibration
    try {
        return policy_gpx_config(st.lvds.timing, refclk_hz_, high_resolution_, ref_override_, stop_override_,
                                 cache.valid ? cache.ddr : true);
    } catch (const std::exception& e) {
        throw DeviceError(std::string("GPX2 frame: ") + e.what());
    }
}

void TdcBackend::configure_gpx(const tdcctl::GpxConfig& config) {
    const auto r = session_->gpx_configure(config.bytes());
    note_response(r);
    if (r.status != Status::ok)
        throw DeviceError("GPX_CONFIGURE failed: " + tdcctl::status_name(r.status) + ", " +
                          tdcctl::describe_gpx_error(r.gpx.last_error));
    if (!r.gpx.verified() || !r.gpx.measuring()) throw DeviceError("the GPX2 configuration did not verify");
}

void TdcBackend::lclkin_off() {
    const auto st = refresh();
    if (!st.lvds.lclkin_enabled()) return;
    const auto r = session_->lvds_set(false, st.lvds.divider_code(), st.lvds.bit_phase(), st.lvds.sdo_taps, st.lvds.frame_taps);
    note_response(r);
    require_ok(r, "LVDS_SET (LCLKIN off)");
}

bool TdcBackend::set_up() {
    const auto st = last_status();
    return calibrated_per_cache(st, read_cache(state_path())) && st.gpx.verified() && st.gpx.measuring() &&
           !st.gpx.test_pattern() && st.lvds.format_valid && tdcctl::capture_problem(st, refclk_hz_).empty();
}

void TdcBackend::record_calibration(const std::string& note) {
    const auto st = refresh();
    write_cache(state_path(), st.lvds, note + " " + now_text());
}

std::string TdcBackend::run_setup(bool force_calibrate) {
    const auto st = refresh();
    const auto cache = read_cache(state_path());
    const bool calibrated = !force_calibrate && calibrated_per_cache(st, cache);
    const auto config = policy_config();
    const auto wanted = config.bytes();
    const bool configured = st.gpx.configured() && st.gpx.verified() && st.gpx.measuring() && !st.gpx.test_pattern() &&
                            st.gpx.written == wanted && st.lvds.format_valid;
    std::string message;
    if (!calibrated) {
        tdcctl::CalibrationOptions co;
        // The LCLKIN rate of the last successful calibration (a fallback chosen in the app stays chosen).
        co.divider_code = cache.valid && st.lvds.timing.release2() ? std::clamp(cache.divider, 0, 3) : 0;
        const auto result = session_->calibrate(config, co);
        refresh();
        if (!result.success) throw DeviceError("LVDS calibration of the TDC board failed: " + result.message);
        record_calibration("calibrated by Dantagger");
        setup_action_ = "calibrated";
        char buf[200];
        std::snprintf(buf, sizeof buf, "LCLKIN %.4g MHz, bit phase %d, phase step %d, error-free window %.0f ps of %.0f ps",
                      st.lvds.timing.lclkin_mhz(co.divider_code), result.chosen_bit_phase, result.chosen_phase,
                      result.good_width_ps, result.bit_ps);
        message = "TDC-GPX2 configured (" + tdcctl::describe_gpx_config(wanted) + "), LVDS calibrated (" + buf + ")";
    } else if (!configured) {
        configure_gpx(config);
        setup_action_ = "configured";
        message = "board still calibrated (phase step " + std::to_string(st.lvds.phase_step()) +
                  "); TDC-GPX2 configuration applied";
    } else {
        setup_action_ = "skipped";
        message = "board already configured and calibrated (phase step " + std::to_string(st.lvds.phase_step()) +
                  "): setup skipped";
    }
    lclkin_off();
    return message;
}

void TdcBackend::lifecycle(const TdcOptions& options) {
    if (options.setup == SetupMode::none) {
        lclkin_off();
        setup_action_ = "none";
        setup_message_ = set_up() ? "board configured and calibrated" : "board not set up (configure and calibrate it)";
        return;
    }
    const auto first = run_setup(options.recalibrate);
    setup_message_ = setup_action_ == "calibrated" ? "first open since the board was plugged in: " + first : first;
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------
double TdcBackend::set_trigger(int channel, double volts) {
    std::uint16_t code;
    try {
        code = tdcctl::threshold_code(volts);
    } catch (const tdcctl::ArgumentError& e) {
        throw std::invalid_argument(e.what());
    }
    std::array<std::uint16_t, kNumChannels> codes{};
    codes[static_cast<std::size_t>(channel - 1)] = code;
    const auto r = session_->thresholds(1U << (channel - 1), codes);
    note_response(r);
    require_ok(r, "SET_THRESHOLDS");
    return tdcctl::threshold_volts(r.board.dac_codes[static_cast<std::size_t>(channel - 1)]);
}

double TdcBackend::set_hysteresis(int channel, double ohms) {
    std::uint16_t wiper;
    try {
        wiper = tdcctl::wiper_code(ohms);
    } catch (const tdcctl::ArgumentError& e) {
        throw std::invalid_argument(e.what());
    }
    std::array<std::uint16_t, kNumChannels> wipers{};
    wipers[static_cast<std::size_t>(channel - 1)] = wiper;
    const auto r = session_->hysteresis(1U << (channel - 1), wipers);
    note_response(r);
    require_ok(r, "SET_HYSTERESIS");
    return tdcctl::wiper_ohms(r.board.wiper(channel - 1));
}

double TdcBackend::current_trigger(int channel) {
    return tdcctl::threshold_volts(last_status().board.dac_codes[static_cast<std::size_t>(channel - 1)]);
}

double TdcBackend::current_hysteresis(int channel) { return tdcctl::wiper_ohms(last_status().board.wiper(channel - 1)); }

int TdcBackend::current_resolution() { return resolution_factor(last_status().gpx.written[1] >> 6U); }

void TdcBackend::set_resolution(int factor) {
    high_resolution_ = factor == 1 ? 0 : factor == 2 ? 1 : 2;
    configure_gpx(policy_config());
}

timestamp_t TdcBackend::min_deadtime() const {
    // GPX2 minimum pulse-to-pulse spacing tPPS (datasheet v1-03).
    const int code = last_status().gpx.written[1] >> 6U;
    return code == 0 ? 20'000 : code == 1 ? 50'000 : 100'000;
}

double TdcBackend::readout_limit() const { return readout_limit_of(last_status()); }

std::string TdcBackend::info() {
    const auto st = last_status();
    std::ostringstream o;
    o << "TDC board " << serial_ << " (USB " << usb_speed_ << "), " << release_name(st.design_version) << "\n";
    if (d3xx_) o << "  USB: " << usb_note_ << "\n";
    o << "  setup: " << setup_message_ << "\n";
    o << "  GPX2: " << tdcctl::describe_gpx_config(st.gpx.written) << "\n";
    o << "  capture: " << describe_capture(st) << "\n";
    o << "  clock ratio " << decoder_.ratio() << " (+-" << decoder_.ratio_uncertainty() << ")\n";
    const auto& s = decoder_.stats();
    o << "  decoded " << s.events << " events, " << s.markers << " markers, FPGA dropped " << s.dropped
      << ", re-anchors " << s.reanchors << ", corrupt " << s.corrupt << "\n";
    return o.str();
}

// ---------------------------------------------------------------------------
// Taps
// ---------------------------------------------------------------------------
void TdcBackend::add_tap(const std::shared_ptr<RecordTap>& tap) {
    std::lock_guard<std::mutex> lock(tap_mu_);
    taps_.push_back({tap, false});
}

void TdcBackend::remove_tap(const RecordTap* tap) {
    std::lock_guard<std::mutex> lock(tap_mu_);
    taps_.erase(std::remove_if(taps_.begin(), taps_.end(), [&](const TapEntry& e) { return e.tap.get() == tap; }),
                taps_.end());
}

RunInfo TdcBackend::make_run_info(const tdcctl::Response& st) const {
    RunInfo i;
    i.design_version = st.design_version;
    i.refclk_hz = refclk_hz_;
    i.divisions = tdcctl::gpx_divisions(st.gpx.written);
    i.ref_bits = st.lvds.ref_bits;
    i.stop_bits = st.lvds.stop_bits;
    i.ddr = st.lvds.ddr;
    i.high_resolution = st.gpx.written[1] >> 6U;
    i.coarse_tick_ns = st.lvds.timing.coarse_tick_ns;
    i.lclkin_mhz = st.lvds.lclkin_mhz();
    i.bit_phase = st.lvds.bit_phase();
    i.lvds_phase = st.lvds.phase_step();
    i.gpx_registers = st.gpx.written;
    for (int c = 0; c < kNumChannels; ++c) {
        i.trigger_v[static_cast<std::size_t>(c)] = tdcctl::threshold_volts(st.board.dac_codes[static_cast<std::size_t>(c)]);
        i.hysteresis_ohm[static_cast<std::size_t>(c)] = tdcctl::wiper_ohms(st.board.wiper(c));
    }
    return i;
}

void TdcBackend::flush_taps(bool final_flush) {
    std::lock_guard<std::mutex> lock(tap_mu_);
    if (!tap_buf_.empty()) {
        const bool starts_run = (tap_buf_.front() >> 63) != 0 && ((tap_buf_.front() >> 60) & 7U) == 0;
        const std::uint64_t synthetic_start = (std::uint64_t{1} << 63) | (tap_buf_coarse_ & ((std::uint64_t{1} << 48) - 1));
        for (auto& e : taps_) {
            if (!e.joined) {
                e.tap->run_started(run_info_);
                if (!starts_run) e.tap->records(&synthetic_start, 1);
                e.joined = true;
            }
            e.tap->records(tap_buf_.data(), tap_buf_.size());
        }
        tap_buf_.clear();
    }
    (void)final_flush;
}

// ---------------------------------------------------------------------------
// Streaming
// ---------------------------------------------------------------------------
void TdcBackend::run(Engine& engine, const std::function<bool()>& should_stop) {
    if (!open_) throw DeviceError("the TDC board is closed");
    const auto st = refresh();
    if (!st.gpx.verified() || !st.gpx.measuring() || !st.lvds.format_valid)
        throw DeviceError("the TDC-GPX2 is not configured and calibrated: set the board up first (the app's Setup "
                          "button, or reopen it from Python)");
    if (st.gpx.test_pattern()) throw DeviceError("the TDC-GPX2 is sending its LVDS test pattern: configure it without");
    if (const auto problem = tdcctl::capture_problem(st, refclk_hz_); !problem.empty()) throw DeviceError(problem);
    {
        DecoderConfig dc;
        dc.divisions = tdcctl::gpx_divisions(st.gpx.written);
        const double lsb = 1e12 / (refclk_hz_ * static_cast<double>(dc.divisions));
        if (std::fabs(lsb - std::round(lsb)) > 1e-6 || lsb < 0.5)
            throw DeviceError("the GPX2 LSB is not a whole number of picoseconds (" + std::to_string(lsb) + " ps)");
        dc.lsb_ps = static_cast<timestamp_t>(std::llround(lsb));
        dc.ref_bits = st.lvds.ref_bits;
        dc.stop_bits = st.lvds.stop_bits;
        dc.coarse_ps = static_cast<timestamp_t>(std::llround(st.lvds.timing.coarse_tick_ns * 1000.0));
        dc.latency_spread_ps = static_cast<timestamp_t>(
            std::llround(tdcctl::max_result_latency_us(st.lvds.timing, st.lvds.divider_code(), st.lvds.ddr,
                                                       dc.ref_bits + dc.stop_bits, st.gpx.written[1] >> 6U) * 1e6));
        decoder_.set_config(dc);
    }
    engine.set_saturation_limit(readout_limit_of(st));
    {
        std::lock_guard<std::mutex> lock(tap_mu_);
        run_info_ = make_run_info(st);
        in_run_ = true;
        for (auto& e : taps_) e.joined = false;
    }
    tap_buf_.clear();

    tdcctl::LclkinScope lclk(*session_, true);
    decoder_.begin_run();
    const auto dropped_before = decoder_.stats().dropped;
    const std::uint64_t push_ticks = static_cast<std::uint64_t>(1e6 / st.lvds.timing.coarse_tick_ns);   // 1 ms

    // Two threads (user, 2026-10-22): this one only receives (USB reads, frame
    // checks) and hands each read's records to the decode thread, which runs
    // the taps, the record decoder and the chunks for the engine (whose own
    // thread merges and measures).  A slow decoder fills the queue first; only
    // a full queue (256 MB) makes the receiver wait.
    RecordQueue queue(kRecordQueueBytes);
    std::atomic<bool> decode_failed{false};
    std::string decode_error;
    std::thread decode_thread([&] {
        set_thread_name("dt-decode");
        RawChunk chunk;
        std::size_t chunk_events = 0;
        std::uint64_t last_push_coarse = 0;
        bool have_taps = false;
        auto push = [&] {
            chunk.watermark = decoder_.watermark();
            engine.push(std::move(chunk));
            chunk = RawChunk{};
            chunk.watermark = decoder_.watermark();
            chunk_events = 0;
            last_push_coarse = decoder_.coarse();
            {
                std::lock_guard<std::mutex> lock(tap_mu_);
                have_taps = !taps_.empty();
            }
            if (!tap_buf_.empty()) flush_taps(false);
        };
        {
            std::lock_guard<std::mutex> lock(tap_mu_);
            have_taps = !taps_.empty();
        }
        std::vector<std::uint64_t> block;
        while (queue.pop(block)) {
            if (decode_failed.load(std::memory_order_relaxed)) continue;   // drain: the receiver is stopping the run
            for (const std::uint64_t raw : block) {
                if (have_taps) {
                    if (tap_buf_.empty()) tap_buf_coarse_ = decoder_.coarse();
                    tap_buf_.push_back(raw);
                }
                const auto before = decoder_.stats().events;
                try {
                    decoder_.feed(raw, chunk);
                } catch (const std::exception& e) {
                    decode_error = e.what();
                    decode_failed.store(true);
                    break;
                }
                chunk_events += decoder_.stats().events - before;
                if (chunk.run_begin || chunk_events >= 32768 || decoder_.coarse() - last_push_coarse >= push_ticks) push();
            }
        }
        if (decoder_.run_started()) {
            chunk.run_end = true;
            chunk.watermark = decoder_.watermark();
            engine.push(std::move(chunk));
        }
        flush_taps(true);
    });

    std::vector<std::uint64_t> block;
    auto sink = [&](const std::uint64_t* raw, std::size_t n) { block.insert(block.end(), raw, raw + n); };
    std::atomic<bool> abort{false};
    auto progress = [&](const tdcctl::StreamProgress&) {   // after every USB read
        if (!block.empty()) {
            queue.push(std::move(block));
            block = {};
        }
        if (should_stop() || decode_failed.load()) abort.store(true);
    };
    tdcctl::StreamRequest request;
    tdcctl::StreamResult result;
    try {
        result = session_->stream_batches(request, sink, progress, &abort);
    } catch (...) {
        queue.close();
        decode_thread.join();
        throw;
    }
    if (!block.empty()) queue.push(std::move(block));
    queue.close();
    decode_thread.join();
    RunEnd end;
    end.pass = result.pass;
    end.failure = result.failure;
    end.records = result.records;
    if (result.final_response) {
        note_response(*result.final_response);
        end.fpga_dropped = result.final_response->stream.dropped;
    }
    {
        std::lock_guard<std::mutex> lock(tap_mu_);
        for (auto& e : taps_)
            if (e.joined) {
                e.tap->run_ended(end);
                e.joined = false;
            }
        in_run_ = false;
    }
    lclk.off();
    refresh();
    if (result.final_response) {
        const std::uint64_t fpga_dropped = result.final_response->stream.dropped;
        const std::uint64_t marked = decoder_.stats().dropped - dropped_before;
        if (fpga_dropped > marked)
            engine.log().push("overflow", "the FPGA reports " + std::to_string(fpga_dropped) +
                                              " events dropped in this run, more than its OVERFLOW markers counted (" +
                                              std::to_string(marked) + "; markers saturate at 4095 each)");
    }
    if (!decode_error.empty()) throw DeviceError("bad record stream from the TDC board: " + decode_error);
    if (!result.pass) throw DeviceError("streaming from the TDC board failed: " + result.failure);
}

}  // namespace dt
