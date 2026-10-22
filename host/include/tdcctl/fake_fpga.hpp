#pragma once

// Software stand-in for the TDC firmware (--simulate and the host tests),
// release 2 by default (`release1()` turns it into release 1).
// Protocol-level only, not a timing model: it applies the command rules of
// PROTOCOL.md (GPX2 rules through the host's own gpx_rule_violation, so the
// RTL testbench, not this, is the independent check of the FPGA), keeps the
// device state, models an LVDS sampling "eye" for calibration (bad while the
// sampling point is between eye_bad_lo and eye_bad_hi, as fractions of the
// bit period), and streams synthetic events with START/TIME/OVERFLOW/END
// markers.  Every read() call advances a virtual clock by `read_step_us`.

#include "au2ft601/transport.hpp"
#include "tdcctl/tdc_protocol.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace tdcctl {

class FakeFpga final : public au2ft601::Transport {
public:
    // ---- Test controls ----
    bool aw_present{true};
    bool drop_next_response{false};
    bool realtime{false};              // reads sleep (GUI demo pace)
    bool corrupt_gpx_readback{false};  // CONFIGURE fails with a mismatch
    double eye_bad_lo{0.214};          // where in the bit period the test pattern fails
    double eye_bad_hi{0.357};
    LinkTiming timing;                 // release 2 (250 MHz build)
    double event_rate_hz{20000.0};     // per channel while streaming
    double read_step_us{1000.0};
    std::size_t commands{0};
    // Dantagger additions (see advance()).
    std::function<void(std::uint64_t t0_ps, std::uint64_t t1_ps, std::vector<std::pair<std::uint64_t, int>>& out)>
        event_source;
    double gpx_ppm{0.0};
    std::uint64_t latency_ps{600000};
    std::uint64_t latency_jitter_ps{0};
    std::uint64_t drop_every{0};
    std::uint64_t dropped_total() const { return dropped_total_; }

    // ---- Board state ----
    std::array<std::uint16_t, kChannels> dac{};
    std::array<std::uint16_t, kChannels> wiper{{128, 128, 128, 128}};
    std::array<std::uint8_t, 256> aw{};
    std::uint8_t iodir{0xF8};
    std::uint8_t olat{0x07};
    std::uint32_t flags{0x7F};
    std::uint32_t first_error{0};
    std::uint32_t error_count{0};
    std::uint32_t op_count{1};
    std::uint32_t cmd_ok{0};
    std::uint32_t cmd_bad{0};
    std::uint8_t led_mask{0};
    bool led_set{false};
    std::uint16_t temp_raw{0x0C80};    // 25.0 C
    std::uint16_t polls{0};
    // ---- GPX2 ----
    std::array<std::uint8_t, kGpxRegisters> gpx_written{};
    std::array<std::uint8_t, kGpxRegisters> gpx_readback{};
    bool gpx_configured{false}, gpx_verified{false}, gpx_measuring{false};
    std::uint32_t gpx_mismatch{0};
    std::uint16_t gpx_ops{0}, gpx_fails{0}, gpx_last_error{0};
    // ---- LVDS ----
    bool lclk_en{false};
    int div_code{0}, bit_phase{0}, phase{0};
    std::array<std::uint8_t, kChannels> sdo_taps{}, frame_taps{};
    std::uint32_t lt_window{0};
    std::array<LinkCounts, kChannels> lt{};
    std::array<std::uint16_t, 8> hist{};

    void release1() { timing = LinkTiming::release1(); }

    FakeFpga() {
        aw.fill(0);
        aw[0x01] = 0x01;
        aw[0x31] = aw[0x32] = aw[0x33] = 0x01;
    }

    std::vector<au2ft601::DeviceInfo> list() override { return {{"SIMULATED", "simulated TDC firmware", 0x0403, 0x601f}}; }
    void open_by_serial(const std::string&) override {}
    void close() noexcept override {}

    au2ft601::IoResult write(const std::uint8_t* source, std::size_t size, std::uint32_t) override {
        in_.insert(in_.end(), source, source + size);
        while (in_.size() >= au2ft601::kFrameSize) {
            Frame frame{};
            std::copy_n(in_.begin(), au2ft601::kFrameSize, frame.begin());
            in_.erase(in_.begin(), in_.begin() + static_cast<std::ptrdiff_t>(au2ft601::kFrameSize));
            handle(frame);
        }
        return {au2ft601::IoStatus::ok, size, 0, {}};
    }

    au2ft601::IoResult read(std::uint8_t* destination, std::size_t capacity, std::uint32_t timeout_ms) override {
        if (running_ || draining_) advance(read_step_us);
        if (out_.empty()) {
            if (realtime) std::this_thread::sleep_for(std::chrono::milliseconds(timeout_ms));
            return {au2ft601::IoStatus::timeout, 0, 0, {}};
        }
        if (realtime && (running_ || draining_))
            std::this_thread::sleep_for(std::chrono::microseconds(static_cast<long>(read_step_us)));
        const std::size_t n = std::min(capacity, out_.size());
        std::copy_n(out_.begin(), n, destination);
        out_.erase(out_.begin(), out_.begin() + static_cast<std::ptrdiff_t>(n));
        return {au2ft601::IoStatus::ok, n, 0, {}};
    }

    void inject_stray(std::size_t bytes) {
        for (std::size_t i = 0; i < bytes; ++i) out_.push_back(static_cast<std::uint8_t>(i * 7U));
    }
    bool streaming() const { return running_ || draining_; }

private:
    std::vector<std::uint8_t> in_;
    std::deque<std::uint8_t> out_;
    std::mt19937_64 rng_{12345};
    // Streaming state
    bool running_{false};
    bool draining_{false};
    bool stop_pending_{false};
    au2ft601::FrameHeader stop_cmd_{};
    std::uint64_t run_id_{0};
    std::uint64_t start_txn_{0};
    std::uint32_t window_us_{0};
    double run_us_{0.0};
    std::uint64_t coarse_{1000};
    std::uint64_t next_time_mark_{0};
    std::vector<std::uint64_t> pending_;
    std::uint64_t seq_{0};
    std::uint64_t records_sent_{0};
    std::uint64_t captured_{0};
    struct Ev {
        std::uint64_t done;
        std::uint64_t t_ps;
        int c;
    };
    std::vector<Ev> carry_;
    std::uint64_t drop_counter_{0};
    std::uint64_t dropped_pending_{0};
    std::uint64_t dropped_total_{0};

    static std::uint32_t le32(const Frame& f, std::size_t o) {
        return static_cast<std::uint32_t>(f[o]) | (static_cast<std::uint32_t>(f[o + 1]) << 8U) |
               (static_cast<std::uint32_t>(f[o + 2]) << 16U) | (static_cast<std::uint32_t>(f[o + 3]) << 24U);
    }

    static std::uint64_t marker(MarkerType type, std::uint16_t info, std::uint64_t coarse) {
        return (1ULL << 63U) | (static_cast<std::uint64_t>(type) << 60U) |
               (static_cast<std::uint64_t>(info & 0xFFFU) << 48U) | (coarse & 0xFFFFFFFFFFFFULL);
    }

    bool ddr() const { return (gpx_written[2] & 0x20U) != 0; }
    double ticks_per_us() const { return 1000.0 / timing.coarse_tick_ns; }

    bool lvds_good() const {
        const int n = timing.samples_per_bit(div_code, ddr());
        const double bit_ps = n * timing.sample_ps;
        const double at = std::fmod((bit_phase % n) * timing.sample_ps + phase * kPhaseStepPs, bit_ps) / bit_ps;
        return lclk_en && gpx_verified && gpx_measuring && !(at >= eye_bad_lo && at <= eye_bad_hi);
    }

    // Virtual time passes while a run is active.
    //
    // Dantagger modification (merged from SDE/Dantagger's copy onto release 2):
    // events come from `event_source` when it is set (true GPX2 times in ps on
    // the GPX2 time base), else Poisson at `event_rate_hz`.  The GPX2 time base
    // runs `gpx_ppm` fast against the FPGA coarse clock; each event's frame
    // completes `latency_ps` (+ a random part up to `latency_jitter_ps`) after
    // its stop edge, and records leave in completion order.  Reference index
    // and stop result use the configured widths and REFCLK divisions.
    // `drop_every` > 0 drops every Nth event at the FPGA FIFO and reports the
    // drops in OVERFLOW markers.
    void advance(double us) {
        const double end_us = run_us_ + us;
        const std::uint64_t base = coarse_ - static_cast<std::uint64_t>(run_us_ * ticks_per_us() + 0.5);
        const auto end_coarse = base + static_cast<std::uint64_t>(end_us * ticks_per_us() + 0.5);
        const auto tick_ps = static_cast<std::uint64_t>(timing.coarse_tick_ns * 1000.0 + 0.5);
        std::vector<Ev> events;
        if (running_) {
            // GPX2 time t_ps corresponds to coarse time t_ps / (tick_ps * (1 + ppm)).
            const double scale = 1.0 + gpx_ppm * 1e-6;
            const double tick = static_cast<double>(tick_ps);
            const std::uint64_t t0 = static_cast<std::uint64_t>(static_cast<double>(coarse_) * tick * scale);
            const std::uint64_t t1 = static_cast<std::uint64_t>(static_cast<double>(end_coarse) * tick * scale);
            std::vector<std::pair<std::uint64_t, int>> raw;
            if (event_source) {
                event_source(t0, t1, raw);
            } else {
                const double ev_per_us = event_rate_hz * 1e-6;
                std::poisson_distribution<int> count(ev_per_us * us);
                std::uniform_int_distribution<std::uint64_t> at(t0, t1 > t0 ? t1 - 1 : t0);
                for (int c = 0; c < kChannels; ++c) {
                    const int n = count(rng_);
                    for (int i = 0; i < n; ++i) raw.emplace_back(at(rng_), c);
                }
            }
            for (const auto& [t, c] : raw) {
                const std::uint64_t jitter = latency_jitter_ps > 0 ? rng_() % latency_jitter_ps : 0;
                const double done_ps = static_cast<double>(t + latency_ps + jitter) / scale;
                events.push_back({static_cast<std::uint64_t>(done_ps / tick), t, c});
            }
            events.insert(events.end(), carry_.begin(), carry_.end());   // still in flight from the last step
            carry_.clear();
            // Per channel the GPX2 keeps order; completion order across channels.
            std::stable_sort(events.begin(), events.end(), [](const Ev& a, const Ev& b) {
                return a.c != b.c ? a.c < b.c : a.t_ps < b.t_ps;
            });
            for (int c = 0; c < kChannels; ++c) {   // a channel's frames never complete out of order
                std::uint64_t last = 0;
                for (auto& ev : events)
                    if (ev.c == c) {
                        if (ev.done < last) ev.done = last;
                        last = ev.done;
                    }
            }
            std::stable_sort(events.begin(), events.end(), [](const Ev& a, const Ev& b) { return a.done < b.done; });
        }
        const int ref_bits = gpx_ref_index_bits(gpx_written[2]);
        const int stop_bits = gpx_stop_bits(gpx_written[2]);
        const std::uint32_t divisions = gpx_divisions(gpx_written) == 0 ? 125000U : gpx_divisions(gpx_written);
        const std::uint64_t ref_mask = ref_bits >= 64 ? ~0ULL : ((1ULL << ref_bits) - 1ULL);
        const std::uint64_t stop_mask = (1ULL << stop_bits) - 1ULL;
        std::size_t e = 0;
        while (true) {
            std::uint64_t ev_t = e < events.size() ? events[e].done : UINT64_MAX;
            if (ev_t != UINT64_MAX && ev_t < coarse_) ev_t = coarse_;   // completed before this step began
            const std::uint64_t mark_t = next_time_mark_;
            if (running_ && mark_t < end_coarse && mark_t <= ev_t) {
                pending_.push_back(marker(MarkerType::time, 0, mark_t));
                next_time_mark_ += 16384;
                continue;
            }
            if (ev_t < end_coarse) {
                const int c = events[e].c;
                ++e;
                if (drop_every > 0 && (++drop_counter_ % drop_every) == 0) {
                    ++dropped_pending_;
                    continue;
                }
                if (dropped_pending_ > 0) {
                    pending_.push_back(marker(MarkerType::overflow,
                                              static_cast<std::uint16_t>(std::min<std::uint64_t>(dropped_pending_, 4095)), ev_t));
                    dropped_total_ += dropped_pending_;
                    dropped_pending_ = 0;
                }
                const std::uint64_t t_ps = events[e - 1].t_ps;
                const std::uint64_t ref = (t_ps / divisions) & ref_mask;
                const std::uint64_t stop = (t_ps % divisions) & stop_mask;
                const std::uint64_t raw = (static_cast<std::uint64_t>(c) << 61U) | ((ev_t & 0xFFFFU) << 44U) |
                                          (ref << 20U) | stop;
                pending_.push_back(raw);
                ++captured_;
                continue;
            }
            break;
        }
        if (running_)
            for (; e < events.size(); ++e) carry_.push_back(events[e]);
        if (dropped_pending_ > 0 && running_) {
            pending_.push_back(marker(MarkerType::overflow,
                                      static_cast<std::uint16_t>(std::min<std::uint64_t>(dropped_pending_, 4095)), end_coarse));
            dropped_total_ += dropped_pending_;
            dropped_pending_ = 0;
        }
        coarse_ = end_coarse;
        run_us_ = end_us;
        if (running_ && (stop_pending_ || (window_us_ != 0 && run_us_ >= window_us_))) {
            const bool by_window = !stop_pending_;
            pending_.push_back(marker(MarkerType::end, by_window ? 1 : 0, coarse_));
            running_ = false;
            draining_ = true;
        }
        flush_records();
        if (draining_) {
            draining_ = false;
            if (stop_pending_) {
                stop_pending_ = false;
                respond(stop_cmd_, Status::ok, static_cast<std::uint32_t>(Opcode::stop));
            } else {
                au2ft601::FrameHeader h;
                h.run_id = run_id_;
                h.sequence_or_transaction = start_txn_;
                respond(h, Status::ok, static_cast<std::uint32_t>(Opcode::run_complete));
            }
        }
    }

    void flush_records() {
        std::size_t i = 0;
        while (i < pending_.size()) {
            const std::size_t n = std::min(kMaxRecordsPerFrame, pending_.size() - i);
            std::vector<std::uint8_t> payload(8 * n);
            for (std::size_t r = 0; r < n; ++r)
                for (std::size_t b = 0; b < 8; ++b)
                    payload[8 * r + b] = static_cast<std::uint8_t>(pending_[i + r] >> (8U * b));
            au2ft601::FrameHeader h;
            h.type = au2ft601::FrameType::data;
            h.run_id = run_id_;
            h.sequence_or_transaction = seq_++;
            h.payload_offset = records_sent_;
            h.pattern_or_opcode = 1;
            h.valid_payload_length = static_cast<std::uint32_t>(payload.size());
            h.timestamp = coarse_;
            const auto frame = au2ft601::encode_frame(h, payload.data(), payload.size());
            out_.insert(out_.end(), frame.begin(), frame.end());
            records_sent_ += n;
            i += n;
        }
        pending_.clear();
    }

    void handle(const Frame& frame) {
        au2ft601::FrameHeader h;
        std::string error;
        const bool ok = au2ft601::decode_frame(frame, h, error) && h.type == au2ft601::FrameType::command &&
                        h.payload_offset == 0 && h.seed == 0 && h.flags_or_status == 0 && h.timestamp == 0;
        if (!ok) {
            ++cmd_bad;
            return;
        }
        ++cmd_ok;
        ++commands;
        std::array<std::uint32_t, 8> a{};
        for (std::size_t i = 0; i < 8; ++i) a[i] = le32(frame, 64 + 4 * i);
        std::size_t nz_end = 0;
        for (std::size_t w = 0; w < 1007; ++w)
            if (le32(frame, 64 + 4 * w) != 0) nz_end = w + 1;
        const std::uint32_t len = h.valid_payload_length;
        auto args_ok = [&](std::uint32_t words) { return len == 4 * words && nz_end <= words; };
        const auto op = static_cast<Opcode>(h.pattern_or_opcode);
        const bool streaming_now = running_ || draining_;
        Status status = Status::bad_command;
        bool good_args = false;
        bool known = true;
        switch (op) {
            case Opcode::status:
                respond(h, args_ok(0) ? Status::ok : Status::bad_argument);
                return;
            case Opcode::stop:
                if (!args_ok(0)) { respond(h, Status::bad_argument); return; }
                if (streaming_now) {
                    if (stop_pending_) { respond(h, Status::busy); return; }
                    stop_pending_ = true;
                    stop_cmd_ = h;
                    return;   // answered after the last DATA frame
                }
                respond(h, Status::ok);
                return;
            case Opcode::start: good_args = args_ok(2) && a[1] == 0; break;
            case Opcode::set_leds: good_args = args_ok(1) && a[0] <= 0xFF; break;
            case Opcode::set_thresholds:
            case Opcode::set_hysteresis: {
                const std::uint32_t limit = op == Opcode::set_thresholds ? kDacCodeMax : 256U;
                good_args = args_ok(5) && a[0] != 0 && a[0] <= 0xF;
                for (int ch = 0; ch < 4 && good_args; ++ch) {
                    const auto v = a[static_cast<std::size_t>(ch) + 1];
                    if (((a[0] >> ch) & 1U) ? v > limit : v != 0) good_args = false;
                }
                break;
            }
            case Opcode::set_led1: good_args = args_ok(4) && a[0] <= 255 && a[1] <= 255 && a[2] <= 255 && a[3] <= 3; break;
            case Opcode::set_led2: good_args = args_ok(1) && a[0] <= 7; break;
            case Opcode::probe: case Opcode::readback: case Opcode::gpx_read_config: case Opcode::gpx_reset:
            case Opcode::gpx_init: case Opcode::gpx_read_results:
                good_args = args_ok(0);
                break;
            case Opcode::gpx_configure: {
                std::array<std::uint8_t, kGpxRegisters> regs{};
                for (std::size_t r = 0; r < kGpxRegisters; ++r) regs[r] = static_cast<std::uint8_t>(a[r / 4] >> (8U * (r % 4)));
                good_args = args_ok(5) && (a[4] >> 8U) == 0 && gpx_rule_violation(regs, timing.ddr_supported()).empty();
                break;
            }
            case Opcode::lvds_set:
                good_args = args_ok(5) && a[0] <= 1 && a[1] <= 3 &&
                            (timing.release2() ? a[2] <= 15 : a[2] < (1U << a[1])) &&
                            (a[3] & 0xE0E0E0E0U) == 0 && (a[4] & 0xE0E0E0E0U) == 0;
                break;
            case Opcode::lvds_phase: good_args = args_ok(1) && a[0] < static_cast<std::uint32_t>(timing.phase_steps); break;
            case Opcode::link_test: good_args = args_ok(1) && a[0] >= 1 && a[0] <= kLinkTestMaxWindow; break;
            default: known = false; break;
        }
        if (!known) { respond(h, Status::bad_command); return; }
        if (!good_args) { respond(h, Status::bad_argument); return; }
        if (streaming_now) { respond(h, Status::busy); return; }
        status = Status::ok;
        switch (op) {
            case Opcode::start:
                if (!(gpx_verified && gpx_measuring && lclk_en)) { respond(h, Status::not_ready); return; }
                run_id_ = h.run_id;
                start_txn_ = h.sequence_or_transaction;
                window_us_ = a[0];
                running_ = true;
                run_us_ = 0;
                seq_ = 0;
                records_sent_ = 0;
                captured_ = 0;
                carry_.clear();
                dropped_pending_ = 0;
                next_time_mark_ = (coarse_ | 0x3FFFU) + 1U;
                respond(h, Status::ok);
                pending_.push_back(marker(MarkerType::start, 0, coarse_));
                return;
            case Opcode::set_leds: led_mask = static_cast<std::uint8_t>(a[0]); led_set = true; break;
            case Opcode::set_thresholds:
                ++op_count;
                for (int ch = 0; ch < 4; ++ch) if ((a[0] >> ch) & 1U) dac[static_cast<std::size_t>(ch)] = static_cast<std::uint16_t>(a[static_cast<std::size_t>(ch) + 1]);
                break;
            case Opcode::set_hysteresis:
                ++op_count;
                for (int ch = 0; ch < 4; ++ch) if ((a[0] >> ch) & 1U) wiper[static_cast<std::size_t>(ch)] = static_cast<std::uint16_t>(a[static_cast<std::size_t>(ch) + 1]);
                break;
            case Opcode::set_led1:
                ++op_count;
                if (!aw_present) { flags &= ~32U; first_error = 0x03000501U; ++error_count; status = Status::device_error; break; }
                flags |= 32U; aw[0x01] = 1;
                for (int i = 0; i < 3; ++i) { aw[static_cast<std::size_t>(0x31 + i)] = static_cast<std::uint8_t>(a[3]); aw[static_cast<std::size_t>(0x34 + i)] = static_cast<std::uint8_t>(a[static_cast<std::size_t>(i)]); }
                aw[0x30] = a[3] != 0 ? 0x07 : 0x00;
                break;
            case Opcode::set_led2: ++op_count; olat = static_cast<std::uint8_t>(~a[0] & 7U); iodir = 0xF8; break;
            case Opcode::probe: case Opcode::readback:
                ++op_count;
                if (op == Opcode::probe) { olat = 0x07; aw[0x30] = 0; aw[0x34] = aw[0x35] = aw[0x36] = 0; }
                break;
            case Opcode::gpx_configure: {
                ++gpx_ops;
                for (std::size_t r = 0; r < kGpxRegisters; ++r) gpx_written[r] = static_cast<std::uint8_t>(a[r / 4] >> (8U * (r % 4)));
                gpx_readback = gpx_written;
                if (corrupt_gpx_readback) gpx_readback[7] ^= 0x20;
                gpx_mismatch = 0;
                for (std::size_t r = 0; r < kGpxRegisters; ++r) if (gpx_readback[r] != gpx_written[r]) gpx_mismatch |= 1U << r;
                gpx_configured = true;
                gpx_verified = gpx_measuring = gpx_mismatch == 0;
                if (!gpx_verified) { ++gpx_fails; gpx_last_error = 0x0101; status = Status::device_error; }
                break;
            }
            case Opcode::gpx_read_config:
                ++gpx_ops;
                if (gpx_configured && gpx_mismatch != 0) { ++gpx_fails; gpx_last_error = 0x0201; status = Status::device_error; }
                break;
            case Opcode::gpx_reset: ++gpx_ops; gpx_configured = gpx_verified = gpx_measuring = false; break;
            case Opcode::gpx_init:
                ++gpx_ops;
                if (!gpx_verified) { ++gpx_fails; gpx_last_error = 0x0403; status = Status::not_ready; break; }
                gpx_measuring = true;
                break;
            case Opcode::gpx_read_results: ++gpx_ops; break;
            case Opcode::lvds_set:
                lclk_en = a[0] != 0; div_code = static_cast<int>(a[1]); bit_phase = static_cast<int>(a[2]);
                for (std::size_t c = 0; c < 4; ++c) { sdo_taps[c] = static_cast<std::uint8_t>(a[3] >> (8U * c)); frame_taps[c] = static_cast<std::uint8_t>(a[4] >> (8U * c)); }
                break;
            case Opcode::lvds_phase: phase = static_cast<int>(a[0]); break;
            case Opcode::link_test: {
                lt_window = a[0];
                const bool tp = (gpx_written[6] & 0x10U) != 0;
                const std::uint32_t nbits = static_cast<std::uint32_t>(gpx_ref_index_bits(gpx_written[2]) + gpx_stop_bits(gpx_written[2]));
                const double frame_ns = nbits * timing.bit_ns(div_code, ddr());
                const std::uint32_t frames =
                    lclk_en && gpx_verified ? static_cast<std::uint32_t>(a[0] * timing.coarse_tick_ns / frame_ns) : 0;
                for (auto& l : lt) {
                    l = LinkCounts{};
                    if (lvds_good()) { l.frames = frames; if (tp) l.good = frames; else l.bad_pattern = frames; }
                    else if (lclk_en && gpx_verified) { l.structure_errors = frames / 2; l.frames = frames / 4; l.bad_pattern = frames / 4; }
                    l.sdo_ones = l.frame_ones = frames * 4;
                }
                hist.fill(0);
                hist[0] = static_cast<std::uint16_t>(
                    std::min<std::uint32_t>(0xFFFF, (phase % timing.phase_steps) < timing.phase_steps / 2 && lclk_en ? a[0] : 0));
                break;
            }
            default: break;
        }
        respond(h, status);
    }

    void respond(const au2ft601::FrameHeader& command, Status status, std::uint32_t opcode = 0) {
        if (drop_next_response) { drop_next_response = false; return; }
        if (opcode == 0) opcode = command.pattern_or_opcode;
        std::array<std::uint32_t, kResponseFields> f{};
        const int n_fields = timing.release2() ? kResponseFields : kResponseFieldsR1;
        auto put64 = [&](std::size_t i, std::uint64_t v) { f[i] = static_cast<std::uint32_t>(v); f[i + 1] = static_cast<std::uint32_t>(v >> 32U); };
        auto pack = [](const std::uint8_t* b) { return static_cast<std::uint32_t>(b[0]) | (static_cast<std::uint32_t>(b[1]) << 8U) | (static_cast<std::uint32_t>(b[2]) << 16U) | (static_cast<std::uint32_t>(b[3]) << 24U); };
        f[0] = opcode; f[1] = timing.design; f[2] = 0; f[3] = cmd_ok; f[4] = cmd_bad; f[5] = led_mask;
        f[6] = flags | (led_set ? 128U : 0U); f[7] = first_error; f[8] = error_count; f[9] = op_count;
        for (std::size_t ch = 0; ch < 4; ++ch) { f[10 + ch] = dac[ch]; f[18 + ch] = wiper[ch]; }
        f[14] = 0x0417; f[15] = 0x010F;
        f[22] = 0x33; f[23] = aw[0x01]; f[24] = aw[0x30];
        for (std::size_t i = 0; i < 3; ++i) { f[25 + i] = aw[0x31 + i]; f[28 + i] = aw[0x34 + i]; }
        f[31] = iodir; f[32] = olat; f[33] = kDacCodeMax;
        f[34] = 0x0117; f[35] = temp_raw; f[36] = 0x0220; f[37] = ++polls;
        std::uint32_t parity = 0;
        for (auto b : gpx_readback) for (int k = 0; k < 8; ++k) parity ^= (b >> k) & 1U;
        f[38] = (gpx_configured ? 1U : 0U) | (gpx_verified ? 2U : 0U) | (gpx_measuring ? 4U : 0U) | (parity << 3U) |
                (parity << 4U) | 32U | ((gpx_last_error != 0 && status != Status::ok) ? 64U : 0U) |
                ((gpx_written[6] & 0x10U) ? 128U : 0U);
        f[39] = gpx_mismatch;
        for (std::size_t w = 0; w < 4; ++w) { f[40 + w] = pack(&gpx_written[4 * w]); f[45 + w] = pack(&gpx_readback[4 * w]); }
        f[44] = gpx_written[16]; f[49] = gpx_readback[16];
        for (std::size_t w = 0; w < 6; ++w) f[50 + w] = 0xFFFFFFFFU;
        f[56] = gpx_ops | (static_cast<std::uint32_t>(gpx_fails) << 16U); f[57] = gpx_last_error;
        f[58] = (lclk_en ? 1U : 0U) | (static_cast<std::uint32_t>(div_code) << 1U) | (static_cast<std::uint32_t>(bit_phase) << 3U) |
                0x300U | (static_cast<std::uint32_t>(phase) << 16U);
        f[59] = pack(sdo_taps.data()); f[60] = pack(frame_taps.data());
        f[61] = gpx_verified ? (static_cast<std::uint32_t>(gpx_ref_index_bits(gpx_written[2])) |
                                (static_cast<std::uint32_t>(gpx_stop_bits(gpx_written[2])) << 8U) | 0x10000U |
                                (ddr() ? 0x20000U : 0U)) : 0U;
        f[62] = lt_window;
        for (std::size_t c = 0; c < 4; ++c) {
            f[63 + 6 * c] = lt[c].frames; f[64 + 6 * c] = lt[c].good; f[65 + 6 * c] = lt[c].bad_pattern;
            f[66 + 6 * c] = lt[c].structure_errors; f[67 + 6 * c] = lt[c].sdo_ones; f[68 + 6 * c] = lt[c].frame_ones;
        }
        for (std::size_t i = 0; i < 4; ++i) f[87 + i] = hist[2 * i] | (static_cast<std::uint32_t>(hist[2 * i + 1]) << 16U);
        const bool capreq = running_;
        f[91] = (capreq ? 1U : 0U) | (draining_ ? 2U : 0U) | (running_ ? 4U : 0U) | (stop_pending_ ? 16U : 0U);
        f[92] = window_us_;
        put64(93, records_sent_ + (opcode == static_cast<std::uint32_t>(Opcode::start) ? 0 : 0));
        put64(95, seq_);
        f[97] = 0;
        put64(98, captured_);
        put64(106, run_id_);
        if (timing.release2()) {
            f[108] = static_cast<std::uint32_t>(timing.lclk_max_mhz * 1000.0 + 0.5);
            f[109] = static_cast<std::uint32_t>(timing.coarse_tick_ns * 1000.0 + 0.5);
            f[110] = static_cast<std::uint32_t>(timing.phase_steps) |
                     (static_cast<std::uint32_t>(timing.coarse_tick_ns * 1000.0 / timing.sample_ps + 0.5) << 16U);
        }
        std::vector<std::uint8_t> payload(4U * static_cast<std::size_t>(n_fields));
        for (std::size_t i = 0; i < static_cast<std::size_t>(n_fields); ++i)
            for (std::size_t b = 0; b < 4; ++b) payload[4 * i + b] = static_cast<std::uint8_t>(f[i] >> (8U * b));
        au2ft601::FrameHeader h;
        h.type = au2ft601::FrameType::response;
        h.run_id = command.run_id;
        h.sequence_or_transaction = command.sequence_or_transaction;
        h.pattern_or_opcode = opcode;
        h.valid_payload_length = static_cast<std::uint32_t>(payload.size());
        h.flags_or_status = static_cast<std::uint32_t>(status);
        const auto frame = au2ft601::encode_frame(h, payload.data(), payload.size());
        out_.insert(out_.end(), frame.begin(), frame.end());
    }
};

}  // namespace tdcctl
