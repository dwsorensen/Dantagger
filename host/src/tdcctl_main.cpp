// tdcctl: host tool for the TDC firmware (PROTOCOL.md; release 1 = design
// 0x54440001 and release 2 = 0x54440002, told apart at connection)
// through the Ft+ (FT601).  One-shot commands, or `serve` for the GUI (one
// command per stdin line, one JSON event per stdout line).
//
// Comparator thresholds are limited for safety to -0.100 .. +1.500 V
// (nominal); there is no override (unchanged from cmpctl).

#include "d3xx_common.hpp"
#include "d3xx_transport.hpp"
#include "tdcctl/fake_fpga.hpp"
#include "tdcctl/run_file.hpp"
#include "tdcctl/tdc_protocol.hpp"
#include "tdcctl/tdc_session.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <deque>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace tdcctl;

namespace {

const char* kUsage =
    "usage: tdcctl [options] COMMAND [ARGS]\n"
    "\n"
    "Configuration mode:\n"
    "  list                          FT601 devices attached\n"
    "  status                        state as last read back by the FPGA (board, TMP117, GPX2, LVDS, run)\n"
    "  readback | probe              comparator board and TMP117: read everything again | rerun start-up\n"
    "  threshold CH VOLTS            comparator threshold, -0.100 .. +1.500 V (limited for safety)\n"
    "  threshold-code CH CODE        the same as a DAC code, 11 .. 41953\n"
    "  hysteresis CH OHMS            hysteresis rheostat, 75 .. 5075 ohm (nominal)\n"
    "  hysteresis-code CH CODE       the same as a wiper code, 0 .. 256\n"
    "  led1 R G B [MA] | led1 off    comparator LED1 (current 5, 10 or 15 mA)\n"
    "  led2 [r][g][b] | off          comparator LED2\n"
    "  leds MASK                     the eight Au LEDs (0..255); status LEDs until set\n"
    "  gpx-config [GPX OPTIONS]      reset, configure, read back, start the TDC-GPX2\n"
    "  gpx-readback | gpx-reset | gpx-init | gpx-results\n"
    "  lvds on|off [--div N] [--bit-phase P] [--sdo-taps a,b,c,d] [--frame-taps a,b,c,d]\n"
    "                                LCLKIN = maximum / 2^N (release 2: 250/125/62.5/31.25 MHz,\n"
    "                                release 1: 100/50/25/12.5 MHz) and sampling settings\n"
    "  phase STEP                    sampling phase (17.857 ps steps; release 2 0..223, release 1 0..559)\n"
    "  link-test [CYCLES]            count LVDS frames for a window (default 1 ms)\n"
    "  calibrate [GPX OPTIONS] [--div N] [--step N] [--window-us U]\n"
    "                                find the LVDS sampling point (bit phase and phase) with the\n"
    "                                GPX2 test pattern; LCLKIN divider N (default 0: fastest)\n"
    "Streaming mode:\n"
    "  stream --out FILE [--duration S | --window-us N] [--refclk-hz F]\n"
    "                                stream events to FILE (raw 8-byte records) and FILE.json\n"
    "  count [SECONDS]               count rate per channel: a run of SECONDS (default 1,\n"
    "                                timed by the FPGA window), no file written\n"
    "  stop                          end a run left running\n"
    "Offline:\n"
    "  decode FILE [--csv OUT] [--refclk-hz F] [--test-pattern]\n"
    "                                records -> CSV (channel, time_ps, ...); --test-pattern counts\n"
    "                                events that differ from the GPX2 LVDS test pattern\n"
    "  serve                         read commands from stdin, print JSON events (for the GUI)\n"
    "\n"
    "GPX OPTIONS: --refclk-hz F (default 8e6) --lsb-ps P (default 1) --ref-bits N --stop-bits N\n"
    "             --ddr | --sdr --channels 1234 --test-pattern --hires 0|1|2\n"
    "             --dv-adjust -160|0|160|320 --lvds-off\n"
    "             Release 2 defaults: DDR, the smallest stop width that holds the LSB (18 bits\n"
    "             at 1 ps) and the smallest reference index that keeps times unambiguous at the\n"
    "             current LCLKIN (6 bits at 250 MHz DDR).  Release 1: SDR, 24 + 20 bits.\n"
    "CH is 1-4, a list such as 1,3, or all.\n"
    "\n"
    "Options:\n"
    "  --serial S      FT601 serial number (default: the only FT601 attached)\n"
    "  --simulate      use a built-in software FPGA\n"
    "  --json          print JSON events instead of text\n"
    "  -h, --help      this help\n";

class Json {
public:
    explicit Json(const std::string& event) { text_ = "{\"event\":\"" + event + "\""; }
    Json& add(const std::string& key, const std::string& value) {
        text_ += ",\"" + key + "\":\"" + escape(value) + "\"";
        return *this;
    }
    Json& add(const std::string& key, const char* value) { return add(key, std::string(value)); }
    Json& add(const std::string& key, double value) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.9g", value);
        text_ += ",\"" + key + "\":" + buf;
        return *this;
    }
    Json& add(const std::string& key, std::uint64_t value) {
        text_ += ",\"" + key + "\":" + std::to_string(value);
        return *this;
    }
    Json& add(const std::string& key, std::uint32_t value) { return add(key, static_cast<std::uint64_t>(value)); }
    Json& add(const std::string& key, int value) {
        text_ += ",\"" + key + "\":" + std::to_string(value);
        return *this;
    }
    Json& add(const std::string& key, bool value) {
        text_ += ",\"" + key + "\":" + (value ? "true" : "false");
        return *this;
    }
    Json& raw(const std::string& key, const std::string& json) {
        text_ += ",\"" + key + "\":" + json;
        return *this;
    }
    std::string str() const { return text_ + "}"; }
    static std::string escape(const std::string& in) {
        std::string out;
        for (char c : in) {
            if (c == '"' || c == '\\') { out += '\\'; out += c; }
            else if (c == '\n') out += "\\n";
            else if (static_cast<unsigned char>(c) < 0x20) out += ' ';
            else out += c;
        }
        return out;
    }

private:
    std::string text_;
};

template <typename T, std::size_t N>
std::string json_array(const std::array<T, N>& values) {
    std::string s = "[";
    for (std::size_t i = 0; i < N; ++i) s += (i ? "," : "") + std::to_string(values[i]);
    return s + "]";
}

std::string fixed(double value, int decimals) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*f", decimals, value);
    return buf;
}

std::string hex(unsigned v, int digits) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%0*X", digits, v);
    return buf;
}

struct Options {
    std::string serial;
    bool simulate{false};
    bool json{false};
};

unsigned parse_channels(const std::string& text) {
    if (text == "all") return 0xF;
    unsigned mask = 0;
    std::string digits = text;
    for (char& c : digits) if (c == ',') c = ' ';
    std::stringstream ss(digits);
    std::string item;
    while (ss >> item) {
        for (char c : item) {
            if (c < '1' || c > '4') throw ArgumentError("channels must be 1-4, a list such as 1,3, or all (got '" + text + "')");
            mask |= 1U << static_cast<unsigned>(c - '1');
        }
    }
    if (mask == 0) throw ArgumentError("no channel given");
    return mask;
}

double parse_number(const std::string& text, const std::string& what) {
    std::size_t used = 0;
    double value = 0;
    try {
        value = std::stod(text, &used);
    } catch (...) {
        used = 0;
    }
    if (used != text.size() || !std::isfinite(value)) throw ArgumentError("bad " + what + " '" + text + "'");
    return value;
}

long parse_integer(const std::string& text, const std::string& what, long min, long max) {
    std::size_t used = 0;
    long value = 0;
    try {
        value = std::stol(text, &used, 0);
    } catch (...) {
        used = 0;
    }
    if (used != text.size() || value < min || value > max)
        throw ArgumentError(what + " must be " + std::to_string(min) + ".." + std::to_string(max) + " (got '" + text + "')");
    return value;
}

std::array<std::uint8_t, kChannels> parse_taps(const std::string& text) {
    std::array<std::uint8_t, kChannels> taps{};
    std::string t = text;
    for (char& c : t) if (c == ',') c = ' ';
    std::stringstream ss(t);
    for (std::size_t i = 0; i < kChannels; ++i) {
        std::string item;
        if (!(ss >> item)) throw ArgumentError("taps need four values a,b,c,d");
        taps[i] = static_cast<std::uint8_t>(parse_integer(item, "IDELAY tap", 0, 31));
    }
    return taps;
}

// Removes "--name value" (or a bare "--name" flag) from words.
class Args {
public:
    explicit Args(std::vector<std::string> words) : w_(std::move(words)) {}
    std::optional<std::string> value(const std::string& name) {
        for (std::size_t i = 1; i < w_.size(); ++i) {
            if (w_[i] == name) {
                if (i + 1 >= w_.size()) throw ArgumentError(name + " needs a value");
                auto v = w_[i + 1];
                w_.erase(w_.begin() + static_cast<std::ptrdiff_t>(i), w_.begin() + static_cast<std::ptrdiff_t>(i + 2));
                return v;
            }
        }
        return std::nullopt;
    }
    bool flag(const std::string& name) {
        for (std::size_t i = 1; i < w_.size(); ++i) {
            if (w_[i] == name) {
                w_.erase(w_.begin() + static_cast<std::ptrdiff_t>(i));
                return true;
            }
        }
        return false;
    }
    const std::vector<std::string>& rest() const { return w_; }
    void done() const {
        for (std::size_t i = 1; i < w_.size(); ++i)
            if (w_[i].rfind("--", 0) == 0) throw ArgumentError("unknown option " + w_[i]);
    }

private:
    std::vector<std::string> w_;
};

// The GPX options shared by gpx-config, calibrate (and remembered for stream).
struct GpxChoice {
    GpxConfig config;
    double refclk_hz{8.0e6};
};

// `st` (a STATUS of the connected image) gives the release and the LCLKIN
// divider the automatic reference-index width is chosen for; `div` overrides
// that divider (calibrate --div).
GpxChoice gpx_options(Args& a, const Response& st, std::optional<int> div = std::nullopt) {
    GpxChoice g;
    const auto& t = st.lvds.timing;
    double lsb_ps = 1.0;
    std::optional<int> ref_bits, stop_bits;
    if (auto v = a.value("--refclk-hz")) g.refclk_hz = parse_number(*v, "REFCLK frequency");
    if (auto v = a.value("--lsb-ps")) lsb_ps = parse_number(*v, "LSB");
    if (auto v = a.value("--ref-bits")) ref_bits = static_cast<int>(parse_integer(*v, "reference-index bits", 0, 24));
    if (auto v = a.value("--stop-bits")) stop_bits = static_cast<int>(parse_integer(*v, "stop bits", 14, 20));
    if (auto v = a.value("--channels")) g.config.channel_mask = parse_channels(*v);
    if (auto v = a.value("--hires")) g.config.high_resolution = static_cast<int>(parse_integer(*v, "high resolution", 0, 2));
    if (auto v = a.value("--dv-adjust")) g.config.data_valid_adjust_ps = static_cast<int>(parse_integer(*v, "data-valid adjust", -160, 320));
    g.config.test_pattern = a.flag("--test-pattern");
    if (a.flag("--lvds-off")) g.config.lvds_out = false;
    const bool want_ddr = a.flag("--ddr"), want_sdr = a.flag("--sdr");
    if (want_ddr && want_sdr) throw ArgumentError("--ddr and --sdr exclude each other");
    if (want_ddr && !t.ddr_supported()) throw ArgumentError("DDR needs TDC firmware release 2 (this image is release 1)");
    g.config.ddr = t.ddr_supported() && !want_sdr;
    g.config.refclk_divisions = GpxConfig::divisions_for(g.refclk_hz, lsb_ps);
    if (t.release2()) {
        g.config.stop_bits = stop_bits.value_or(min_stop_bits(g.config.refclk_divisions));
        if (g.config.stop_bits == 0) throw ArgumentError("the LSB is too fine for 20 stop bits");
        const int d = div.value_or(st.lvds.divider_code());
        const int auto_ref = min_ref_index_bits(t, d, g.config.ddr, g.config.stop_bits, g.config.high_resolution, g.refclk_hz);
        if (auto_ref == 0) throw ArgumentError("no reference-index width keeps times unambiguous at this LCLKIN");
        g.config.ref_index_bits = ref_bits.value_or(auto_ref);
    } else {
        g.config.ref_index_bits = ref_bits.value_or(24);
        g.config.stop_bits = stop_bits.value_or(20);
    }
    (void)g.config.bytes();   // validates
    return g;
}

// ---- Minimal sidecar JSON reading (numbers only) ---------------------------------
std::optional<double> json_number(const std::string& text, const std::string& key) {
    const auto at = text.find("\"" + key + "\":");
    if (at == std::string::npos) return std::nullopt;
    try {
        return std::stod(text.substr(at + key.size() + 3));
    } catch (...) {
        return std::nullopt;
    }
}

// ---- Line queue for `serve` (a STOP can arrive while a run streams) --------
class LineQueue {
public:
    void start() {
        reader_ = std::thread([this] {
            std::string line;
            while (std::getline(std::cin, line)) push(line);
            push("\x04");   // end of input
        });
        reader_.detach();
    }
    void push(const std::string& line) {
        std::lock_guard<std::mutex> lock(mutex_);
        lines_.push_back(line);
        cv_.notify_one();
    }
    std::string pop() {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return !lines_.empty(); });
        auto line = lines_.front();
        lines_.pop_front();
        return line;
    }
    // Puts lines back at the front, in order.
    void push_front(const std::vector<std::string>& lines) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = lines.rbegin(); it != lines.rend(); ++it) lines_.push_front(*it);
        if (!lines.empty()) cv_.notify_one();
    }
    std::optional<std::string> try_pop() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (lines_.empty()) return std::nullopt;
        auto line = lines_.front();
        lines_.pop_front();
        return line;
    }

private:
    std::thread reader_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::string> lines_;
};

class App {
public:
    explicit App(Options options) : options_(std::move(options)) {}
    ~App() { disconnect(); }

    int run_one(const std::vector<std::string>& words) {
        if (words[0] == "list") return list();
        if (words[0] == "decode") return execute(words) ? 0 : 1;
        if (!connect(false)) return 1;
        return execute(words) ? 0 : 1;
    }

    int serve() {
        options_.json = true;
        serving_ = true;
        queue_.start();
        connect();
        while (true) {
            const auto line = queue_.pop();
            if (line == "\x04") break;
            std::vector<std::string> words;
            std::stringstream ss(line);
            std::string w;
            while (ss >> w) words.push_back(w);
            if (words.empty()) continue;
            if (words[0] == "quit" || words[0] == "exit") break;
            if (words[0] == "connect") {
                connect();
                continue;
            }
            if (words[0] != "decode" && !session_ && !connect()) continue;
            execute(words);
        }
        // The GUI is closing: leave LCLKIN off (it may have been switched on by hand).
        if (session_) {
            try {
                if (session_->status().lvds.lclkin_enabled()) LclkinScope(*session_, false).off();
            } catch (const std::exception&) {
            }
        }
        return 0;
    }

private:
    Options options_;
    bool serving_{false};
    LineQueue queue_;
    std::unique_ptr<au2ft601::D3xxTransport> d3xx_;
    std::unique_ptr<FakeFpga> fake_;
    std::unique_ptr<Session> session_;

    void say(const std::string& text) {
        if (!options_.json) std::cout << text << std::endl;
    }
    void emit(const Json& json) {
        if (options_.json) std::cout << json.str() << std::endl;
    }
    void error(const std::string& text) {
        if (options_.json) emit(Json("error").add("message", text));
        else std::cerr << "error: " << text << std::endl;
    }

    int list() {
        if (options_.simulate) {
            say("SIMULATED  simulated TDC firmware");
            emit(Json("devices").raw("serials", "[\"SIMULATED\"]"));
            return 0;
        }
        try {
            const auto devices = au2ft601::d3xx::enumerate_devices();
            if (devices.empty()) say("no FT601 devices found");
            std::string serials = "[";
            for (const auto& device : devices) {
                const auto speed = au2ft601::d3xx::usb_speed_for_serial(device.serial);
                say(device.serial + "  " + device.description + "  USB " + (speed ? *speed + " Mb/s" : "?"));
                serials += (serials.size() > 1 ? ",\"" : "\"") + Json::escape(device.serial) + "\"";
            }
            emit(Json("devices").raw("serials", serials + "]"));
            return 0;
        } catch (const std::exception& failure) {
            error(failure.what());
            return 1;
        }
    }

    void disconnect() {
        session_.reset();
        if (d3xx_) d3xx_->close();
        d3xx_.reset();
        fake_.reset();
    }

    bool connect(bool show_state = true) {
        disconnect();
        std::string serial = "SIMULATED";
        std::string usb = "simulated";
        try {
            const auto first = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                                              std::chrono::system_clock::now().time_since_epoch())
                                                              .count())
                               << 8U;
            if (options_.simulate) {
                fake_ = std::make_unique<FakeFpga>();
                fake_->realtime = serving_;
                session_ = std::make_unique<Session>(*fake_, SessionOptions{}, first);
            } else {
                if (options_.serial.empty()) {
                    const auto devices = au2ft601::d3xx::enumerate_devices();
                    if (devices.size() != 1)
                        throw au2ft601::DeviceError(devices.empty() ? "no FT601 found; is the Ft+ USB cable connected?"
                                                                    : "more than one FT601 found; choose one with --serial");
                    serial = devices[0].serial;
                } else {
                    serial = options_.serial;
                }
                au2ft601::D3xxTransportOptions transport_options;
                // Refuses an FT601 that is not in the recorded 245-mode,
                // one-channel, 66.67 MHz configuration before any byte is sent.
                transport_options.expected_fifo_clock = au2ft601::FifoClock::mhz66;
                transport_options.in_urb_buffer_size = static_cast<std::uint32_t>(au2ft601::kFrameSize);
                d3xx_ = std::make_unique<au2ft601::D3xxTransport>(transport_options);
                d3xx_->open_by_serial(serial);
                const auto speed = au2ft601::d3xx::usb_speed_for_serial(serial);
                usb = speed ? *speed + " Mb/s" : "unknown speed";
                session_ = std::make_unique<Session>(*d3xx_, SessionOptions{}, first);
            }
            const auto report = session_->sync();
            const auto& t = report.status.lvds.timing;
            say("Connected to FT601 " + serial + " (USB " + usb + "); TDC firmware " + hex(report.status.design_version, 8) +
                (t.release2() ? " (release 2, LCLKIN up to " + fixed(t.lclk_max_mhz, 0) + " MHz, SDR/DDR)" : " (release 1)"));
            if (report.stale_frames != 0 || report.discarded_bytes != 0 || report.attempts > 1)
                say("  cleared leftovers from an earlier session: " + std::to_string(report.stale_frames) + " frames, " +
                    std::to_string(report.discarded_bytes) + " stray bytes");
            emit(Json("connected").add("serial", serial).add("usb", usb)
                     .add("threshold_min_v", kThresholdMinVolts).add("threshold_max_v", kThresholdMaxVolts)
                     .add("stale_frames", static_cast<std::uint64_t>(report.stale_frames)));
            if (show_state || options_.json) show(report.status);
            return true;
        } catch (const std::exception& failure) {
            error(std::string("could not connect: ") + failure.what());
            disconnect();
            if (options_.json) emit(Json("disconnected"));
            return false;
        }
    }

    // Prints/emits a response; returns true if its status is OK.
    // After an LVDS operation: LCLKIN is off again.  The GUI gets the new
    // state; the terminal gets one line.
    void lclkin_off(LclkinScope& scope) {
        const auto r = scope.off();
        if (!r) return;
        if (serving_) show(*r);
        else say("LCLKIN off (high impedance) again");
    }

    bool show(const Response& r) {
        const auto& s = r.board;
        const auto& g = r.gpx;
        const auto& l = r.lvds;
        const auto& st = r.stream;
        if (options_.json) {
            std::string dac = "[", dacv = "[", wip = "[", wipo = "[";
            for (int ch = 0; ch < kChannels; ++ch) {
                const auto code = s.dac_codes[static_cast<std::size_t>(ch)];
                const auto w = s.wiper(ch);
                const char* sep = ch ? "," : "";
                dac += sep + std::to_string(code);
                dacv += sep + fixed(threshold_volts(code), 5);
                wip += sep + std::to_string(w);
                wipo += sep + fixed(wiper_ohms(w), 1);
            }
            std::string link = "[";
            for (std::size_t c = 0; c < kChannels; ++c) {
                const auto& lc = l.link[c];
                link += std::string(c ? "," : "") + "[" + std::to_string(lc.frames) + "," + std::to_string(lc.good) + "," +
                        std::to_string(lc.bad_pattern) + "," + std::to_string(lc.structure_errors) + "," +
                        std::to_string(lc.sdo_ones) + "," + std::to_string(lc.frame_ones) + "]";
            }
            emit(Json("state").add("command", opcode_name(r.opcode)).add("status", status_name(r.status))
                     .add("ok", r.status == Status::ok)
                     .raw("dac_codes", dac + "]").raw("thresholds_v", dacv + "]")
                     .raw("wiper_codes", wip + "]").raw("hysteresis_ohms", wipo + "]")
                     .add("led1_red", static_cast<int>(s.aw_pwm[0])).add("led1_green", static_cast<int>(s.aw_pwm[1]))
                     .add("led1_blue", static_cast<int>(s.aw_pwm[2]))
                     .add("led1_current_ma", s.led1_current() * kLed1CurrentStepMilliamps).add("led1_on", s.led1_enabled())
                     .add("led2_mask", static_cast<int>(s.led2_mask()))
                     .add("au_led_mask", r.au_led_mask).add("au_led_set", s.led_mask_set())
                     .add("boot_done", s.boot_done()).add("dac_ok", s.dac_ok()).add("gpio_ok", s.gpio_ok())
                     .add("u3_ok", s.u3_ok()).add("u8_ok", s.u8_ok()).add("aw_ok", s.aw_ok()).add("tmp_ok", s.tmp_ok())
                     .add("first_error", describe_first_error(s.first_error))
                     .add("error_count", s.error_count).add("faults", describe_faults(r.fault_bits))
                     .add("temperature_c", r.tmp.celsius()).add("tmp_id_ok", r.tmp.id_ok())
                     .add("tmp_polls_ok", static_cast<int>(r.tmp.polls_ok)).add("tmp_polls_failed", static_cast<int>(r.tmp.polls_failed))
                     .add("gpx_configured", g.configured()).add("gpx_verified", g.verified()).add("gpx_measuring", g.measuring())
                     .add("gpx_parity_pin", g.parity_pin()).add("gpx_parity_readback", g.parity_readback())
                     .add("gpx_test_pattern", g.test_pattern()).add("gpx_mismatch", g.mismatch)
                     .add("gpx_written", hex_bytes(g.written.data(), kGpxRegisters))
                     .add("gpx_readback", hex_bytes(g.readback.data(), kGpxRegisters))
                     .add("gpx_error", describe_gpx_error(g.last_error))
                     .add("gpx_config", describe_gpx_config(g.written))
                     .add("design_version", r.design_version).add("release2", l.timing.release2())
                     .add("lclk_max_mhz", l.timing.lclk_max_mhz).add("phase_steps", l.timing.phase_steps)
                     .add("coarse_tick_ns", l.timing.coarse_tick_ns).add("ddr", l.ddr)
                     .add("lclkin_on", l.lclkin_enabled()).add("lclkin_mhz", l.lclkin_mhz())
                     .add("divider", l.divider_code()).add("bit_phase", l.bit_phase()).add("phase", l.phase_step())
                     .add("mmcm_locked", l.mmcm_locked()).add("idelay_ready", l.idelay_ready())
                     .add("format_valid", l.format_valid).add("ref_bits", l.ref_bits).add("stop_bits", l.stop_bits)
                     .raw("sdo_taps", json_array(l.sdo_taps)).raw("frame_taps", json_array(l.frame_taps))
                     .add("link_window", l.link_window).raw("link", link + "]")
                     .raw("lclkout_hist", json_array(l.lclkout_hist))
                     .add("streaming", st.streaming()).add("capture_active", st.capture_active())
                     .add("run_records", st.records).add("run_frames", st.frames).add("run_dropped", st.dropped)
                     .add("run_captured", st.captured).add("fifo_max", st.fifo_max));
        } else {
            say(opcode_name(r.opcode) + ": " + status_name(r.status));
            for (int ch = 0; ch < kChannels; ++ch) {
                const auto code = s.dac_codes[static_cast<std::size_t>(ch)];
                const auto w = s.wiper(ch);
                say("  comparator ch" + std::to_string(ch + 1) + ": threshold " + fixed(threshold_volts(code), 4) +
                    " V (code " + std::to_string(code) + "), hysteresis " + fixed(wiper_ohms(w), 0) + " ohm (wiper " +
                    std::to_string(w) + ")");
            }
            say("  LED1: " + (s.led1_enabled() ? "R" + std::to_string(s.aw_pwm[0]) + " G" + std::to_string(s.aw_pwm[1]) +
                                                     " B" + std::to_string(s.aw_pwm[2]) + " at " +
                                                     std::to_string(s.led1_current() * kLed1CurrentStepMilliamps) + " mA"
                                               : std::string("off")) +
                "   LED2: " + (s.led2_mask() ? std::string(s.led2_mask() & 1 ? "red " : "") +
                                                    (s.led2_mask() & 2 ? "green " : "") + (s.led2_mask() & 4 ? "blue" : "")
                                              : std::string("off")));
            say(std::string("  devices: DAC ") + (s.dac_ok() ? "OK" : "FAIL") + ", MCP23S08 " + (s.gpio_ok() ? "OK" : "FAIL") +
                ", U3 " + (s.u3_ok() ? "OK" : "FAIL") + ", U8 " + (s.u8_ok() ? "OK" : "FAIL") + ", AW2013 " +
                (s.aw_ok() ? "OK" : "FAIL") + ", TMP117 " + (s.tmp_ok() ? "OK" : "FAIL") +
                (s.boot_done() ? "" : "  (boot not finished)"));
            say("  temperature: " + fixed(r.tmp.celsius(), 3) + " C (TMP117 ID " + hex(r.tmp.device_id, 4) + ", polls " +
                std::to_string(r.tmp.polls_ok) + " ok / " + std::to_string(r.tmp.polls_failed) + " failed)");
            if (s.first_error != 0) say("  last board error: " + describe_first_error(s.first_error));
            say(std::string("  GPX2: ") + (g.configured() ? "configured" : "not configured") +
                (g.verified() ? ", readback verified" : "") + (g.measuring() ? ", measuring" : "") +
                (g.test_pattern() ? ", TEST PATTERN" : "") + "; PARITY pin " + (g.parity_pin() ? "1" : "0") +
                " (readback parity " + (g.parity_readback() ? "1" : "0") + (g.parity_pin() == g.parity_readback() ? ", agrees" : ", DIFFERS") +
                "); INTERRUPT " + (g.interrupt_low() ? "low" : "high"));
            say("    written:  " + hex_bytes(g.written.data(), kGpxRegisters));
            say("    readback: " + hex_bytes(g.readback.data(), kGpxRegisters) +
                (g.mismatch ? "  (mismatch mask " + hex(g.mismatch, 5) + ")" : ""));
            if (g.configured()) say("    " + describe_gpx_config(g.written));
            if (g.last_error != 0) say("    last GPX2 error: " + describe_gpx_error(g.last_error));
            say(std::string("  LVDS: LCLKIN ") + (l.lclkin_enabled() ? fixed(l.lclkin_mhz(), 2) + " MHz" : std::string("off (high impedance)")) +
                " (divider " + std::to_string(l.divider_code()) + ": " + fixed(l.lclkin_mhz(), 2) + " MHz), bit phase " +
                std::to_string(l.bit_phase()) + ", sampling phase " + std::to_string(l.phase_step()) +
                " (" + fixed(l.phase_step() * kPhaseStepPs, 0) + " ps); MMCM " + (l.mmcm_locked() ? "locked" : "NOT locked") +
                ", IDELAYCTRL " + (l.idelay_ready() ? "ready" : "NOT ready") +
                (l.format_valid ? "; frames " + std::to_string(l.ref_bits) + "+" + std::to_string(l.stop_bits) + " bits " +
                                      (l.ddr ? "DDR" : "SDR") + " (" + std::to_string(l.samples_per_bit()) + " samples per bit)"
                                : "; no capture format"));
            if (l.link_window != 0) {
                say("    last link test (" + std::to_string(l.link_window) + " cycles):  frames / good / bad / structure / SDO ones / FRAME ones");
                for (std::size_t c = 0; c < kChannels; ++c) {
                    const auto& lc = l.link[c];
                    say("      ch" + std::to_string(c + 1) + ": " + std::to_string(lc.frames) + " / " + std::to_string(lc.good) +
                        " / " + std::to_string(lc.bad_pattern) + " / " + std::to_string(lc.structure_errors) + " / " +
                        std::to_string(lc.sdo_ones) + " / " + std::to_string(lc.frame_ones));
                }
                std::string h;
                const int slots = l.timing.release2() ? 8 : (1 << l.divider_code());
                for (int s8 = 0; s8 < slots; ++s8) h += " " + std::to_string(l.lclkout_hist[static_cast<std::size_t>(s8)]);
                say("      LCLKOUT ones per sample slot:" + h);
            }
            say(std::string("  run: ") + (st.streaming() ? "STREAMING" : "stopped") + ", records " +
                std::to_string(st.records) + " in " + std::to_string(st.frames) + " frames, events " +
                std::to_string(st.captured) + ", dropped " + std::to_string(st.dropped) + ", FIFO max " +
                std::to_string(st.fifo_max));
            if (r.fault_bits != 0) say("  faults: " + describe_faults(r.fault_bits));
        }
        if (r.status != Status::ok) {
            std::string why;
            if (r.status == Status::device_error)
                why = (r.opcode >= 0x30 && r.opcode <= 0x34) ? describe_gpx_error(g.last_error) : describe_first_error(s.first_error);
            error(opcode_name(r.opcode) + " returned " + status_name(r.status) + (why.empty() ? "" : ": " + why));
            return false;
        }
        return true;
    }

    bool calibrate(Args& a) {
        CalibrationOptions o;
        if (auto v = a.value("--step")) o.step = static_cast<int>(parse_integer(*v, "step", 1, 560));
        if (auto v = a.value("--window-us")) o.window_us = static_cast<std::uint32_t>(parse_integer(*v, "window", 10, 100000));
        if (auto v = a.value("--div")) o.divider_code = static_cast<int>(parse_integer(*v, "divider code", 0, 3));
        const auto st = session_->status();
        auto g = gpx_options(a, st, o.divider_code);
        o.channel_mask = g.config.channel_mask;
        a.done();
        const auto& t = st.lvds.timing;
        say("calibrating: GPX2 test pattern, LCLKIN " + fixed(t.lclkin_mhz(o.divider_code), 2) + " MHz " +
            (g.config.ddr ? "DDR" : "SDR") + ", " + std::to_string(t.samples_per_bit(o.divider_code, g.config.ddr)) +
            " samples per bit, " + std::to_string(o.window_us) + " us per point; frames " +
            std::to_string(g.config.ref_index_bits) + "+" + std::to_string(g.config.stop_bits) + " bits");
        say("  bp phase  ps      ch1 good/bad/struct     ch2               ch3               ch4");
        LclkinScope lclk(*session_, false);   // calibrate() switches LCLKIN on itself
        const auto result = session_->calibrate(g.config, o, [&](const CalibrationPoint& p) {
            std::string line = "  " + std::to_string(p.bit_phase);
            while (line.size() < 5) line += ' ';
            line += std::to_string(p.phase);
            while (line.size() < 11) line += ' ';
            line += fixed(p.offset_ps, 0);
            while (line.size() < 19) line += ' ';
            for (std::size_t c = 0; c < kChannels; ++c) {
                const auto& l = p.link[c];
                std::string cell = std::to_string(l.good) + "/" + std::to_string(l.bad_pattern) + "/" + std::to_string(l.structure_errors);
                while (cell.size() < 18) cell += ' ';
                line += cell;
            }
            say(line + (p.good ? "ok" : "--"));
            emit(Json("calibration_point").add("bit_phase", p.bit_phase).add("phase", p.phase)
                     .add("offset_ps", p.offset_ps).add("good", p.good)
                     .raw("good_frames", "[" + std::to_string(p.link[0].good) + "," + std::to_string(p.link[1].good) + "," +
                                             std::to_string(p.link[2].good) + "," + std::to_string(p.link[3].good) + "]")
                     .raw("errors", "[" + std::to_string(p.link[0].bad_pattern + p.link[0].structure_errors) + "," +
                                        std::to_string(p.link[1].bad_pattern + p.link[1].structure_errors) + "," +
                                        std::to_string(p.link[2].bad_pattern + p.link[2].structure_errors) + "," +
                                        std::to_string(p.link[3].bad_pattern + p.link[3].structure_errors) + "]"));
        });
        say(std::string(result.success ? "calibration OK: " : "calibration FAILED: ") + result.message);
        emit(Json("calibration_done").add("success", result.success).add("bit_phase", result.chosen_bit_phase)
                 .add("phase", result.chosen_phase).add("width_ps", result.good_width_ps).add("bit_ps", result.bit_ps)
                 .add("message", result.message));
        if (result.final_test) show(*result.final_test);
        lclkin_off(lclk);
        show(session_->status());
        if (!result.success) error("calibration failed: " + result.message);
        return result.success;
    }

    bool stream(Args& a) {
        StreamRequest req;
        double refclk_hz = 8.0e6;
        const auto out = a.value("--out");
        if (auto v = a.value("--duration")) req.duration_s = parse_number(*v, "duration");
        if (auto v = a.value("--window-us")) req.window_us = static_cast<std::uint32_t>(parse_integer(*v, "window", 1, 4294967295L));
        if (auto v = a.value("--refclk-hz")) refclk_hz = parse_number(*v, "REFCLK frequency");
        a.done();
        if (!out) throw ArgumentError("stream needs --out FILE");
        if (req.duration_s <= 0 && req.window_us == 0 && !serving_)
            throw ArgumentError("stream needs --duration S or --window-us N (or use the GUI's Stop)");
        const auto before = session_->status();
        if (!before.lvds.format_valid || !before.gpx.verified())
            throw ArgumentError("the GPX2 is not configured and verified (run gpx-config, then calibrate)");
        if (const auto why = capture_problem(before, refclk_hz); !why.empty()) throw ArgumentError(why);
        std::ofstream file(*out, std::ios::binary);
        if (!file) throw ArgumentError("cannot write " + *out);
        LclkinScope lclk(*session_, true);
        std::atomic<bool> abort{false};
        auto last_print = std::chrono::steady_clock::now();
        std::uint64_t last_records = 0;
        say("streaming to " + *out + (req.window_us ? " (window " + std::to_string(req.window_us) + " us)" : "") +
            (req.duration_s > 0 ? " for " + fixed(req.duration_s, 1) + " s" : ""));
        const auto result = session_->stream(
            req,
            [&](std::uint64_t raw) {
                char b[8];
                for (int i = 0; i < 8; ++i) b[i] = static_cast<char>((raw >> (8 * i)) & 0xFFU);
                file.write(b, 8);
            },
            [&](const StreamProgress& p) {
                if (serving_) {
                    // "stop" ends the run; end of input ends it too and is
                    // kept (with every other line) for the serve loop.
                    std::vector<std::string> keep;
                    while (auto line = queue_.try_pop()) {
                        if (*line == "stop") {
                            abort = true;
                        } else {
                            if (*line == "\x04") abort = true;
                            keep.push_back(*line);
                        }
                    }
                    queue_.push_front(keep);
                }
                const auto now = std::chrono::steady_clock::now();
                if (now - last_print >= std::chrono::milliseconds(serving_ ? 250 : 1000)) {
                    const double dt = std::chrono::duration<double>(now - last_print).count();
                    say("  " + fixed(p.elapsed_s, 1) + " s: " + std::to_string(p.records) + " records, events " +
                        std::to_string(p.events[0]) + "/" + std::to_string(p.events[1]) + "/" + std::to_string(p.events[2]) +
                        "/" + std::to_string(p.events[3]) + ", " + fixed(static_cast<double>(p.records - last_records) / dt, 0) +
                        " records/s" + (p.dropped_marked ? ", dropped >= " + std::to_string(p.dropped_marked) : ""));
                    emit(Json("stream_progress").add("elapsed_s", p.elapsed_s).add("records", p.records)
                             .raw("events", json_array(p.events)).add("dropped_marked", p.dropped_marked)
                             .add("records_per_s", static_cast<double>(p.records - last_records) / dt).add("stopping", p.stopping));
                    last_print = now;
                    last_records = p.records;
                }
            },
            &abort);
        file.close();
        lclkin_off(lclk);
        write_run_sidecar(*out, before, result, refclk_hz);
        say(std::string(result.pass ? "run complete" : "run FAILED") + ": " + std::to_string(result.records) + " records in " +
            std::to_string(result.frames) + " frames, events " + std::to_string(result.events[0]) + "/" +
            std::to_string(result.events[1]) + "/" + std::to_string(result.events[2]) + "/" +
            std::to_string(result.events[3]) + ", " + fixed(result.seconds, 2) + " s" +
            (result.by_window ? " (window ended the run)" : "") +
            (result.final_response ? ", FPGA dropped " + std::to_string(result.final_response->stream.dropped) : ""));
        emit(Json("stream_done").add("pass", result.pass).add("records", result.records).add("frames", result.frames)
                 .raw("events", json_array(result.events)).add("seconds", result.seconds).add("by_window", result.by_window)
                 .add("dropped", result.final_response ? result.final_response->stream.dropped : 0U)
                 .add("failure", result.failure).add("file", *out));
        if (!result.pass) {
            error("run failed: " + result.failure + (result.recovered ? " (STOP acknowledged afterwards)" : ""));
            return false;
        }
        return true;
    }

    // Count rate: a run ended by the FPGA's window, records discarded.  The
    // rate is events / window; dropped events (FIFO full) are not attributed
    // to a channel, so with drops the rates are lower bounds.
    bool count(Args& a) {
        double seconds = 1.0;
        if (a.rest().size() >= 2) seconds = parse_number(a.rest()[1], "count time");
        a.done();
        if (!(seconds >= 0.001 && seconds <= 60.0)) throw ArgumentError("count time must be 0.001 ... 60 s");
        const auto before = session_->status();
        if (!before.lvds.format_valid || !before.gpx.verified())
            throw ArgumentError("the GPX2 is not configured and verified (run gpx-config, then calibrate)");
        if (const auto why = capture_problem(before); !why.empty()) throw ArgumentError(why);
        StreamRequest req;
        req.window_us = static_cast<std::uint32_t>(std::llround(seconds * 1e6));
        req.duration_s = seconds + 5.0;   // host-timed STOP only if the window never ends the run
        LclkinScope lclk(*session_, true);
        std::atomic<bool> abort{false};
        const auto result = session_->stream(
            req, [](std::uint64_t) {},
            [&](const StreamProgress&) {
                if (!serving_) return;
                std::vector<std::string> keep;
                while (auto line = queue_.try_pop()) {
                    if (*line == "stop" || *line == "\x04") abort = true;
                    if (*line != "stop") keep.push_back(*line);
                }
                queue_.push_front(keep);
            },
            &abort);
        lclkin_off(lclk);
        const double window_s = static_cast<double>(req.window_us) * 1e-6;
        const std::uint64_t dropped = result.final_response ? result.final_response->stream.dropped : 0;
        std::array<double, kChannels> rates{};
        for (std::size_t c = 0; c < kChannels; ++c) rates[c] = static_cast<double>(result.events[c]) / window_s;
        const bool ok = result.pass && result.by_window;
        std::string line = std::string(ok ? "COUNT" : "COUNT FAILED") + " (" + fixed(window_s, 3) + " s window):";
        for (std::size_t c = 0; c < kChannels; ++c)
            line += "  ch" + std::to_string(c + 1) + " " + fixed(rates[c], 1) + "/s";
        if (dropped) line += "  (FPGA dropped " + std::to_string(dropped) + " events: rates are lower bounds)";
        say(line);
        std::string rates_json = "[";
        for (std::size_t c = 0; c < kChannels; ++c) rates_json += (c ? "," : "") + fixed(rates[c], 3);
        rates_json += "]";
        emit(Json("count_done").add("pass", ok).raw("events", json_array(result.events)).raw("rates_hz", rates_json)
                 .add("window_s", window_s).add("dropped", dropped).add("by_window", result.by_window)
                 .add("failure", ok ? std::string() : (result.failure.empty() ? std::string("the window did not end the run")
                                                                                : result.failure)));
        if (!ok) {
            error("count failed: " + (result.failure.empty() ? std::string("the window did not end the run") : result.failure));
            return false;
        }
        return true;
    }

    bool decode(Args& a) {
        if (a.rest().size() < 2) throw ArgumentError("usage: decode FILE [--csv OUT] [--refclk-hz F] [--test-pattern]");
        const auto csv = a.value("--csv");
        auto refclk = a.value("--refclk-hz");
        const bool test_pattern = a.flag("--test-pattern");
        a.done();
        const std::string path = a.rest()[1];
        std::ifstream side(path + ".json");
        std::string sidecar((std::istreambuf_iterator<char>(side)), std::istreambuf_iterator<char>());
        double refclk_hz = refclk ? parse_number(*refclk, "REFCLK frequency") : json_number(sidecar, "refclk_hz").value_or(8.0e6);
        const auto divisions = static_cast<std::uint32_t>(json_number(sidecar, "divisions").value_or(125000));
        const int ref_bits = static_cast<int>(json_number(sidecar, "ref_bits").value_or(24));
        double tick_ns = json_number(sidecar, "coarse_tick_ns").value_or(kCoarseTickNsR1);
        if (!(tick_ns > 0)) tick_ns = kCoarseTickNsR1;
        const int stop_bits = static_cast<int>(json_number(sidecar, "stop_bits").value_or(20));
        // The datasheet test pattern cut to the configured widths (PROTOCOL.md 2.3).
        const std::uint32_t tp_ref = 0xF0CCAAU & ((1U << static_cast<unsigned>(ref_bits)) - 1U);
        const std::uint32_t tp_stop = 0x0AACF0U & ((1U << static_cast<unsigned>(stop_bits)) - 1U);
        std::uint64_t pattern_bad = 0;
        std::ifstream file(path, std::ios::binary);
        if (!file) throw ArgumentError("cannot read " + path);
        std::ofstream out;
        if (csv) {
            out.open(*csv);
            if (!out) throw ArgumentError("cannot write " + *csv);
            out << "channel,time_ps,ref_index,stop,coarse_cycles\n";
        }
        TimeUnwrapper unwrap(refclk_hz, divisions, ref_bits, tick_ns);
        std::array<std::uint64_t, kChannels> events{};
        std::uint64_t records = 0, markers = 0, drops = 0;
        double last_ps = 0;
        std::optional<std::uint64_t> start_coarse;
        char b[8];
        while (file.read(b, 8)) {
            std::uint64_t raw = 0;
            for (int i = 0; i < 8; ++i) raw |= static_cast<std::uint64_t>(static_cast<unsigned char>(b[i])) << (8 * i);
            ++records;
            const auto rec = decode_record(raw);
            if (rec.marker) {
                ++markers;
                if (rec.marker_type == MarkerType::overflow) drops += rec.info;
            }
            if (test_pattern && !rec.marker && (rec.ref_index != tp_ref || rec.stop != tp_stop)) {
                if (pattern_bad < 5)
                    say("  event " + std::to_string(records) + " (ch" + std::to_string(rec.channel + 1) + "): ref " +
                        hex(rec.ref_index, 6) + " stop " + hex(rec.stop, 5) + ", expected " + hex(tp_ref, 6) + " " +
                        hex(tp_stop, 5));
                ++pattern_bad;
            }
            if (test_pattern && !rec.marker) {   // test-pattern events carry no time
                if (!start_coarse) start_coarse = unwrap.coarse();
                ++events[static_cast<std::size_t>(rec.channel)];
                continue;
            }
            const auto e = unwrap.feed(rec);
            if (!start_coarse) start_coarse = unwrap.coarse();
            if (e) {
                ++events[static_cast<std::size_t>(e->channel)];
                last_ps = e->time_ps;
                if (csv) out << e->channel + 1 << "," << fixed(e->time_ps, 1) << "," << e->ref_index << "," << e->stop << ","
                             << e->coarse << "\n";
            }
        }
        // Run length from the marker (coarse) clock; the event span is the
        // last event's time relative to the first event.
        const double run_s = start_coarse ? static_cast<double>(unwrap.coarse() - *start_coarse) * tick_ns * 1e-9 : 0.0;
        say(path + ": " + std::to_string(records) + " records (" + std::to_string(markers) + " markers), events " +
            std::to_string(events[0]) + "/" + std::to_string(events[1]) + "/" + std::to_string(events[2]) + "/" +
            std::to_string(events[3]) + ", run " + fixed(run_s, 6) + " s, first-to-last event " +
            fixed(last_ps * 1e-12, 6) + " s, dropped >= " + std::to_string(drops) +
            " (REFCLK " + fixed(refclk_hz / 1e6, 3) + " MHz, " + std::to_string(divisions) + " divisions, " +
            std::to_string(ref_bits) + "-bit reference index)" + (csv ? "; CSV " + *csv : ""));
        if (test_pattern) {
            const std::uint64_t total = events[0] + events[1] + events[2] + events[3];
            say(std::string(pattern_bad == 0 ? "TEST PATTERN OK" : "TEST PATTERN ERRORS") + ": " + std::to_string(total) +
                " events, " + std::to_string(pattern_bad) + " differ from ref " + hex(tp_ref, 6) + " / stop " +
                hex(tp_stop, 5) + " (" + std::to_string(ref_bits) + " + " + std::to_string(stop_bits) + " bits); " +
                fixed(run_s > 0 ? static_cast<double>(total) / run_s * 1e-6 : 0.0, 3) + " M events/s received");
        }
        emit(Json("decoded").add("records", records).raw("events", json_array(events)).add("run_s", run_s).add("span_s", last_ps * 1e-12)
                 .add("dropped_marked", drops).add("pattern_errors", pattern_bad));
        return !test_pattern || pattern_bad == 0;
    }

    bool execute(const std::vector<std::string>& words) {
        try {
            Args a(words);
            const std::string verb = words[0];
            auto need = [&](std::size_t n, const char* usage) {
                a.done();
                if (a.rest().size() != n) throw ArgumentError(std::string("usage: ") + usage);
            };
            const auto& w = a.rest();
            if (verb == "decode") return decode(a);
            if (verb == "status") { need(1, "status"); return show(session_->status()); }
            if (verb == "readback") { need(1, "readback"); return show(session_->readback()); }
            if (verb == "probe") { need(1, "probe"); return show(session_->probe()); }
            if (verb == "stop") { need(1, "stop"); return show(session_->stop()); }
            if (verb == "threshold" || verb == "threshold-code") {
                need(3, verb == "threshold" ? "threshold CH VOLTS" : "threshold-code CH CODE");
                const unsigned mask = parse_channels(w[1]);
                const std::uint16_t code = verb == "threshold"
                                               ? threshold_code(parse_number(w[2], "voltage"))
                                               : static_cast<std::uint16_t>(parse_integer(w[2], "threshold code", kDacCodeMin, kDacCodeMax));
                return show(session_->thresholds(mask, {code, code, code, code}));
            }
            if (verb == "hysteresis" || verb == "hysteresis-code") {
                need(3, verb == "hysteresis" ? "hysteresis CH OHMS" : "hysteresis-code CH CODE");
                const unsigned mask = parse_channels(w[1]);
                const std::uint16_t code = verb == "hysteresis"
                                               ? wiper_code(parse_number(w[2], "resistance"))
                                               : static_cast<std::uint16_t>(parse_integer(w[2], "wiper code", 0, kWiperCodeMax));
                return show(session_->hysteresis(mask, {code, code, code, code}));
            }
            if (verb == "led1") {
                a.done();
                if (w.size() == 2 && w[1] == "off") return show(session_->led1(0, 0, 0, 0));
                if (w.size() != 4 && w.size() != 5) throw ArgumentError("usage: led1 R G B [MA] | led1 off");
                const auto r = static_cast<std::uint8_t>(parse_integer(w[1], "red", 0, 255));
                const auto g = static_cast<std::uint8_t>(parse_integer(w[2], "green", 0, 255));
                const auto b = static_cast<std::uint8_t>(parse_integer(w[3], "blue", 0, 255));
                const int ma = w.size() == 5 ? static_cast<int>(parse_integer(w[4], "current (mA)", 5, 15)) : 5;
                return show(session_->led1(r, g, b, led1_current_setting(ma)));
            }
            if (verb == "led2") {
                need(2, "led2 [r][g][b] | off");
                unsigned mask = 0;
                if (w[1] != "off") {
                    for (char c : w[1]) {
                        if (c == 'r') mask |= 1U;
                        else if (c == 'g') mask |= 2U;
                        else if (c == 'b') mask |= 4U;
                        else throw ArgumentError("led2 takes letters r, g, b, or off");
                    }
                }
                return show(session_->led2(mask));
            }
            if (verb == "leds") {
                need(2, "leds MASK");
                return show(session_->set_leds(static_cast<std::uint8_t>(parse_integer(w[1], "LED mask", 0, 255))));
            }
            if (verb == "gpx-config") {
                auto g = gpx_options(a, session_->status());
                need(1, "gpx-config [GPX OPTIONS]");
                const auto regs = g.config.bytes();
                say("GPX2 registers: " + hex_bytes(regs.data(), kGpxRegisters) + " (" + describe_gpx_config(regs) + ")");
                const auto r = session_->gpx_configure(regs);
                const bool ok = show(r);
                if (ok)
                    if (const auto why = capture_problem(r, g.refclk_hz); !why.empty()) say("warning: " + why);
                return ok;
            }
            if (verb == "gpx-readback") { need(1, "gpx-readback"); return show(session_->gpx_read_config()); }
            if (verb == "gpx-reset") { need(1, "gpx-reset"); return show(session_->gpx_reset()); }
            if (verb == "gpx-init") { need(1, "gpx-init"); return show(session_->gpx_init()); }
            if (verb == "gpx-results") {
                need(1, "gpx-results");
                const auto r = session_->gpx_read_results();
                const bool ok = show(r);
                for (std::size_t c = 0; c < kChannels; ++c)
                    say("  result ch" + std::to_string(c + 1) + ": " + hex_bytes(&r.gpx.results[6 * c], 6));
                return ok;
            }
            if (verb == "lvds") {
                int div = 0, bp = 0;
                std::array<std::uint8_t, kChannels> st{}, ft{};
                if (auto v = a.value("--div")) div = static_cast<int>(parse_integer(*v, "divider code", 0, 3));
                if (auto v = a.value("--bit-phase")) bp = static_cast<int>(parse_integer(*v, "bit phase", 0, 15));
                if (auto v = a.value("--sdo-taps")) st = parse_taps(*v);
                if (auto v = a.value("--frame-taps")) ft = parse_taps(*v);
                need(2, "lvds on|off [--div N] [--bit-phase P] [--sdo-taps a,b,c,d] [--frame-taps a,b,c,d]");
                if (w[1] != "on" && w[1] != "off") throw ArgumentError("lvds takes on or off");
                const auto r = session_->lvds_set(w[1] == "on", div, bp, st, ft);
                const bool ok = show(r);
                if (ok && w[1] == "on")
                    say("note: LCLKIN stays on until 'lvds off' or the end of the next link test, calibration or run");
                if (ok && r.lvds.format_valid)
                    if (const auto why = capture_problem(r); !why.empty()) say("warning: " + why);
                return ok;
            }
            if (verb == "phase") {
                need(2, "phase STEP");
                const int steps = session_->timing() ? session_->timing()->phase_steps : 560;
                return show(session_->lvds_phase(static_cast<int>(parse_integer(w[1], "phase step", 0, steps - 1))));
            }
            if (verb == "link-test") {
                a.done();
                const std::uint32_t one_ms = 1000U * (session_->timing() ? session_->timing()->cycles_per_us() : 100U);
                const auto window = w.size() > 1 ? static_cast<std::uint32_t>(parse_integer(w[1], "window", 1, kLinkTestMaxWindow)) : one_ms;
                LclkinScope lclk(*session_, true);
                const bool ok = show(session_->link_test(window));
                lclkin_off(lclk);
                return ok;
            }
            if (verb == "calibrate") return calibrate(a);
            if (verb == "stream") return stream(a);
            if (verb == "count") return count(a);
            throw ArgumentError("unknown command '" + verb + "' (see --help)");
        } catch (const ArgumentError& bad) {
            error(bad.what());
            return false;
        } catch (const std::exception& failure) {
            error(failure.what());
            // A transport or protocol failure leaves the link in an unknown
            // state: drop it; `serve` reconnects on the next command.
            disconnect();
            emit(Json("disconnected"));
            return false;
        }
    }
};

}  // namespace

int main(int argc, char** argv) {
    Options options;
    std::vector<std::string> words;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (words.empty() && (arg == "-h" || arg == "--help")) {
            std::cout << kUsage;
            return 0;
        } else if (words.empty() && arg == "--serial" && i + 1 < argc) {
            options.serial = argv[++i];
        } else if (words.empty() && arg == "--simulate") {
            options.simulate = true;
        } else if (words.empty() && arg == "--json") {
            options.json = true;
        } else if (!arg.empty() && arg[0] == '-' && words.empty()) {
            std::cerr << "error: unknown option '" << arg << "'\n" << kUsage;
            return 2;
        } else {
            words.push_back(arg);
        }
    }
    if (words.empty()) {
        std::cerr << kUsage;
        return 2;
    }
    App app(options);
    if (words[0] == "serve") return app.serve();
    return app.run_one(words);
}
