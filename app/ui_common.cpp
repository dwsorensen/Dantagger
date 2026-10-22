#include "ui_common.hpp"

#include <portable-file-dialogs.h>

#include <algorithm>
#include <cctype>
#include <ctime>
#include <filesystem>

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <fstream>
#include <vector>

extern const unsigned char k_font_roboto[];
extern const std::size_t k_font_roboto_size;
extern const unsigned char k_font_cousine[];
extern const std::size_t k_font_cousine_size;

namespace ui {

Fonts fonts;

namespace {

ImFont* add_font(const unsigned char* data, std::size_t size, float px) {
    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = false;   // the data is a static array
    cfg.OversampleH = 3;
    cfg.OversampleV = 2;
    static const ImWchar ranges[] = {0x0020, 0x00FF, 0x0394, 0x0394, 0x03A9, 0x03A9, 0x03BC, 0x03BC, 0x2013, 0x2014, 0x2022, 0x2022, 0x2248, 0x2248,
                                     0x2026, 0x2026, 0x2192, 0x2192, 0x2212, 0x2212, 0};
    return ImGui::GetIO().Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(data), static_cast<int>(size), px, &cfg,
                                                      ranges);
}

}  // namespace

void load_fonts(float scale) {
    auto& io = ImGui::GetIO();
    io.Fonts->Clear();
    fonts.body = add_font(k_font_roboto, k_font_roboto_size, std::round(16.0f * scale));
    fonts.small = add_font(k_font_roboto, k_font_roboto_size, std::round(13.5f * scale));
    fonts.heading = add_font(k_font_roboto, k_font_roboto_size, std::round(19.0f * scale));
    fonts.title = add_font(k_font_roboto, k_font_roboto_size, std::round(26.0f * scale));
    fonts.mono = add_font(k_font_cousine, k_font_cousine_size, std::round(15.0f * scale));
    fonts.big = add_font(k_font_cousine, k_font_cousine_size, std::round(30.0f * scale));
    io.FontDefault = fonts.body;
}

void apply_theme(float scale) {
    ImGuiStyle& st = ImGui::GetStyle();
    st = ImGuiStyle();
    st.WindowPadding = ImVec2(16, 14);
    st.FramePadding = ImVec2(10, 6);
    st.CellPadding = ImVec2(10, 5);
    st.ItemSpacing = ImVec2(10, 8);
    st.ItemInnerSpacing = ImVec2(8, 6);
    st.IndentSpacing = 18;
    st.ScrollbarSize = 12;
    st.GrabMinSize = 12;
    st.WindowRounding = 0;
    st.TabBarBorderSize = 1;
    st.DockingSeparatorSize = 3;
    st.ChildRounding = 10;
    st.FrameRounding = 6;
    st.PopupRounding = 8;
    st.ScrollbarRounding = 8;
    st.GrabRounding = 6;
    st.TabRounding = 6;
    st.WindowBorderSize = 0;
    st.ChildBorderSize = 1;
    st.FrameBorderSize = 0;
    st.PopupBorderSize = 1;
    st.SeparatorTextBorderSize = 1;
    st.SelectableTextAlign = ImVec2(0.0f, 0.5f);

    ImVec4* c = st.Colors;
    const ImVec4 bg(0.067f, 0.078f, 0.098f, 1.0f);
    const ImVec4 panel(0.098f, 0.114f, 0.141f, 1.0f);
    const ImVec4 frame(0.137f, 0.157f, 0.196f, 1.0f);
    const ImVec4 frame_hi(0.176f, 0.204f, 0.255f, 1.0f);
    const ImVec4 accent = c_accent();
    c[ImGuiCol_Text] = ImVec4(0.90f, 0.92f, 0.95f, 1.0f);
    c[ImGuiCol_TextDisabled] = ImVec4(0.45f, 0.50f, 0.57f, 1.0f);
    c[ImGuiCol_WindowBg] = bg;
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = ImVec4(0.110f, 0.125f, 0.157f, 0.98f);
    c[ImGuiCol_Border] = ImVec4(0.20f, 0.23f, 0.29f, 0.80f);
    c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = frame;
    c[ImGuiCol_FrameBgHovered] = frame_hi;
    c[ImGuiCol_FrameBgActive] = ImVec4(0.20f, 0.24f, 0.31f, 1.0f);
    c[ImGuiCol_TitleBg] = panel;
    c[ImGuiCol_TitleBgActive] = ImVec4(0.12f, 0.14f, 0.18f, 1.0f);
    c[ImGuiCol_TitleBgCollapsed] = panel;
    c[ImGuiCol_MenuBarBg] = panel;
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = ImVec4(0.25f, 0.29f, 0.36f, 1.0f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.31f, 0.36f, 0.44f, 1.0f);
    c[ImGuiCol_ScrollbarGrabActive] = accent;
    c[ImGuiCol_CheckMark] = accent;
    c[ImGuiCol_SliderGrab] = accent;
    c[ImGuiCol_SliderGrabActive] = ImVec4(0.45f, 0.65f, 1.0f, 1.0f);
    c[ImGuiCol_Button] = frame_hi;
    c[ImGuiCol_ButtonHovered] = ImVec4(0.23f, 0.27f, 0.34f, 1.0f);
    c[ImGuiCol_ButtonActive] = ImVec4(0.27f, 0.32f, 0.40f, 1.0f);
    c[ImGuiCol_Header] = ImVec4(accent.x, accent.y, accent.z, 0.18f);
    c[ImGuiCol_HeaderHovered] = ImVec4(accent.x, accent.y, accent.z, 0.26f);
    c[ImGuiCol_HeaderActive] = ImVec4(accent.x, accent.y, accent.z, 0.34f);
    c[ImGuiCol_Separator] = ImVec4(0.20f, 0.23f, 0.29f, 1.0f);
    c[ImGuiCol_ResizeGrip] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_Tab] = frame;
    c[ImGuiCol_TabHovered] = frame_hi;
    c[ImGuiCol_TabActive] = ImVec4(accent.x, accent.y, accent.z, 0.35f);
    c[ImGuiCol_TabUnfocused] = frame;
    c[ImGuiCol_TabUnfocusedActive] = ImVec4(accent.x, accent.y, accent.z, 0.22f);
    c[ImGuiCol_DockingPreview] = ImVec4(accent.x, accent.y, accent.z, 0.45f);
    c[ImGuiCol_DockingEmptyBg] = bg;
    c[ImGuiCol_PlotHistogram] = accent;
    c[ImGuiCol_PlotLines] = accent;
    c[ImGuiCol_TableHeaderBg] = ImVec4(0.118f, 0.137f, 0.169f, 1.0f);
    c[ImGuiCol_TableBorderStrong] = ImVec4(0.20f, 0.23f, 0.29f, 1.0f);
    c[ImGuiCol_TableBorderLight] = ImVec4(0.16f, 0.18f, 0.23f, 1.0f);
    c[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1, 1, 1, 0.025f);
    c[ImGuiCol_TextSelectedBg] = ImVec4(accent.x, accent.y, accent.z, 0.35f);
    c[ImGuiCol_NavHighlight] = accent;
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.55f);
    st.ScaleAllSizes(scale);
}

bool card_begin(const char* id, const char* title, float height, float width) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.098f, 0.114f, 0.141f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16 * ImGui::GetIO().FontGlobalScale, 12));
    const ImGuiChildFlags flags = ImGuiChildFlags_Border | (height <= 0 ? ImGuiChildFlags_AutoResizeY : 0) |
                                  ImGuiChildFlags_AlwaysUseWindowPadding;
    const bool open = ImGui::BeginChild(id, ImVec2(width, height), flags);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    if (title && *title) heading(title);
    return open;
}

void card_end() {
    ImGui::EndChild();
    ImGui::Dummy(ImVec2(0, 2));
}

void heading(const char* text) {
    ImGui::PushFont(fonts.heading);
    ImGui::TextUnformatted(text);
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, 2));
}

void dim_text(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    ImGui::PushStyleColor(ImGuiCol_Text, c_dim());
    ImGui::TextV(fmt, args);
    ImGui::PopStyleColor();
    va_end(args);
}

void ok_badge(bool ok, const char* ok_text, const char* bad_text) {
    ImGui::TextColored(ok ? c_ok() : c_bad(), "%s", ok ? ok_text : bad_text);
}

void help(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

bool accent_button(const char* label, const ImVec2& size) {
    const ImVec4 a = c_accent();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(a.x * 0.85f, a.y * 0.85f, a.z * 0.85f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, a);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(a.x * 0.7f, a.y * 0.7f, a.z * 0.7f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
    const bool pressed = ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
    return pressed;
}

void status_dot(ImU32 colour, float radius) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = ImGui::GetTextLineHeight();
    ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + radius, p.y + h * 0.5f), radius, colour, 16);
    ImGui::Dummy(ImVec2(radius * 2 + 2, h));
}

std::string fmt_rate(double hz) {
    char buf[48];
    if (hz >= 1e6) std::snprintf(buf, sizeof(buf), "%.3f M/s", hz / 1e6);
    else if (hz >= 1e3) std::snprintf(buf, sizeof(buf), "%.3f k/s", hz / 1e3);
    else std::snprintf(buf, sizeof(buf), "%.2f /s", hz);
    return buf;
}

std::string fmt_time_ps(double ps) {
    char buf[48];
    const double a = std::fabs(ps);
    if (a >= 1e12) std::snprintf(buf, sizeof(buf), "%.4g s", ps / 1e12);
    else if (a >= 1e9) std::snprintf(buf, sizeof(buf), "%.4g ms", ps / 1e9);
    else if (a >= 1e6) std::snprintf(buf, sizeof(buf), "%.4g µs", ps / 1e6);
    else if (a >= 1e3) std::snprintf(buf, sizeof(buf), "%.4g ns", ps / 1e3);
    else std::snprintf(buf, sizeof(buf), "%.4g ps", ps);
    return buf;
}

double hysteresis_mv(double ohms) {
    static const double r[] = {0.0, 800.0, 4000.0};
    static const double mv[] = {47.5, 16.5, 1.0};
    if (ohms <= r[0]) return mv[0];
    if (ohms >= r[2]) return mv[2];
    const int k = ohms < r[1] ? 0 : 1;
    return mv[k] + (mv[k + 1] - mv[k]) * (ohms - r[k]) / (r[k + 1] - r[k]);
}

const char* kHysteresisHelp =
    "The setting is the TLV3901's HYS resistance: LOWER resistance = MORE hysteresis (datasheet typ. at 5 V: 0 Ω 47.5 mV, "
    "0.8 kΩ 16.5 mV, 4 kΩ and above 1 mV; the mV shown is interpolated and rough). With little hysteresis, noise at the "
    "trigger makes the comparator chatter (many false counts). With a lot, the output only resets when the input falls "
    "about half the hysteresis below the trigger: if the signal's baseline never gets that low, nothing is counted.";

ImVec4 channel_colour(int channel) {
    static const ImVec4 k[4] = {ImVec4(0.33f, 0.56f, 1.00f, 1.0f), ImVec4(0.30f, 0.82f, 0.52f, 1.0f),
                                ImVec4(1.00f, 0.74f, 0.30f, 1.0f), ImVec4(0.80f, 0.50f, 1.00f, 1.0f)};
    return k[std::clamp(channel, 1, 4) - 1];
}

void warn_text(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    ImGui::PushTextWrapPos(0);
    ImGui::PushStyleColor(ImGuiCol_Text, c_warn());
    ImGui::TextV(fmt, args);
    ImGui::PopStyleColor();
    ImGui::PopTextWrapPos();
    va_end(args);
}

bool danger_button(const char* label, const ImVec2& size) {
    const ImVec4 a = c_bad();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(a.x * 0.55f, a.y * 0.30f, a.z * 0.30f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(a.x * 0.75f, a.y * 0.40f, a.z * 0.40f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(a.x * 0.45f, a.y * 0.25f, a.z * 0.25f, 1.0f));
    const bool pressed = ImGui::Button(label, size);
    ImGui::PopStyleColor(3);
    return pressed;
}

bool icon_button(const char* id, Icon icon, float size, bool accent) {
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(size, size));
    const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
    const bool active = ImGui::IsItemActive();
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    ImVec4 bg = accent ? c_accent() : c_bad();
    const float k = active ? 0.65f : hovered ? 1.0f : 0.82f;
    bg = ImVec4(bg.x * k, bg.y * k, bg.z * k, ImGui::GetStyle().Alpha);
    auto* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(a, b, ImGui::GetColorU32(bg), ImGui::GetStyle().FrameRounding);
    const ImU32 fg = ImGui::GetColorU32(ImVec4(1, 1, 1, ImGui::GetStyle().Alpha));
    const ImVec2 c((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
    const float r = size * 0.22f;
    if (icon == Icon::play) dl->AddTriangleFilled(ImVec2(c.x - r * 0.8f, c.y - r), ImVec2(c.x - r * 0.8f, c.y + r), ImVec2(c.x + r, c.y), fg);
    else dl->AddRectFilled(ImVec2(c.x - r * 0.85f, c.y - r * 0.85f), ImVec2(c.x + r * 0.85f, c.y + r * 0.85f), fg, 1.5f);
    return pressed;
}

void row_label(const char* text, float label_w) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(text);
    ImGui::SameLine(label_w);
}

void toolbar_next(float wanted) {
    const float right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
    if (ImGui::GetItemRectMax().x + 18 + wanted <= right) ImGui::SameLine(0, 18);
}

std::string fmt_bytes(double b) {
    char buf[32];
    if (b >= 1e9) std::snprintf(buf, sizeof buf, "%.2f GB", b / 1e9);
    else if (b >= 1e6) std::snprintf(buf, sizeof buf, "%.1f MB", b / 1e6);
    else if (b >= 1e3) std::snprintf(buf, sizeof buf, "%.1f kB", b / 1e3);
    else std::snprintf(buf, sizeof buf, "%.0f B", b);
    return buf;
}

std::string fmt_time_err(double v, double err) {
    const double a = std::fabs(v);
    const char* unit = "ps";
    double k = 1;
    if (a >= 1e9) unit = "ms", k = 1e9;
    else if (a >= 1e6) unit = "µs", k = 1e6;
    else if (a >= 1e3) unit = "ns", k = 1e3;
    const double e = err / k;
    const int digits = e > 0 ? std::clamp(1 - static_cast<int>(std::floor(std::log10(e))), 0, 7) : 3;
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*f ± %.*f %s", digits, v / k, digits, e, unit);
    return buf;
}

const char* kUnits[6] = {"auto", "ps", "ns", "µs", "ms", "s"};
const double kUnitPs[6] = {1.0, 1.0, 1e3, 1e6, 1e9, 1e12};

int resolve_unit(int unit, double range_ps) {
    if (unit != 0) return unit;
    const double a = std::fabs(range_ps);
    return a < 1e4 ? 1 : a < 1e7 ? 2 : a < 1e10 ? 3 : a < 1e13 ? 4 : 5;
}

bool time_input(const char* id, double* ps, double min_ps, double max_ps, float width, bool* committed) {
    static std::map<ImGuiID, int> units;        // per entry: 1 ps ... 5 s
    static std::map<ImGuiID, double> pending;   // the value being typed (the caller's copy may be fresh each frame)
    static const char* names[] = {"ps", "ns", "µs", "ms", "s"};
    ImGui::PushID(id);
    const ImGuiID key = ImGui::GetID("##unit");
    const ImGuiID vid = ImGui::GetID("##v");
    auto it = units.find(key);
    if (it == units.end()) {   // start with the unit that shows the value best
        const double a = std::fabs(*ps);
        it = units.emplace(key, a >= 1e12 ? 5 : a >= 1e9 ? 4 : a >= 1e6 ? 3 : a >= 1e3 ? 2 : 1).first;
    }
    int& u = it->second;
    const auto p = pending.find(vid);
    double shown = (p != pending.end() ? p->second : *ps) / kUnitPs[u];
    ImGui::SetNextItemWidth(width);
    bool changed = false;
    if (ImGui::InputDouble("##v", &shown, 0, 0, u == 1 ? "%.0f" : "%.6g")) {
        *ps = std::clamp(std::round(shown * kUnitPs[u]), min_ps, max_ps);
        pending[vid] = *ps;
        changed = true;
    }
    const bool done = ImGui::IsItemDeactivatedAfterEdit();
    if (done) {
        if (const auto q = pending.find(vid); q != pending.end()) *ps = q->second;
        pending.erase(vid);
    } else if (!ImGui::IsItemActive()) {
        pending.erase(vid);
    }
    if (committed) *committed = done;
    ImGui::SameLine(0, 4);
    int choice = u - 1;
    ImGui::SetNextItemWidth(ImGui::CalcTextSize("ms").x + ImGui::GetFrameHeight() + 2 * ImGui::GetStyle().FramePadding.x);
    if (ImGui::Combo("##unit", &choice, names, 5)) u = choice + 1;
    ImGui::PopID();
    return changed;
}

bool input_commit(const char* id, double* value, double step, double step_fast, const char* format, float width) {
    static std::map<ImGuiID, double> pending;
    const ImGuiID key = ImGui::GetID(id);
    if (const auto p = pending.find(key); p != pending.end()) *value = p->second;
    ImGui::SetNextItemWidth(width);
    if (ImGui::InputDouble(id, value, step, step_fast, format)) pending[key] = *value;
    const bool done = ImGui::IsItemDeactivatedAfterEdit();
    if (done || !ImGui::IsItemActive()) {
        if (const auto p = pending.find(key); p != pending.end()) *value = p->second;
        pending.erase(key);
    }
    return done;
}

std::string timestamped(const std::string& prefix, const std::string& ext) {
    char buf[32];
    const std::time_t t = std::time(nullptr);
    std::strftime(buf, sizeof(buf), "_%Y%m%d_%H%M%S", std::localtime(&t));
    return prefix + buf + ext;
}

std::string safe_name(const std::string& text) {
    std::string out;
    for (char c : text) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_') out += c;
        else if (c == ' ' && !out.empty() && out.back() != '_') out += '_';
    }
    return out.empty() ? std::string("measurement") : out;
}

std::map<std::string, std::string> g_headless_csv;
std::map<std::string, std::string> g_force_csv;
namespace {
bool g_headless = false;
std::map<std::string, std::unique_ptr<pfd::save_file>> g_csv_dialogs;
}  // namespace
bool headless() { return g_headless; }
void set_headless(bool on) { g_headless = on; }

std::string csv_button(const std::string& key, const char* label, const std::string& default_path) {
    auto& dlg = g_csv_dialogs[key];
    const bool pressed = ImGui::Button(label);
    if (const auto f = g_force_csv.find(key); f != g_force_csv.end()) {
        std::string path = f->second;
        g_force_csv.erase(f);
        return path;
    }
    if (pressed && g_headless) {
        auto it = g_headless_csv.find(key);
        return it == g_headless_csv.end() ? default_path : it->second;
    }
    if (pressed && !dlg)
        dlg = std::make_unique<pfd::save_file>("Export CSV", default_path, std::vector<std::string>{"CSV (*.csv)", "*.csv"});
    if (dlg && dlg->ready(0)) {
        std::string path = dlg->result();
        dlg.reset();
        return path;
    }
    return {};
}

std::map<std::string, std::pair<ImVec2, ImVec2>> g_probes;
void probe(const std::string& name) { g_probes[name] = {ImGui::GetItemRectMin(), ImGui::GetItemRectMax()}; }

std::string fmt_count(std::uint64_t n) {
    std::string s = std::to_string(n);
    for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) s.insert(static_cast<std::size_t>(i), ",");
    return s;
}

// ---- PNG (stored deflate; for self-test screenshots) -------------------------------
namespace {
std::uint32_t crc32(const unsigned char* d, std::size_t n, std::uint32_t c = 0xFFFFFFFFU) {
    static std::uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t k = i;
            for (int j = 0; j < 8; ++j) k = (k & 1U) ? 0xEDB88320U ^ (k >> 1U) : k >> 1U;
            table[i] = k;
        }
        init = true;
    }
    for (std::size_t i = 0; i < n; ++i) c = table[(c ^ d[i]) & 0xFFU] ^ (c >> 8U);
    return c;
}
void put32(std::vector<unsigned char>& v, std::uint32_t x) {
    for (int i = 3; i >= 0; --i) v.push_back(static_cast<unsigned char>(x >> (8 * i)));
}
void chunk(std::ofstream& out, const char* type, const std::vector<unsigned char>& data) {
    std::vector<unsigned char> c;
    put32(c, static_cast<std::uint32_t>(data.size()));
    c.insert(c.end(), type, type + 4);
    c.insert(c.end(), data.begin(), data.end());
    const std::uint32_t crc = crc32(c.data() + 4, c.size() - 4) ^ 0xFFFFFFFFU;
    put32(c, crc);
    out.write(reinterpret_cast<const char*>(c.data()), static_cast<std::streamsize>(c.size()));
}
}  // namespace

bool write_png(const std::string& path, int width, int height, const unsigned char* rgba) {
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    const unsigned char sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    out.write(reinterpret_cast<const char*>(sig), 8);
    std::vector<unsigned char> ihdr;
    put32(ihdr, static_cast<std::uint32_t>(width));
    put32(ihdr, static_cast<std::uint32_t>(height));
    ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});
    chunk(out, "IHDR", ihdr);
    std::vector<unsigned char> raw;
    const std::size_t row = static_cast<std::size_t>(width) * 4;
    for (int y = 0; y < height; ++y) {
        raw.push_back(0);
        raw.insert(raw.end(), rgba + static_cast<std::size_t>(y) * row, rgba + static_cast<std::size_t>(y + 1) * row);
    }
    std::vector<unsigned char> z = {0x78, 0x01};
    std::uint32_t a = 1, b = 0;
    for (unsigned char x : raw) {
        a = (a + x) % 65521U;
        b = (b + a) % 65521U;
    }
    for (std::size_t off = 0; off < raw.size(); off += 65535) {
        const std::size_t n = std::min<std::size_t>(65535, raw.size() - off);
        z.push_back(off + n == raw.size() ? 1 : 0);
        z.push_back(static_cast<unsigned char>(n & 0xFF));
        z.push_back(static_cast<unsigned char>(n >> 8));
        z.push_back(static_cast<unsigned char>(~n & 0xFF));
        z.push_back(static_cast<unsigned char>((~n >> 8) & 0xFF));
        z.insert(z.end(), raw.begin() + static_cast<std::ptrdiff_t>(off), raw.begin() + static_cast<std::ptrdiff_t>(off + n));
    }
    put32(z, (b << 16U) | a);
    chunk(out, "IDAT", z);
    chunk(out, "IEND", {});
    return static_cast<bool>(out);
}

}  // namespace ui
