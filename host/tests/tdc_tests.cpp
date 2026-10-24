// Host tests for tdcctl: protocol encoding/decoding, the GPX2 configuration
// builder and its rules, record decoding and time unwrapping, and sessions
// (commands, calibration, streaming) against the simulated FPGA.

#include "tdcctl/fake_fpga.hpp"
#include "tdcctl/gauss_fit.hpp"
#include "tdcctl/histogram.hpp"
#include "tdcctl/run_file.hpp"
#include "tdcctl/tdc_protocol.hpp"
#include "tdcctl/tdc_session.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <random>
#include <string>
#include <vector>

using namespace tdcctl;

namespace {

int checks = 0;
int failures = 0;

void check(bool ok, const std::string& what) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

template <typename F>
void expect_throw(F f, const std::string& what) {
    bool threw = false;
    try {
        f();
    } catch (const ArgumentError&) {
        threw = true;
    }
    check(threw, what + " throws ArgumentError");
}

std::uint64_t event(int ch, std::uint16_t coarse_low, std::uint32_t ref, std::uint32_t stop) {
    return (static_cast<std::uint64_t>(ch) << 61U) | (static_cast<std::uint64_t>(coarse_low) << 44U) |
           (static_cast<std::uint64_t>(ref & 0xFFFFFFU) << 20U) | (stop & 0xFFFFFU);
}
std::uint64_t marker(MarkerType t, std::uint16_t info, std::uint64_t coarse) {
    return (1ULL << 63U) | (static_cast<std::uint64_t>(t) << 60U) | (static_cast<std::uint64_t>(info) << 48U) |
           (coarse & 0xFFFFFFFFFFFFULL);
}

void test_gpx_config() {
    const GpxConfig c;
    const auto r = c.bytes();
    const std::array<std::uint8_t, kGpxRegisters> expected{
        {0x3F, 0x0F, 0x1D, 0x48, 0xE8, 0x01, 0xC0, 0x53, 0xA1, 0x13, 0x00, 0x0A, 0xCC, 0xCC, 0xF1, 0x7D, 0x00}};
    check(r == expected, "default configuration bytes (8 MHz, 1 ps, 24+20 bits, all channels, LVDS)");
    check(GpxConfig::divisions_for(8.0e6, 1.0) == 125000, "125000 divisions for 8 MHz at 1 ps");
    check(GpxConfig::divisions_for(10.0e6, 5.0) == 20000, "20000 divisions for 10 MHz at 5 ps");
    expect_throw([] { GpxConfig::divisions_for(8.0e6, 3.0); }, "8 MHz at 3 ps (not whole)");
    expect_throw([] { GpxConfig::divisions_for(20.0e6, 1.0); }, "REFCLK above 12.5 MHz");
    GpxConfig tp;
    tp.test_pattern = true;
    check(tp.bytes()[6] == 0xD0, "test pattern bit");
    GpxConfig small;
    small.stop_bits = 16;
    expect_throw([&] { (void)small.bytes(); }, "125000 divisions in 16 stop bits");
    GpxConfig bad_ref;
    bad_ref.ref_index_bits = 10;
    expect_throw([&] { (void)bad_ref.bytes(); }, "10 reference-index bits");
    GpxConfig adj;
    adj.data_valid_adjust_ps = 320;
    check(adj.bytes()[7] == 0x73, "data-valid adjust +320 ps");
    // Every FPGA rule, on top of a valid configuration.
    struct Case { int reg; std::uint8_t value; const char* what; };
    const Case cases[] = {
        {16, 0x04, "CMOS_INPUT"}, {7, 0xD3, "REFCLK_BY_XOSC"}, {0, 0x2F, "PIN_ENA_REFCLK 0"},
        {1, 0xCF, "HIGH_RESOLUTION 3"}, {1, 0x3F, "CHANNEL_COMBINE 3"},
        {8, 0xA0, "fixed register 8"}, {15, 0x8E, "fixed register 15"}, {6, 0xE0, "register 6 fixed"},
        {5, 0x11, "register 5 fixed"}, {7, 0x13, "register 7 bit 6"}, {7, 0x50, "register 7 low nibble"},
        {2, 0x0D, "divisions above 16 stop bits"},
    };
    for (const auto& k : cases) {
        auto regs = expected;
        regs[static_cast<std::size_t>(k.reg)] = k.value;
        check(!gpx_rule_violation(regs).empty(), std::string("rule: ") + k.what);
        expect_throw([&] { (void)encode_gpx_configure(1, regs); }, std::string("encode refuses ") + k.what);
    }
    auto zero = expected;
    zero[3] = zero[4] = zero[5] = 0;
    check(!gpx_rule_violation(zero).empty(), "rule: divisions 0");
    check(gpx_rule_violation(expected).empty(), "valid configuration passes");
    // DDR: allowed for release 2, refused for release 1.
    auto ddr_regs = expected;
    ddr_regs[2] = 0x3D;
    check(gpx_rule_violation(ddr_regs).empty() && !gpx_rule_violation(ddr_regs, false).empty(),
          "DDR allowed for release 2 only");
    GpxConfig fast;
    fast.ddr = true;
    fast.ref_index_bits = 6;
    fast.stop_bits = 18;
    check(fast.bytes()[2] == 0x36, "release-2 default register 2 (DDR, 18 stop bits, 6 index bits)");
    check(min_stop_bits(125000) == 18 && min_stop_bits(65535) == 16 && min_stop_bits(15625) == 14 &&
              min_stop_bits(1048576) == 0,
          "smallest stop width for the divisions");
    // Reference-index width that keeps times unambiguous.
    const LinkTiming r2;
    const auto r1 = LinkTiming::release1();
    check(min_ref_index_bits(r2, 0, true, 18, 0, 8e6) == 6, "250 MHz DDR, 18 stop bits: 6 index bits");
    check(min_ref_index_bits(r2, 1, true, 18, 0, 8e6) == 8, "125 MHz DDR: 8 index bits");
    check(min_ref_index_bits(r2, 0, false, 18, 0, 8e6) == 8, "250 MHz SDR: 8 index bits");
    check(min_ref_index_bits(r1, 0, false, 20, 0, 8e6) == 8, "release 1, 100 MHz SDR: 8 index bits");
    check(min_ref_index_bits(r2, 3, false, 20, 0, 8e6) == 12, "31.25 MHz SDR: 12 index bits");
    check(std::abs(max_result_latency_us(r2, 0, true, 24, 0) - (17 * 24 * 2e-3 + 0.5)) < 1e-9,
          "latency: 17 frames of 24 bits at 2 ns plus 0.5 us");
    check(r2.samples_per_bit(0, true) == 1 && r2.samples_per_bit(0, false) == 2 && r2.samples_per_bit(3, false) == 16 &&
              r1.samples_per_bit(2, false) == 4 && r2.steps_per_sample() == 112 && r1.steps_per_sample() == 560,
          "samples per bit and steps per sample");
    check(gpx_ref_index_bits(0x1D) == 24 && gpx_stop_bits(0x1D) == 20 && gpx_divisions(expected) == 125000,
          "register-2 decoding");
}

void test_encoding() {
    expect_throw([] { (void)encode_thresholds(1, 1, {10, 0, 0, 0}); }, "threshold code below the window");
    expect_throw([] { (void)encode_thresholds(1, 1, {41954, 0, 0, 0}); }, "threshold code above the window");
    expect_throw([] { (void)encode_lvds_set(1, true, 4, 0, {}, {}); }, "divider 4");
    expect_throw([] { (void)encode_lvds_set(1, true, 1, 16, {}, {}); }, "bit phase 16");
    expect_throw([] { (void)encode_lvds_set(1, true, 0, 0, {32, 0, 0, 0}, {}); }, "tap 32");
    expect_throw([] { (void)encode_lvds_phase(1, 1024); }, "phase 1024");
    expect_throw([] { (void)encode_link_test(1, 0); }, "link-test window 0");
    const auto f = encode_lvds_set(7, true, 2, 3, {1, 2, 3, 4}, {31, 0, 0, 5});
    au2ft601::FrameHeader h;
    std::string error;
    check(au2ft601::decode_frame(f, h, error) && h.pattern_or_opcode == 0x40 && h.valid_payload_length == 20,
          "LVDS_SET frame header");
    check(f[64] == 1 && f[68] == 2 && f[72] == 3 && f[76] == 1 && f[79] == 4 && f[80] == 31 && f[83] == 5,
          "LVDS_SET payload bytes");
    const auto s = encode_start(0xABCDEF, 9, 300);
    check(au2ft601::decode_frame(s, h, error) && h.run_id == 0xABCDEF && h.pattern_or_opcode == 1 &&
              h.valid_payload_length == 8 && s[64] == 0x2C && s[65] == 0x01,
          "START frame");
    check(std::abs(threshold_volts(threshold_code(0.25)) - 0.25) < 4e-5, "threshold conversion round trip");
}

void test_records() {
    const auto e = decode_record(event(2, 0xBEEF, 0x123456, 0xABCDE));
    check(!e.marker && e.channel == 2 && e.coarse_low == 0xBEEF && e.ref_index == 0x123456 && e.stop == 0xABCDE,
          "event record fields");
    const auto m = decode_record(marker(MarkerType::overflow, 77, 0x123456789ABCULL));
    check(m.marker && m.marker_type == MarkerType::overflow && m.info == 77 && m.coarse == 0x123456789ABCULL,
          "marker record fields");

    // 8 MHz, 1 ps LSB, 8-bit reference index (wraps every 32 us = 3200 cycles):
    // events every ~10 us with coarse time wrapping its 16 bits.
    TimeUnwrapper u(8.0e6, 125000, 8);
    const std::uint64_t t0 = 0xFFFF0000ULL;     // low 16 bits wrap soon
    (void)u.feed(decode_record(marker(MarkerType::start, 0, t0)));
    bool ok = true;
    double first = 0;
    for (int i = 0; i < 400; ++i) {
        // true time: i * 10.3 us + 17 ps; reference index = floor(t / 125 ns)
        const double t_ps = i * 10.3e6 + 17.0 + 125000.0 * 5;
        const auto ref_full = static_cast<std::uint64_t>(t_ps / 125000.0);
        const auto stop = static_cast<std::uint32_t>(std::fmod(t_ps, 125000.0));
        const std::uint64_t coarse = t0 + static_cast<std::uint64_t>(t_ps / 10000.0) + 37;   // + latency
        if (i % 13 == 5) (void)u.feed(decode_record(marker(MarkerType::time, 0, coarse - 5)));
        const auto ev = u.feed(decode_record(event(i % 4, static_cast<std::uint16_t>(coarse & 0xFFFFU),
                                                   static_cast<std::uint32_t>(ref_full & 0xFFU), stop)));
        if (!ev) { ok = false; break; }
        if (i == 0) first = t_ps;
        if (std::abs(ev->time_ps - (t_ps - first)) > 1.0 || ev->coarse != coarse) ok = false;
    }
    check(ok, "coarse and reference-index unwrapping over 4 ms (8-bit index, coarse wraps)");

    // Release 2: 8 ns coarse ticks, 6-bit reference index (wraps every 8 us),
    // events 0.3 ... 9 us apart, each arriving 0.1 ... 1.3 us after its stop.
    TimeUnwrapper w(8.0e6, 125000, 6, 8.0);
    const std::uint64_t t2 = 0xFFFFF000ULL;
    (void)w.feed(decode_record(marker(MarkerType::start, 0, t2)));
    std::mt19937 rng(7);
    double t_ps = 125000.0 * 3 + 11.0, first2 = t_ps;
    std::uint64_t last_coarse = t2;
    ok = true;
    for (int i = 0; i < 20000 && ok; ++i) {
        t_ps += 300e3 + static_cast<double>(rng() % 8700000);
        const auto ref_full = static_cast<std::uint64_t>(t_ps / 125000.0);
        const auto stop = static_cast<std::uint32_t>(std::fmod(t_ps, 125000.0));
        std::uint64_t coarse = t2 + static_cast<std::uint64_t>((t_ps + 100e3 + static_cast<double>(rng() % 1200000)) / 8000.0);
        coarse = std::max(coarse, last_coarse);     // the FPGA keeps arrival order
        last_coarse = coarse;
        const auto ev = w.feed(decode_record(event(i % 4, static_cast<std::uint16_t>(coarse & 0xFFFFU),
                                                   static_cast<std::uint32_t>(ref_full & 0x3FU), stop)));
        if (i == 0) first2 = t_ps;
        if (!ev || std::abs(ev->time_ps - (t_ps - first2)) > 1.0) ok = false;
    }
    check(ok, "release-2 unwrapping: 8 ns ticks, 6-bit index, latency jitter up to 1.2 us");
    TimeUnwrapper v(8.0e6, 125000, 24);
    bool threw = false;
    try {
        (void)v.feed(decode_record(event(0, 0, 0, 0)));
    } catch (const std::runtime_error&) {
        threw = true;
    }
    check(threw, "a run must start with a START marker");
}

void test_session() {
    FakeFpga fpga;
    Session s(fpga, SessionOptions{}, 0x1000);
    fpga.inject_stray(100);
    const auto sync = s.sync();
    check(sync.status.status == Status::ok && sync.status.board.tmp_ok() && sync.status.tmp.id_ok(), "sync and STATUS");
    check(std::abs(sync.status.tmp.celsius() - 25.0) < 1e-9, "temperature decoding");
    auto r = s.thresholds(0x5, {1000, 0, 2000, 0});
    check(r.status == Status::ok && fpga.dac[0] == 1000 && fpga.dac[2] == 2000 && r.board.dac_codes[2] == 2000,
          "SET_THRESHOLDS");

    // Streaming needs a verified GPX2 configuration and LCLKIN.
    auto refused = s.stream({0, 0.01}, nullptr);
    check(!refused.pass && refused.failure.find("NOT_READY") != std::string::npos, "START refused before configuration");
    r = s.gpx_init();
    check(r.status == Status::not_ready, "GPX_INIT before configuration is NOT_READY");

    const GpxConfig c;
    fpga.corrupt_gpx_readback = true;
    r = s.gpx_configure(c.bytes());
    check(r.status == Status::device_error && r.gpx.mismatch == 0x80 && !r.gpx.verified(), "corrupted readback");
    fpga.corrupt_gpx_readback = false;
    r = s.gpx_configure(c.bytes());
    check(r.status == Status::ok && r.gpx.verified() && r.gpx.measuring() && r.gpx.written == c.bytes() &&
              r.lvds.format_valid && r.lvds.ref_bits == 24 && r.lvds.stop_bits == 20,
          "GPX_CONFIGURE and capture format");

    // Calibration finds a sampling point outside the simulated bad window:
    // release 2, 250 MHz SDR (two samples per 4 ns bit; bad 856 ... 1428 ps).
    CalibrationOptions o;
    o.step = 8;
    o.window_us = 200;
    o.min_frames = 10;
    int points = 0;
    auto cal = s.calibrate(c, o, [&](const CalibrationPoint&) { ++points; });
    auto outside = [](double ps, double bit) { const double f = ps / bit; return f < 0.214 || f > 0.357; };
    check(cal.success && points == 28 && cal.bit_ps == 4000.0 && outside(cal.chosen_offset_ps, cal.bit_ps) &&
              fpga.phase == cal.chosen_phase && fpga.bit_phase == cal.chosen_bit_phase && fpga.lclk_en &&
              fpga.gpx_written[6] == 0xC0 && cal.good_width_ps >= 3000,
          "calibration, 250 MHz SDR (bit phase " + std::to_string(cal.chosen_bit_phase) + ", phase " +
              std::to_string(cal.chosen_phase) + ", " + std::to_string(cal.good_width_ps) + " ps)");
    // 250 MHz DDR, 6 + 18 bits: one sample per 2 ns bit.
    GpxConfig fast;
    fast.ddr = true;
    fast.ref_index_bits = 6;
    fast.stop_bits = 18;
    points = 0;
    o.step = 4;
    cal = s.calibrate(fast, o, [&](const CalibrationPoint&) { ++points; });
    check(cal.success && points == 28 && cal.bit_ps == 2000.0 && cal.chosen_bit_phase == 0 &&
              outside(cal.chosen_offset_ps, cal.bit_ps) && cal.good_width_ps >= 1500 && fpga.gpx_written[2] == 0x36,
          "calibration, 250 MHz DDR (phase " + std::to_string(cal.chosen_phase) + ")");
    r = s.status();
    check(r.lvds.ddr && r.lvds.ref_bits == 6 && r.lvds.stop_bits == 18 && r.lvds.samples_per_bit() == 1 &&
              capture_problem(r).empty() && r.lvds.timing.release2() && r.lvds.timing.coarse_tick_ns == 8.0,
          "release-2 status: DDR capture format, clocking fields, index long enough");
    (void)s.lvds_set(true, 1, 0);
    r = s.status();
    check(!capture_problem(r).empty(), "6 index bits refused at 125 MHz DDR");
    (void)s.lvds_set(true, 0, 0);
    expect_throw([&] { (void)s.lvds_phase(224); }, "release-2 phase 224");
    r = s.gpx_configure(c.bytes());

    // Release 1: 560 phase steps, SDR only, bit phase below 2^divider.
    {
        FakeFpga old;
        old.release1();
        Session s1(old, SessionOptions{}, 0x3000);
        const auto st1 = s1.sync().status;
        check(st1.design_version == kDesignVersionR1 && !st1.lvds.timing.release2() && st1.lvds.timing.phase_steps == 560,
              "release-1 image recognised");
        expect_throw([&] { (void)s1.gpx_configure(fast.bytes()); }, "release 1 refuses DDR");
        (void)s1.gpx_configure(c.bytes());
        expect_throw([&] { (void)s1.lvds_set(true, 1, 2); }, "release 1: bit phase 2 at divider 1");
        CalibrationOptions o1;
        o1.step = 20;
        o1.window_us = 200;
        o1.min_frames = 10;
        points = 0;
        const auto cal1 = s1.calibrate(c, o1, [&](const CalibrationPoint&) { ++points; });
        check(cal1.success && points == 28 && (cal1.chosen_phase < 120 || cal1.chosen_phase > 200) &&
                  old.phase == cal1.chosen_phase && cal1.good_width_ps >= 300 * kPhaseStepPs,
              "release-1 calibration (phase " + std::to_string(cal1.chosen_phase) + ")");
    }

    // A host-timed run.
    std::vector<std::uint64_t> recs;
    auto res = s.stream({0, 0.05}, [&](std::uint64_t x) { recs.push_back(x); });
    bool order_ok = res.pass && recs.size() == res.records && recs.size() > 10;
    if (order_ok) {
        TimeUnwrapper u(8.0e6, 125000, 24, 8.0);
        try {
            for (auto x : recs) (void)u.feed(decode_record(x));
        } catch (const std::exception&) {
            order_ok = false;
        }
        order_ok = order_ok && decode_record(recs.front()).marker_type == MarkerType::start &&
                   decode_record(recs.back()).marker_type == MarkerType::end && !res.by_window;
    }
    check(order_ok, "streaming run by duration (" + res.failure + ")");
    check(res.events[0] + res.events[1] + res.events[2] + res.events[3] > 0, "events streamed");

    // A run ended by the FPGA window.
    recs.clear();
    res = s.stream({3000, 0}, [&](std::uint64_t x) { recs.push_back(x); });
    check(res.pass && res.by_window && res.final_response &&
              res.final_response->opcode == static_cast<std::uint32_t>(Opcode::run_complete),
          "streaming run ended by a 3 ms window (" + res.failure + ")");

    // Commands while streaming are BUSY; STOP ends the run.
    const auto t = s.next_transaction();
    r = s.transact(encode_start(0x77, t, 0), t);
    check(r.status == Status::ok && fpga.streaming(), "raw START");
    r = s.thresholds(1, {500, 0, 0, 0});
    check(r.status == Status::busy && fpga.dac[0] == 1000, "SET_THRESHOLDS while streaming is BUSY");
    r = s.lvds_phase(10);
    check(r.status == Status::busy, "LVDS_PHASE while streaming is BUSY");
    r = s.status();
    check(r.status == Status::ok && r.stream.streaming(), "STATUS while streaming");
    r = s.stop();
    check(r.status == Status::ok && !fpga.streaming() && !r.stream.streaming(), "STOP ends the run");

    fpga.aw_present = false;
    r = s.led1(1, 2, 3, 1);
    check(r.status == Status::device_error && !r.board.aw_ok(), "LED1 device error");
}

// Release 3 compact records (PROTOCOL.md 3.4): the packer (the FPGA's, in
// software) and the expander against each other, malformed word streams,
// compact DATA frames, and compact runs on the simulated FPGA.
void test_compact() {
    // Records: START, then events with reference indices crossing and
    // re-crossing epochs (bits 23:12), stops in the marker space (dropped),
    // TIME / OVERFLOW markers, END; enough for several frames, with markers
    // and epoch changes landing at every position near a frame's end.
    std::mt19937_64 rng(7);
    std::vector<std::uint64_t> in;
    std::uint64_t coarse = 0x123456789AULL;
    in.push_back(marker(MarkerType::start, 0, coarse));
    std::size_t invalid = 0;
    for (int i = 0; i < 6000; ++i) {
        const int kind = static_cast<int>(rng() % 23);
        if (kind == 0) {
            coarse += 16384;
            in.push_back(marker(MarkerType::time, 0, coarse));
        } else if (kind == 1) {
            coarse += 3;
            in.push_back(marker(MarkerType::overflow, static_cast<std::uint16_t>(1 + rng() % 4095), coarse));
        } else {
            std::uint32_t ref = static_cast<std::uint32_t>(rng() & 0xFFFFFFU);
            if (kind < 12) ref = (static_cast<std::uint32_t>(i / 5 % 3) << 12U) | (ref & 0xFFFU);   // alternating epochs
            std::uint32_t stop = static_cast<std::uint32_t>(rng() % 125000);
            if (kind == 2) { stop = 0x3F000U | static_cast<std::uint32_t>(rng() & 0xFFFU); ++invalid; }
            if (kind == 3) stop = 0x3EFFFU;                     // largest stop that is not a marker word
            in.push_back(event(static_cast<int>(rng() % 4), 0, ref, stop));
        }
    }
    in.push_back(marker(MarkerType::end, 1, coarse + 5));
    // Pack in builds of random sizes (as the FPGA builds frames from what waits).
    CompactPacker packer;
    std::vector<CompactFrame> frames;
    for (std::size_t i = 0; i < in.size();) {
        const std::size_t n = std::min<std::size_t>(in.size() - i, 1 + rng() % 1500);
        packer.pack(in.data() + i, n, frames);
        i += n;
    }
    bool frames_ok = packer.invalid() == invalid;
    std::size_t full = 0;
    for (const auto& f : frames) {
        if (f.used > kCompactWordsPerFrame) frames_ok = false;
        for (std::size_t w = f.used; w < kCompactWordsPerFrame; ++w)
            if (f.words[w] != kCompactPad) frames_ok = false;
        for (std::size_t w = 0; w < f.used; ++w)
            if (f.words[w] == kCompactPad) frames_ok = false;
        if (f.used >= kCompactWordsPerFrame - 2) ++full;
    }
    check(frames_ok && frames.size() > 8 && full > 3,
          "compact packer: PAD tails only, invalid stops dropped (" + std::to_string(packer.invalid()) + ")");
    CompactExpander ex;
    std::vector<std::uint64_t> out;
    std::string error;
    bool expand_ok = true;
    for (const auto& f : frames)
        if (!ex.expand(f.words.data(), f.used, out, error)) {
            expand_ok = false;
            break;
        }
    // Expected: the input without the invalid events; an event gets the
    // last marker's coarse time in its low 16 bits.
    std::vector<std::uint64_t> want;
    std::uint64_t last_coarse = 0;
    for (const auto r : in) {
        if (r >> 63U) {
            last_coarse = r & 0xFFFFFFFFFFFFULL;
            want.push_back(r);
        } else if (((r & 0xFFFFFU) >> 12U) != 0x3FU) {
            want.push_back(r | ((last_coarse & 0xFFFFU) << 44U));
        }
    }
    check(expand_ok && out == want, "compact expand(pack(records)) gives the records back (" + error + ")");

    // Malformed word streams.
    auto bad = [&](std::vector<std::uint32_t> w, const std::string& what) {
        CompactExpander e;
        std::vector<std::uint64_t> o;
        std::string err;
        check(!e.expand(w.data(), w.size(), o, err) && !err.empty(), "compact: " + what + " is refused");
    };
    const std::uint32_t head_start = 0xFF000000U, hi = 0xFD000000U, lo = 0xFE000000U, ep = 0xFC000000U;
    bad({0x00004001U}, "an event before any EPOCH");
    bad({head_start, hi, lo, ep}, "an EPOCH at the end of a frame");
    bad({ep, head_start, hi, lo}, "an EPOCH followed by a marker");
    bad({head_start, hi}, "a marker without its LO word");
    bad({head_start, lo, hi}, "HEAD LO HI");
    bad({hi}, "a lone HI word");
    bad({0xFF000000U | (5U << 21U), hi, lo}, "marker type 5");
    bad({0xFF000001U, hi, lo}, "a HEAD with reserved bits");
    bad({ep | 0x1000U, 0x00004001U}, "an EPOCH with reserved bits");
    bad({kCompactPad}, "PAD inside the payload");

    // Compact DATA frames.
    auto data_frame = [](const CompactFrame& f, std::uint32_t length) {
        std::vector<std::uint8_t> payload(4 * kCompactWordsPerFrame);
        for (std::size_t w = 0; w < kCompactWordsPerFrame; ++w)
            for (std::size_t b = 0; b < 4; ++b) payload[4 * w + b] = static_cast<std::uint8_t>(f.words[w] >> (8U * b));
        au2ft601::FrameHeader h;
        h.type = au2ft601::FrameType::data;
        h.run_id = 5;
        h.pattern_or_opcode = 2;
        h.valid_payload_length = length;
        return au2ft601::encode_frame(h, payload.data(), std::min<std::size_t>(payload.size(), length));
    };
    auto d = decode_data_frame(data_frame(frames[1], 4028), error);
    check(d && d->format == 2 && d->words.size() == frames[1].used &&
              std::equal(d->words.begin(), d->words.end(), frames[1].words.begin()),
          "compact DATA frame decodes (" + error + ")");
    CompactFrame tail = frames.back();
    tail.words[kCompactWordsPerFrame - 1] = 0x00004001U;
    check(!decode_data_frame(data_frame(tail, 4028), error), "compact DATA frame: a word after the PAD tail is refused");
    check(!decode_data_frame(data_frame(frames[0], 4024), error), "compact DATA frame: payload length 4024 is refused");

    // Compact runs on the simulated FPGA (release 3): events known to the
    // test come back with exact reference index and stop result.
    FakeFpga fpga;
    Session s(fpga, SessionOptions{}, 0x5000);
    (void)s.sync();
    std::vector<std::pair<std::uint64_t, int>> produced;
    fpga.event_source = [&](std::uint64_t t0, std::uint64_t t1, std::vector<std::pair<std::uint64_t, int>>& ev) {
        for (std::uint64_t t = (t0 / 977777 + 1) * 977777; t < t1; t += 977777) {   // every 0.98 us
            ev.emplace_back(t, static_cast<int>((t / 977777) % 4));
            produced.emplace_back(t, static_cast<int>((t / 977777) % 4));
        }
    };
    GpxConfig c6;
    c6.ddr = true;
    c6.ref_index_bits = 6;
    c6.stop_bits = 18;
    (void)s.gpx_configure(c6.bytes());
    (void)s.lvds_set(true, 0, 0);
    StreamRequest req;
    req.duration_s = 0.02;
    req.compact = true;
    auto res = s.stream(req, nullptr);
    check(!res.pass && res.failure.find("NOT_READY") != std::string::npos, "compact START refused with a 6-bit reference index");
    GpxConfig c24;
    c24.ddr = true;
    c24.ref_index_bits = 24;
    c24.stop_bits = 18;
    auto st = s.gpx_configure(c24.bytes());
    check(compact_problem(st).empty() && st.stream.record_format == 1, "24 + 18 bits allow compact records");
    produced.clear();
    std::vector<std::uint64_t> recs;
    res = s.stream(req, [&](std::uint64_t x) { recs.push_back(x); });
    std::size_t events = 0, matched = 0;
    for (const auto x : recs) {
        if (x >> 63U) continue;
        const auto r = decode_record(x);
        if (events < produced.size()) {
            const auto [t, ch] = produced[events];
            if (r.channel == ch && r.ref_index == ((t / 125000) & 0xFFFFFFU) && r.stop == t % 125000) ++matched;
        }
        ++events;
    }
    check(res.pass && res.compact && res.final_response && res.final_response->stream.record_format == 2 &&
              res.records < recs.size() * 2 && events > 1000 && matched == events && events <= produced.size() &&
              events + 4 >= produced.size(),   // events still in flight when the run ends are not sent
          "compact run on the simulated FPGA: " + std::to_string(matched) + " of " + std::to_string(events) +
              " events exact, " + std::to_string(produced.size()) + " produced (" + res.failure + ")");
    req.compact = false;
    res = s.stream(req, nullptr);
    check(res.pass && !res.compact && res.final_response && res.final_response->stream.record_format == 1,
          "a 64-bit run after a compact one");
    FakeFpga r2;
    r2.release2();
    Session s2(r2, SessionOptions{}, 0x6000);
    (void)s2.sync();
    req.compact = true;
    expect_throw([&] { (void)s2.stream(req, nullptr); }, "compact records on a release-2 image");
}

// Histogram semantics (Swabian Histogram / StartStop) on hand-made clicks,
// and a run file written by a session and loaded back.
void test_run_files() {
    Measurement m;
    m.times_ps[0] = {0, 100};
    m.times_ps[1] = {50, 150, 260};
    m.times_ps[2] = {0, 10, 30};
    m.times_ps[3] = {5};
    HistogramRequest q;
    q.start_channel = 0;
    q.stop_channel = 1;
    q.binwidth_ps = 10;
    q.n_bins = 30;
    auto h = compute_histogram(m, q);
    check(h.in_range == 5 && h.counts[5] == 2 && h.counts[15] == 1 && h.counts[16] == 1 && h.counts[26] == 1 &&
              h.starts == 2 && h.stops == 3,
          "Histogram: multiple start, multiple stop");
    q.mode = HistogramMode::start_stop;
    h = compute_histogram(m, q);
    check(h.in_range == 2 && h.counts[5] == 2 && h.outside == 0, "StartStop: each start used once");
    q.n_bins = 3;   // range 30 ps
    h = compute_histogram(m, q);
    check(h.in_range == 0 && h.outside == 2, "StartStop: pairs beyond the range");
    q.mode = HistogramMode::histogram;
    h = compute_histogram(m, q);
    check(h.in_range == 0, "Histogram: pairs beyond the range are not counted");
    q.start_channel = q.stop_channel = 2;
    q.n_bins = 30;
    h = compute_histogram(m, q);
    check(h.in_range == 3 && h.counts[0] == 0 && h.counts[1] == 1 && h.counts[2] == 1 && h.counts[3] == 1,
          "Histogram on one channel: no click pairs with itself");
    q.mode = HistogramMode::start_stop;
    h = compute_histogram(m, q);
    check(h.in_range == 2 && h.counts[1] == 1 && h.counts[2] == 1, "StartStop on one channel: successive intervals");
    q.mode = HistogramMode::histogram;
    q.start_channel = 3;
    q.stop_channel = 3;
    m.times_ps[2] = {5};
    q.start_channel = 2;
    h = compute_histogram(m, q);
    check(h.in_range == 1 && h.counts[0] == 1, "equal times on two channels count as a zero delay");
    // START/STOP delays slide the histogram: STOP +20 ps moves every pair by
    // +20 ps; START +60 ps (shift -60) brings a STOP 50 ps before its START
    // into view; on one channel a click still never pairs with itself.
    Measurement d;
    d.times_ps[0] = {0, 100};
    d.times_ps[1] = {50, 150};
    HistogramRequest qd;
    qd.start_channel = 0;
    qd.stop_channel = 1;
    qd.binwidth_ps = 10;
    qd.n_bins = 30;
    qd.stop_delay_ps = 20;
    h = compute_histogram(d, qd);
    check(h.in_range == 3 && h.counts[7] == 2 && h.counts[17] == 1, "STOP delay slides the histogram");
    qd.mode = HistogramMode::start_stop;
    h = compute_histogram(d, qd);
    check(h.in_range == 2 && h.counts[7] == 2, "STOP delay in StartStop mode");
    qd.mode = HistogramMode::histogram;
    d.times_ps[0] = {100};
    d.times_ps[1] = {50};
    qd.stop_delay_ps = 60;
    h = compute_histogram(d, qd);
    check(h.in_range == 1 && h.counts[1] == 1, "a STOP before its START shifted into range");
    qd.stop_delay_ps = 0;
    qd.start_delay_ps = 60;
    d.times_ps[0] = {0};
    d.times_ps[1] = {100};
    h = compute_histogram(d, qd);
    check(h.in_range == 1 && h.counts[4] == 1, "START delay slides the histogram left");
    qd.start_channel = qd.stop_channel = 0;
    qd.start_delay_ps = 0;
    qd.stop_delay_ps = 5;
    d.times_ps[0] = {0, 10};
    h = compute_histogram(d, qd);
    check(h.in_range == 1 && h.counts[1] == 1, "one channel with a delay: no click pairs with itself");
    q.binwidth_ps = 0;
    expect_throw([&] { (void)compute_histogram(m, q); }, "bin width 0");

    // Session -> run file -> load_measurement.
    FakeFpga fpga;
    Session s(fpga, SessionOptions{}, 0x2000);
    (void)s.sync();
    const GpxConfig c;
    (void)s.gpx_configure(c.bytes());
    (void)s.lvds_set(true, 0, 0);
    const auto before = s.status();
    const auto path = (std::filesystem::temp_directory_path() / "tdc_tests_run.bin").string();
    std::ofstream out(path, std::ios::binary);
    const auto res = s.stream({3000, 0}, [&](std::uint64_t x) {
        char b[8];
        for (int i = 0; i < 8; ++i) b[i] = static_cast<char>((x >> (8 * i)) & 0xFFU);
        out.write(b, 8);
    });
    out.close();
    write_run_sidecar(path, before, res, 8.0e6);
    const auto loaded = load_measurement(path);
    bool same = res.pass && loaded.records == res.records && loaded.end_seen && loaded.ended_by_window &&
                loaded.settings.have_sidecar && loaded.settings.divisions == 125000 && loaded.warning.empty();
    for (int ch = 0; ch < kChannels; ++ch) same = same && loaded.events(ch) == res.events[static_cast<std::size_t>(ch)];
    check(same, "run file round trip (" + loaded.warning + ")");
    check(std::abs(loaded.run_s - 0.003) < 0.0002, "run length from the marker clock (" + std::to_string(loaded.run_s) + " s)");
    std::filesystem::remove(path);
    std::filesystem::remove(path + ".json");
}

// The incremental accumulator must equal the static reference for every
// shift it covers: both modes, swapped and shared channels, equal times.
void test_accumulator() {
    std::mt19937_64 rng(7);
    Measurement m;
    for (int c = 0; c < 3; ++c) {
        std::uniform_int_distribution<std::int64_t> t(0, 200000);
        auto& v = m.times_ps[static_cast<std::size_t>(c)];
        for (int i = 0; i < 400; ++i) v.push_back(t(rng) / 50 * 50);   // coarse grid: some equal times
        std::sort(v.begin(), v.end());
    }
    int cases = 0, mismatches = 0;
    for (auto mode : {HistogramMode::histogram, HistogramMode::start_stop}) {
        for (auto [a, b] : {std::pair<int, int>{0, 1}, {1, 0}, {2, 2}}) {
            for (std::int64_t centre : {0LL, 3000LL, -2000LL}) {
                HistogramSettings hs;
                hs.mode = mode;
                hs.start_channel = a;
                hs.stop_channel = b;
                hs.binwidth_ps = 100;
                hs.n_bins = 60;   // R = 6000 ps
                HistogramAccumulator acc(hs, centre);
                acc.begin_segment();
                // merge in time order, START channel first at equal times
                std::vector<std::pair<std::int64_t, int>> ev;
                for (auto t : m.times_ps[static_cast<std::size_t>(a)]) ev.push_back({t, a});
                if (b != a)
                    for (auto t : m.times_ps[static_cast<std::size_t>(b)]) ev.push_back({t, b});
                std::sort(ev.begin(), ev.end(), [a = a](const auto& x, const auto& y) {
                    return x.first < y.first || (x.first == y.first && (x.second == a) > (y.second == a));
                });
                for (const auto& [t, c] : ev) acc.add(c, t);
                for (std::int64_t shift = centre - 6000; shift <= centre + 6000; shift += 1500) {
                    HistogramRequest q;
                    q.mode = mode;
                    q.start_channel = a;
                    q.stop_channel = b;
                    q.binwidth_ps = 100;
                    q.n_bins = 60;
                    q.stop_delay_ps = shift;
                    const auto ref = compute_histogram(m, q);
                    std::vector<std::int64_t> left;
                    std::vector<std::uint64_t> counts;
                    acc.shown_bins(shift, left, counts);
                    ++cases;
                    if (counts != ref.counts || left.empty() || left.front() != 0) ++mismatches;
                    // Bidirectional window: [origin, origin + R) at shift = [0, R) at shift - origin, moved by origin.
                    const std::int64_t origin = histogram_origin(hs, true);
                    if (acc.covers(shift + origin, origin) != acc.covers(shift)) ++mismatches;
                    std::vector<std::int64_t> left_bi;
                    std::vector<std::uint64_t> counts_bi;
                    acc.shown_bins(shift + origin, left_bi, counts_bi, origin);
                    ++cases;
                    bool same = counts_bi == counts && left_bi.size() == left.size();
                    for (std::size_t i = 0; same && i < left.size(); ++i) same = left_bi[i] == left[i] + origin;
                    if (!same) ++mismatches;
                }
            }
        }
    }
    check(mismatches == 0, "accumulator equals the static histogram (" + std::to_string(mismatches) + " of " +
                               std::to_string(cases) + " cases differ)");

    // Segments do not pair across runs; clear() keeps the settings.
    HistogramSettings hs;
    hs.binwidth_ps = 10;
    hs.n_bins = 10;
    HistogramAccumulator acc(hs, 0);
    acc.begin_segment();
    acc.add(0, 0);
    acc.add(0, 900);
    acc.begin_segment();
    acc.add(1, 50);
    check(acc.totals().pairs == 0 && acc.totals().segments == 2 && acc.totals().late == 0,
          "no pairs across segments, and a new segment's earlier times are not late");
    acc.add(0, 60);
    acc.add(1, 75);
    check(acc.totals().pairs == 2, "pairs within a segment (incl. the -10 ps pair in the window)");
    acc.clear();
    check(acc.totals().pairs == 0 && acc.shown(0, 100).total == 0, "clear");
    check(acc.covers(100) && !acc.covers(101), "covered shifts");

    // Bidirectional: a STOP 30 ps before its START lands in the bin [-30, -20) ps.
    {
        HistogramSettings bs;
        bs.binwidth_ps = 10;
        bs.n_bins = 10;
        check(histogram_origin(bs, true) == -50 && histogram_origin(bs, false) == 0, "bidirectional origin = -(bins/2) x width");
        HistogramSettings ss = bs;
        ss.mode = HistogramMode::start_stop;
        check(histogram_origin(ss, true) == 0, "StartStop has no negative side");
        HistogramAccumulator bi(bs, -histogram_origin(bs, true));   // centred for shift 0, bidirectional
        bi.begin_segment();
        bi.add(1, 1000);   // STOP
        bi.add(0, 1030);   // START
        bi.add(1, 1045);   // STOP 15 ps after it
        std::vector<std::int64_t> left;
        std::vector<std::uint64_t> counts;
        bi.shown_bins(0, left, counts, -50);
        std::uint64_t at_m30 = 0, at_10 = 0, total = 0;
        for (std::size_t i = 0; i < left.size(); ++i) {
            total += counts[i];
            if (left[i] == -30) at_m30 = counts[i];
            if (left[i] == 10) at_10 = counts[i];
        }
        check(left.size() == 10 && left.front() == -50 && at_m30 == 1 && at_10 == 1 && total == 2,
              "bidirectional: STOP before START in a negative bin, STOP after START in a positive one");
        bi.shown_bins(0, left, counts, 0);
        std::uint64_t one_way = 0;
        for (auto c : counts) one_way += c;
        check(left.front() == 0 && one_way == 1 && bi.covers(0, 0) && bi.covers(0, -50), "one direction: only the positive pair");
    }
    expect_throw([] { HistogramSettings bad; bad.n_bins = 0; HistogramAccumulator x(bad, 0); }, "zero bins");
}

// 2D histogram (Swabian Histogram2D): hand-made clicks, and the accumulator
// against a direct formulation (for each START, the first STOP1 and STOP2
// before the next START) for every covered, bin-aligned shift.
void test_histogram2d() {
    Histogram2DSettings hs;
    hs.start_channel = 0;
    hs.stop1_channel = 1;
    hs.stop2_channel = 2;
    hs.binwidth1_ps = 10;
    hs.binwidth2_ps = 20;
    hs.n_bins1 = 10;   // R1 = 100 ps
    hs.n_bins2 = 10;   // R2 = 200 ps
    auto count_at = [](const Histogram2DAccumulator& a, std::int64_t s1, std::int64_t s2, std::int64_t x, std::int64_t y) {
        std::vector<std::int64_t> l1, l2;
        std::vector<std::uint64_t> c;
        a.shown_bins(s1, s2, l1, l2, c);
        std::uint64_t n = 0;
        for (std::size_t i = 0; i < c.size(); ++i)
            if (l1[i] == x && l2[i] == y) n += c[i];
        return n;
    };
    {
        Histogram2DAccumulator a(hs, 0, 0);
        a.begin_segment();
        a.add(0, 1000);   // START
        a.add(2, 1050);   // STOP2 first: order of the axes does not matter
        a.add(1, 1030);   // late by 20 ps (processed anyway)
        a.add(1, 1060);   // a second STOP1: the START is used up
        check(a.totals().pairs == 1 && count_at(a, 0, 0, 30, 40) == 1 && a.totals().late == 1,
              "2D: first STOP1 and STOP2 after the START give the bin");
        a.add(0, 2000);
        a.add(1, 2010);
        a.add(0, 2100);   // before any STOP2: replaces the waiting START
        a.add(1, 2120);
        a.add(2, 2170);
        check(a.totals().abandoned == 1 && a.totals().pairs == 2 && count_at(a, 0, 0, 20, 60) == 1,
              "2D: a new START replaces one still waiting");
        a.add(0, 3000);
        a.add(1, 3005);
        a.add(2, 3900);   // d2 = 900 ps: beyond the window (base2 + 3 R2 = 400 ps)
        check(a.totals().outside == 1 && a.totals().pairs == 2, "2D: a pair beyond the window is outside");
        check(count_at(a, 50, 0, 80, 40) == 1 && count_at(a, 0, -20, 30, 20) == 1, "2D: delays slide each axis");
        check(a.covers(100, 200) && !a.covers(101, 0) && !a.covers(0, -201), "2D: covered shifts");
        a.add(0, 5000);
        a.add(1, 5010);
        a.begin_segment();
        a.add(2, 20);
        check(a.totals().pairs == 2 && a.totals().segments == 2, "2D: no pairs across segments");
        const auto sh = a.shown(0, 0, 4);   // 10 x 10 bins shown as 4 x 4 cells of 3 x 3
        double sum = 0;
        for (double v : sh.values) sum += v;
        check(sh.cols == 4 && sh.rows == 4 && sh.merged1 == 3 && sh.merged2 == 3 && sum == 2 && sh.total == 2 &&
                  sh.x0_ps == 0 && sh.x1_ps == 120 && sh.y1_ps == 240 && sh.values[0 * 4 + 1] == 1 && sh.values[1 * 4 + 0] == 1 &&
                  sh.peak == 1,
              "2D: display cells merge bins (row = axis 2 from the bottom)");
        a.clear();
        check(a.totals().pairs == 0 && a.shown(0, 0, 100).total == 0, "2D: clear");
    }
    {   // START = STOP1: a click is STOP1 of the previous START, then a new START
        Histogram2DSettings ss = hs;
        ss.stop1_channel = 0;
        Histogram2DAccumulator a(ss, 0, 0);
        a.begin_segment();
        a.add(0, 0);
        a.add(2, 50);
        a.add(0, 70);
        check(a.totals().pairs == 1 && count_at(a, 0, 0, 70, 40) == 1 && a.totals().starts == 2 && a.totals().stops1 == 2,
              "2D: START = STOP1 pairs with the next click, never with itself");
        Histogram2DSettings dd = hs;
        dd.stop2_channel = 1;
        Histogram2DAccumulator b(dd, 0, 0);
        b.begin_segment();
        b.add(0, 0);
        b.add(1, 45);
        check(b.totals().pairs == 1 && count_at(b, 0, 0, 40, 40) == 1, "2D: STOP1 = STOP2 fills both axes (diagonal)");
    }

    std::mt19937_64 rng(11);
    std::array<std::vector<std::int64_t>, 3> times;
    for (int c = 0; c < 3; ++c) {
        std::uniform_int_distribution<std::int64_t> t(0, 400000);
        for (int i = 0; i < (c == 0 ? 300 : 900); ++i) times[static_cast<std::size_t>(c)].push_back(t(rng) / 50 * 50);
        std::sort(times[static_cast<std::size_t>(c)].begin(), times[static_cast<std::size_t>(c)].end());
    }
    int cases = 0, mismatches = 0;
    for (auto [c1, c2] : {std::pair<int, int>{1, 2}, {2, 1}, {1, 1}}) {
        for (auto [k1, k2] : {std::pair<std::int64_t, std::int64_t>{0, 0}, {1000, -500}, {-1500, 2000}}) {
            Histogram2DSettings rs;
            rs.start_channel = 0;
            rs.stop1_channel = c1;
            rs.stop2_channel = c2;
            rs.binwidth1_ps = 100;
            rs.binwidth2_ps = 250;
            rs.n_bins1 = 30;   // R1 = 3000 ps
            rs.n_bins2 = 16;   // R2 = 4000 ps
            Histogram2DAccumulator a(rs, k1, k2);
            a.begin_segment();
            std::vector<std::pair<std::int64_t, int>> ev;
            for (int c : {0, c1, c2})
                if (c == 0 || (c == c1 && c != 0) || (c == c2 && c != c1))
                    for (auto t : times[static_cast<std::size_t>(c)]) ev.push_back({t, c});
            std::sort(ev.begin(), ev.end(), [](const auto& x, const auto& y) {
                return x.first < y.first || (x.first == y.first && (x.second == 0) > (y.second == 0));
            });
            for (const auto& [t, c] : ev) a.add(c, t);
            // direct: (d1, d2) for every START whose both STOPs come before the next START
            std::vector<std::pair<std::int64_t, std::int64_t>> pairs;
            const auto& st = times[0];
            for (std::size_t i = 0; i < st.size(); ++i) {
                const std::int64_t next = i + 1 < st.size() ? st[i + 1] : INT64_MAX;
                auto first = [&](int c) -> std::int64_t {
                    const auto& v = times[static_cast<std::size_t>(c)];
                    const auto it = std::lower_bound(v.begin(), v.end(), st[i]);
                    return it != v.end() && *it < next ? *it - st[i] : -1;
                };
                const std::int64_t d1 = first(c1), d2 = first(c2);
                if (d1 >= 0 && d2 >= 0) pairs.push_back({d1, d2});
            }
            for (std::int64_t s1 = k1 - 3000; s1 <= k1 + 3000; s1 += 1500) {
                for (std::int64_t s2 = k2 - 4000; s2 <= k2 + 4000; s2 += 2000) {
                    std::vector<std::uint64_t> ref(30 * 16, 0);
                    for (const auto& [d1, d2] : pairs) {
                        const std::int64_t x = d1 + s1, y = d2 + s2;
                        if (x >= 0 && x < 3000 && y >= 0 && y < 4000) ++ref[static_cast<std::size_t>((y / 250) * 30 + x / 100)];
                    }
                    std::vector<std::int64_t> l1, l2;
                    std::vector<std::uint64_t> counts;
                    a.shown_bins(s1, s2, l1, l2, counts);
                    ++cases;
                    if (!a.covers(s1, s2) || counts != ref || l1.front() != 0 || l2.front() != 0 || l1.back() != 2900 ||
                        l2.back() != 3750)
                        ++mismatches;
                }
            }
            if (a.totals().pairs + a.totals().outside != pairs.size() ||
                a.totals().pairs + a.totals().outside + a.totals().abandoned > a.totals().starts)
                ++mismatches;
        }
    }
    check(mismatches == 0, "2D accumulator equals the direct formulation (" + std::to_string(mismatches) + " of " +
                               std::to_string(cases) + " cases differ)");
    expect_throw([] { Histogram2DSettings bad; bad.n_bins1 = 0; Histogram2DAccumulator x(bad, 0, 0); }, "2D: zero bins");
    expect_throw([] { Histogram2DSettings bad; bad.n_bins1 = 1024; bad.n_bins2 = 512; Histogram2DAccumulator x(bad, 0, 0); },
                 "2D: too many bins");
    expect_throw([] { Histogram2DSettings bad; bad.stop2_channel = 4; Histogram2DAccumulator x(bad, 0, 0); }, "2D: channel 5");
}

// 1D projection of 2D bins: plain sums at 0 and 90 degrees; the total and the
// mean (u of the bin centres) kept at every angle; footprint spreading.
void test_projection() {
    std::mt19937_64 rng(5);
    std::vector<std::int64_t> l1, l2;
    std::vector<std::uint64_t> n;
    const int nx = 12, ny = 7;
    const std::int64_t w1 = 30, w2 = 50, x0 = -45, y0 = 100;   // x edges not on a multiple of w1
    std::uniform_int_distribution<int> cnt(0, 40);
    for (int j = 0; j < ny; ++j)
        for (int i = 0; i < nx; ++i) {
            l1.push_back(x0 + i * w1);
            l2.push_back(y0 + j * w2);
            n.push_back(static_cast<std::uint64_t>(cnt(rng)));
        }
    double total = 0, sx = 0, sy = 0;
    std::vector<double> col(nx, 0), row(ny, 0);
    for (std::size_t k = 0; k < n.size(); ++k) {
        const double v = static_cast<double>(n[k]);
        total += v;
        sx += v * (static_cast<double>(l1[k]) + w1 / 2.0);
        sy += v * (static_cast<double>(l2[k]) + w2 / 2.0);
        col[k % nx] += v;
        row[k / nx] += v;
    }
    auto close = [](double a, double b) { return std::fabs(a - b) <= 1e-9 * std::max(1.0, std::fabs(b)); };

    auto p = project_2d(l1, l2, n, w1, w2, 0);
    bool ok = p.counts.size() == nx && p.bin_ps == w1 && p.u0_ps == x0;
    for (int i = 0; ok && i < nx; ++i) ok = close(p.counts[static_cast<std::size_t>(i)], col[static_cast<std::size_t>(i)]);
    check(ok, "projection at 0 deg = counts summed over y (onto x)");
    p = project_2d(l1, l2, n, w1, w2, 90);
    ok = p.counts.size() == ny && p.bin_ps == w2 && p.u0_ps == y0;
    for (int j = 0; ok && j < ny; ++j) ok = close(p.counts[static_cast<std::size_t>(j)], row[static_cast<std::size_t>(j)]);
    check(ok, "projection at 90 deg = counts summed over x (onto y)");
    p = project_2d(l1, l2, n, w1, w2, -90);
    ok = p.counts.size() == ny && p.u0_ps == -(y0 + ny * w2);
    for (int j = 0; ok && j < ny; ++j) ok = close(p.counts[static_cast<std::size_t>(j)], row[static_cast<std::size_t>(ny - 1 - j)]);
    check(ok, "projection at -90 deg = onto -y (mirrored)");

    int bad = 0;
    for (double deg : {-89.99, -73.0, -45.0, -1.0, 0.5, 10.0, 33.3, 45.0, 60.0, 89.9}) {
        const auto q = project_2d(l1, l2, n, w1, w2, deg);
        double sum = 0, mean = 0;
        for (std::size_t k = 0; k < q.counts.size(); ++k) {
            if (q.counts[k] < -1e-9) ++bad;
            sum += q.counts[k];
            mean += q.counts[k] * (q.u0_ps + (static_cast<double>(k) + 0.5) * q.bin_ps);
        }
        const double r = deg * 3.14159265358979323846 / 180;
        const double want = (sx * std::cos(r) + sy * std::sin(r)) / total;
        // binning the spread counts moves the mean by at most half a projection bin; spreading itself keeps it
        if (!close(sum, total) || !close(q.total, total) || std::fabs(mean / sum - want) > q.bin_ps / 2) ++bad;
    }
    check(bad == 0, "projection keeps the total and the mean at every angle (" + std::to_string(bad) + " bad)");

    // One 10 x 10 ps bin at 45 deg: a triangle 14.1 ps wide, half in each of two 7.07 ps bins.
    p = project_2d({0}, {0}, {100}, 10, 10, 45);
    check(p.counts.size() == 2 && close(p.counts[0], 50) && close(p.counts[1], 50) && std::fabs(p.bin_ps - 7.0710678) < 1e-6,
          "projection spreads a bin over its footprint (45 deg)");
    // Smooth in the angle: a 0.5 deg step moves no projection bin by more than a few percent of the peak.
    {
        const auto a = project_2d(l1, l2, n, w1, w1, 30.0), b = project_2d(l1, l2, n, w1, w1, 30.5);
        double peak = 0, step = 0;
        for (std::size_t k = 0; k < std::min(a.counts.size(), b.counts.size()); ++k) {
            peak = std::max(peak, a.counts[k]);
            step = std::max(step, std::fabs(a.counts[k] - b.counts[k]));
        }
        check(step < 0.1 * peak, "projection changes smoothly with the angle");
    }
    expect_throw([&] { (void)project_2d(l1, l2, n, w1, w2, 91); }, "projection angle 91");
    expect_throw([&] { (void)project_2d(l1, l2, {1}, w1, w2, 0); }, "projection: bins and counts differ");
}

// Gaussian fit: recovers known parameters from Poisson-noisy histograms
// (narrow, merged-wide and few-bin peaks) and refuses empty data.
void test_gauss_fit() {
    std::mt19937_64 rng(11);
    auto make = [&](std::int64_t bw, int n, std::int64_t first, double a, double mu, double sigma, double b,
                    std::vector<std::int64_t>& left, std::vector<std::uint64_t>& counts) {
        left.clear();
        counts.clear();
        for (int i = 0; i < n; ++i) {
            const std::int64_t x = first + i * bw;
            const double c = static_cast<double>(x) + 0.5 * static_cast<double>(bw);
            const double d = (c - mu) / sigma;
            std::poisson_distribution<long long> pois(a * std::exp(-0.5 * d * d) + b);
            left.push_back(x);
            counts.push_back(static_cast<std::uint64_t>(pois(rng)));
        }
    };
    std::vector<std::int64_t> left;
    std::vector<std::uint64_t> counts;
    auto within = [](double got, double want, double err, double n) { return std::abs(got - want) <= n * err && err > 0; };

    make(1, 2000, -1000, 500, 12.3, 8.0, 5, left, counts);
    auto f = fit_gaussian(left, counts, 1, -1e9, 1e9);
    check(f.ok && within(f.centre_ps, 12.3, f.centre_err_ps, 4) && within(f.sigma_ps, 8.0, f.sigma_err_ps, 4) &&
              within(f.background, 5, f.background_err, 4) && f.merged == 1 && f.chi2_dof > 0.7 && f.chi2_dof < 1.4,
          "Gaussian fit recovers centre, sigma and background (" + std::to_string(f.centre_ps) + ", " +
              std::to_string(f.sigma_ps) + ", " + std::to_string(f.background) + ", chi2/dof " + std::to_string(f.chi2_dof) + ")");
    check(std::abs(f.fwhm_ps() - 2.3548200450309493 * f.sigma_ps) < 1e-9, "FWHM = 2 sqrt(2 ln 2) sigma");

    make(1, 200000, -100000, 50, -2500, 5000, 1, left, counts);   // wide: fitted on merged points
    f = fit_gaussian(left, counts, 1, -1e9, 1e9);
    check(f.ok && f.merged > 1 && f.points <= kMaxFitPoints && within(f.centre_ps, -2500, f.centre_err_ps, 4) &&
              within(f.sigma_ps, 5000, f.sigma_err_ps, 4),
          "Gaussian fit of a wide peak on merged points (" + std::to_string(f.sigma_ps) + " ps, " + std::to_string(f.merged) + " bins per point)");

    make(10, 400, 0, 2000, 1503, 7, 0, left, counts);   // under one bin wide
    f = fit_gaussian(left, counts, 10, -1e9, 1e9);
    check(f.ok && std::abs(f.centre_ps - 1503) < 3 && f.sigma_ps > 3 && f.sigma_ps < 12, "Gaussian fit of a peak about one bin wide");

    make(1, 2000, -1000, 500, 12.3, 8.0, 5, left, counts);   // two peaks; the view holds the smaller one
    const auto first = counts;
    make(1, 2000, -1000, 300, 600, 8.0, 0, left, counts);
    for (std::size_t i = 0; i < counts.size(); ++i) counts[i] += first[i];
    f = fit_gaussian(left, counts, 1, 400, 800);
    check(f.ok && std::abs(f.centre_ps - 600) < 2 && f.lo_ps >= 400 && f.hi_ps <= 800, "Gaussian fit uses the bins in view only");

    // A comb of 1 ps-wide peaks every 44 ps under a 30 ps envelope (the GPX2's steps): the tallest-peak fit
    // finds one tooth, the all-in-view fit the envelope (sigma = the spread of all the counts).
    {
        left.clear();
        counts.clear();
        std::vector<double> mean(1200, 0.0);
        for (int k = -15; k <= 15; ++k) {
            const double tooth = 13.0 + 44.0 * k;
            const double weight = 4000 * std::exp(-0.5 * (tooth - 20) * (tooth - 20) / (30.0 * 30.0));
            for (int i = 0; i < 1200; ++i) {
                const double d = (i - 600 + 0.5 - tooth) / 1.0;
                mean[static_cast<std::size_t>(i)] += weight * std::exp(-0.5 * d * d);
            }
        }
        double s = 0, sx = 0, sxx = 0;
        for (int i = 0; i < 1200; ++i) {
            std::poisson_distribution<long long> pois(mean[static_cast<std::size_t>(i)] + 0.2);
            left.push_back(i - 600);
            counts.push_back(static_cast<std::uint64_t>(pois(rng)));
            const double x = i - 600 + 0.5, m = mean[static_cast<std::size_t>(i)];
            s += m;
            sx += m * x;
        }
        const double mu = sx / s;
        for (int i = 0; i < 1200; ++i) sxx += mean[static_cast<std::size_t>(i)] * (i - 600 + 0.5 - mu) * (i - 600 + 0.5 - mu);
        const double spread = std::sqrt(sxx / s);
        const auto tooth = fit_gaussian(left, counts, 1, -1e9, 1e9);
        const auto env = fit_gaussian(left, counts, 1, -1e9, 1e9, FitTarget::envelope);
        const double tallest = static_cast<double>(*std::max_element(counts.begin(), counts.end()));
        check(tooth.ok && tooth.sigma_ps < 2, "comb: the tallest-peak fit finds one tooth (" + std::to_string(tooth.sigma_ps) + " ps)");
        check(env.ok && env.teeth >= 6, "comb: envelope fit through the tooth tops (" + std::to_string(env.teeth) + " tops)");
        check(env.ok && std::abs(env.sigma_ps - spread) < 0.05 * spread && std::abs(env.centre_ps - mu) < 3,
              "comb: envelope width = the spread of the counts (sigma " + std::to_string(env.sigma_ps) + " ps, spread " +
                  std::to_string(spread) + " ps)");
        check(env.ok && std::abs(env.eval(env.centre_ps) - tallest) < 0.15 * tallest,
              "comb: the envelope runs through the tooth tops (" + std::to_string(env.eval(env.centre_ps)) + " at the centre, tallest bin " +
                  std::to_string(tallest) + ")");
        // An uneven comb: doublets 8 ps apart (the step grid jumps at a reference edge), the weaker half under the hull.
        left.clear();
        counts.clear();
        for (int i = 0; i < 600; ++i) {
            double m = 0.1;
            for (int k = -6; k <= 6; ++k) {
                const double env_k = 5000 * std::exp(-0.5 * (44.0 * k) * (44.0 * k) / (60.0 * 60.0));
                for (double off : {0.0, 8.0}) {
                    const double d = i - 300 + 0.5 - (44.0 * k + off);
                    m += (off == 0 ? 1.0 : 0.3) * env_k * std::exp(-0.5 * d * d);
                }
            }
            std::poisson_distribution<long long> pois(m);
            left.push_back(i - 300);
            counts.push_back(static_cast<std::uint64_t>(pois(rng)));
        }
        const auto uneven = fit_gaussian(left, counts, 1, -1e9, 1e9, FitTarget::envelope);
        check(uneven.ok && std::abs(uneven.sigma_ps - 60) < 6 && std::abs(uneven.centre_ps) < 4 &&
                  std::abs(uneven.eval(uneven.centre_ps) - 0.88 * 5000) < 0.15 * 0.88 * 5000,
              "uneven comb: envelope through the main tops (sigma " + std::to_string(uneven.sigma_ps) + " ps, centre " +
                  std::to_string(uneven.centre_ps) + ", height " + std::to_string(uneven.eval(uneven.centre_ps)) + ")");
        // A smooth peak: the envelope runs through its upper noise, about the plain fit.
        make(1, 2000, -1000, 500, 12.3, 8.0, 5, left, counts);
        const auto smooth = fit_gaussian(left, counts, 1, -1e9, 1e9, FitTarget::envelope);
        check(smooth.ok && std::abs(smooth.sigma_ps - 8.0) < 1.2 && std::abs(smooth.centre_ps - 12.3) < 1.5,
              "envelope fit of a smooth peak is close to the plain fit (sigma " + std::to_string(smooth.sigma_ps) + " ps)");
    }

    counts.assign(counts.size(), 0);
    check(!fit_gaussian(left, counts, 1, -1e9, 1e9).ok, "no fit without counts");
}
}  // namespace

int main() {
    test_gpx_config();
    test_encoding();
    test_records();
    test_session();
    test_compact();
    test_run_files();
    test_accumulator();
    test_histogram2d();
    test_projection();
    test_gauss_fit();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
