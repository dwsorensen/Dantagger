#include "tdcctl/tdc_session.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <future>
#include <random>
#include <thread>

namespace tdcctl {
namespace {

using Clock = std::chrono::steady_clock;

double seconds_since(Clock::time_point start) { return std::chrono::duration<double>(Clock::now() - start).count(); }

std::uint64_t random_u64() {
    std::random_device device;
    std::uint64_t value = (static_cast<std::uint64_t>(device()) << 32U) ^ device();
    value ^= static_cast<std::uint64_t>(Clock::now().time_since_epoch().count());
    return value;
}

au2ft601::FrameType frame_type(const Frame& frame) {
    return static_cast<au2ft601::FrameType>(static_cast<std::uint16_t>(frame[10] | (frame[11] << 8U)));
}

}  // namespace

// ---------------------------------------------------------------------------
// Frame scanner (Test_project au2ft601::FrameScanner algorithm)
// ---------------------------------------------------------------------------
std::vector<Frame> FrameScanner::feed(const std::uint8_t* data, std::size_t size) {
    std::vector<Frame> frames;
    if (size != 0) buffer_.insert(buffer_.end(), data, data + size);
    std::size_t start = 0;
    while (true) {
        const auto magic = std::search(buffer_.begin() + static_cast<std::ptrdiff_t>(start), buffer_.end(),
                                       au2ft601::kMagic.begin(), au2ft601::kMagic.end());
        if (magic == buffer_.end()) {
            const std::size_t keep = std::min(buffer_.size() - start, au2ft601::kMagic.size() - 1);
            discarded_ += buffer_.size() - start - keep;
            start = buffer_.size() - keep;
            break;
        }
        const auto position = static_cast<std::size_t>(magic - buffer_.begin());
        discarded_ += position - start;
        start = position;
        if (buffer_.size() - start < au2ft601::kFrameSize) break;
        Frame candidate{};
        std::copy_n(buffer_.begin() + static_cast<std::ptrdiff_t>(start), au2ft601::kFrameSize, candidate.begin());
        au2ft601::FrameHeader header;
        std::string error;
        if (au2ft601::decode_frame(candidate, header, error)) {
            frames.push_back(candidate);
            start += au2ft601::kFrameSize;
        } else {
            ++discarded_;
            ++start;
        }
    }
    buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(start));
    return frames;
}

// ---------------------------------------------------------------------------
// Session basics
// ---------------------------------------------------------------------------
Session::Session(au2ft601::Transport& transport, SessionOptions options, std::uint64_t first_transaction)
    : transport_(transport),
      options_(options),
      next_transaction_(first_transaction != 0 ? first_transaction : (random_u64() >> 1U) | 1U) {}

void Session::write_frame(const Frame& frame) {
    std::string failure;
    std::promise<void> finished;
    auto done = finished.get_future();
    std::thread writer([&] {
        std::size_t written = 0;
        unsigned idle_calls = 0;
        while (written < frame.size()) {
            const auto io = transport_.write(frame.data() + written, frame.size() - written, options_.write_timeout_ms);
            if (io.bytes > frame.size() - written) {
                failure = "transport reported writing more bytes than offered";
                break;
            }
            written += io.bytes;
            if (io.status == au2ft601::IoStatus::error) {
                failure = "write failed after " + std::to_string(written) + " of 4096 bytes: " + io.detail;
                break;
            }
            if (io.bytes == 0 && ++idle_calls >= 3) {
                failure = "the FT601 accepted no command bytes for " + std::to_string(3 * options_.write_timeout_ms) +
                          " ms (" + std::to_string(written) + " of 4096 written)";
                break;
            }
        }
        finished.set_value();
    });
    try {
        done.wait_for(std::chrono::milliseconds(options_.drain_after_ms));
        while (done.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            const std::size_t want = au2ft601::kFrameSize - scanner_.buffered_bytes() % au2ft601::kFrameSize;
            for (auto& drained : read_transport(options_.drain_read_timeout_ms, want)) early_frames_.push_back(drained);
        }
    } catch (...) {
        writer.join();
        throw;
    }
    writer.join();
    if (!failure.empty()) throw SessionError(failure);
}

std::vector<Frame> Session::read_frames(std::uint32_t timeout_ms, std::size_t max_bytes) {
    if (!early_frames_.empty()) {
        std::vector<Frame> frames(early_frames_.begin(), early_frames_.end());
        early_frames_.clear();
        return frames;
    }
    return read_transport(timeout_ms, max_bytes);
}

std::vector<Frame> Session::read_transport(std::uint32_t timeout_ms, std::size_t max_bytes) {
    if (read_buffer_.size() < max_bytes) read_buffer_.resize(max_bytes);
    const auto io = transport_.read(read_buffer_.data(), max_bytes, timeout_ms);
    if (io.bytes > max_bytes) throw SessionError("transport reported reading more bytes than requested");
    auto frames = scanner_.feed(read_buffer_.data(), io.bytes);
    if (io.status == au2ft601::IoStatus::error)
        throw SessionError("read failed: " + (io.detail.empty() ? std::string("transport error") : io.detail));
    return frames;
}

std::optional<Response> Session::await_response(std::uint64_t transaction, std::uint32_t timeout_ms) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    while (Clock::now() < deadline) {
        const std::size_t want = au2ft601::kFrameSize - scanner_.buffered_bytes() % au2ft601::kFrameSize;
        for (const auto& frame : read_frames(options_.read_timeout_ms, want)) {
            if (frame_type(frame) == au2ft601::FrameType::response) {
                std::string error;
                auto response = decode_response(frame, error);
                if (response) timing_ = response->lvds.timing;
                if (response && response->transaction_id == transaction) return response;
                if (!response && (error.find("design version") != std::string::npos ||
                                  error.find("payload length") != std::string::npos))
                    throw SessionError(error);
            }
            ++stale_frames_;
        }
    }
    return std::nullopt;
}

Response Session::transact(const Frame& command, std::uint64_t transaction) {
    write_frame(command);
    auto response = await_response(transaction, options_.response_timeout_ms);
    if (!response)
        throw SessionError("no response from the FPGA within " + std::to_string(options_.response_timeout_ms) +
                           " ms (is the TDC firmware image loaded and the USB link up?)");
    return *response;
}

SyncReport Session::sync() {
    SyncReport report;
    const auto stale_before = stale_frames_;
    const auto discarded_before = scanner_.discarded_bytes();
    std::optional<Response> stopped;
    // STOP ends a run left by another host (its final response arrives first
    // and is discarded); the FPGA discards a partial command after 10 ms, so a
    // retry realigns.
    for (unsigned attempt = 1; attempt <= options_.sync_attempts && !stopped; ++attempt) {
        report.attempts = attempt;
        const auto transaction = next_transaction();
        write_frame(encode_simple(Opcode::stop, transaction));
        stopped = await_response(transaction, options_.final_timeout_ms);
    }
    if (!stopped)
        throw SessionError("the FPGA did not answer STOP after " + std::to_string(options_.sync_attempts) +
                           " attempts (is the TDC firmware image loaded and the USB link up?)");
    report.status = status();
    report.stale_frames = stale_frames_ - stale_before;
    report.discarded_bytes = scanner_.discarded_bytes() - discarded_before;
    return report;
}

Response Session::simple(Opcode opcode) {
    const auto t = next_transaction();
    return transact(encode_simple(opcode, t), t);
}

Response Session::status() { return simple(Opcode::status); }
Response Session::probe() { return simple(Opcode::probe); }
Response Session::readback() { return simple(Opcode::readback); }
Response Session::gpx_read_config() { return simple(Opcode::gpx_read_config); }
Response Session::gpx_reset() { return simple(Opcode::gpx_reset); }
Response Session::gpx_init() { return simple(Opcode::gpx_init); }
Response Session::gpx_read_results() { return simple(Opcode::gpx_read_results); }
Response Session::stop() { return simple(Opcode::stop); }

Response Session::set_leds(std::uint8_t mask) {
    const auto t = next_transaction();
    return transact(encode_set_leds(t, mask), t);
}
Response Session::thresholds(unsigned mask, const std::array<std::uint16_t, kChannels>& codes) {
    const auto t = next_transaction();
    return transact(encode_thresholds(t, mask, codes), t);
}
Response Session::hysteresis(unsigned mask, const std::array<std::uint16_t, kChannels>& wipers) {
    const auto t = next_transaction();
    return transact(encode_hysteresis(t, mask, wipers), t);
}
Response Session::led1(std::uint8_t red, std::uint8_t green, std::uint8_t blue, int current_setting) {
    const auto t = next_transaction();
    return transact(encode_led1(t, red, green, blue, current_setting), t);
}
Response Session::led2(unsigned on_mask) {
    const auto t = next_transaction();
    return transact(encode_led2(t, on_mask), t);
}
Response Session::gpx_configure(const std::array<std::uint8_t, kGpxRegisters>& regs) {
    const auto violation = gpx_rule_violation(regs, !timing_ || timing_->ddr_supported());
    if (!violation.empty()) throw ArgumentError("GPX2 configuration: " + violation);
    const auto t = next_transaction();
    return transact(encode_gpx_configure(t, regs), t);
}
Response Session::lvds_set(bool enable, int divider_code, int bit_phase,
                           const std::array<std::uint8_t, kChannels>& sdo_taps,
                           const std::array<std::uint8_t, kChannels>& frame_taps) {
    if (timing_ && !timing_->release2() && (divider_code < 0 || divider_code > 3 || bit_phase >= (1 << divider_code)))
        throw ArgumentError("bit phase must be 0.." + std::to_string((1 << std::clamp(divider_code, 0, 3)) - 1) +
                            " at this divider (release 1)");
    const auto t = next_transaction();
    return transact(encode_lvds_set(t, enable, divider_code, bit_phase, sdo_taps, frame_taps), t);
}
Response Session::lvds_phase(int step) {
    if (timing_ && (step < 0 || step >= timing_->phase_steps))
        throw ArgumentError("phase step must be 0.." + std::to_string(timing_->phase_steps - 1));
    const auto t = next_transaction();
    return transact(encode_lvds_phase(t, step), t);
}
Response Session::link_test(std::uint32_t window_cycles) {
    const auto t = next_transaction();
    return transact(encode_link_test(t, window_cycles), t);
}

// ---------------------------------------------------------------------------
// Streaming
// ---------------------------------------------------------------------------
StreamResult Session::stream(const StreamRequest& request, const RecordSink& sink,
                             const StreamProgressCallback& progress, const std::atomic<bool>* abort) {
    if (!sink) return stream_batches(request, {}, progress, abort);
    return stream_batches(
        request,
        [&sink](const std::uint64_t* raw, std::size_t n) {
            for (std::size_t i = 0; i < n; ++i) sink(raw[i]);
        },
        progress, abort);
}

StreamResult Session::stream_batches(const StreamRequest& request, const RecordBatchSink& sink,
                                     const StreamProgressCallback& progress, const std::atomic<bool>* abort) {
    StreamResult result;
    result.run_id = (random_u64() >> 1U) | 1U;
    const auto start_txn = next_transaction();
    std::uint64_t stop_txn = 0;
    bool stop_sent = false;
    bool completed = false;
    bool got_run_complete = false;
    bool start_seen = false;
    bool end_seen = false;
    const auto start = Clock::now();
    auto last_frame = start;
    Clock::time_point stop_time{};
    const auto fail = [&](const std::string& why) {
        if (result.failure.empty()) result.failure = why;
    };

    const unsigned depth = options_.pipeline_depth > 0
                               ? transport_.begin_pipelined_reads(options_.pipeline_read_bytes, options_.pipeline_depth)
                               : 0;
    const bool pipelined = depth > 0;
    result.pipeline_depth = depth;

    // Bookkeeping per record (events: a bit test and a count; markers decoded);
    // false if the run's record rules are broken.
    auto check_record = [&](std::uint64_t raw) {
        if (end_seen) {
            fail("record after the END marker");
            return false;
        }
        if ((raw >> 63U) == 0) {   // event
            if (!start_seen) {
                fail("the first record of the run is not a START marker");
                return false;
            }
            ++result.events[static_cast<std::size_t>((raw >> 61U) & 3U)];
            return true;
        }
        const auto r = decode_record(raw);
        if (!start_seen) {
            if (r.marker_type != MarkerType::start) {
                fail("the first record of the run is not a START marker");
                return false;
            }
            start_seen = true;
        } else if (r.marker_type == MarkerType::start) {
            fail("START marker inside the run");
            return false;
        }
        if (r.marker_type == MarkerType::end) {
            end_seen = true;
            result.by_window = (r.info & 1U) != 0;
        }
        if (r.marker_type == MarkerType::overflow) result.dropped_marked += r.info;
        return true;
    };

    try {
        write_frame(encode_start(result.run_id, start_txn, request.window_us));
        while (result.failure.empty() && !completed) {
            const double elapsed = seconds_since(start);
            if (!result.start_ack && elapsed * 1000.0 > options_.response_timeout_ms) {
                fail("no START acknowledgement within " + std::to_string(options_.response_timeout_ms) + " ms");
                break;
            }
            if (result.start_ack && !stop_sent && !got_run_complete &&
                ((abort != nullptr && abort->load()) || (request.duration_s > 0 && elapsed >= request.duration_s))) {
                stop_txn = next_transaction();
                write_frame(encode_simple(Opcode::stop, stop_txn));
                stop_sent = true;
                stop_time = Clock::now();
            }
            if (stop_sent && seconds_since(stop_time) * 1000.0 > options_.final_timeout_ms) {
                fail("no final response within " + std::to_string(options_.final_timeout_ms) + " ms of STOP");
                break;
            }
            const std::size_t want = pipelined ? options_.pipeline_read_bytes
                                               : au2ft601::kFrameSize - scanner_.buffered_bytes() % au2ft601::kFrameSize;
            const auto frames = read_frames(options_.read_timeout_ms, want);
            if (!frames.empty()) {
                last_frame = Clock::now();
            } else if (result.start_ack && seconds_since(last_frame) * 1000.0 > options_.stream_silence_ms) {
                fail("no frames from the FPGA for " + std::to_string(options_.stream_silence_ms) + " ms during the run");
                break;
            }
            for (const auto& frame : frames) {
                std::string error;
                const auto type = frame_type(frame);
                if (type == au2ft601::FrameType::response) {
                    auto response = decode_response(frame, error);
                    if (!response) {
                        if (result.start_ack) fail("bad response during the run: " + error);
                        else ++stale_frames_;
                        continue;
                    }
                    if (!result.start_ack) {
                        if (response->transaction_id != start_txn) {
                            ++stale_frames_;
                            continue;
                        }
                        result.start_ack = response;
                        if (response->status != Status::ok) {
                            result.failure = "START was refused: " + status_name(response->status);
                            if (pipelined) (void)transport_.end_pipelined_reads();
                            return result;   // nothing started
                        }
                        if (!response->stream.capture_requested() || response->stream.run_id != result.run_id ||
                            response->stream.records != 0 || response->stream.window_us != request.window_us)
                            fail("START acknowledgement does not describe the requested run");
                        continue;
                    }
                    if (response->opcode == static_cast<std::uint32_t>(Opcode::run_complete) &&
                        response->transaction_id == start_txn) {
                        got_run_complete = true;
                        result.final_response = response;
                        completed = !stop_sent;   // a STOP already sent is still answered
                    } else if (stop_sent && response->transaction_id == stop_txn) {
                        if (!result.final_response) result.final_response = response;
                        completed = true;
                    } else {
                        fail("unexpected " + opcode_name(response->opcode) + " response during the run");
                    }
                    continue;
                }
                if (type == au2ft601::FrameType::data) {
                    auto data = decode_data_frame(frame, error);
                    if (!data) {
                        fail("DATA frame " + std::to_string(result.frames) + ": " + error);
                        break;
                    }
                    if (!result.start_ack) {
                        if (data->run_id == result.run_id) fail("DATA of this run arrived before the START acknowledgement");
                        else ++stale_frames_;
                        continue;
                    }
                    if (data->run_id != result.run_id || data->sequence != result.frames ||
                        data->records_before != result.records) {
                        fail("DATA frame out of order: run " + std::to_string(data->run_id) + " sequence " +
                             std::to_string(data->sequence) + " (expected " + std::to_string(result.frames) +
                             ") records before " + std::to_string(data->records_before) + " (expected " +
                             std::to_string(result.records) + ")");
                        break;
                    }
                    ++result.frames;
                    std::size_t good = 0;
                    while (good < data->records.size() && check_record(data->records[good])) ++good;
                    result.records += good;
                    if (sink && good) sink(data->records.data(), good);
                    continue;
                }
                fail("unexpected frame type during the run");
            }
            if (progress && result.start_ack) {
                StreamProgress p;
                p.elapsed_s = seconds_since(start);
                p.frames = result.frames;
                p.records = result.records;
                p.events = result.events;
                p.dropped_marked = result.dropped_marked;
                p.stopping = stop_sent;
                progress(p);
            }
        }
    } catch (const SessionError& error) {
        fail(error.what());
    }
    result.seconds = seconds_since(start);
    if (pipelined) {
        const auto leftover = transport_.end_pipelined_reads();
        if (completed && !leftover.empty() && result.failure.empty()) fail("bytes after the final response of the run");
        for (const auto& frame : scanner_.feed(leftover.data(), leftover.size())) early_frames_.push_back(frame);
    }
    if (result.failure.empty() && completed) {
        const auto& final = *result.final_response;
        if (final.status != Status::ok || final.stream.streaming())
            fail("the run did not end stopped (" + status_name(final.status) + ")");
        else if (!end_seen)
            fail("the run ended without an END marker");
        else if (final.stream.records != result.records || final.stream.frames != result.frames)
            fail("the FPGA reports " + std::to_string(final.stream.records) + " records in " +
                 std::to_string(final.stream.frames) + " frames; received " + std::to_string(result.records) + " in " +
                 std::to_string(result.frames));
    }
    result.pass = result.failure.empty() && completed;
    if (!result.failure.empty()) {
        try {
            const auto t = next_transaction();
            write_frame(encode_simple(Opcode::stop, t));
            result.recovered = await_response(t, options_.final_timeout_ms).has_value();
        } catch (const SessionError&) {
            result.recovered = false;
        }
    }
    return result;
}

// ---------------------------------------------------------------------------
// Calibration
// ---------------------------------------------------------------------------
CalibrationResult Session::calibrate(const GpxConfig& config, const CalibrationOptions& options,
                                     const std::function<void(const CalibrationPoint&)>& point_done) {
    CalibrationResult result;
    if (!timing_) (void)status();
    const LinkTiming timing = *timing_;
    if (config.ddr && !timing.ddr_supported()) throw ArgumentError("DDR needs TDC firmware release 2");
    if (options.divider_code < 0 || options.divider_code > 3) throw ArgumentError("divider code must be 0..3");
    const int per_sample = timing.steps_per_sample();
    const int samples = timing.samples_per_bit(options.divider_code, config.ddr);
    const int step = options.step > 0 ? options.step : std::max(1, samples * per_sample / 100);
    if (step > per_sample) throw ArgumentError("calibration step must be 1.." + std::to_string(per_sample));
    if (options.window_us < 1 || options.window_us > kLinkTestMaxWindow / timing.cycles_per_us())
        throw ArgumentError("calibration window must be 1.." +
                            std::to_string(kLinkTestMaxWindow / timing.cycles_per_us()) + " us");
    const std::uint32_t window = options.window_us * timing.cycles_per_us();
    result.bit_ps = samples * timing.sample_ps;
    GpxConfig test = config;
    test.test_pattern = true;
    test.lvds_out = true;
    auto r = gpx_configure(test.bytes());
    if (r.status != Status::ok) {
        result.message = "GPX_CONFIGURE (test pattern) failed: " + status_name(r.status) + ", " +
                         describe_gpx_error(r.gpx.last_error);
        return result;
    }
    auto good_point = [&](const std::array<LinkCounts, kChannels>& link) {
        for (int c = 0; c < kChannels; ++c) {
            if (!((options.channel_mask >> c) & 1U)) continue;
            const auto& l = link[static_cast<std::size_t>(c)];
            if (l.frames < options.min_frames || l.bad_pattern != 0 || l.structure_errors != 0) return false;
        }
        return true;
    };
    for (int bp = 0; bp < samples; ++bp) {
        r = lvds_set(true, options.divider_code, bp);
        if (r.status != Status::ok) {
            result.message = "LVDS_SET failed: " + status_name(r.status);
            (void)gpx_configure(config.bytes());
            return result;
        }
        for (int phase = 0; phase < per_sample; phase += step) {
            r = lvds_phase(phase);
            if (r.status != Status::ok) {
                result.message = "LVDS_PHASE " + std::to_string(phase) + " failed: " + status_name(r.status);
                (void)gpx_configure(config.bytes());
                return result;
            }
            r = link_test(window);
            if (r.status != Status::ok) {
                result.message = "LINK_TEST failed: " + status_name(r.status);
                (void)gpx_configure(config.bytes());
                return result;
            }
            CalibrationPoint p;
            p.bit_phase = bp;
            p.phase = phase;
            p.offset_ps = bp * timing.sample_ps + phase * kPhaseStepPs;
            p.link = r.lvds.link;
            p.lclkout_hist = r.lvds.lclkout_hist;
            p.good = good_point(p.link);
            result.points.push_back(p);
            if (point_done) point_done(p);
        }
    }
    // Widest circular run of good points; choose its centre.
    const int n = static_cast<int>(result.points.size());
    int best_len = 0, best_start = 0;
    for (int s = 0; s < n; ++s) {
        int len = 0;
        while (len < n && result.points[static_cast<std::size_t>((s + len) % n)].good) ++len;
        if (len > best_len) {
            best_len = len;
            best_start = s;
        }
    }
    if (best_len == 0) {
        result.message = "no sampling point decoded the test pattern on every selected channel (see the table; check "
                         "that the frames/ones counters move and the LCLKOUT histogram shows a clock)";
        (void)gpx_configure(config.bytes());
        return result;
    }
    const auto& chosen = result.points[static_cast<std::size_t>((best_start + best_len / 2) % n)];
    result.chosen_bit_phase = chosen.bit_phase;
    result.chosen_phase = chosen.phase;
    result.chosen_offset_ps = chosen.offset_ps;
    result.good_width_ps = best_len == n ? result.bit_ps : best_len * step * kPhaseStepPs;
    r = lvds_set(true, options.divider_code, chosen.bit_phase);
    r = lvds_phase(chosen.phase);
    const std::uint32_t final_window = std::min<std::uint32_t>(kLinkTestMaxWindow, window * 10U);
    r = link_test(final_window);
    result.final_test = r;
    const bool final_ok = r.status == Status::ok && good_point(r.lvds.link);
    const auto restore = gpx_configure(config.bytes());
    if (restore.status != Status::ok) {
        result.message = "the final GPX_CONFIGURE failed: " + status_name(restore.status);
        return result;
    }
    result.success = final_ok;
    char text[200];
    std::snprintf(text, sizeof(text), "bit phase %d, phase %d (%.0f ps into the %.0f ps bit; %.0f ps error-free)",
                  chosen.bit_phase, chosen.phase, chosen.offset_ps, result.bit_ps, result.good_width_ps);
    result.message = final_ok ? std::string(text)
                              : "the final link test at bit phase " + std::to_string(chosen.bit_phase) + ", phase " +
                                    std::to_string(chosen.phase) + " had errors";
    return result;
}

LclkinScope::LclkinScope(Session& session, bool turn_on) : session_(session) {
    if (!turn_on) return;
    const auto st = session_.status();
    if (st.lvds.lclkin_enabled()) return;
    const auto r = session_.lvds_set(true, st.lvds.divider_code(), st.lvds.bit_phase(), st.lvds.sdo_taps, st.lvds.frame_taps);
    if (r.status != Status::ok) {
        active_ = false;
        throw SessionError("LVDS_SET (LCLKIN on) refused: " + status_name(r.status));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
}

LclkinScope::~LclkinScope() {
    try {
        off();
    } catch (...) {
        // Reported by off() when called explicitly; a destructor must not throw.
    }
}

std::optional<Response> LclkinScope::off() {
    if (!active_) return std::nullopt;
    active_ = false;
    const auto st = session_.status();
    const auto r = session_.lvds_set(false, st.lvds.divider_code(), st.lvds.bit_phase(), st.lvds.sdo_taps, st.lvds.frame_taps);
    if (r.status != Status::ok) throw SessionError("LVDS_SET (LCLKIN off) refused: " + status_name(r.status));
    return r;
}

}  // namespace tdcctl
