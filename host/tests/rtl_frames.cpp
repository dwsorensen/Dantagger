// Cross-check of the host decoder against frames produced by the RTL.
//
//   rtl_frames FRAMES.hex
//
// FRAMES.hex is written by tb_tdc_top with +dump (one frame per line, 1024
// hex words).  Every RESPONSE must decode with tdcctl::decode_response, every
// DATA frame with decode_data_frame; each run's records must unwrap with
// TimeUnwrapper (START first, END last, the run's reference-index width and
// the release-2 8 ns coarse tick); and fields must land where the host
// expects them: every normal GPX2 configuration the testbench writes is the
// host's default GpxConfig (24 + 20 bits, SDR) or the release-2 default
// (6 + 18 bits, DDR), so a GPX_CONFIGURE response must read back those bytes
// and report the matching capture format; every response must carry the
// 250 MHz build's clocking fields.

#include "tdcctl/tdc_protocol.hpp"

#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace tdcctl;

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: rtl_frames FRAMES.hex\n");
        return 2;
    }
    std::ifstream in(argv[1]);
    if (!in) {
        std::fprintf(stderr, "cannot read %s\n", argv[1]);
        return 2;
    }
    int failures = 0;
    auto fail = [&](const std::string& what) {
        ++failures;
        std::printf("FAIL: %s\n", what.c_str());
    };
    std::size_t responses = 0, data_frames = 0, gpx_config_checked = 0, records_total = 0;
    std::map<std::uint64_t, std::vector<std::uint64_t>> runs;
    std::map<std::uint64_t, int> run_ref_bits;
    int current_ref_bits = 24;
    GpxConfig fast;
    fast.ddr = true;
    fast.ref_index_bits = 6;
    fast.stop_bits = 18;
    const auto default_regs = GpxConfig{}.bytes();
    const auto fast_regs = fast.bytes();
    std::string line;
    std::size_t n = 0;
    while (std::getline(in, line)) {
        ++n;
        std::istringstream words(line);
        Frame f{};
        for (std::size_t w = 0; w < 1024; ++w) {
            std::string hexword;
            if (!(words >> hexword)) {
                fail("frame " + std::to_string(n) + " has fewer than 1024 words");
                break;
            }
            const auto v = static_cast<std::uint32_t>(std::stoul(hexword, nullptr, 16));
            for (std::size_t b = 0; b < 4; ++b) f[4 * w + b] = static_cast<std::uint8_t>(v >> (8U * b));
        }
        std::string error;
        au2ft601::FrameHeader h;
        if (!au2ft601::decode_frame(f, h, error)) {
            fail("frame " + std::to_string(n) + ": " + error);
            continue;
        }
        if (h.type == au2ft601::FrameType::response) {
            const auto r = decode_response(f, error);
            if (!r) {
                fail("response " + std::to_string(n) + ": " + error);
                continue;
            }
            ++responses;
            const auto& t = r->lvds.timing;
            if (r->design_version != kDesignVersionR2 || !t.release2() || t.lclk_max_mhz != 250.0 ||
                t.coarse_tick_ns != 8.0 || t.phase_steps != 224 || t.sample_ps != 2000.0)
                fail("response " + std::to_string(n) + ": design version or clocking fields 108-110");
            if (r->opcode == static_cast<std::uint32_t>(Opcode::gpx_configure) && r->status == Status::ok &&
                !r->gpx.test_pattern() && r->gpx.verified()) {
                ++gpx_config_checked;
                const bool is_fast = r->gpx.written == fast_regs;
                if ((r->gpx.written != default_regs && !is_fast) || r->gpx.readback != r->gpx.written)
                    fail("GPX_CONFIGURE response: written/readback are not a host default configuration");
                if (r->lvds.ref_bits != (is_fast ? 6 : 24) || r->lvds.stop_bits != (is_fast ? 18 : 20) ||
                    r->lvds.ddr != is_fast || !r->lvds.format_valid)
                    fail("GPX_CONFIGURE response: capture format fields");
                current_ref_bits = r->lvds.ref_bits;
            }
            if (r->status == Status::ok && r->opcode == static_cast<std::uint32_t>(Opcode::status) &&
                r->board.tmp_ok() && !r->tmp.id_ok())
                fail("STATUS: TMP117 OK but the Device_ID field is wrong");
        } else if (h.type == au2ft601::FrameType::data) {
            const auto d = decode_data_frame(f, error);
            if (!d) {
                fail("DATA " + std::to_string(n) + ": " + error);
                continue;
            }
            ++data_frames;
            if (!runs.count(d->run_id)) run_ref_bits[d->run_id] = current_ref_bits;
            auto& recs = runs[d->run_id];
            if (d->records_before != recs.size())
                fail("DATA frame records-before " + std::to_string(d->records_before) + " vs " +
                     std::to_string(recs.size()) + " received");
            recs.insert(recs.end(), d->records.begin(), d->records.end());
        } else {
            fail("unexpected frame type");
        }
    }
    std::size_t complete_runs = 0;
    for (const auto& [run, recs] : runs) {
        records_total += recs.size();
        if (recs.empty()) continue;
        const bool has_end = decode_record(recs.back()).marker && decode_record(recs.back()).marker_type == MarkerType::end;
        if (!has_end) continue;   // a run cut by a link loss or reset has no END
        ++complete_runs;
        TimeUnwrapper u(8.0e6, 125000, run_ref_bits[run], 8.0);
        std::size_t events = 0;
        try {
            for (auto raw : recs)
                if (u.feed(decode_record(raw))) ++events;
        } catch (const std::exception& e) {
            fail("run " + std::to_string(run) + ": " + e.what());
        }
        std::printf("run %016llx: %zu records, %zu events, %d-bit reference index\n",
                    static_cast<unsigned long long>(run), recs.size(), events, run_ref_bits[run]);
    }
    if (responses < 50 || data_frames < 10 || gpx_config_checked < 2 || complete_runs < 4)
        fail("too few frames to be a meaningful check (" + std::to_string(responses) + " responses, " +
             std::to_string(data_frames) + " DATA, " + std::to_string(gpx_config_checked) + " configure checks, " +
             std::to_string(complete_runs) + " complete runs)");
    std::printf("%zu frames: %zu responses, %zu DATA frames (%zu records, %zu complete runs), %zu GPX_CONFIGURE layout checks; %d failures\n",
                n, responses, data_frames, records_total, complete_runs, gpx_config_checked, failures);
    std::printf("%s\n", failures == 0 ? "RTL FRAMES PASS" : "RTL FRAMES FAIL");
    return failures == 0 ? 0 : 1;
}
