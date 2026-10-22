#pragma once

// Shared look of the Dantagger app (fonts, theme, cards, small widgets,
// number formatting), taken from the TDC Control app.

#include <imgui.h>

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ui {

struct Fonts {
    ImFont* body{nullptr};
    ImFont* small{nullptr};
    ImFont* heading{nullptr};
    ImFont* title{nullptr};
    ImFont* mono{nullptr};
    ImFont* big{nullptr};               // large numbers (count-rate tiles)
};
extern Fonts fonts;

void load_fonts(float scale);
void apply_theme(float scale);

// Palette
inline ImVec4 c_accent() { return ImVec4(0.33f, 0.56f, 1.00f, 1.00f); }
inline ImVec4 c_ok() { return ImVec4(0.30f, 0.82f, 0.52f, 1.00f); }
inline ImVec4 c_bad() { return ImVec4(1.00f, 0.38f, 0.38f, 1.00f); }
inline ImVec4 c_warn() { return ImVec4(1.00f, 0.74f, 0.30f, 1.00f); }
inline ImVec4 c_dim() { return ImVec4(0.58f, 0.63f, 0.71f, 1.00f); }
inline ImVec4 c_fit() { return ImVec4(1.00f, 0.55f, 0.35f, 1.00f); }
inline ImVec4 c_panel() { return ImVec4(0.098f, 0.114f, 0.141f, 1.0f); }
// One colour per channel (1..4), the same in every plot.
ImVec4 channel_colour(int channel);

// A rounded card with a heading; returns false when clipped (call card_end anyway).
bool card_begin(const char* id, const char* title, float height = 0.0f, float width = 0.0f);
void card_end();
void heading(const char* text);
void dim_text(const char* fmt, ...) IM_FMTARGS(1);
void warn_text(const char* fmt, ...) IM_FMTARGS(1);   // wrapped, warning colour
void ok_badge(bool ok, const char* ok_text = "OK", const char* bad_text = "FAIL");
void help(const char* text);
bool accent_button(const char* label, const ImVec2& size = ImVec2(0, 0));
bool danger_button(const char* label, const ImVec2& size = ImVec2(0, 0));
void status_dot(ImU32 colour, float radius);
// A square button with a drawn icon (the fonts have no ▶ / ■ glyphs).
enum class Icon { play, stop };
bool icon_button(const char* id, Icon icon, float size, bool accent);
void row_label(const char* text, float label_w);
// The next toolbar item on this line if `wanted` more pixels fit, else on a new line.
void toolbar_next(float wanted);

std::string fmt_rate(double hz);                 // "12.3 k/s"
std::string fmt_time_ps(double ps);              // auto ps / ns / µs / ms / s
std::string fmt_count(std::uint64_t n);          // thousands separators
std::string fmt_bytes(double bytes);
// Value with its uncertainty, in a time unit chosen by the value, to the error's two significant digits.
std::string fmt_time_err(double value_ps, double err_ps);
// Typical TLV3901 input hysteresis (mV) for a HYS resistance, interpolated
// between the datasheet's typical points at 5 V (0 Ω 47.5 mV, 0.8 kΩ
// 16.5 mV, >= 4 kΩ 1 mV).  Rough: lower resistance = more hysteresis.
double hysteresis_mv(double ohms);
extern const char* kHysteresisHelp;

// Y-axis limits with a margin around the plotted data [lo, hi]: 5 % of the
// span on each side (on a log axis 5 % of the decades, at least 0.05 decade;
// non-positive values are ignored there).  Set them for the plot being set
// up: every frame with auto-fit, once without (the user zooms).
void padded_limits(double lo, double hi, bool log, double& out_lo, double& out_hi);
void setup_y_padded(double lo, double hi, bool log, bool autofit);
// The same for a set of values (NaNs skipped; an empty set gives 0 ... 1).
void setup_y_padded(const double* values, std::size_t n, bool log, bool autofit);

// Plot time units: 0 auto, 1 ps, 2 ns, 3 µs, 4 ms, 5 s.
extern const char* kUnits[6];
extern const double kUnitPs[6];
int resolve_unit(int unit, double range_ps);

// A time entry with its own unit menu (ps ... s); the value is kept in ps,
// rounded to whole ps and clamped to [min_ps, max_ps].  Returns true when
// the value changed; `committed` (optional) is set when an edit is finished
// (Enter or focus lost).
bool time_input(const char* id, double* ps, double min_ps, double max_ps, float width = 120.0f,
                bool* committed = nullptr);

// A number entry for a value that is applied elsewhere (the caller's copy is
// fresh every frame): returns true once an edit is finished (Enter, focus
// lost, or a -/+ step), with *value the entered number.
bool input_commit(const char* id, double* value, double step, double step_fast, const char* format, float width = -1.0f);

// "<prefix>_YYYYmmdd_HHMMSS<ext>"
std::string timestamped(const std::string& prefix, const std::string& ext);
// A file name made of letters, digits, '-' and '_' from `text`.
std::string safe_name(const std::string& text);

// A "Save CSV…" button with an asynchronous save dialog (one per key);
// returns the chosen path once (empty otherwise).  `default_path` is the
// suggestion.  Headless runs (self-test) skip the dialog: `headless_path`
// set by the test is returned at once.
std::string csv_button(const std::string& key, const char* label, const std::string& default_path);
extern std::map<std::string, std::string> g_headless_csv;
// Self-test: the next frame's csv_button(key) acts as if pressed, writing to this path.
extern std::map<std::string, std::string> g_force_csv;
bool headless();
void set_headless(bool on);

bool write_png(const std::string& path, int width, int height, const unsigned char* rgba_top_down);

// Screen rectangles of named widgets, recorded for the self-test.
extern std::map<std::string, std::pair<ImVec2, ImVec2>> g_probes;
void probe(const std::string& name);   // records the last item

}  // namespace ui
