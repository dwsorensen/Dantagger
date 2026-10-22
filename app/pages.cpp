#include "pages.hpp"

#include "tdcctl/run_file.hpp"

#include <implot.h>

#include <algorithm>
#include <cmath>
#include <fstream>

namespace app {

using tdcctl::Response;
using tdcctl::Session;

namespace {

const char* kRefBits[] = {"0", "2", "4", "6", "8", "12", "16", "24"};
const int kRefBitsVal[] = {0, 2, 4, 6, 8, 12, 16, 24};
const char* kStopBits[] = {"14", "16", "18", "20"};
const char* kHires[] = {"off", "2x", "4x"};

bool divider_combo(const char* label, int* divider, const tdcctl::LinkTiming& t) {
    char names[4][24];
    const char* items[4];
    for (int k = 0; k < 4; ++k) {
        std::snprintf(names[k], sizeof(names[k]), "%g MHz", t.lclkin_mhz(k));
        items[k] = names[k];
    }
    return ImGui::Combo(label, divider, items, 4);
}

double refclk_of(AppContext& ctx) {
    auto* b = ctx.dev.backend();
    return b ? b->refclk_hz() : 8.0e6;
}

// GPX2 configuration from the inputs for the connected image (release 2 assumed before the first STATUS).
tdcctl::GpxConfig gpx_config_from(const PagesState& s, const Response* state, double refclk_hz) {
    const tdcctl::LinkTiming timing = state ? state->lvds.timing : tdcctl::LinkTiming{};
    tdcctl::GpxConfig c;
    c.ddr = timing.ddr_supported() && s.ddr;
    c.ref_index_bits = kRefBitsVal[s.ref_bits_idx];
    c.stop_bits = 14 + 2 * s.stop_bits_idx;
    c.channel_mask = 0;
    for (int i = 0; i < 4; ++i)
        if (s.channels[static_cast<std::size_t>(i)]) c.channel_mask |= 1U << static_cast<unsigned>(i);
    if (c.channel_mask == 0) throw tdcctl::ArgumentError("enable at least one STOP channel");
    c.high_resolution = s.hires;
    c.test_pattern = s.test_pattern;
    c.refclk_divisions = tdcctl::GpxConfig::divisions_for(refclk_hz, s.lsb_ps);
    if (timing.release2() && s.auto_widths) {
        c.stop_bits = tdcctl::min_stop_bits(c.refclk_divisions);
        if (c.stop_bits == 0) throw tdcctl::ArgumentError("the LSB is too fine for 20 stop bits");
        c.ref_index_bits = tdcctl::min_ref_index_bits(timing, s.divider, c.ddr, c.stop_bits, c.high_resolution, refclk_hz);
        if (c.ref_index_bits == 0) throw tdcctl::ArgumentError("no reference-index width keeps times unambiguous at this LCLKIN");
    }
    (void)c.bytes();   // validates
    return c;
}

bool locked(const Device::View& v) { return v.busy || !v.connected; }

}  // namespace

// ---------------------------------------------------------------------------
// Board setup card
// ---------------------------------------------------------------------------
void board_setup_card(AppContext& ctx) {
    const auto& v = ctx.v;
    if (!ui::card_begin("##setup", "Firmware and board setup")) {
        ui::card_end();
        return;
    }
    if (!v.connected) {
        ui::dim_text("Not connected.");
        ui::card_end();
        return;
    }
    ImGui::PushFont(ui::fonts.heading);
    ImGui::TextColored(ui::c_accent(), "%s", v.release.c_str());
    ImGui::PopFont();
    ImGui::SameLine(0, 16);
    ImGui::AlignTextToFramePadding();
    ui::dim_text("%s", v.link_text.c_str());
    ImGui::TextUnformatted("Capture");
    ImGui::SameLine(110);
    ImGui::PushFont(ui::fonts.mono);
    ImGui::TextWrapped("%s", v.capture.c_str());
    ImGui::PopFont();
    ImGui::TextUnformatted("Frame policy");
    ImGui::SameLine(110);
    ImGui::PushTextWrapPos(0);
    ui::dim_text("%s: %s", v.policy_frame ? "in use" : "NOT in use (the GPX2 has another configuration)",
                 v.policy_text.c_str());
    ImGui::PopTextWrapPos();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Release 2: 24-bit reference index + 18 stop bits (1 ps LSB), DDR unless the last calibration\n"
                          "was made in SDR; the setup calibrates at the LCLKIN of the last calibration (a fallback rate\n"
                          "chosen below stays chosen). Release 1: SDR, 24 + 20 bits. The Python library applies the same.");
    ImGui::TextUnformatted("State");
    ImGui::SameLine(110);
    ui::status_dot(ImGui::GetColorU32(v.set_up ? ui::c_ok() : ui::c_warn()), 5);
    ImGui::SameLine(0, 4);
    ImGui::TextUnformatted(v.set_up ? "configured and calibrated" : "not set up (GPX2 not configured, or LVDS not calibrated since power-up)");
    const bool lock = locked(v) || ctx.dev.tagger() == nullptr;
    if (lock) ImGui::BeginDisabled();
    if (ui::accent_button(v.set_up ? "Set up again (keeps the calibration)" : "Set up the board")) ctx.dev.setup(false);
    ui::probe("setup_button");
    ImGui::SameLine();
    if (ImGui::Button("Recalibrate")) ctx.dev.setup(true);
    if (lock) ImGui::EndDisabled();
    ui::help("Set up = what the Python library does on its first open after power-up: configure the TDC-GPX2 with the "
             "frame policy and calibrate the LVDS sampling point with the test pattern (about 1-2 s) at the LCLKIN rate of "
             "the last calibration, skipping the calibration if the board is still calibrated. To use a lower LCLKIN "
             "(release-2 fallback), pick it under LVDS link below and Calibrate there once. The result is recorded in the same state file, so a Python "
             "script started later skips its setup. Measurements must be stopped.");
    ui::card_end();
}

// ---------------------------------------------------------------------------
// Comparator
// ---------------------------------------------------------------------------
void page_comparator(AppContext& ctx, PagesState& s) {
    const auto& v = ctx.v;
    const Response* r = v.state ? &*v.state : nullptr;
    const bool lock = !v.connected || v.busy;

    if (ui::card_begin("##chans", "Channels · shared by all measurements")) {
        channel_table(ctx, 0xF);
        ImGui::Spacing();
        if (lock || !v.set_up) ImGui::BeginDisabled();
        if (ui::accent_button("Count 1 s")) ctx.dev.quick_count(1.0);
        if (lock || !v.set_up) ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        if (v.counting) {
            ui::dim_text("counting for 1 s (LCLKIN on, off again afterwards)…");
        } else if (v.last_count) {
            const auto& c = *v.last_count;
            if (c.pass) {
                for (int ch = 0; ch < 4; ++ch) {
                    if (ch) ImGui::SameLine(0, 16);
                    ImGui::TextColored(ui::channel_colour(ch + 1), "ch%d %s", ch + 1, ui::fmt_rate(c.rates_hz[static_cast<std::size_t>(ch)]).c_str());
                }
                if (c.dropped) {
                    ImGui::SameLine(0, 16);
                    ImGui::TextColored(ui::c_warn(), "FPGA dropped events: lower bounds");
                }
            } else {
                ImGui::TextColored(ui::c_bad(), "count failed: %s", c.failure.c_str());
            }
        } else {
            ui::dim_text("A 1 s count timed by the FPGA (measurements must be stopped). While measurements run, the rate "
                         "bar above shows the rates.");
        }
    }
    ui::card_end();

    const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    if (ui::card_begin("##led1", "LED1  ·  AW2013, dimmable", 0, half)) {
        if (lock) ImGui::BeginDisabled();
        ImGui::SetNextItemWidth(240);
        ImGui::ColorEdit3("Colour", s.led1_rgb.data(), ImGuiColorEditFlags_DisplayRGB | ImGuiColorEditFlags_Uint8);
        ImGui::SetNextItemWidth(100);
        const char* currents[] = {"5 mA", "10 mA", "15 mA"};
        ImGui::Combo("Current", &s.led1_current, currents, 3);
        ImGui::SameLine();
        if (ImGui::Button("Apply##led1")) {
            const auto red = static_cast<std::uint8_t>(std::lround(s.led1_rgb[0] * 255));
            const auto green = static_cast<std::uint8_t>(std::lround(s.led1_rgb[1] * 255));
            const auto blue = static_cast<std::uint8_t>(std::lround(s.led1_rgb[2] * 255));
            const int setting = tdcctl::led1_current_setting(5 * (s.led1_current + 1));
            ctx.dev.command("LED1", [=](Session& se) { return se.led1(red, green, blue, setting); }, true);
        }
        ImGui::SameLine();
        if (ImGui::Button("Off##led1")) ctx.dev.command("LED1 off", [](Session& se) { return se.led1(0, 0, 0, 0); }, true);
        if (lock) ImGui::EndDisabled();
        if (r) {
            const auto& b = r->board;
            if (b.led1_enabled())
                ui::dim_text("board: R%u G%u B%u at %d mA", b.aw_pwm[0], b.aw_pwm[1], b.aw_pwm[2],
                             b.led1_current() * tdcctl::kLed1CurrentStepMilliamps);
            else
                ui::dim_text("board: off");
        }
    }
    ImGui::EndChild();
    ImGui::SameLine();
    if (ui::card_begin("##led2", "LED2  ·  MCP23S08, on/off", 0, half)) {
        if (lock) ImGui::BeginDisabled();
        ImGui::Checkbox("Red", &s.led2[0]);
        ImGui::SameLine();
        ImGui::Checkbox("Green", &s.led2[1]);
        ImGui::SameLine();
        ImGui::Checkbox("Blue", &s.led2[2]);
        ImGui::SameLine();
        if (ImGui::Button("Apply##led2")) {
            const unsigned mask = (s.led2[0] ? 1U : 0U) | (s.led2[1] ? 2U : 0U) | (s.led2[2] ? 4U : 0U);
            ctx.dev.command("LED2", [mask](Session& se) { return se.led2(mask); }, true);
        }
        if (lock) ImGui::EndDisabled();
        if (r) {
            const auto m = r->board.led2_mask();
            std::string names;
            if (m & 1U) names += "red ";
            if (m & 2U) names += "green ";
            if (m & 4U) names += "blue";
            ui::dim_text("board: %s", names.empty() ? "off" : names.c_str());
        }
        ImGui::Dummy(ImVec2(0, ImGui::GetFrameHeight()));
    }
    ui::card_end();

    if (ui::card_begin("##board", "Board status")) {
        if (r) {
            const auto& b = r->board;
            const std::pair<const char*, bool> devs[] = {{"DAC", b.dac_ok()}, {"MCP23S08", b.gpio_ok()}, {"U3", b.u3_ok()},
                                                         {"U8", b.u8_ok()},   {"AW2013", b.aw_ok()},     {"TMP117", b.tmp_ok()}};
            for (const auto& [name, ok] : devs) {
                ui::status_dot(ImGui::GetColorU32(ok ? ui::c_ok() : ui::c_bad()), 5);
                ImGui::SameLine(0, 4);
                ImGui::TextUnformatted(name);
                ImGui::SameLine(0, 22);
            }
            ImGui::NewLine();
            if (!b.boot_done()) ImGui::TextColored(ui::c_warn(), "board boot not finished");
            ImGui::Text("Temperature");
            ImGui::SameLine(140);
            ImGui::PushFont(ui::fonts.mono);
            ImGui::Text("%.3f °C", r->tmp.celsius());
            ImGui::PopFont();
            ImGui::SameLine();
            ui::dim_text("TMP117 polls %u ok / %u failed", r->tmp.polls_ok, r->tmp.polls_failed);
            ImGui::Text("Last error");
            ImGui::SameLine(140);
            ImGui::TextUnformatted(b.first_error ? tdcctl::describe_first_error(b.first_error).c_str() : "none");
            ImGui::Text("Faults");
            ImGui::SameLine(140);
            ImGui::TextUnformatted(r->fault_bits ? tdcctl::describe_faults(r->fault_bits).c_str() : "none");
        } else {
            ui::dim_text("no state yet");
        }
        if (lock) ImGui::BeginDisabled();
        if (ImGui::Button("Read back")) ctx.dev.command("readback", [](Session& se) { return se.readback(); });
        ImGui::SameLine();
        if (ImGui::Button("Re-probe")) ctx.dev.command("probe", [](Session& se) { return se.probe(); });
        if (lock) ImGui::EndDisabled();
    }
    ui::card_end();
}

// ---------------------------------------------------------------------------
// TDC-GPX2 & LVDS
// ---------------------------------------------------------------------------
void page_tdc(AppContext& ctx, PagesState& s) {
    const auto& v = ctx.v;
    const bool lock = locked(v);
    const Response* r = v.state ? &*v.state : nullptr;
    const double refclk = refclk_of(ctx);
    if (r && !s.synced_from_board && r->gpx.configured()) {   // start the inputs at the board's configuration
        const auto& w = r->gpx.written;
        s.hires = std::clamp(w[1] >> 6U, 0, 2);
        s.ddr = r->lvds.ddr;
        for (int i = 0; i < 8; ++i)
            if (kRefBitsVal[i] == tdcctl::gpx_ref_index_bits(w[2])) s.ref_bits_idx = i;
        s.stop_bits_idx = std::clamp((tdcctl::gpx_stop_bits(w[2]) - 14) / 2, 0, 3);
        s.divider = r->lvds.divider_code();
        s.synced_from_board = true;
    }

    board_setup_card(ctx);

    if (ui::card_begin("##gpxcfg", "GPX2 configuration (manual)")) {
        const tdcctl::LinkTiming timing = r ? r->lvds.timing : tdcctl::LinkTiming{};
        ImGui::PushTextWrapPos(0);
        ui::dim_text("For experiments: \"Set up the board\" applies the frame policy. Board rules always applied: LVDS "
                     "inputs, REFCLK from the LMK1D1204 (%.0f MHz, SW1 \"Int\"). %s",
                     refclk * 1e-6, timing.release2() ? "Release 2: SDR or DDR." : "Release 1: SDR only.");
        ImGui::PopTextWrapPos();
        if (lock) ImGui::BeginDisabled();
        ImGui::PushItemWidth(110);
        ImGui::InputDouble("LSB (ps)", &s.lsb_ps, 0, 0, "%.3f");
        ImGui::SameLine(0, 24);
        ImGui::Combo("High resolution", &s.hires, kHires, 3);
        if (timing.release2()) {
            ImGui::Checkbox("DDR", &s.ddr);
            ui::help("Double data rate: the GPX2 sends a bit on both LCLK edges, twice the readout rate.");
            ImGui::SameLine(0, 24);
            ImGui::Checkbox("Shortest frame", &s.auto_widths);
            ui::help("The smallest stop width that holds the LSB (18 bits at 1 ps) and the smallest reference index that "
                     "keeps event times unambiguous at the LCLKIN chosen under LVDS link (6 bits at 250 MHz DDR).");
            ImGui::SameLine(0, 24);
        }
        const bool fixed_widths = timing.release2() && s.auto_widths;
        ImGui::BeginDisabled(fixed_widths);
        ImGui::Combo("Ref index bits", &s.ref_bits_idx, kRefBits, 8);
        ImGui::SameLine(0, 24);
        ImGui::Combo("Stop bits", &s.stop_bits_idx, kStopBits, 4);
        ImGui::EndDisabled();
        ImGui::PopItemWidth();
        try {
            const auto c = gpx_config_from(s, r, refclk);
            if (fixed_widths) {
                for (int i = 0; i < 8; ++i)
                    if (kRefBitsVal[i] == c.ref_index_bits) s.ref_bits_idx = i;
                s.stop_bits_idx = (c.stop_bits - 14) / 2;
            }
            const int bits = c.ref_index_bits + c.stop_bits;
            const double per_s = 1e9 / (bits * timing.bit_ns(s.divider, c.ddr));
            ui::dim_text("Frame %d + %d bits %s at LCLKIN %g MHz: at most %.2f M events/s per channel", c.ref_index_bits,
                         c.stop_bits, c.ddr ? "DDR" : "SDR", timing.lclkin_mhz(s.divider), per_s * 1e-6);
        } catch (const std::exception& bad) {
            ImGui::TextColored(ui::c_bad(), "%s", bad.what());
        }
        ImGui::TextUnformatted("Channels");
        for (int ch = 0; ch < 4; ++ch) {
            ImGui::SameLine();
            ImGui::PushID(ch);
            char label[8];
            std::snprintf(label, sizeof(label), "%d", ch + 1);
            ImGui::Checkbox(label, &s.channels[static_cast<std::size_t>(ch)]);
            ImGui::PopID();
        }
        ImGui::SameLine(0, 24);
        ImGui::Checkbox("LVDS test pattern", &s.test_pattern);
        ui::help("The GPX2 sends a fixed result (0xF0CCAA / 0x0AACF0) on every LVDS frame. Calibration uses it automatically.");
        ImGui::Spacing();
        if (ui::accent_button("Configure")) {
            try {
                const auto bytes = gpx_config_from(s, r, refclk).bytes();
                ctx.dev.command("GPX2 configure", [bytes](Session& se) { return se.gpx_configure(bytes); });
            } catch (const std::exception& bad) {
                ctx.dev.log(Level::error, std::string("GPX2 configuration: ") + bad.what());
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Read back")) ctx.dev.command("GPX2 read back", [](Session& se) { return se.gpx_read_config(); });
        ImGui::SameLine();
        if (ImGui::Button("Init")) ctx.dev.command("GPX2 init", [](Session& se) { return se.gpx_init(); });
        ImGui::SameLine();
        if (ImGui::Button("Reset")) ctx.dev.command("GPX2 reset", [](Session& se) { return se.gpx_reset(); });
        ImGui::SameLine();
        if (ImGui::Button("SPI results")) ctx.dev.command("GPX2 results", [](Session& se) { return se.gpx_read_results(); });
        if (lock) ImGui::EndDisabled();
    }
    ui::card_end();

    if (ui::card_begin("##gpxhealth", "GPX2 health  ·  read back over SPI")) {
        if (r) {
            const auto& g = r->gpx;
            auto badge = [](const char* text, bool on, ImVec4 col) {
                ui::status_dot(ImGui::GetColorU32(on ? col : ImVec4(0.30f, 0.34f, 0.40f, 1.0f)), 5);
                ImGui::SameLine(0, 4);
                ImGui::TextColored(on ? ImGui::GetStyleColorVec4(ImGuiCol_Text) : ui::c_dim(), "%s", text);
                ImGui::SameLine(0, 18);
            };
            badge("configured", g.configured(), ui::c_ok());
            badge("readback verified", g.verified(), ui::c_ok());
            badge("measuring", g.measuring(), ui::c_ok());
            if (g.test_pattern()) badge("TEST PATTERN", true, ui::c_warn());
            ImGui::NewLine();
            const float col = 120;
            ImGui::TextUnformatted("Written");
            ImGui::SameLine(col);
            ImGui::PushFont(ui::fonts.mono);
            ImGui::TextUnformatted(tdcctl::hex_bytes(g.written.data(), tdcctl::kGpxRegisters).c_str());
            ImGui::PopFont();
            ImGui::TextUnformatted("Read back");
            ImGui::SameLine(col);
            ImGui::PushFont(ui::fonts.mono);
            ImGui::TextColored(g.mismatch ? ui::c_bad() : ImGui::GetStyleColorVec4(ImGuiCol_Text), "%s",
                               tdcctl::hex_bytes(g.readback.data(), tdcctl::kGpxRegisters).c_str());
            ImGui::PopFont();
            if (g.mismatch) {
                ImGui::SameLine();
                ImGui::TextColored(ui::c_bad(), "mismatch mask 0x%05X", g.mismatch);
            }
            ImGui::TextUnformatted("Parity");
            ImGui::SameLine(col);
            ImGui::Text("pin %d, readback %d", g.parity_pin() ? 1 : 0, g.parity_readback() ? 1 : 0);
            ImGui::SameLine();
            ui::dim_text(g.parity_pin() == g.parity_readback() ? "(agree)" : "(differ - see the hardware notes)");
            ImGui::TextUnformatted("INTERRUPT");
            ImGui::SameLine(col);
            ImGui::TextUnformatted(g.interrupt_low() ? "low (data waiting)" : "high");
            if (g.configured()) {
                ImGui::TextUnformatted("Decoded");
                ImGui::SameLine(col);
                ImGui::TextWrapped("%s", tdcctl::describe_gpx_config(g.written).c_str());
            }
            if (g.last_error) {
                ImGui::TextUnformatted("Last error");
                ImGui::SameLine(col);
                ImGui::TextColored(ui::c_bad(), "%s", tdcctl::describe_gpx_error(g.last_error).c_str());
            }
            if (r->opcode == static_cast<std::uint32_t>(tdcctl::Opcode::gpx_read_results)) {
                ImGui::TextUnformatted("SPI results");
                for (int ch = 0; ch < 4; ++ch) {
                    ImGui::SameLine(ch == 0 ? col : 0, ch == 0 ? -1.0f : 24.0f);
                    ImGui::PushFont(ui::fonts.mono);
                    ImGui::Text("ch%d %s", ch + 1, tdcctl::hex_bytes(g.results.data() + 6 * ch, 6).c_str());
                    ImGui::PopFont();
                }
            }
        } else {
            ui::dim_text("no state yet");
        }
    }
    ui::card_end();

    if (ui::card_begin("##lvds", "LVDS link")) {
        if (r) {
            const auto& l = r->lvds;
            ui::status_dot(ImGui::GetColorU32(l.lclkin_enabled() ? ui::c_warn() : ui::c_dim()), 5);
            ImGui::SameLine(0, 4);
            if (l.lclkin_enabled()) ImGui::Text("LCLKIN %.1f MHz", l.lclkin_mhz());
            else ImGui::TextUnformatted("LCLKIN off (high impedance)");
            ImGui::SameLine(0, 20);
            ui::dim_text("sampling phase %d (%.0f ps) · bit phase %d · MMCM %s · IDELAYCTRL %s · %s", l.phase_step(),
                         l.phase_step() * tdcctl::kPhaseStepPs, l.bit_phase(), l.mmcm_locked() ? "locked" : "NOT locked",
                         l.idelay_ready() ? "ready" : "NOT ready",
                         l.format_valid ? (std::to_string(l.ref_bits) + "+" + std::to_string(l.stop_bits) + "-bit frames").c_str()
                                        : "no capture format");
        }
        ui::dim_text("LCLKIN is switched on only for link tests, calibration, counts and runs, and off again afterwards.");
        const tdcctl::LinkTiming timing = r ? r->lvds.timing : tdcctl::LinkTiming{};
        ImGui::Spacing();
        if (lock) ImGui::BeginDisabled();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("LCLKIN");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110);
        divider_combo("##lclk", &s.divider, timing);
        ui::help("The rate calibration, counts and runs use. Fall back to a lower rate (or SDR, GPX2 configuration above) "
                 "if 250 MHz DDR does not calibrate: Calibrate here once; \"Set up the board\" and the Python library then "
                 "keep that rate and data rate.");
        ImGui::SameLine(0, 20);
        if (ui::accent_button("Calibrate")) {
            try {
                tdcctl::CalibrationOptions o;
                o.divider_code = s.divider;
                auto c = gpx_config_from(s, r, refclk);
                c.test_pattern = false;
                o.channel_mask = c.channel_mask;
                ctx.dev.calibrate(c, o);
            } catch (const std::exception& bad) {
                ctx.dev.log(Level::error, std::string("calibration: ") + bad.what());
            }
        }
        ui::help("With the configuration above: configures the GPX2 with its test pattern, sweeps the sampling point "
                 "across one bit (about 100 points, 1 ms link test each), picks the centre of the widest error-free "
                 "window, checks it for 10 ms and restores the configuration. Recorded in the state file.");
        ImGui::SameLine(0, 30);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Link test window");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(60);
        ImGui::InputInt("ms##lt", &s.link_window_ms, 0, 0);
        s.link_window_ms = std::clamp(s.link_window_ms, 1, 1000);
        ImGui::SameLine();
        if (ImGui::Button("Link test"))
            ctx.dev.link_test(static_cast<std::uint32_t>(s.link_window_ms) * 1000U * timing.cycles_per_us());
        ImGui::SameLine(0, 30);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Phase step");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70);
        ImGui::InputInt("##phase", &s.phase_step, 0, 0);
        s.phase_step = std::clamp(s.phase_step, 0, timing.phase_steps - 1);
        ImGui::SameLine();
        if (ImGui::Button("Set phase")) {
            const int step = s.phase_step;
            ctx.dev.command("phase " + std::to_string(step), [step](Session& se) { return se.lvds_phase(step); });
        }
        if (ImGui::TreeNode("Manual LCLKIN (scope checks)")) {
            ImGui::Checkbox("LCLKIN on", &s.lclk_manual_on);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(110);
            divider_combo("Rate", &s.divider, timing);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(90);
            ImGui::InputInt("Bit phase", &s.bit_phase);
            s.bit_phase = std::clamp(s.bit_phase, 0, timing.max_bit_phase(s.divider, r && r->lvds.ddr));
            ImGui::SameLine();
            if (ImGui::Button("Apply##lvds")) {
                const bool on = s.lclk_manual_on;
                const int div = s.divider, bp = s.bit_phase;
                ctx.dev.command(on ? "LCLKIN on" : "LCLKIN off", [=](Session& se) { return se.lvds_set(on, div, bp); });
                if (on) ctx.dev.log(Level::warn, "LCLKIN stays on until switched off or the end of the next LVDS operation");
            }
            ImGui::TreePop();
        }
        if (lock) ImGui::EndDisabled();

        if (r && r->lvds.link_window) {
            ImGui::Spacing();
            ui::dim_text("Last link test: %u cycles", r->lvds.link_window);
            const ImGuiTableFlags tf = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchSame;
            if (ImGui::BeginTable("link", 7, tf)) {
                for (const char* h : {"Channel", "Frames", "Good", "Bad pattern", "Structure err", "SDO ones", "FRAME ones"})
                    ImGui::TableSetupColumn(h);
                ImGui::TableHeadersRow();
                for (int c = 0; c < 4; ++c) {
                    const auto& lc = r->lvds.link[static_cast<std::size_t>(c)];
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::Text("STOP%d", c + 1);
                    const std::uint32_t vals[] = {lc.frames, lc.good, lc.bad_pattern, lc.structure_errors, lc.sdo_ones, lc.frame_ones};
                    for (int k = 0; k < 6; ++k) {
                        ImGui::TableNextColumn();
                        ImGui::PushFont(ui::fonts.mono);
                        const bool bad = (k == 2 || k == 3) && vals[k] != 0;
                        ImGui::TextColored(bad ? ui::c_bad() : ImGui::GetStyleColorVec4(ImGuiCol_Text), "%s", ui::fmt_count(vals[k]).c_str());
                        ImGui::PopFont();
                    }
                }
                ImGui::EndTable();
            }
        }

        if (!v.cal_points.empty() || v.calibrating) {
            ImGui::Spacing();
            if (v.last_cal)
                ImGui::TextColored(v.last_cal->success ? ui::c_ok() : ui::c_bad(), "calibration %s: %s",
                                   v.last_cal->success ? "OK" : "FAILED", v.last_cal->message.c_str());
            else
                ui::dim_text("calibrating… %zu points", v.cal_points.size());
            std::vector<double> xs_good, ys_good, xs_bad, ys_bad;
            for (const auto& p : v.cal_points) {
                double minimum = 1e18;
                for (int c = 0; c < 4; ++c) minimum = std::min(minimum, static_cast<double>(p.link[static_cast<std::size_t>(c)].good));
                (p.good ? xs_good : xs_bad).push_back(p.offset_ps);
                (p.good ? ys_good : ys_bad).push_back(p.good ? minimum : 1.0);
            }
            if (ImPlot::BeginPlot("##cal", ImVec2(-1, 190), ImPlotFlags_NoMenus)) {
                const double bit_ps = v.last_cal ? v.last_cal->bit_ps : timing.samples_per_bit(s.divider, r && r->lvds.ddr) * timing.sample_ps;
                ImPlot::SetupAxes("sampling point in the bit (ps)", "good frames (min of lanes)");
                ImPlot::SetupAxisLimits(ImAxis_X1, -bit_ps * 0.02, bit_ps * 1.02, ImPlotCond_Always);
                ImPlot::SetupAxisLimits(ImAxis_Y1, 0, 2600, ImPlotCond_Once);
                ImPlot::PushStyleColor(ImPlotCol_Fill, ui::c_ok());
                const double bar = std::max(1.0, bit_ps / 150.0);
                if (!xs_good.empty()) ImPlot::PlotBars("error-free", xs_good.data(), ys_good.data(), static_cast<int>(xs_good.size()), bar);
                ImPlot::PopStyleColor();
                ImPlot::PushStyleColor(ImPlotCol_Fill, ui::c_bad());
                if (!xs_bad.empty()) ImPlot::PlotBars("errors", xs_bad.data(), ys_bad.data(), static_cast<int>(xs_bad.size()), bar);
                ImPlot::PopStyleColor();
                if (v.last_cal && v.last_cal->chosen_phase >= 0) {
                    const double x = v.last_cal->chosen_offset_ps;
                    ImPlot::PushStyleColor(ImPlotCol_Line, ui::c_accent());
                    ImPlot::PlotInfLines("chosen", &x, 1);
                    ImPlot::PopStyleColor();
                }
                ImPlot::EndPlot();
            }
        }
    }
    ui::card_end();
}

// ---------------------------------------------------------------------------
// Countrate vs trigger
// ---------------------------------------------------------------------------
namespace {
void write_scan_csv(const std::string& path, const ScanRequest& q, const std::vector<ScanPoint>& pts,
                    const std::array<double, 4>& hyst) {
    std::ofstream out(path);
    out << "# countrate vs trigger: " << q.points << " points per channel from " << q.from_v << " V to " << q.to_v
        << " V, " << q.seconds << " s per point; hysteresis (ohm, read back) ch1..4 " << hyst[0] << " " << hyst[1] << " "
        << hyst[2] << " " << hyst[3] << "\n";
    out << "channel,trigger_v,events,rate_hz,fpga_dropped\n";
    for (const auto& p : pts)
        out << p.channel + 1 << "," << p.volts << "," << p.events << "," << p.rate_hz << "," << p.dropped << "\n";
}
}  // namespace

void page_scan(AppContext& ctx, PagesState& s) {
    const auto& v = ctx.v;
    auto& q = s.scan;
    const float avail_h = ImGui::GetContentRegionAvail().y;
    const float left_w = std::min(430.0f, ImGui::GetContentRegionAvail().x * 0.36f);
    ImGui::BeginChild("##sleft", ImVec2(left_w, avail_h));

    if (ui::card_begin("##sset", "Scan")) {
        const bool lock = v.busy || !v.connected;
        if (lock) ImGui::BeginDisabled();
        const float label_w = 130;
        ui::row_label("Channels", label_w);
        for (int c = 0; c < 4; ++c) {
            if (c) ImGui::SameLine();
            ImGui::PushID(c);
            char name[8];
            std::snprintf(name, sizeof(name), "%d", c + 1);
            ImGui::Checkbox(name, &q.channels[static_cast<std::size_t>(c)]);
            ImGui::PopID();
        }
        ui::row_label("Trigger from", label_w);
        ImGui::SetNextItemWidth(120);
        ImGui::InputDouble("V##from", &q.from_v, 0, 0, "%.3f");
        ui::row_label("Trigger to", label_w);
        ImGui::SetNextItemWidth(120);
        ImGui::InputDouble("V##to", &q.to_v, 0, 0, "%.3f");
        q.from_v = std::clamp(q.from_v, tdcctl::kThresholdMinVolts, tdcctl::kThresholdMaxVolts);
        q.to_v = std::clamp(q.to_v, tdcctl::kThresholdMinVolts, tdcctl::kThresholdMaxVolts);
        ImGui::SameLine();
        if (ImGui::SmallButton("full range")) {
            q.from_v = tdcctl::kThresholdMinVolts;
            q.to_v = tdcctl::kThresholdMaxVolts;
        }
        ui::row_label("Points", label_w);
        ImGui::SetNextItemWidth(120);
        ImGui::InputInt("##pts", &q.points, 0, 0);
        q.points = std::clamp(q.points, 1, 1000);
        ui::row_label("Time per point", label_w);
        ImGui::SetNextItemWidth(120);
        ImGui::InputDouble("s##t", &q.seconds, 0, 0, "%.3f");
        q.seconds = std::clamp(q.seconds, 0.001, 60.0);
        ui::row_label("Set hysteresis", label_w);
        if (ImGui::Checkbox("##sethy", &q.set_hysteresis) && q.set_hysteresis && v.state) {
            for (int c = 0; c < 4; ++c)
                if (q.channels[static_cast<std::size_t>(c)]) {
                    q.hysteresis_ohm = tdcctl::wiper_ohms(v.state->board.wiper(c));
                    break;
                }
        }
        ImGui::SameLine();
        if (!q.set_hysteresis) ImGui::BeginDisabled();
        ImGui::SetNextItemWidth(92);
        ImGui::InputDouble("Ω##hy", &q.hysteresis_ohm, 0, 0, "%.0f");
        q.hysteresis_ohm = std::clamp(q.hysteresis_ohm, tdcctl::kWiperOhms, tdcctl::kWiperOhms + tdcctl::kRheostatOhms);
        ImGui::SameLine();
        ui::dim_text("≈%.0f mV", ui::hysteresis_mv(q.hysteresis_ohm));
        if (!q.set_hysteresis) ImGui::EndDisabled();
        ui::help("Off: each channel keeps its present hysteresis. On: this value is applied to the scanned channels before "
                 "the first point and left set afterwards (only the triggers are put back).\n\nLOWER resistance = MORE "
                 "hysteresis (typ. 0 Ω 47.5 mV, 0.8 kΩ 16.5 mV, 4 kΩ+ 1 mV).");
        int channels = 0;
        for (bool c : q.channels) channels += c ? 1 : 0;
        const double step = q.points > 1 ? (q.to_v - q.from_v) / (q.points - 1) : 0.0;
        ui::dim_text("step %.1f mV · %d points · about %.0f s", step * 1000.0, channels * q.points,
                     channels * q.points * (q.seconds + 0.03));
        ImGui::Spacing();
        if (ui::accent_button("Start scan", ImVec2(130, 0))) ctx.dev.threshold_scan(q);
        if (lock) ImGui::EndDisabled();
        ImGui::SameLine();
        if (!v.scanning) ImGui::BeginDisabled();
        if (ImGui::Button("Stop", ImVec2(100, 0))) ctx.dev.abort();
        if (!v.scanning) ImGui::EndDisabled();
        if (v.scanning || v.scan_total) {
            const float frac = v.scan_total ? static_cast<float>(v.scan_points.size()) / static_cast<float>(v.scan_total) : 0.0f;
            char overlay[64];
            std::snprintf(overlay, sizeof(overlay), "%zu / %d points", v.scan_points.size(), v.scan_total);
            ImGui::ProgressBar(frac, ImVec2(-1, 0), overlay);
            if (!v.scanning && !v.scan_message.empty())
                ImGui::TextColored(v.scan_message == "complete" ? ui::c_ok() : ui::c_warn(), "scan %s; triggers restored",
                                   v.scan_message.c_str());
            else if (v.scanning && !v.scan_points.empty()) {
                const auto& p = v.scan_points.back();
                ui::dim_text("ch%d at %+.3f V: %s", p.channel + 1, p.volts, ui::fmt_rate(p.rate_hz).c_str());
            }
        }
        ImGui::PushTextWrapPos(0);
        if (!v.set_up) ImGui::TextColored(ui::c_warn(), "Set up the board first (TDC-GPX2 & LVDS).");
        ui::dim_text("Each point sets the channel's trigger, then counts for the time per point (timed by the FPGA). "
                     "Channels are scanned one after the other; the other triggers are left alone, and every trigger is "
                     "put back at the end. Measurements must be stopped. A point where the FPGA dropped events is a lower "
                     "bound, drawn hollow.");
        ImGui::PopTextWrapPos();
    }
    ui::card_end();

    if (ui::card_begin("##stable", "Points")) {
        const ImGuiTableFlags tf = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY |
                                   ImGuiTableFlags_SizingStretchProp;
        if (ImGui::BeginTable("pts", 4, tf, ImVec2(0, std::max(160.0f, ImGui::GetContentRegionAvail().y - 4)))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Ch", 0, 0.5f);
            ImGui::TableSetupColumn("Trigger", 0, 1.0f);
            ImGui::TableSetupColumn("Rate", 0, 1.2f);
            ImGui::TableSetupColumn("Dropped", 0, 0.9f);
            ImGui::TableHeadersRow();
            for (const auto& p : v.scan_points) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("%d", p.channel + 1);
                ImGui::TableNextColumn();
                ImGui::PushFont(ui::fonts.mono);
                ImGui::Text("%+.3f V", p.volts);
                ImGui::TableNextColumn();
                ImGui::TextColored(ui::channel_colour(p.channel + 1), "%s", ui::fmt_rate(p.rate_hz).c_str());
                ImGui::TableNextColumn();
                ImGui::TextColored(p.dropped ? ui::c_bad() : ui::c_dim(), "%s", ui::fmt_count(p.dropped).c_str());
                ImGui::PopFont();
            }
            ImGui::EndTable();
        }
    }
    ui::card_end();
    ImGui::EndChild();

    ImGui::SameLine();
    if (ui::card_begin("##splot", "Count rate vs trigger", avail_h - 2)) {
        ImGui::Checkbox("log y", &s.scan_log_y);
        ImGui::SameLine(0, 18);
        const std::string path = ui::csv_button("scan", "Export CSV…", ctx.data_dir + "/" + ui::timestamped("trigger_scan", ".csv"));
        if (!path.empty() && !v.scan_points.empty()) {
            write_scan_csv(path, v.scan_request, v.scan_points, v.scan_hysteresis_ohm);
            ctx.dev.log(Level::ok, "scan exported to " + path);
        }
        if (ImPlot::BeginPlot("##scan", ImVec2(-1, std::max(200.0f, ImGui::GetContentRegionAvail().y - 2)))) {
            ImPlot::SetupAxes("trigger (V)", "count rate (1/s)", ImPlotAxisFlags_None, ImPlotAxisFlags_AutoFit);
            if (s.scan_log_y) ImPlot::SetupAxisScale(ImAxis_Y1, ImPlotScale_Log10);
            const auto& rq = v.scan_total ? v.scan_request : q;
            ImPlot::SetupAxisLimits(ImAxis_X1, std::min(rq.from_v, rq.to_v) - 0.02, std::max(rq.from_v, rq.to_v) + 0.02,
                                    ImPlotCond_Always);
            for (int c = 0; c < 4; ++c) {
                std::vector<double> xs, ys, xl, yl;
                for (const auto& p : v.scan_points) {
                    if (p.channel != c) continue;
                    const double y = s.scan_log_y ? std::max(p.rate_hz, 0.5) : p.rate_hz;
                    xs.push_back(p.volts);
                    ys.push_back(y);
                    if (p.dropped) {
                        xl.push_back(p.volts);
                        yl.push_back(y);
                    }
                }
                if (xs.empty()) continue;
                char name[64];
                const double ohm = v.scan_hysteresis_ohm[static_cast<std::size_t>(c)];
                std::snprintf(name, sizeof(name), "ch%d  (hysteresis %.0f Ω ≈%.0f mV)", c + 1, ohm, ui::hysteresis_mv(ohm));
                ImPlot::SetNextLineStyle(ui::channel_colour(c + 1), 2.0f);
                ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle, 4.0f, ui::channel_colour(c + 1), 0.0f);
                ImPlot::PlotLine(name, xs.data(), ys.data(), static_cast<int>(xs.size()));
                if (!xl.empty()) {
                    ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle, 7.0f, ImVec4(0, 0, 0, 0), 2.0f, ui::c_bad());
                    ImPlot::PlotScatter("##drops", xl.data(), yl.data(), static_cast<int>(xl.size()));
                }
            }
            ImPlot::EndPlot();
        }
    }
    ImGui::EndChild();
}

}  // namespace app
