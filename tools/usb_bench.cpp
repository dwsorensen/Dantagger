// USB throughput of the TDC board under different D3XX buffering settings.
//
//   build/usb_bench [--serial S] [--seconds 2] [--usb 0,0 --usb 64,4096 ...] [--live] [--load N]
//
// For each --usb URBS,QUEUE (0,0 = the library defaults 8 and 256, what
// TDC_Firmware's tools use; default: 0,0 then 64,4096) it opens the board
// with that buffering and streams for --seconds.  Default mode: the GPX2's
// LVDS test pattern on all channels, which delivers more events than USB can
// carry (release 2: 47.6 M events/s with 24 + 18-bit frames), so the
// received rate is the link's ceiling and every received event is checked
// against the pattern.  --live streams the real inputs instead (your signals,
// triggers as the board has them).  --load N runs N busy threads meanwhile, to
// reproduce a computer short of CPU.
//
// Needs the board configured and calibrated (Dantagger's setup).  It changes
// nothing permanently: the GPX2 configuration is put back after the test
// pattern, LCLKIN is off afterwards, triggers are not touched.

#include "d3xx_common.hpp"
#include "d3xx_transport.hpp"
#include "tdcctl/tdc_protocol.hpp"
#include "tdcctl/tdc_session.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/resource.h>
#include <thread>
#include <vector>

namespace {

struct Setting {
    int urbs, queue;
};

double cpu_seconds() {
    rusage u{};
    getrusage(RUSAGE_SELF, &u);
    return static_cast<double>(u.ru_utime.tv_sec + u.ru_stime.tv_sec) + 1e-6 * static_cast<double>(u.ru_utime.tv_usec + u.ru_stime.tv_usec);
}

int run_one(const std::string& serial, const Setting& s, double seconds, bool live, int load) {
    au2ft601::D3xxTransportOptions o;
    o.expected_fifo_clock = au2ft601::FifoClock::mhz66;
    o.in_urb_buffer_size = static_cast<std::uint32_t>(au2ft601::kFrameSize);   // as Dantagger: one FPGA frame
    o.in_urb_count = static_cast<std::uint8_t>(s.urbs);
    o.in_urb_buffer_count = static_cast<std::uint16_t>(s.queue);
    au2ft601::D3xxTransport transport(o);
    try {
        transport.open_by_serial(serial);
    } catch (const std::exception& e) {
        std::printf("USB %d,%d: cannot open: %s\n", s.urbs, s.queue, e.what());
        return 1;
    }
    std::printf("\n=== USB %d,%d ===\n%s", s.urbs, s.queue, transport.session_info().c_str());
    tdcctl::Session session(transport, tdcctl::SessionOptions{},
                            static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()) << 4U);
    const auto st = session.sync().status;
    if (!st.gpx.verified() || !st.gpx.measuring() || !st.lvds.format_valid) {
        std::printf("the GPX2 is not configured: set the board up first (app: Set up the board)\n");
        return 1;
    }
    const auto original = st.gpx.written;
    const int ref_bits = st.lvds.ref_bits, stop_bits = st.lvds.stop_bits;
    const std::uint32_t want_ref = 0xF0CCAAU & ((1U << ref_bits) - 1U);
    const std::uint32_t want_stop = 0x0AACF0U & ((1U << stop_bits) - 1U);
    std::printf("%s, %d + %d-bit frames%s, LCLKIN %.4g MHz; %s for %.1f s%s\n",
                st.design_version == tdcctl::kDesignVersionR2 ? "release 2" : "release 1", ref_bits, stop_bits,
                st.lvds.ddr ? " DDR" : " SDR", st.lvds.lclkin_mhz(), live ? "live inputs" : "test pattern", seconds,
                load ? (", " + std::to_string(load) + " busy threads").c_str() : "");
    if (!live) {
        auto regs = original;
        regs[6] = static_cast<std::uint8_t>(regs[6] | 0x10U);   // TEST_PATTERN
        const auto r = session.gpx_configure(regs);
        if (r.status != tdcctl::Status::ok) {
            std::printf("GPX_CONFIGURE (test pattern) failed: %s\n", tdcctl::status_name(r.status).c_str());
            return 1;
        }
    }
    std::atomic<bool> spin{load > 0};
    std::vector<std::thread> busy;
    for (int i = 0; i < load; ++i)
        busy.emplace_back([&spin] {
            volatile std::uint64_t x = 0;
            while (spin.load(std::memory_order_relaxed)) x = x + 1;
        });
    std::uint64_t events = 0, bad = 0;
    std::uint64_t per_channel[4] = {};
    tdcctl::StreamResult result;
    const double cpu0 = cpu_seconds();
    {
        tdcctl::LclkinScope lclk(session, true);
        tdcctl::StreamRequest req;
        req.duration_s = seconds;
        result = session.stream(req, [&](std::uint64_t raw) {
            if (raw >> 63) return;
            ++events;
            ++per_channel[(raw >> 61) & 3U];
            if (!live && ((((raw >> 20) & 0xFFFFFFU) != want_ref) || ((raw & 0xFFFFFU) != want_stop))) ++bad;
        });
        lclk.off();
    }
    const double cpu = cpu_seconds() - cpu0;
    spin = false;
    for (auto& t : busy) t.join();
    if (!live) {
        const auto r = session.gpx_configure(original);
        std::printf("GPX2 configuration restored: %s\n", tdcctl::status_name(r.status).c_str());
    }
    const double t = result.seconds > 0 ? result.seconds : seconds;
    const std::uint64_t dropped = result.final_response ? result.final_response->stream.dropped : 0;
    std::printf("run %s%s: %.3f s, %llu records = %.2f M records/s (%.1f MB/s); events %.2f M/s "
                "(ch1 %.2f, ch2 %.2f, ch3 %.2f, ch4 %.2f M/s)\n",
                result.pass ? "OK" : "FAILED ", result.pass ? "" : result.failure.c_str(), t,
                static_cast<unsigned long long>(result.records), result.records / t * 1e-6, result.records * 8.0 / t * 1e-6,
                events / t * 1e-6, per_channel[0] / t * 1e-6, per_channel[1] / t * 1e-6, per_channel[2] / t * 1e-6,
                per_channel[3] / t * 1e-6);
    std::printf("FPGA dropped %llu events (%.1f %% of all), pipeline depth %u, this process used %.0f %% of one core%s\n",
                static_cast<unsigned long long>(dropped), events + dropped ? 100.0 * dropped / (events + dropped) : 0.0,
                result.pipeline_depth, 100.0 * cpu / t, load ? " (busy threads included)" : "");
    if (!live) std::printf("test pattern: %llu events differ%s\n", static_cast<unsigned long long>(bad), bad ? "  <-- SAMPLING ERRORS" : " (OK)");
    return result.pass && bad == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    std::string serial;
    double seconds = 2.0;
    bool live = false;
    int load = 0;
    std::vector<Setting> settings;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--serial" && i + 1 < argc) serial = argv[++i];
        else if (a == "--seconds" && i + 1 < argc) seconds = std::atof(argv[++i]);
        else if (a == "--live") live = true;
        else if (a == "--load" && i + 1 < argc) load = std::atoi(argv[++i]);
        else if (a == "--usb" && i + 1 < argc) {
            Setting s{};
            if (std::sscanf(argv[++i], "%d,%d", &s.urbs, &s.queue) != 2) {
                std::fprintf(stderr, "--usb URBS,QUEUE\n");
                return 2;
            }
            settings.push_back(s);
        } else {
            std::printf("usage: usb_bench [--serial S] [--seconds 2] [--usb URBS,QUEUE ...] [--live] [--load N]\n");
            return a == "-h" || a == "--help" ? 0 : 2;
        }
    }
    if (settings.empty()) settings = {{0, 0}, {64, 4096}};
    if (serial.empty()) {
        const auto devices = au2ft601::d3xx::enumerate_devices();
        if (devices.size() != 1) {
            std::fprintf(stderr, "%zu FT601 devices attached: pass --serial\n", devices.size());
            return 1;
        }
        serial = devices[0].serial;
        if (devices[0].flags & 1U) {
            std::fprintf(stderr, "the board is open in another program (the Dantagger app or Python): close it first\n");
            return 1;
        }
    }
    int failures = 0;
    for (const auto& s : settings) failures += run_one(serial, s, seconds, live, load);
    return failures ? 1 : 0;
}
