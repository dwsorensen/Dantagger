#pragma once

// TDC firmware protocol (PROTOCOL.md; designs 0x54440001 = release 1 and
// 0x54440002 = release 2, told apart by the response): command encoding,
// response decoding, the GPX2 configuration builder (with the same board
// rules the FPGA enforces), event-record decoding and time unwrapping, and the
// comparator unit conversions (unchanged from Comparator_Control's cmpctl).
// SDK-neutral; frames and CRC come from Test_project's protocol v1 code.

#include "au2ft601/protocol.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace tdcctl {

using au2ft601::Frame;

enum class Opcode : std::uint32_t {
    start = 1,
    stop = 2,
    status = 3,
    set_leds = 6,
    set_thresholds = 0x20,
    set_hysteresis = 0x21,
    set_led1 = 0x22,
    set_led2 = 0x23,
    probe = 0x24,
    readback = 0x25,
    gpx_configure = 0x30,
    gpx_read_config = 0x31,
    gpx_reset = 0x32,
    gpx_init = 0x33,
    gpx_read_results = 0x34,
    lvds_set = 0x40,
    lvds_phase = 0x41,
    link_test = 0x42,
    run_complete = 0x101,   // response only
};

enum class Status : std::uint32_t {
    ok = 0,
    bad_command = 1,
    bad_argument = 2,
    busy = 3,
    device_error = 5,
    not_ready = 6,
};

constexpr std::uint32_t kDesignVersionR1 = 0x54440001;
constexpr std::uint32_t kDesignVersionR2 = 0x54440002;
constexpr std::uint32_t kDesignVersion = kDesignVersionR2;   // newest (the simulated FPGA's default)
constexpr int kResponseFields = 111;              // release 2; release 1 sends 109 (the rest read 0)
constexpr int kResponseFieldsR1 = 109;
constexpr int kChannels = 4;
constexpr int kGpxRegisters = 17;
constexpr double kPhaseStepPs = 10000.0 / 560.0;  // MMCM fine-phase step: 1/56 of the 1 GHz VCO period (both releases)
constexpr std::uint32_t kLinkTestMaxWindow = 100000000;
constexpr std::size_t kMaxRecordsPerFrame = 503;
constexpr double kCoarseTickNsR1 = 10.0;          // release 1 coarse time: 100 MHz cycles

// LVDS clocking of the loaded image (release 1: fixed; release 2: response
// fields 108-110).  Times in the FPGA: coarse ticks = LINK_TEST cycles =
// lclk_rx periods; a lane is sampled every sample_ps.
struct LinkTiming {
    std::uint32_t design{kDesignVersionR2};
    double lclk_max_mhz{250.0};        // LCLKIN at divider code 0
    double coarse_tick_ns{8.0};
    int phase_steps{224};              // LVDS_PHASE positions (0 .. phase_steps - 1)
    double sample_ps{2000.0};

    static LinkTiming release1() { return {kDesignVersionR1, 100.0, 10.0, 560, 10000.0}; }
    bool release2() const { return design == kDesignVersionR2; }
    bool ddr_supported() const { return release2(); }
    int max_bit_phase(int divider_code, bool ddr) const { return samples_per_bit(divider_code, ddr) - 1; }
    double lclkin_mhz(int divider_code) const { return lclk_max_mhz / (1 << divider_code); }
    // Samples the FPGA takes per GPX2 bit: release 1 one per 10 ns (SDR
    // only); release 2 two per period of lclk_max (both edges).
    int samples_per_bit(int divider_code, bool ddr) const {
        return release2() ? (1 << (divider_code + (ddr ? 0 : 1))) : (1 << divider_code);
    }
    double bit_ns(int divider_code, bool ddr) const { return 1000.0 / lclkin_mhz(divider_code) / (ddr ? 2.0 : 1.0); }
    // Fine-phase steps per sample (release 1: 560, release 2: 112).
    int steps_per_sample() const { return static_cast<int>(sample_ps / kPhaseStepPs + 0.5); }
    std::uint32_t cycles_per_us() const { return static_cast<std::uint32_t>(1000.0 / coarse_tick_ns + 0.5); }
};

// Longest time from a stop edge until its result is in the FPGA (us): the
// GPX2 FIFO full (16 / 8 / 4 results with HIGH_RESOLUTION off / 2x / 4x)
// ahead of it, plus 0.5 us for conversion, LVDS sync and the FPGA pipeline.
double max_result_latency_us(const LinkTiming& timing, int divider_code, bool ddr, int frame_bits, int high_resolution);
// The reference index lets the host place each result in its REFCLK period
// using the FPGA's coarse time, as long as two results' latencies differ by
// less than half a wrap.  Smallest REF_INDEX_BITWIDTH (2 ... 24) whose wrap
// is at least 4 x max_result_latency_us (a factor 2 of margin); 0 if none.
int min_ref_index_bits(const LinkTiming& timing, int divider_code, bool ddr, int stop_bits, int high_resolution,
                       double refclk_hz);

// ---- Comparator (Comparator_Control release 1, unchanged) -------------------
constexpr double kThresholdMinVolts = -0.100;
constexpr double kThresholdMaxVolts = 1.500;
constexpr std::uint16_t kDacCodeMin = 11;
constexpr std::uint16_t kDacCodeMax = 41953;
constexpr double kDacFullScale = 2.5;
constexpr double kThresholdOffset = 2.5 / 24.9;
constexpr double kWiperOhms = 75.0;
constexpr double kRheostatOhms = 5000.0;
constexpr std::uint16_t kWiperCodeMax = 256;
constexpr int kLed1CurrentStepMilliamps = 5;

class ArgumentError : public std::invalid_argument {
public:
    using std::invalid_argument::invalid_argument;
};

struct BoardState {
    std::uint32_t flags{0};             // bit 0 boot, 1 DAC, 2 MCP23S08, 3 U3, 4 U8, 5 AW2013, 6 TMP117, 7 LED mask set
    std::uint32_t first_error{0};
    std::uint32_t error_count{0};
    std::uint32_t op_count{0};
    std::array<std::uint16_t, kChannels> dac_codes{};
    std::uint16_t dac_id{0};
    std::uint16_t dac_gain{0};
    std::uint16_t dac_config{0};
    std::uint16_t dac_status{0};
    std::array<std::uint16_t, kChannels> wipers_raw{};
    std::uint8_t aw_id{0};
    std::uint8_t aw_gcr{0};
    std::uint8_t aw_lctr{0};
    std::array<std::uint8_t, 3> aw_lcfg{};
    std::array<std::uint8_t, 3> aw_pwm{};
    std::uint8_t gpio_iodir{0};
    std::uint8_t gpio_olat{0};
    std::uint32_t dac_code_max{0};

    bool boot_done() const { return (flags & 1U) != 0; }
    bool dac_ok() const { return (flags & 2U) != 0; }
    bool gpio_ok() const { return (flags & 4U) != 0; }
    bool u3_ok() const { return (flags & 8U) != 0; }
    bool u8_ok() const { return (flags & 16U) != 0; }
    bool aw_ok() const { return (flags & 32U) != 0; }
    bool tmp_ok() const { return (flags & 64U) != 0; }
    bool led_mask_set() const { return (flags & 128U) != 0; }
    std::uint16_t wiper(int channel) const { return wipers_raw.at(static_cast<std::size_t>(channel)) & 0x1FFU; }
    int led1_current() const { return aw_lcfg[0] & 3; }
    bool led1_enabled() const { return (aw_gcr & 1U) != 0 && (aw_lctr & 7U) != 0; }
    unsigned led2_mask() const { return (~gpio_olat & ~gpio_iodir) & 7U; }
};

struct TmpState {
    std::uint16_t device_id{0};
    std::uint16_t temp_raw{0};
    std::uint16_t config{0};
    std::uint16_t polls_ok{0};
    std::uint16_t polls_failed{0};
    double celsius() const { return static_cast<std::int16_t>(temp_raw) * 0.0078125; }
    bool id_ok() const { return (device_id & 0x0FFFU) == 0x0117U; }
};

struct GpxState {
    std::uint32_t flags{0};
    std::uint32_t mismatch{0};
    std::array<std::uint8_t, kGpxRegisters> written{};
    std::array<std::uint8_t, kGpxRegisters> readback{};
    std::array<std::uint8_t, 24> results{};      // result registers 8..31
    std::uint16_t ops{0};
    std::uint16_t failed_ops{0};
    std::uint16_t last_error{0};

    bool configured() const { return (flags & 1U) != 0; }
    bool verified() const { return (flags & 2U) != 0; }
    bool measuring() const { return (flags & 4U) != 0; }
    bool parity_pin() const { return (flags & 8U) != 0; }
    bool parity_readback() const { return (flags & 16U) != 0; }
    bool interrupt_low() const { return (flags & 32U) == 0; }
    bool last_failed() const { return (flags & 64U) != 0; }
    bool test_pattern() const { return (flags & 128U) != 0; }
};

struct LinkCounts {
    std::uint32_t frames{0};
    std::uint32_t good{0};
    std::uint32_t bad_pattern{0};
    std::uint32_t structure_errors{0};
    std::uint32_t sdo_ones{0};
    std::uint32_t frame_ones{0};
};

struct LvdsState {
    std::uint32_t raw{0};
    std::array<std::uint8_t, kChannels> sdo_taps{};
    std::array<std::uint8_t, kChannels> frame_taps{};
    int ref_bits{0};
    int stop_bits{0};
    bool format_valid{false};
    std::uint32_t link_window{0};
    std::array<LinkCounts, kChannels> link{};
    std::array<std::uint16_t, 8> lclkout_hist{};
    bool ddr{false};                   // capture format (release 2)
    LinkTiming timing;

    bool lclkin_enabled() const { return (raw & 1U) != 0; }
    int divider_code() const { return static_cast<int>((raw >> 1U) & 3U); }
    int bit_phase() const { return static_cast<int>((raw >> 3U) & 15U); }   // release 1: bit 6 always 0
    bool mmcm_locked() const { return (raw & 0x100U) != 0; }
    bool idelay_ready() const { return (raw & 0x200U) != 0; }
    bool controller_timeout() const { return (raw & 0x400U) != 0; }
    int phase_step() const { return static_cast<int>(raw >> 16U); }
    double lclkin_mhz() const { return timing.lclkin_mhz(divider_code()); }
    int samples_per_bit() const { return timing.samples_per_bit(divider_code(), ddr); }
};

struct StreamState {
    std::uint32_t flags{0};
    std::uint32_t window_us{0};
    std::uint64_t records{0};
    std::uint64_t frames{0};
    std::uint32_t dropped{0};
    std::uint64_t captured{0};
    std::uint32_t structure_errors{0};
    std::uint32_t fifo_max{0};
    std::uint64_t stall_cycles{0};
    std::uint64_t rxf_tx_cycles{0};
    std::uint64_t run_id{0};

    bool capture_requested() const { return (flags & 1U) != 0; }
    bool draining() const { return (flags & 2U) != 0; }
    bool capture_active() const { return (flags & 4U) != 0; }
    bool window_ended() const { return (flags & 8U) != 0; }
    bool stop_pending() const { return (flags & 16U) != 0; }
    bool streaming() const { return (flags & 3U) != 0; }
};

struct Response {
    std::uint64_t run_id{0};
    std::uint64_t transaction_id{0};
    std::uint32_t opcode{0};
    Status status{Status::ok};
    std::array<std::uint32_t, kResponseFields> fields{};
    std::uint32_t design_version{0};
    std::uint32_t fault_bits{0};
    std::uint32_t commands_ok{0};
    std::uint32_t commands_bad{0};
    std::uint32_t au_led_mask{0};
    BoardState board;
    TmpState tmp;
    GpxState gpx;
    LvdsState lvds;
    StreamState stream;
};

// ---- GPX2 configuration -------------------------------------------------------
// Named settings -> registers 0..16 (datasheet figures 23-30), always with the
// board rules: every input LVDS (CMOS_INPUT 0), REFCLK from the LMK1D1204
// (PIN_ENA_REFCLK 1, REFCLK_BY_XOSC 0), fixed bits as defined by ams.  DDR
// only with release 2 (gpx_rule_violation's allow_ddr).
struct GpxConfig {
    unsigned channel_mask{0xF};         // STOP1..4 pin and hit enables
    bool lvds_out{true};
    bool test_pattern{false};
    int high_resolution{0};             // 0 off, 1 = 2x, 2 = 4x
    int channel_combine{0};             // 0 none, 1 pulse distance, 2 pulse width
    int ref_index_bits{24};             // 0, 2, 4, 6, 8, 12, 16 or 24
    int stop_bits{20};                  // 14, 16, 18 or 20
    bool ddr{false};                    // LVDS_DOUBLE_DATA_RATE
    std::uint32_t refclk_divisions{125000};
    int data_valid_adjust_ps{0};        // -160, 0, 160 or 320
    bool common_fifo_read{false};
    bool blockwise_fifo_read{false};
    bool pin_ena_disable{false};
    bool pin_ena_rstidx{false};

    // The register bytes; throws ArgumentError for any invalid setting.
    std::array<std::uint8_t, kGpxRegisters> bytes() const;
    // Divisions for an LSB of lsb_ps at a reference clock of refclk_hz;
    // throws unless the reference period is a whole number of LSBs.
    static std::uint32_t divisions_for(double refclk_hz, double lsb_ps);
};

// Empty if `regs` satisfies every rule the FPGA checks (PROTOCOL.md 2.1),
// otherwise the first rule broken.
std::string gpx_rule_violation(const std::array<std::uint8_t, kGpxRegisters>& regs, bool allow_ddr = true);
// Decoded widths of register 2.
int gpx_ref_index_bits(std::uint8_t reg2);
int gpx_stop_bits(std::uint8_t reg2);
std::uint32_t gpx_divisions(const std::array<std::uint8_t, kGpxRegisters>& regs);
std::string describe_gpx_config(const std::array<std::uint8_t, kGpxRegisters>& regs);

// ---- Command frames ---------------------------------------------------------
Frame encode_simple(Opcode opcode, std::uint64_t transaction);          // no payload
Frame encode_start(std::uint64_t run_id, std::uint64_t transaction, std::uint32_t window_us);
Frame encode_set_leds(std::uint64_t transaction, std::uint8_t mask);
Frame encode_thresholds(std::uint64_t transaction, unsigned mask, const std::array<std::uint16_t, kChannels>& codes);
Frame encode_hysteresis(std::uint64_t transaction, unsigned mask, const std::array<std::uint16_t, kChannels>& wipers);
Frame encode_led1(std::uint64_t transaction, std::uint8_t red, std::uint8_t green, std::uint8_t blue, int current_setting);
Frame encode_led2(std::uint64_t transaction, unsigned on_mask);
Frame encode_gpx_configure(std::uint64_t transaction, const std::array<std::uint8_t, kGpxRegisters>& regs);
Frame encode_lvds_set(std::uint64_t transaction, bool enable, int divider_code, int bit_phase,
                      const std::array<std::uint8_t, kChannels>& sdo_taps,
                      const std::array<std::uint8_t, kChannels>& frame_taps);
Frame encode_lvds_phase(std::uint64_t transaction, int step);
Frame encode_link_test(std::uint64_t transaction, std::uint32_t window_cycles);

// Empty if the capture settings in a STATUS response keep event times
// unambiguous (reference index long enough for the current LCLKIN, frame and
// HIGH_RESOLUTION; min_ref_index_bits), otherwise what to change.
std::string capture_problem(const Response& status, double refclk_hz = 8.0e6);
// Stop-result width (14 ... 20) that holds `divisions` (REFCLK_DIVISIONS <= 2^bits - 1).
int min_stop_bits(std::uint32_t divisions);

// Decodes a CRC-valid RESPONSE frame of this design (fixed fields, padding,
// design version).  Returns nullopt and sets `error` otherwise.
std::optional<Response> decode_response(const Frame& frame, std::string& error);

// ---- DATA frames and records --------------------------------------------------
struct DataFrame {
    std::uint64_t run_id{0};
    std::uint64_t sequence{0};
    std::uint64_t records_before{0};
    std::uint64_t timestamp{0};
    std::vector<std::uint64_t> records;
};
// Checks a CRC-valid DATA frame (header fields, length, zero padding).
std::optional<DataFrame> decode_data_frame(const Frame& frame, std::string& error);

enum class MarkerType : int { start = 0, time = 1, overflow = 2, end = 3 };

struct Record {
    std::uint64_t raw{0};
    bool marker{false};
    // event
    int channel{0};
    std::uint16_t coarse_low{0};
    std::uint32_t ref_index{0};
    std::uint32_t stop{0};
    // marker
    MarkerType marker_type{MarkerType::time};
    std::uint16_t info{0};             // OVERFLOW: drops (saturating 4095); END: bit 0 window
    std::uint64_t coarse{0};           // full 48-bit coarse time
};
Record decode_record(std::uint64_t raw);

// Unwraps the coarse time of every record and the reference index of every
// event (PROTOCOL.md 3.3), giving event times relative to the run.
struct EventTime {
    int channel{0};
    std::uint64_t coarse{0};            // unwrapped FPGA coarse time (coarse ticks)
    std::int64_t ref_index{0};          // unwrapped reference index (first event's wraps = 0)
    std::uint32_t stop{0};
    double time_ps{0.0};                // (ref_index * divisions + stop) * lsb, minus the first event's
};

class TimeUnwrapper {
public:
    // refclk_hz: reference clock (8 MHz on board); divisions/ref_bits from
    // the GPX2 configuration of the run; coarse_tick_ns from the image
    // (LinkTiming: 10 ns release 1, 8 ns release 2 at 250 MHz).
    TimeUnwrapper(double refclk_hz, std::uint32_t divisions, int ref_bits, double coarse_tick_ns = kCoarseTickNsR1);
    // Returns the event time for an event record (nullopt for markers);
    // throws std::runtime_error if the stream breaks the record rules.
    std::optional<EventTime> feed(const Record& record);
    std::uint64_t coarse() const { return coarse_; }
    bool started() const { return started_; }

private:
    double ref_period_ps_;
    double lsb_ps_;
    double coarse_tick_ps_;
    std::uint32_t divisions_;
    int ref_bits_;
    bool started_{false};
    std::uint64_t coarse_{0};
    bool have_event_{false};
    std::uint64_t last_event_coarse_{0};
    std::int64_t last_ref_{0};
    double first_time_ps_{0.0};
};

// ---- Comparator conversions ---------------------------------------------------
std::uint16_t threshold_code(double volts);
double threshold_volts(std::uint16_t code);
std::uint16_t wiper_code(double ohms);
double wiper_ohms(std::uint16_t code);
int led1_current_setting(int milliamps);

// ---- Names ------------------------------------------------------------------------
std::string status_name(Status status);
std::string opcode_name(std::uint32_t opcode);
std::string describe_first_error(std::uint32_t first_error);
std::string describe_faults(std::uint32_t fault_bits);
std::string describe_gpx_error(std::uint16_t last_error);

}  // namespace tdcctl
