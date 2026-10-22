#include "tdcctl/tdc_protocol.hpp"

#include <cmath>
#include <cstdio>
#include <stdexcept>

namespace tdcctl {
namespace {

std::uint32_t le32(const Frame& frame, std::size_t offset) {
    return static_cast<std::uint32_t>(frame[offset]) | (static_cast<std::uint32_t>(frame[offset + 1]) << 8U) |
           (static_cast<std::uint32_t>(frame[offset + 2]) << 16U) |
           (static_cast<std::uint32_t>(frame[offset + 3]) << 24U);
}

Frame encode(Opcode opcode, std::uint64_t run_id, std::uint64_t transaction, const std::vector<std::uint32_t>& args) {
    std::vector<std::uint8_t> payload(args.size() * 4);
    for (std::size_t i = 0; i < args.size(); ++i) {
        for (std::size_t b = 0; b < 4; ++b) {
            payload[4 * i + b] = static_cast<std::uint8_t>((args[i] >> (8U * b)) & 0xFFU);
        }
    }
    au2ft601::FrameHeader header;
    header.type = au2ft601::FrameType::command;
    header.run_id = run_id;
    header.sequence_or_transaction = transaction;
    header.pattern_or_opcode = static_cast<std::uint32_t>(opcode);
    header.valid_payload_length = static_cast<std::uint32_t>(payload.size());
    return au2ft601::encode_frame(header, payload.empty() ? nullptr : payload.data(), payload.size());
}

std::string hex(std::uint32_t value, int digits) {
    char text[16];
    std::snprintf(text, sizeof(text), "0x%0*X", digits, value);
    return text;
}

std::uint32_t pack4(const std::uint8_t* b) {
    return static_cast<std::uint32_t>(b[0]) | (static_cast<std::uint32_t>(b[1]) << 8U) |
           (static_cast<std::uint32_t>(b[2]) << 16U) | (static_cast<std::uint32_t>(b[3]) << 24U);
}

}  // namespace

// ---------------------------------------------------------------------------
// GPX2 configuration
// ---------------------------------------------------------------------------
int gpx_ref_index_bits(std::uint8_t reg2) {
    static const int widths[8] = {0, 2, 4, 8, 16, 24, 6, 12};
    return widths[reg2 & 7U];
}

int gpx_stop_bits(std::uint8_t reg2) { return 14 + 2 * ((reg2 >> 3U) & 3U); }

std::uint32_t gpx_divisions(const std::array<std::uint8_t, kGpxRegisters>& regs) {
    return static_cast<std::uint32_t>(regs[3]) | (static_cast<std::uint32_t>(regs[4]) << 8U) |
           (static_cast<std::uint32_t>(regs[5] & 0xFU) << 16U);
}

std::string gpx_rule_violation(const std::array<std::uint8_t, kGpxRegisters>& r, bool allow_ddr) {
    static const std::uint8_t fixed[8] = {0xA1, 0x13, 0x00, 0x0A, 0xCC, 0xCC, 0xF1, 0x7D};
    if (!(r[0] & 0x10U)) return "PIN_ENA_REFCLK must be 1 (REFCLK comes from the LMK1D1204)";
    if ((r[1] >> 6U) == 3U) return "HIGH_RESOLUTION 3 is undefined";
    if (((r[1] >> 4U) & 3U) == 3U) return "CHANNEL_COMBINE 3 is undefined";
    if ((r[2] & 0x20U) && !allow_ddr) return "LVDS double data rate needs TDC firmware release 2 (release 1 is SDR only)";
    if (r[5] & 0xF0U) return "register 5 bits 7:4 are fixed 0000";
    if ((r[6] >> 5U) != 6U || (r[6] & 0x0FU) != 0U) return "register 6 fixed bits must be 110x0000";
    if (r[7] & 0x80U) return "REFCLK_BY_XOSC must be 0 (no quartz on this board)";
    if (!(r[7] & 0x40U) || (r[7] & 0x0FU) != 0x3U) return "register 7 fixed bits must be x1xx0011";
    for (int i = 0; i < 8; ++i)
        if (r[static_cast<std::size_t>(8 + i)] != fixed[i])
            return "register " + std::to_string(8 + i) + " must be " + hex(fixed[i], 2) + " (fixed by ams)";
    if (r[16] != 0) return "register 16 must be 0 (CMOS_INPUT 0: every input on this board is LVDS)";
    const std::uint32_t div = gpx_divisions(r);
    const int sb = gpx_stop_bits(r[2]);
    if (div == 0) return "REFCLK_DIVISIONS must not be 0";
    if (div > (1U << static_cast<unsigned>(sb)) - 1U)
        return "REFCLK_DIVISIONS " + std::to_string(div) + " does not fit " + std::to_string(sb) + " stop bits";
    return {};
}

std::array<std::uint8_t, kGpxRegisters> GpxConfig::bytes() const {
    if (channel_mask > 0xFU) throw ArgumentError("channel mask must be 0..15");
    if (high_resolution < 0 || high_resolution > 2) throw ArgumentError("high resolution must be 0, 1 (2x) or 2 (4x)");
    if (channel_combine < 0 || channel_combine > 2) throw ArgumentError("channel combine must be 0, 1 or 2");
    int ref_code = -1;
    static const int widths[8] = {0, 2, 4, 8, 16, 24, 6, 12};
    for (int i = 0; i < 8; ++i)
        if (widths[i] == ref_index_bits) ref_code = i;
    if (ref_code < 0) throw ArgumentError("reference-index bits must be 0, 2, 4, 6, 8, 12, 16 or 24");
    if (stop_bits < 14 || stop_bits > 20 || (stop_bits % 2) != 0)
        throw ArgumentError("stop bits must be 14, 16, 18 or 20");
    int adjust = -1;
    switch (data_valid_adjust_ps) {
        case -160: adjust = 0; break;
        case 0: adjust = 1; break;
        case 160: adjust = 2; break;
        case 320: adjust = 3; break;
        default: throw ArgumentError("LVDS data-valid adjust must be -160, 0, 160 or 320 ps");
    }
    if (refclk_divisions == 0 || refclk_divisions > 0xFFFFFU)
        throw ArgumentError("REFCLK_DIVISIONS must be 1..1048575");
    std::array<std::uint8_t, kGpxRegisters> r{};
    r[0] = static_cast<std::uint8_t>((pin_ena_rstidx ? 0x80U : 0U) | (pin_ena_disable ? 0x40U : 0U) |
                                     (lvds_out ? 0x20U : 0U) | 0x10U | channel_mask);
    r[1] = static_cast<std::uint8_t>((static_cast<unsigned>(high_resolution) << 6U) |
                                     (static_cast<unsigned>(channel_combine) << 4U) | channel_mask);
    r[2] = static_cast<std::uint8_t>((blockwise_fifo_read ? 0x80U : 0U) | (common_fifo_read ? 0x40U : 0U) |
                                     (ddr ? 0x20U : 0U) |
                                     (static_cast<unsigned>((stop_bits - 14) / 2) << 3U) |
                                     static_cast<unsigned>(ref_code));
    r[3] = static_cast<std::uint8_t>(refclk_divisions & 0xFFU);
    r[4] = static_cast<std::uint8_t>((refclk_divisions >> 8U) & 0xFFU);
    r[5] = static_cast<std::uint8_t>((refclk_divisions >> 16U) & 0x0FU);
    r[6] = static_cast<std::uint8_t>(0xC0U | (test_pattern ? 0x10U : 0U));
    r[7] = static_cast<std::uint8_t>(0x40U | (static_cast<unsigned>(adjust) << 4U) | 0x03U);
    static const std::uint8_t fixed[8] = {0xA1, 0x13, 0x00, 0x0A, 0xCC, 0xCC, 0xF1, 0x7D};
    for (std::size_t i = 0; i < 8; ++i) r[8 + i] = fixed[i];
    r[16] = 0x00;
    const auto violation = gpx_rule_violation(r);
    if (!violation.empty()) throw ArgumentError("GPX2 configuration: " + violation);
    return r;
}

std::uint32_t GpxConfig::divisions_for(double refclk_hz, double lsb_ps) {
    if (!(refclk_hz >= 2.0e6 && refclk_hz <= 12.5e6))
        throw ArgumentError("REFCLK must be 2 .. 12.5 MHz (datasheet)");
    if (!(lsb_ps > 0.0)) throw ArgumentError("LSB must be positive");
    const double period_ps = 1.0e12 / refclk_hz;
    const double exact = period_ps / lsb_ps;
    const double rounded = std::round(exact);
    if (std::fabs(exact - rounded) > 1e-6 || rounded < 1.0 || rounded > 1048575.0)
        throw ArgumentError("the REFCLK period " + std::to_string(period_ps) + " ps is not a whole number of " +
                            std::to_string(lsb_ps) + " ps LSBs");
    return static_cast<std::uint32_t>(rounded);
}

std::string describe_gpx_config(const std::array<std::uint8_t, kGpxRegisters>& r) {
    char text[256];
    std::snprintf(text, sizeof(text),
                  "STOP pins %X hits %X, LVDS out %s, ref index %d bits, stop %d bits, divisions %u, %s, test pattern %s",
                  r[0] & 0xFU, r[1] & 0xFU, (r[0] & 0x20U) ? "on" : "off", gpx_ref_index_bits(r[2]),
                  gpx_stop_bits(r[2]), gpx_divisions(r), (r[2] & 0x20U) ? "DDR" : "SDR",
                  (r[6] & 0x10U) ? "on" : "off");
    return text;
}

double max_result_latency_us(const LinkTiming& timing, int divider_code, bool ddr, int frame_bits, int high_resolution) {
    const int fifo_depth = high_resolution == 0 ? 16 : high_resolution == 1 ? 8 : 4;
    return (fifo_depth + 1) * frame_bits * timing.bit_ns(divider_code, ddr) * 1e-3 + 0.5;
}

int min_ref_index_bits(const LinkTiming& timing, int divider_code, bool ddr, int stop_bits, int high_resolution,
                       double refclk_hz) {
    static const int widths[] = {2, 4, 6, 8, 12, 16, 24};
    for (int w : widths) {
        const double wrap_us = std::ldexp(1.0, w) / refclk_hz * 1e6;
        if (wrap_us >= 4.0 * max_result_latency_us(timing, divider_code, ddr, w + stop_bits, high_resolution))
            return w;
    }
    return 0;
}

int min_stop_bits(std::uint32_t divisions) {
    for (int bits = 14; bits <= 20; bits += 2)
        if (divisions <= (1U << static_cast<unsigned>(bits)) - 1U) return bits;
    return 0;
}

std::string capture_problem(const Response& st, double refclk_hz) {
    const auto& l = st.lvds;
    if (!l.format_valid) return "no capture format (configure the GPX2 first)";
    const int hires = st.gpx.written[1] >> 6U;
    const int need = min_ref_index_bits(l.timing, l.divider_code(), l.ddr, l.stop_bits, hires, refclk_hz);
    if (need != 0 && l.ref_bits >= need) return {};
    char text[320];
    std::snprintf(text, sizeof(text),
                  "a %d-bit reference index wraps every %.1f us, but at LCLKIN %.2f MHz %s with %d-bit frames a result "
                  "can wait up to %.2f us in the GPX2: times would be ambiguous. Configure at least %d reference-index "
                  "bits (gpx-config), or a faster LCLKIN",
                  l.ref_bits, std::ldexp(1.0, l.ref_bits) / refclk_hz * 1e6, l.lclkin_mhz(), l.ddr ? "DDR" : "SDR",
                  l.ref_bits + l.stop_bits,
                  max_result_latency_us(l.timing, l.divider_code(), l.ddr, l.ref_bits + l.stop_bits, hires), need);
    return text;
}

// ---------------------------------------------------------------------------
// Command frames
// ---------------------------------------------------------------------------
Frame encode_simple(Opcode opcode, std::uint64_t transaction) { return encode(opcode, 0, transaction, {}); }

Frame encode_start(std::uint64_t run_id, std::uint64_t transaction, std::uint32_t window_us) {
    return encode(Opcode::start, run_id, transaction, {window_us, 0});
}

Frame encode_set_leds(std::uint64_t transaction, std::uint8_t mask) {
    return encode(Opcode::set_leds, 0, transaction, {mask});
}

Frame encode_thresholds(std::uint64_t transaction, unsigned mask, const std::array<std::uint16_t, kChannels>& codes) {
    if (mask == 0 || mask > 0xFU) throw ArgumentError("channel mask must select 1 to 4 channels");
    std::vector<std::uint32_t> args{mask};
    for (int ch = 0; ch < kChannels; ++ch) {
        const bool selected = (mask >> ch) & 1U;
        const auto code = codes[static_cast<std::size_t>(ch)];
        if (selected && (code < kDacCodeMin || code > kDacCodeMax)) {
            throw ArgumentError("threshold code " + std::to_string(code) + " for channel " + std::to_string(ch + 1) +
                                " is outside the safety window " + std::to_string(kDacCodeMin) + ".." +
                                std::to_string(kDacCodeMax));
        }
        args.push_back(selected ? code : 0U);
    }
    return encode(Opcode::set_thresholds, 0, transaction, args);
}

Frame encode_hysteresis(std::uint64_t transaction, unsigned mask, const std::array<std::uint16_t, kChannels>& wipers) {
    if (mask == 0 || mask > 0xFU) throw ArgumentError("channel mask must select 1 to 4 channels");
    std::vector<std::uint32_t> args{mask};
    for (int ch = 0; ch < kChannels; ++ch) {
        const bool selected = (mask >> ch) & 1U;
        const auto code = wipers[static_cast<std::size_t>(ch)];
        if (selected && code > kWiperCodeMax) {
            throw ArgumentError("wiper code " + std::to_string(code) + " for channel " + std::to_string(ch + 1) +
                                " is above 256");
        }
        args.push_back(selected ? code : 0U);
    }
    return encode(Opcode::set_hysteresis, 0, transaction, args);
}

Frame encode_led1(std::uint64_t transaction, std::uint8_t red, std::uint8_t green, std::uint8_t blue,
                  int current_setting) {
    if (current_setting < 0 || current_setting > 3) throw ArgumentError("LED1 current setting must be 0..3");
    return encode(Opcode::set_led1, 0, transaction, {red, green, blue, static_cast<std::uint32_t>(current_setting)});
}

Frame encode_led2(std::uint64_t transaction, unsigned on_mask) {
    if (on_mask > 7U) throw ArgumentError("LED2 mask must be 0..7");
    return encode(Opcode::set_led2, 0, transaction, {on_mask});
}

Frame encode_gpx_configure(std::uint64_t transaction, const std::array<std::uint8_t, kGpxRegisters>& regs) {
    const auto violation = gpx_rule_violation(regs);
    if (!violation.empty()) throw ArgumentError("GPX2 configuration: " + violation);
    std::vector<std::uint32_t> args;
    for (std::size_t w = 0; w < 4; ++w) args.push_back(pack4(&regs[4 * w]));
    args.push_back(regs[16]);
    return encode(Opcode::gpx_configure, 0, transaction, args);
}

Frame encode_lvds_set(std::uint64_t transaction, bool enable, int divider_code, int bit_phase,
                      const std::array<std::uint8_t, kChannels>& sdo_taps,
                      const std::array<std::uint8_t, kChannels>& frame_taps) {
    if (divider_code < 0 || divider_code > 3) throw ArgumentError("divider code must be 0..3 (LCLKIN = maximum / 2^code)");
    if (bit_phase < 0 || bit_phase > 15) throw ArgumentError("bit phase must be 0..15");
    for (int c = 0; c < kChannels; ++c)
        if (sdo_taps[static_cast<std::size_t>(c)] > 31 || frame_taps[static_cast<std::size_t>(c)] > 31)
            throw ArgumentError("IDELAY taps must be 0..31");
    return encode(Opcode::lvds_set, 0, transaction,
                  {enable ? 1U : 0U, static_cast<std::uint32_t>(divider_code), static_cast<std::uint32_t>(bit_phase),
                   pack4(sdo_taps.data()), pack4(frame_taps.data())});
}

Frame encode_lvds_phase(std::uint64_t transaction, int step) {
    if (step < 0 || step > 1023) throw ArgumentError("phase step must be 0..1023");
    return encode(Opcode::lvds_phase, 0, transaction, {static_cast<std::uint32_t>(step)});
}

Frame encode_link_test(std::uint64_t transaction, std::uint32_t window_cycles) {
    if (window_cycles < 1 || window_cycles > kLinkTestMaxWindow)
        throw ArgumentError("link-test window must be 1..100000000 cycles");
    return encode(Opcode::link_test, 0, transaction, {window_cycles});
}

// ---------------------------------------------------------------------------
// Responses
// ---------------------------------------------------------------------------
std::optional<Response> decode_response(const Frame& frame, std::string& error) {
    au2ft601::FrameHeader header;
    if (!au2ft601::decode_frame(frame, header, error)) {
        return std::nullopt;
    }
    if (header.type != au2ft601::FrameType::response) {
        error = "not a RESPONSE frame";
        return std::nullopt;
    }
    const std::uint32_t length = header.valid_payload_length;
    if (length != 4U * kResponseFields && length != 4U * kResponseFieldsR1) {
        error = "response payload length " + std::to_string(length) + " (expected " +
                std::to_string(4 * kResponseFieldsR1) + " or " + std::to_string(4 * kResponseFields) +
                "; is the TDC firmware image loaded?)";
        return std::nullopt;
    }
    if (header.payload_offset != 0 || header.seed != 0) {
        error = "nonzero reserved response header fields";
        return std::nullopt;
    }
    for (std::size_t i = au2ft601::kHeaderSize + length; i < au2ft601::kFrameSize - 4; ++i) {
        if (frame[i] != 0) {
            error = "nonzero response padding";
            return std::nullopt;
        }
    }
    Response r;
    r.run_id = header.run_id;
    r.transaction_id = header.sequence_or_transaction;
    r.opcode = header.pattern_or_opcode;
    r.status = static_cast<Status>(header.flags_or_status);
    for (std::size_t i = 0; i < length / 4U; ++i)
        r.fields[i] = le32(frame, au2ft601::kHeaderSize + 4U * i);
    const auto& f = r.fields;
    auto u64 = [&](int i) {
        return static_cast<std::uint64_t>(f[static_cast<std::size_t>(i)]) |
               (static_cast<std::uint64_t>(f[static_cast<std::size_t>(i) + 1]) << 32U);
    };
    auto byte_of = [&](int first_field, int index) {
        return static_cast<std::uint8_t>(f[static_cast<std::size_t>(first_field + index / 4)] >> (8U * static_cast<unsigned>(index % 4)));
    };
    if (f[0] != r.opcode) {
        error = "response field 0 does not repeat the opcode";
        return std::nullopt;
    }
    r.design_version = f[1];
    const bool r2 = r.design_version == kDesignVersionR2;
    if (!(r2 && length == 4U * kResponseFields) && !(r.design_version == kDesignVersionR1 && length == 4U * kResponseFieldsR1)) {
        error = "unexpected FPGA design version " + hex(r.design_version, 8) + " with a " + std::to_string(length) +
                "-byte response (expected " + hex(kDesignVersionR1, 8) + " / 436 or " + hex(kDesignVersionR2, 8) +
                " / 444, TDC firmware release 1 or 2)";
        return std::nullopt;
    }
    r.fault_bits = f[2];
    r.commands_ok = f[3];
    r.commands_bad = f[4];
    r.au_led_mask = f[5];
    auto& b = r.board;
    b.flags = f[6];
    b.first_error = f[7];
    b.error_count = f[8];
    b.op_count = f[9];
    for (std::size_t ch = 0; ch < kChannels; ++ch) {
        b.dac_codes[ch] = static_cast<std::uint16_t>(f[10 + ch]);
        b.wipers_raw[ch] = static_cast<std::uint16_t>(f[18 + ch]);
    }
    b.dac_id = static_cast<std::uint16_t>(f[14]);
    b.dac_gain = static_cast<std::uint16_t>(f[15]);
    b.dac_config = static_cast<std::uint16_t>(f[16]);
    b.dac_status = static_cast<std::uint16_t>(f[17]);
    b.aw_id = static_cast<std::uint8_t>(f[22]);
    b.aw_gcr = static_cast<std::uint8_t>(f[23]);
    b.aw_lctr = static_cast<std::uint8_t>(f[24]);
    for (std::size_t i = 0; i < 3; ++i) {
        b.aw_lcfg[i] = static_cast<std::uint8_t>(f[25 + i]);
        b.aw_pwm[i] = static_cast<std::uint8_t>(f[28 + i]);
    }
    b.gpio_iodir = static_cast<std::uint8_t>(f[31]);
    b.gpio_olat = static_cast<std::uint8_t>(f[32]);
    b.dac_code_max = f[33];
    r.tmp.device_id = static_cast<std::uint16_t>(f[34]);
    r.tmp.temp_raw = static_cast<std::uint16_t>(f[35]);
    r.tmp.config = static_cast<std::uint16_t>(f[36]);
    r.tmp.polls_ok = static_cast<std::uint16_t>(f[37] & 0xFFFFU);
    r.tmp.polls_failed = static_cast<std::uint16_t>(f[37] >> 16U);
    r.gpx.flags = f[38];
    r.gpx.mismatch = f[39];
    for (int i = 0; i < kGpxRegisters; ++i) {
        r.gpx.written[static_cast<std::size_t>(i)] = byte_of(40, i);
        r.gpx.readback[static_cast<std::size_t>(i)] = byte_of(45, i);
    }
    for (int i = 0; i < 24; ++i) r.gpx.results[static_cast<std::size_t>(i)] = byte_of(50, i);
    r.gpx.ops = static_cast<std::uint16_t>(f[56] & 0xFFFFU);
    r.gpx.failed_ops = static_cast<std::uint16_t>(f[56] >> 16U);
    r.gpx.last_error = static_cast<std::uint16_t>(f[57]);
    auto& l = r.lvds;
    l.raw = f[58];
    for (int c = 0; c < kChannels; ++c) {
        l.sdo_taps[static_cast<std::size_t>(c)] = byte_of(59, c);
        l.frame_taps[static_cast<std::size_t>(c)] = byte_of(60, c);
    }
    l.ref_bits = static_cast<int>(f[61] & 0xFFU);
    l.stop_bits = static_cast<int>((f[61] >> 8U) & 0xFFU);
    l.format_valid = (f[61] & 0x10000U) != 0;
    l.ddr = (f[61] & 0x20000U) != 0;
    if (r2) {
        const std::uint32_t steps = f[110] & 0xFFFFU, per_cycle = (f[110] >> 16U) & 0xFFU;
        if (f[108] == 0 || f[109] == 0 || steps == 0 || per_cycle == 0 || steps > 1024) {
            error = "release-2 clocking fields 108-110 are invalid";
            return std::nullopt;
        }
        l.timing = LinkTiming{kDesignVersionR2, f[108] / 1000.0, f[109] / 1000.0, static_cast<int>(steps),
                              f[109] / static_cast<double>(per_cycle)};
    } else {
        l.timing = LinkTiming::release1();
    }
    l.link_window = f[62];
    for (std::size_t c = 0; c < kChannels; ++c) {
        auto& lc = l.link[c];
        lc.frames = f[63 + 6 * c];
        lc.good = f[64 + 6 * c];
        lc.bad_pattern = f[65 + 6 * c];
        lc.structure_errors = f[66 + 6 * c];
        lc.sdo_ones = f[67 + 6 * c];
        lc.frame_ones = f[68 + 6 * c];
    }
    for (std::size_t s = 0; s < 8; ++s)
        l.lclkout_hist[s] = static_cast<std::uint16_t>(f[87 + s / 2] >> (16U * (s % 2)));
    auto& st = r.stream;
    st.flags = f[91];
    st.window_us = f[92];
    st.records = u64(93);
    st.frames = u64(95);
    st.dropped = f[97];
    st.captured = u64(98);
    st.structure_errors = f[100];
    st.fifo_max = f[101];
    st.stall_cycles = u64(102);
    st.rxf_tx_cycles = u64(104);
    st.run_id = u64(106);
    error.clear();
    return r;
}

// ---------------------------------------------------------------------------
// DATA frames and records
// ---------------------------------------------------------------------------
std::optional<DataFrame> decode_data_frame(const Frame& frame, std::string& error) {
    au2ft601::FrameHeader h;
    if (!au2ft601::decode_frame(frame, h, error)) return std::nullopt;
    if (h.type != au2ft601::FrameType::data) {
        error = "not a DATA frame";
        return std::nullopt;
    }
    if (h.pattern_or_opcode != 1 || h.seed != 0 || h.flags_or_status != 0) {
        error = "DATA header: record format " + std::to_string(h.pattern_or_opcode) + ", reserved words nonzero";
        return std::nullopt;
    }
    const std::uint32_t len = h.valid_payload_length;
    if (len == 0 || len % 8 != 0 || len > kMaxRecordsPerFrame * 8) {
        error = "DATA payload length " + std::to_string(len);
        return std::nullopt;
    }
    for (std::size_t i = au2ft601::kHeaderSize + len; i < au2ft601::kFrameSize - 4; ++i) {
        if (frame[i] != 0) {
            error = "nonzero DATA padding";
            return std::nullopt;
        }
    }
    DataFrame d;
    d.run_id = h.run_id;
    d.sequence = h.sequence_or_transaction;
    d.records_before = h.payload_offset;
    d.timestamp = h.timestamp;
    d.records.reserve(len / 8);
    for (std::size_t r = 0; r < len / 8; ++r) {
        const std::size_t o = au2ft601::kHeaderSize + 8 * r;
        d.records.push_back(static_cast<std::uint64_t>(le32(frame, o)) |
                            (static_cast<std::uint64_t>(le32(frame, o + 4)) << 32U));
    }
    error.clear();
    return d;
}

Record decode_record(std::uint64_t raw) {
    Record r;
    r.raw = raw;
    r.marker = (raw >> 63U) != 0;
    if (r.marker) {
        r.marker_type = static_cast<MarkerType>((raw >> 60U) & 7U);
        r.info = static_cast<std::uint16_t>((raw >> 48U) & 0xFFFU);
        r.coarse = raw & 0xFFFFFFFFFFFFULL;
    } else {
        r.channel = static_cast<int>((raw >> 61U) & 3U);
        r.coarse_low = static_cast<std::uint16_t>((raw >> 44U) & 0xFFFFU);
        r.ref_index = static_cast<std::uint32_t>((raw >> 20U) & 0xFFFFFFU);
        r.stop = static_cast<std::uint32_t>(raw & 0xFFFFFU);
    }
    return r;
}

TimeUnwrapper::TimeUnwrapper(double refclk_hz, std::uint32_t divisions, int ref_bits, double coarse_tick_ns)
    : ref_period_ps_(1.0e12 / refclk_hz),
      lsb_ps_(1.0e12 / refclk_hz / static_cast<double>(divisions == 0 ? 1 : divisions)),
      coarse_tick_ps_(coarse_tick_ns * 1000.0),
      divisions_(divisions),
      ref_bits_(ref_bits) {
    if (!(refclk_hz > 0) || divisions == 0 || !(coarse_tick_ns > 0))
        throw std::invalid_argument("refclk, divisions and the coarse tick must be positive");
}

std::optional<EventTime> TimeUnwrapper::feed(const Record& record) {
    if (!started_) {
        if (!record.marker || record.marker_type != MarkerType::start)
            throw std::runtime_error("the first record of a run is not a START marker");
        started_ = true;
        coarse_ = record.coarse;
        return std::nullopt;
    }
    if (record.marker) {
        if (record.marker_type == MarkerType::start) throw std::runtime_error("START marker inside a run");
        if (record.coarse + 64 < coarse_) throw std::runtime_error("marker coarse time went backwards");
        coarse_ = record.coarse;
        return std::nullopt;
    }
    // Signed difference of the low 16 bits from the previous record.
    const auto low_prev = static_cast<std::uint16_t>(coarse_ & 0xFFFFU);
    const auto diff = static_cast<std::int16_t>(static_cast<std::uint16_t>(record.coarse_low - low_prev));
    coarse_ = static_cast<std::uint64_t>(static_cast<std::int64_t>(coarse_) + diff);
    EventTime e;
    e.channel = record.channel;
    e.coarse = coarse_;
    e.stop = record.stop;
    const double wrap = std::ldexp(1.0, ref_bits_);        // reference periods per index wrap
    std::int64_t ref = record.ref_index;
    if (!have_event_) {
        have_event_ = true;
        first_time_ps_ = (static_cast<double>(ref) * divisions_ + record.stop) * lsb_ps_;
    } else {
        // Predict the reference index from the coarse time since the last
        // event and choose the wrap count closest to it.
        const double elapsed_ps =
            (static_cast<double>(static_cast<std::int64_t>(coarse_) - static_cast<std::int64_t>(last_event_coarse_))) *
            coarse_tick_ps_;
        const double predicted = static_cast<double>(last_ref_) + elapsed_ps / ref_period_ps_;
        const double k = std::round((predicted - static_cast<double>(ref)) / wrap);
        ref += static_cast<std::int64_t>(k * wrap);
    }
    last_ref_ = ref;
    last_event_coarse_ = coarse_;
    e.ref_index = ref;
    e.time_ps = (static_cast<double>(ref) * divisions_ + record.stop) * lsb_ps_ - first_time_ps_;
    return e;
}

// ---------------------------------------------------------------------------
// Comparator conversions (Comparator_Control cmpctl, unchanged)
// ---------------------------------------------------------------------------
std::uint16_t threshold_code(double volts) {
    if (!std::isfinite(volts) || volts < kThresholdMinVolts - 1e-9 || volts > kThresholdMaxVolts + 1e-9) {
        throw ArgumentError("threshold " + std::to_string(volts) + " V is outside the safety window " +
                            "-0.100 .. +1.500 V (limited for safety)");
    }
    const double exact = (volts + kThresholdOffset) * 65536.0 / kDacFullScale;
    long code = std::lround(exact);
    if (code < kDacCodeMin) code = kDacCodeMin;
    if (code > kDacCodeMax) code = kDacCodeMax;
    return static_cast<std::uint16_t>(code);
}

double threshold_volts(std::uint16_t code) { return kDacFullScale * static_cast<double>(code) / 65536.0 - kThresholdOffset; }

std::uint16_t wiper_code(double ohms) {
    const double min = kWiperOhms;
    const double max = kWiperOhms + kRheostatOhms;
    if (!std::isfinite(ohms) || ohms < min - 1e-6 || ohms > max + 1e-6) {
        throw ArgumentError("hysteresis resistance " + std::to_string(ohms) + " ohm is outside 75 .. 5075 ohm");
    }
    long code = std::lround((ohms - kWiperOhms) * 256.0 / kRheostatOhms);
    if (code < 0) code = 0;
    if (code > kWiperCodeMax) code = kWiperCodeMax;
    return static_cast<std::uint16_t>(code);
}

double wiper_ohms(std::uint16_t code) { return kWiperOhms + kRheostatOhms * static_cast<double>(code) / 256.0; }

int led1_current_setting(int milliamps) {
    if (milliamps == 0 || milliamps == 5 || milliamps == 10 || milliamps == 15) {
        return milliamps / kLed1CurrentStepMilliamps;
    }
    throw ArgumentError("LED1 current must be 0, 5, 10 or 15 mA");
}

// ---------------------------------------------------------------------------
// Names
// ---------------------------------------------------------------------------
std::string status_name(Status status) {
    switch (status) {
        case Status::ok: return "OK";
        case Status::bad_command: return "BAD_COMMAND";
        case Status::bad_argument: return "BAD_ARGUMENT";
        case Status::busy: return "BUSY (not allowed while streaming)";
        case Status::device_error: return "DEVICE_ERROR";
        case Status::not_ready: return "NOT_READY";
    }
    return "status " + std::to_string(static_cast<std::uint32_t>(status));
}

std::string opcode_name(std::uint32_t opcode) {
    switch (static_cast<Opcode>(opcode)) {
        case Opcode::start: return "START";
        case Opcode::stop: return "STOP";
        case Opcode::status: return "STATUS";
        case Opcode::set_leds: return "SET_LEDS";
        case Opcode::set_thresholds: return "SET_THRESHOLDS";
        case Opcode::set_hysteresis: return "SET_HYSTERESIS";
        case Opcode::set_led1: return "SET_LED1";
        case Opcode::set_led2: return "SET_LED2";
        case Opcode::probe: return "PROBE";
        case Opcode::readback: return "READBACK";
        case Opcode::gpx_configure: return "GPX_CONFIGURE";
        case Opcode::gpx_read_config: return "GPX_READ_CONFIG";
        case Opcode::gpx_reset: return "GPX_RESET";
        case Opcode::gpx_init: return "GPX_INIT";
        case Opcode::gpx_read_results: return "GPX_READ_RESULTS";
        case Opcode::lvds_set: return "LVDS_SET";
        case Opcode::lvds_phase: return "LVDS_PHASE";
        case Opcode::link_test: return "LINK_TEST";
        case Opcode::run_complete: return "RUN_COMPLETE";
    }
    return "opcode " + hex(opcode, 2);
}

std::string describe_first_error(std::uint32_t first_error) {
    if (first_error == 0) return "none";
    static const char* kinds[] = {"?", "no ACK (NACK)", "readback mismatch", "I2C timeout", "DAC echo wrong"};
    static const char* devices[] = {"bus", "DAC80504", "MCP23S08 (LED2)", "MCP4652 U3 (ch1/2)",
                                    "MCP4652 U8 (ch3/4)", "AW2013 (LED1)", "TMP117"};
    static const char* ops[] = {"?", "thresholds", "hysteresis", "LED1", "LED2", "probe/boot", "readback", "temperature"};
    const unsigned kind = first_error & 0xFFU;
    const unsigned device = (first_error >> 8U) & 0xFFU;
    const unsigned step = (first_error >> 16U) & 0xFFU;
    const unsigned op = (first_error >> 24U) & 0xFFU;
    return std::string(kind < 5 ? kinds[kind] : "?") + " from " + (device < 7 ? devices[device] : "?") +
           " during " + (op < 8 ? ops[op] : "?") + " (step " + std::to_string(step) + ")";
}

std::string describe_faults(std::uint32_t fault_bits) {
    if (fault_bits == 0) return "none";
    std::string text;
    if (fault_bits & 1U) text += "USB link was lost since the FPGA reset; ";
    if (fault_bits & 2U) text += "a partial command timed out; ";
    if (fault_bits & 4U) text += "event FIFO read underrun (please report); ";
    return text.substr(0, text.size() - 2);
}

std::string describe_gpx_error(std::uint16_t last_error) {
    if (last_error == 0) return "none";
    static const char* kinds[] = {"?", "readback differs from the written configuration",
                                  "readback after INIT differs (chip stopped again)", "INIT refused (not verified)"};
    static const char* ops[] = {"?", "CONFIGURE", "READ_CONFIG", "RESET", "INIT", "READ_RESULTS"};
    const unsigned kind = last_error & 0xFFU;
    const unsigned op = (last_error >> 8U) & 0xFFU;
    return std::string(kind < 4 ? kinds[kind] : "?") + " (" + (op < 6 ? ops[op] : "?") + ")";
}

}  // namespace tdcctl
