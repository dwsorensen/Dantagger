// Dantagger: the measurement app for the TDC board.  Every measurement type
// of the Dantagger library (Countrate, Counter, Histogram, Correlation,
// HistogramND, TimeTagStream) as a panel; any number run at once on the live
// stream or on saved tag streams.  Panels are dockable windows: tabs in one
// window, split side by side, or separate OS windows (Dear ImGui docking +
// multi-viewport).  The layout of the TDC Control app (sidebar, cards, rate
// bar, log) with its setup pages.  Dear ImGui + ImPlot on GLFW/OpenGL,
// everything compiled in.
//
//   dantagger [--simulate] [--serial S] [--data DIR] [--selftest DIR] [--snapshot DIR] [--reset-layout]
//
// --simulate uses the simulated FPGA (synthetic laser + detectors);
// --selftest DIR drives the whole app against it (setup, every panel, live
// trigger change, recording and replay, layouts, scan), saves screenshots in
// DIR and exits 0 on PASS; --snapshot DIR connects to the real board (STOP +
// STATUS only), saves screenshots of the setup pages and exits.

#include "config.hpp"
#include "pages.hpp"
#include "panels.hpp"
#include "ui_common.hpp"

#include "tdcctl/fake_fpga.hpp"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <imgui_internal.h>
#include <implot.h>
#include <portable-file-dialogs.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

extern const int k_icon_count;
extern const int k_icon_sizes[];
extern const unsigned char* const k_icon_pixels[];

namespace {

namespace fs = std::filesystem;

// The app icon (icon.png, embedded by build.py) on a GLFW window.
void set_icon(GLFWwindow* window) {
    std::vector<GLFWimage> images;
    for (int i = 0; i < k_icon_count; ++i)
        images.push_back({k_icon_sizes[i], k_icon_sizes[i], const_cast<unsigned char*>(k_icon_pixels[i])});
    glfwSetWindowIcon(window, static_cast<int>(images.size()), images.data());
}
using app::Kind;
using app::Level;

// ---- OpenGL 1.x entry points through GLFW (the binary does not link libGL) ----
using PfnViewport = void (*)(int, int, int, int);
using PfnClearColor = void (*)(float, float, float, float);
using PfnClear = void (*)(unsigned);
using PfnReadPixels = void (*)(int, int, int, int, unsigned, unsigned, void*);
using PfnPixelStorei = void (*)(unsigned, int);
PfnViewport gl_viewport;
PfnClearColor gl_clear_color;
PfnClear gl_clear;
PfnReadPixels gl_read_pixels;
PfnPixelStorei gl_pixel_storei;
constexpr unsigned kGlColorBufferBit = 0x4000, kGlRgba = 0x1908, kGlUnsignedByte = 0x1401, kGlPackAlignment = 0x0D05;

template <typename T>
T gl_proc(const char* name) {
    return reinterpret_cast<T>(glfwGetProcAddress(name));
}

struct Args {
    bool simulate{false};
    std::string serial;
    std::string data;
    std::string selftest;
    std::string snapshot;
    bool reset_layout{false};
};

// ---- log ------------------------------------------------------------------------------
struct LogView {
    std::deque<app::LogLine> lines;
    bool scroll{false};
    void add(std::vector<app::LogLine> more) {
        for (auto& l : more) {
            lines.push_back(std::move(l));
            if (lines.size() > 3000) lines.pop_front();
            scroll = true;
        }
    }
    void draw() {
        ImGui::PushFont(ui::fonts.small);
        for (const auto& l : lines) {
            ImGui::TextDisabled("%s", l.time.c_str());
            ImGui::SameLine();
            const ImVec4 c = l.level == Level::error ? ui::c_bad()
                             : l.level == Level::warn ? ui::c_warn()
                             : l.level == Level::ok   ? ImVec4(0.75f, 0.90f, 0.80f, 1.0f)
                                                      : ImVec4(0.80f, 0.83f, 0.88f, 1.0f);
            ImGui::PushTextWrapPos(0);
            ImGui::TextColored(c, "%s", l.text.c_str());
            ImGui::PopTextWrapPos();
        }
        if (scroll) {
            ImGui::SetScrollHereY(1.0f);
            scroll = false;
        }
        ImGui::PopFont();
    }
};

// ---- the app's windows ---------------------------------------------------------------
enum class Layout { none, tabs, grid, windows };

struct PageWin {
    const char* name;      // window name (stable id after ###)
    const char* label;
    bool open;
};

struct App {
    app::Device& dev;
    std::string data_dir;
    std::vector<std::unique_ptr<app::Panel>> panels;
    std::vector<bool> own_window;          // per panel: floats in its own OS window
    std::vector<int> undock_frames;        // per panel: frames left to force the undocked position
    int next_id{1};
    app::PagesState pages;
    PageWin cmp{"Comparator###page_cmp", "Comparator", true};
    PageWin tdc{"TDC-GPX2 & LVDS###page_tdc", "TDC-GPX2 & LVDS", true};
    PageWin scan{"Countrate vs trigger###page_scan", "Countrate vs trigger", false};
    bool show_log{true};
    Layout pending{Layout::none};
    std::string focus;                     // window to focus next frame
    ImGuiID dock_id{0};

    explicit App(app::Device& d) : dev(d) {}

    app::Panel& add_panel(Kind kind, int id = 0) {
        if (id <= 0) id = next_id;
        next_id = std::max(next_id, id + 1);
        panels.push_back(app::make_panel(kind, id));
        own_window.push_back(false);
        undock_frames.push_back(0);
        focus = panels.back()->window_name();
        return *panels.back();
    }
    std::vector<app::Panel*> live_panels() {
        std::vector<app::Panel*> out;
        for (auto& p : panels)
            if (!p->use_file) out.push_back(p.get());
        return out;
    }
};

std::string session_path() { return app::config_dir() + "/session.ini"; }

void save_session(App& a) {
    app::KeyValues kv;
    kv.emplace_back("data_dir", a.data_dir);
    kv.emplace_back("page.comparator", a.cmp.open ? "1" : "0");
    kv.emplace_back("page.tdc", a.tdc.open ? "1" : "0");
    kv.emplace_back("page.scan", a.scan.open ? "1" : "0");
    kv.emplace_back("log", a.show_log ? "1" : "0");
    kv.emplace_back("next_id", std::to_string(a.next_id));
    kv.emplace_back("panels", std::to_string(a.panels.size()));
    for (std::size_t i = 0; i < a.panels.size(); ++i) {
        const std::string p = "panel." + std::to_string(i) + ".";
        kv.emplace_back(p + "id", std::to_string(a.panels[i]->id()));
        kv.emplace_back(p + "own_window", a.own_window[i] ? "1" : "0");
        a.panels[i]->save(kv, p);
    }
    app::write_kv(session_path(), kv);
}

void load_session(App& a) {
    const auto kv = app::read_kv(session_path());
    if (kv.empty()) return;
    if (const auto d = app::kv_get(kv, "data_dir"); !d.empty() && a.data_dir.empty()) a.data_dir = d;
    a.cmp.open = app::kv_get(kv, "page.comparator", "1") == "1";
    a.tdc.open = app::kv_get(kv, "page.tdc", "1") == "1";
    a.scan.open = app::kv_get(kv, "page.scan", "0") == "1";
    a.show_log = app::kv_get(kv, "log", "1") == "1";
    const int n = static_cast<int>(app::kv_num(kv, "panels", 0));
    for (int i = 0; i < n; ++i) {
        const std::string p = "panel." + std::to_string(i) + ".";
        Kind k;
        if (!app::kind_from_key(app::kv_get(kv, p + "kind"), k)) continue;
        auto& panel = a.add_panel(k, static_cast<int>(app::kv_num(kv, p + "id", 0)));
        panel.load(kv, p);
        a.own_window.back() = app::kv_get(kv, p + "own_window") == "1";
    }
    a.next_id = std::max(a.next_id, static_cast<int>(app::kv_num(kv, "next_id", a.next_id)));
    a.focus.clear();
}

// ---- layouts --------------------------------------------------------------------------
void apply_layout(App& a, Layout layout, ImVec2 size) {
    const ImGuiID root = a.dock_id;
    ImGui::DockBuilderRemoveNode(root);
    ImGui::DockBuilderAddNode(root, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(root, size);
    std::vector<const char*> pages;
    for (PageWin* p : {&a.cmp, &a.tdc, &a.scan})
        if (p->open) pages.push_back(p->name);
    std::vector<std::string> names;
    for (auto& p : a.panels) names.push_back(p->window_name());
    if (layout == Layout::grid && !names.empty()) {
        const int n = static_cast<int>(names.size());
        const int cols = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(n))));
        const int rows = (n + cols - 1) / cols;
        std::vector<ImGuiID> cells;
        ImGuiID rest = root;
        for (int r = 0; r < rows; ++r) {
            ImGuiID row = rest;
            if (r < rows - 1) ImGui::DockBuilderSplitNode(rest, ImGuiDir_Up, 1.0f / static_cast<float>(rows - r), &row, &rest);
            const int in_row = std::min(cols, n - r * cols);
            ImGuiID row_rest = row;
            for (int c = 0; c < in_row; ++c) {
                ImGuiID cell = row_rest;
                if (c < in_row - 1) ImGui::DockBuilderSplitNode(row_rest, ImGuiDir_Left, 1.0f / static_cast<float>(in_row - c), &cell, &row_rest);
                cells.push_back(cell);
            }
        }
        for (const char* p : pages) ImGui::DockBuilderDockWindow(p, cells.front());
        for (std::size_t i = 0; i < names.size(); ++i) ImGui::DockBuilderDockWindow(names[i].c_str(), cells[i]);
        for (std::size_t i = 0; i < a.panels.size(); ++i) a.own_window[i] = false;
    } else {
        for (const char* p : pages) ImGui::DockBuilderDockWindow(p, root);
        for (std::size_t i = 0; i < names.size(); ++i) {
            if (layout == Layout::windows) {
                a.own_window[i] = true;
                a.undock_frames[i] = 2;
            } else {
                ImGui::DockBuilderDockWindow(names[i].c_str(), root);
                a.own_window[i] = false;
            }
        }
    }
    ImGui::DockBuilderFinish(root);
    if (!a.panels.empty() && layout != Layout::windows) a.focus = a.panels.front()->window_name();
}

// ---- sidebar ----------------------------------------------------------------------------
void new_panel_menu(App& a) {
    for (int k = 0; k < app::kKinds; ++k) {
        const auto kind = static_cast<Kind>(k);
        if (ImGui::MenuItem(app::kind_title(kind))) a.add_panel(kind);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", app::kind_summary(kind));
    }
}

void draw_sidebar(App& a, app::AppContext& ctx, float width) {
    const auto& v = ctx.v;
    ImGui::BeginChild("##side", ImVec2(width, 0), ImGuiChildFlags_None);
    ImGui::PushFont(ui::fonts.title);
    ImGui::TextUnformatted("Dantagger");
    ImGui::PopFont();
    ui::dim_text("TDC board · Au V2 · Ft+ · TDC-GPX2");
    if (a.dev.simulated()) ImGui::TextColored(ui::c_warn(), "SIMULATED FPGA");
    ImGui::Dummy(ImVec2(0, 4));

    // connection
    if (ui::card_begin("##conn", nullptr)) {
        const ImVec4 col = !v.connected ? ui::c_bad() : (v.busy ? ui::c_warn() : ui::c_ok());
        ui::status_dot(ImGui::GetColorU32(col), 5);
        ImGui::SameLine(0, 6);
        ImGui::TextUnformatted(v.connecting ? "Connecting…" : !v.connected ? "Not connected" : (v.busy ? "Busy" : "Connected"));
        ImGui::PushTextWrapPos(0);
        ImGui::PushFont(ui::fonts.small);
        ui::dim_text("%s", v.link_text.c_str());
        if (v.connected) {
            ImGui::TextColored(ui::c_accent(), "%s", v.release.c_str());
            ui::dim_text("%s", v.capture.c_str());
        }
        if (v.busy && !v.busy_label.empty()) ImGui::TextColored(ui::c_warn(), "%s…", v.busy_label.c_str());
        else if (v.state) ui::dim_text("board %.1f °C", v.state->tmp.celsius());
        if (!v.error.empty()) ImGui::TextColored(ui::c_bad(), "%s", v.error.c_str());
        ImGui::PopFont();
        ImGui::PopTextWrapPos();
        if (v.connected && !v.set_up) {
            ImGui::TextColored(ui::c_warn(), "Board not set up");
            if (v.busy) ImGui::BeginDisabled();
            if (ui::accent_button("Set up the board", ImVec2(-1, 0))) a.dev.setup(false);
            if (v.busy) ImGui::EndDisabled();
        }
        const float w = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
        if (v.busy) ImGui::BeginDisabled();
        if (ImGui::Button(v.connected ? "Reconnect" : "Connect", ImVec2(w, 0))) a.dev.connect();
        ImGui::SameLine();
        if (!v.connected) ImGui::BeginDisabled();
        if (ImGui::Button("Disconnect", ImVec2(w, 0))) a.dev.disconnect();
        if (!v.connected) ImGui::EndDisabled();
        if (v.busy) ImGui::EndDisabled();
    }
    ui::card_end();

    auto group = [](const char* name) {
        ImGui::Dummy(ImVec2(0, 4));
        ImGui::PushFont(ui::fonts.small);
        ImGui::TextDisabled("%s", name);
        ImGui::PopFont();
    };
    group("SETUP");
    for (PageWin* p : {&a.cmp, &a.tdc, &a.scan}) {
        if (ImGui::Selectable(p->label, false, 0, ImVec2(0, ImGui::GetFrameHeight() * 1.2f))) {
            p->open = true;
            a.focus = p->name;
        }
    }

    group("MEASUREMENTS");
    int remove = -1, duplicate = -1;
    for (std::size_t i = 0; i < a.panels.size(); ++i) {
        auto& p = *a.panels[i];
        ImGui::PushID(p.id());
        ui::status_dot(ImGui::GetColorU32(p.state_colour()), 5);
        ImGui::SameLine(0, 6);
        ImGui::BeginGroup();
        if (ImGui::Selectable(p.title, false, ImGuiSelectableFlags_AllowOverlap,
                              ImVec2(ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight() - 6, 0)))
            a.focus = p.window_name();
        ImGui::PushFont(ui::fonts.small);
        ui::dim_text("%s · %s%s", app::kind_title(p.kind()), p.state_text().c_str(), p.use_file ? " · file" : "");
        ImGui::PopFont();
        ImGui::EndGroup();
        if (ImGui::BeginPopupContextItem("##ctx")) {
            if (ImGui::MenuItem("Show")) a.focus = p.window_name();
            if (ImGui::MenuItem("In its own window", nullptr, a.own_window[i])) {
                a.own_window[i] = !a.own_window[i];
                if (a.own_window[i]) a.undock_frames[i] = 2;
            }
            if (ImGui::MenuItem("Dock into the main window")) {
                a.own_window[i] = false;
                a.undock_frames[i] = -2;   // dock request
            }
            ImGui::Separator();
            ImGui::SetNextItemWidth(200);
            ImGui::InputText("title", p.title, sizeof p.title);
            if (ImGui::MenuItem("Duplicate")) duplicate = static_cast<int>(i);
            if (ImGui::MenuItem("Close")) remove = static_cast<int>(i);
            ImGui::EndPopup();
        }
        // start / stop at the right of the row
        ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::GetFrameHeight());
        if (p.running()) {
            if (ui::icon_button("##stop", ui::Icon::stop, ImGui::GetFrameHeight(), false)) p.stop(ctx);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Stop");
        } else {
            std::string why;
            const bool can = p.startable(ctx, &why);
            if (!can) ImGui::BeginDisabled();
            if (ui::icon_button("##start", ui::Icon::play, ImGui::GetFrameHeight(), true)) p.start(ctx, true);
            if (!can) ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", can ? "Start" : why.c_str());
        }
        ImGui::PopID();
    }
    if (remove >= 0) a.panels[static_cast<std::size_t>(remove)]->open = false;
    if (duplicate >= 0) {   // after the loop: `panels` grows
        const Kind kind = a.panels[static_cast<std::size_t>(duplicate)]->kind();
        app::KeyValues kv;
        a.panels[static_cast<std::size_t>(duplicate)]->save(kv, "");
        auto& q = a.add_panel(kind);
        const int qid = q.id();
        q.load(kv, "");
        std::snprintf(q.title, sizeof q.title, "%s %d", app::kind_title(kind), qid);
    }
    if (a.panels.empty()) ui::dim_text("none yet");
    if (ImGui::Button("+ New measurement", ImVec2(-1, 0))) ImGui::OpenPopup("##newm");
    ui::probe("new_measurement");
    if (ImGui::BeginPopup("##newm")) {
        new_panel_menu(a);
        ImGui::EndPopup();
    }
    ImGui::Dummy(ImVec2(0, 2));
    {
        const float w = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
        if (ImGui::Button("Start all", ImVec2(w, 0)))
            for (auto& p : a.panels)
                if (!p->running() && p->startable(ctx)) p->start(ctx, true);
        ImGui::SameLine();
        if (ImGui::Button("Stop all", ImVec2(w, 0)))
            for (auto& p : a.panels)
                if (p->running()) p->stop(ctx);
    }

    group("LAYOUT");
    {
        const float w = (ImGui::GetContentRegionAvail().x - 2 * ImGui::GetStyle().ItemSpacing.x) / 3.0f;
        if (ImGui::Button("Tabs", ImVec2(w, 0))) a.pending = Layout::tabs;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Every window as a tab of the main window");
        ImGui::SameLine();
        if (ImGui::Button("Grid", ImVec2(w, 0))) a.pending = Layout::grid;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("The measurements side by side in the main window");
        ImGui::SameLine();
        if (ImGui::Button("Windows", ImVec2(w, 0))) a.pending = Layout::windows;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Each measurement in its own window");
    }
    ImGui::PushFont(ui::fonts.small);
    ImGui::PushTextWrapPos(0);
    ui::dim_text("Drag a window's tab to dock it anywhere, split, or pull it out into its own window. Right-click a "
                 "measurement above for more.");
    ImGui::PopTextWrapPos();
    ImGui::PopFont();
    ImGui::EndChild();
}

// ---- rate bar -------------------------------------------------------------------------
void rate_bar(App& a, app::AppContext& ctx) {
    const auto& v = ctx.v;
    const auto r = a.dev.rates().snapshot();
    int running = 0;
    for (auto& p : a.panels) running += p->running() && !p->use_file ? 1 : 0;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ui::c_panel());
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14, 8));
    ImGui::BeginChild("##ratebar", ImVec2(0, 0), ImGuiChildFlags_Border | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleVar();
    ImGui::BeginGroup();
    const bool live = r.active;
    ui::status_dot(ImGui::GetColorU32(live ? ui::c_ok() : ImVec4(0.35f, 0.39f, 0.46f, 1.0f)), 5);
    ImGui::SameLine(0, 6);
    ImGui::TextColored(live ? ImGui::GetStyleColorVec4(ImGuiCol_Text) : ui::c_dim(), "%s",
                       live ? "STREAMING" : v.scanning ? "SCANNING" : v.calibrating ? "CALIBRATING" : "IDLE");
    ImGui::PushFont(ui::fonts.small);
    ui::dim_text("%d live measurement%s running · FPGA time %.1f s", running, running == 1 ? "" : "s", r.fpga_seconds);
    ImGui::PopFont();
    ImGui::EndGroup();
    const float cell = 118;
    for (int c = 0; c < 4; ++c) {
        ImGui::SameLine(c == 0 ? 280.0f : 0.0f, c == 0 ? -1.0f : 8.0f);
        ImGui::BeginGroup();
        ImGui::PushFont(ui::fonts.small);
        ImGui::TextColored(ui::channel_colour(c + 1), "ch %d", c + 1);
        ImGui::PopFont();
        ImGui::PushFont(ui::fonts.mono);
        ImGui::TextColored(r.active ? ImGui::GetStyleColorVec4(ImGuiCol_Text) : ui::c_dim(), "%s",
                           ui::fmt_rate(r.rate_hz[static_cast<std::size_t>(c)]).c_str());
        ImGui::PopFont();
        ImGui::EndGroup();
        ImGui::SameLine(0, 0);
        ImGui::Dummy(ImVec2(std::max(0.0f, cell - ImGui::GetItemRectSize().x), 1));
    }
    ImGui::SameLine(0, 10);
    ImGui::BeginGroup();
    ImGui::PushFont(ui::fonts.small);
    ui::dim_text("dropped by the FPGA");
    ImGui::PopFont();
    ImGui::PushFont(ui::fonts.mono);
    ImGui::TextColored(r.dropped > 0 ? ui::c_bad() : (r.active ? ui::c_ok() : ui::c_dim()), "%s · %s total",
                       ui::fmt_rate(r.dropped_hz).c_str(), ui::fmt_count(r.dropped).c_str());
    ImGui::PopFont();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Events the FPGA dropped because its FIFO was full (OVERFLOW markers; the FPGA's exact count at "
                          "the end of each run).\nRates over those intervals are lower bounds.");
    ImGui::EndGroup();
    ImGui::SameLine();
    const float bw = 110;
    const float x_right = ImGui::GetWindowContentRegionMax().x - bw;
    if (ImGui::GetCursorPosX() + 16 < x_right) ImGui::SetCursorPosX(x_right);
    else ImGui::SameLine(0, 16);
    if (ImGui::Button("Reset totals", ImVec2(bw, 0))) a.dev.rates().reset_totals();
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

// ---- self-test ---------------------------------------------------------------------------
struct SelfTest {
    struct Step {
        std::string name;
        std::function<bool()> poll;   // true: done
        double timeout_s;
    };
    std::string dir;
    std::vector<Step> steps;
    std::size_t at{0};
    int frames{0};
    std::chrono::steady_clock::time_point since{std::chrono::steady_clock::now()};
    std::vector<std::string> failures;
    bool done{false};
    bool want_shot{false};
    std::string shot_name;

    void expect(bool ok, const std::string& what) {
        if (!ok) failures.push_back(steps[at].name + ": " + what);
    }
    double elapsed() const { return std::chrono::duration<double>(std::chrono::steady_clock::now() - since).count(); }
    void shot(const std::string& name) {
        want_shot = true;
        shot_name = name;
    }
    void tick() {
        if (done || at >= steps.size()) {
            done = true;
            return;
        }
        ++frames;
        if (steps[at].poll()) {
            std::printf("  step %2zu %-58s %.1f s\n", at, steps[at].name.c_str(), elapsed());
            std::fflush(stdout);
            ++at;
            frames = 0;
            since = std::chrono::steady_clock::now();
        } else if (elapsed() > steps[at].timeout_s) {
            failures.push_back(steps[at].name + ": timed out");
            done = true;
        }
        if (at >= steps.size()) done = true;
    }
};

void screenshot(GLFWwindow* window, const std::string& path) {
    int w = 0, h = 0;
    glfwGetFramebufferSize(window, &w, &h);
    std::vector<unsigned char> px(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4);
    gl_pixel_storei(kGlPackAlignment, 1);
    gl_read_pixels(0, 0, w, h, kGlRgba, kGlUnsignedByte, px.data());
    std::vector<unsigned char> flipped(px.size());
    const std::size_t row = static_cast<std::size_t>(w) * 4;
    for (int y = 0; y < h; ++y)
        std::memcpy(flipped.data() + static_cast<std::size_t>(y) * row, px.data() + static_cast<std::size_t>(h - 1 - y) * row, row);
    for (std::size_t i = 3; i < flipped.size(); i += 4) flipped[i] = 255;
    if (!ui::write_png(path, w, h, flipped.data())) std::fprintf(stderr, "cannot write %s\n", path.c_str());
}

double metric(const app::Panel& p, const std::string& key) {
    const auto m = p.metrics();
    const auto it = m.find(key);
    return it == m.end() ? std::nan("") : it->second;
}

void build_selftest(SelfTest& t, App& a, app::AppContext*& ctx_ref, GLFWwindow* window) {
    auto ctx = [&ctx_ref]() -> app::AppContext& { return *ctx_ref; };
    auto v = [&ctx_ref]() -> const app::Device::View& { return ctx_ref->v; };
    // Idle in this frame's view (busy cleared after a job's results are in the view) and now.
    auto idle = [&a, v] { return v().connected && !v().busy && a.dev.idle(); };
    auto panel = [&a](std::size_t i) -> app::Panel& { return *a.panels[i]; };
    auto step = [&t](std::string name, std::function<bool()> poll, double timeout = 60) {
        t.steps.push_back({std::move(name), std::move(poll), timeout});
    };
    (void)window;
    static std::uint64_t runs_before = 0;
    static double capture_before[6] = {};
    static std::string recording;

    step("connect to the simulated FPGA (nothing applied)", [=, &t] {
        if (!(idle() && v().state && t.frames > 5)) return false;
        t.expect(!v().set_up, "a fresh simulated board is not set up");
        t.expect(v().release.find("Release 2") == 0, "release shown: " + v().release);
        // Opening applied nothing: the comparator still has the power-up codes.
        t.expect(v().state->board.dac_codes[0] == 0, "connect changed no trigger");
        return true;
    });
    step("set up the board (configure + calibrate)", [=, &a, &t] {
        if (t.frames == 1) a.dev.setup(false);
        if (t.frames < 3 || !idle()) return false;
        t.expect(v().set_up, "configured and calibrated");
        t.expect(v().capture.find("24 + 18-bit frames (42 bits), DDR at LCLKIN 250 MHz: at most 11.90 M") == 0,
                 "release-2 frame policy: " + v().capture);
        t.expect(v().policy_frame, "the policy configuration is in use");
        return true;
    });
    step("apply the saved trigger levels", [=, &a, &t] {
        if (t.frames == 1) {
            t.expect(v().comparator_differs, "the board's power-up comparator differs from the saved values");
            a.dev.apply_saved_comparator();
        }
        if (t.frames < 3 || !idle()) return false;
        t.expect(std::fabs(tdcctl::threshold_volts(v().state->board.dac_codes[3]) - 0.05) < 1e-3, "trigger 4 read back 0.05 V");
        t.expect(!v().comparator_differs, "board = saved values");
        a.focus = a.tdc.name;
        return true;
    });
    step("screenshot: TDC page", [=, &t] {
        if (t.frames == 4) t.shot("01_tdc.png");
        return t.frames > 6;
    });
    step("one panel of every type, tabbed, all started", [=, &a, &t] {
        if (t.frames == 1) {
            app::KeyValues kv;
            auto set = [&kv](const std::string& k, const std::string& val) { kv.emplace_back(k, val); };
            a.add_panel(Kind::countrate);
            a.add_panel(Kind::counter);
            set("channels", "15");
            set("binwidth_ps", "10000000000");   // 10 ms
            set("n_values", "500");
            a.panels.back()->load(kv, "");
            kv.clear();
            a.add_panel(Kind::histogram);
            set("a", "1");
            set("b", "4");
            set("binwidth_ps", "10");
            set("n_bins", "5000");
            set("fit", "1");
            a.panels.back()->load(kv, "");
            kv.clear();
            a.add_panel(Kind::correlation);
            set("a", "1");
            set("b", "2");
            set("binwidth_ps", "10");
            set("n_bins", "2000");
            set("fit", "1");
            a.panels.back()->load(kv, "");
            kv.clear();
            a.add_panel(Kind::histogram2d);
            set("start", "4");
            set("stop_x", "1");
            set("stop_y", "2");
            set("bw_x", "100");
            set("bw_y", "100");
            set("n_x", "400");
            set("n_y", "400");
            set("log_z", "1");
            a.panels.back()->load(kv, "");
            a.add_panel(Kind::stream);
            a.panels.back()->run_s = 0.5;
            a.pending = Layout::tabs;
        }
        if (t.frames == 3) {
            runs_before = a.dev.rates().snapshot().runs;
            for (std::size_t i = 0; i < 5; ++i) panel(i).start(ctx(), true);
        }
        if (t.frames < 4) return false;
        for (std::size_t i = 0; i < 5; ++i) t.expect(panel(i).running(), std::string(panel(i).title) + " running (" + panel(i).error() + ")");
        return true;
    });
    step("live data in every panel", [=, &t] {
        if (t.elapsed() < 3.0) return false;
        auto& cr = panel(0);
        const double r4 = metric(cr, "rate4"), r1 = metric(cr, "rate1"), r2 = metric(cr, "rate2"), r3 = metric(cr, "rate3");
        char buf[200];
        std::snprintf(buf, sizeof buf, "countrate 1 MHz laser / 100 k / 80 k / 50 k: got %.0f / %.0f / %.0f / %.0f", r4, r1, r2, r3);
        t.expect(std::fabs(r4 - 1e6) < 2e4 && std::fabs(r1 - 1.003e5) < 6e3 && std::fabs(r2 - 8.03e4) < 5e3 &&
                     std::fabs(r3 - 5e4) < 4e3, buf);
        t.expect(metric(cr, "history") > 5, "countrate history samples");
        t.expect(metric(panel(1), "filled") > 50 && std::fabs(metric(panel(1), "mean4") - 1e4) < 400, "counter: 10 ms bins of the 1 MHz laser");
        std::snprintf(buf, sizeof buf, "histogram peak at 31.55 ns: %.0f ps", metric(panel(2), "peak_ps"));
        t.expect(std::fabs(metric(panel(2), "peak_ps") - 31550) < 40, buf);
        std::snprintf(buf, sizeof buf, "correlation peak at -200 ps: %.0f ps", metric(panel(3), "peak_ps"));
        t.expect(std::fabs(metric(panel(3), "peak_ps") + 200) < 50, buf);
        std::snprintf(buf, sizeof buf, "2D peak at (31.55, 31.75) ns: (%.0f, %.0f) ps, %.0f pairs", metric(panel(4), "peak_x_ps"),
                      metric(panel(4), "peak_y_ps"), metric(panel(4), "total"));
        t.expect(std::fabs(metric(panel(4), "peak_x_ps") - 31550) < 150 && std::fabs(metric(panel(4), "peak_y_ps") - 31750) < 150 &&
                     metric(panel(4), "total") > 1000, buf);
        for (std::size_t i = 0; i < 5; ++i) capture_before[i] = panel(i).capture_s();
        return true;
    }, 20);
    for (std::size_t i = 0; i < 6; ++i) {
        static const char* names[] = {"02_countrate.png", "03_counter.png", "04_histogram.png", "05_correlation.png", "06_hist2d.png", "07_stream_idle.png"};
        step(std::string("screenshot: ") + names[i], [=, &a, &t] {
            if (t.frames == 1) a.focus = panel(i).window_name();
            if (t.frames == 6) t.shot(names[i]);
            if (t.frames <= 8) return false;
            if (i == 2 || i == 3) {   // the live Gaussian fit (computed while the panel is shown)
                const double want = i == 2 ? 31550 : -200, lo = i == 2 ? 40 : 60, hi = i == 2 ? 80 : 125;
                char buf[160];
                std::snprintf(buf, sizeof buf, "fit centre %.1f ps (want %.0f), FWHM %.1f ps (want %.0f ... %.0f)",
                              metric(panel(i), "fit_centre_ps"), want, metric(panel(i), "fit_fwhm_ps"), lo, hi);
                t.expect(std::fabs(metric(panel(i), "fit_centre_ps") - want) < 6 && metric(panel(i), "fit_fwhm_ps") > lo &&
                             metric(panel(i), "fit_fwhm_ps") < hi, buf);
            }
            return true;
        });
    }
    step("trigger change while measuring: the run restarts, data kept", [=, &a, &t] {
        if (t.frames == 1) a.dev.set_trigger(3, 0.3);
        if (t.elapsed() < 2.0) return false;
        t.expect(a.dev.rates().snapshot().runs > runs_before, "a new run after the change");
        t.expect(v().state && std::fabs(tdcctl::threshold_volts(v().state->board.dac_codes[2]) - 0.3) < 1e-3, "trigger 3 applied on the board");
        for (std::size_t i = 0; i < 5; ++i)
            t.expect(panel(i).running() && panel(i).capture_s() > capture_before[i] + 0.3,
                     std::string(panel(i).title) + " kept running and accumulating");
        t.expect(std::fabs(a.dev.channels().trigger_v[2] - 0.3) < 1e-9, "saved value updated");
        return true;
    }, 30);
    step("TimeTagStream: stream to disk for 0.5 s", [=, &a, &t] {
        auto& s = panel(5);
        if (t.frames == 1) {
            a.focus = s.window_name();
            s.start(ctx(), true);
            t.expect(s.running() && s.recorder() != nullptr, "recording (" + s.error() + ")");
        }
        if (t.frames == 25) t.shot("08_stream_recording.png");
        if (t.frames < 26 || s.running()) return false;
        t.expect(s.recordings().size() == 1, "one recording");
        if (!s.recordings().empty()) recording = s.recordings().front();
        std::error_code ec;
        t.expect(!recording.empty() && fs::file_size(recording, ec) > 100000 && fs::exists(recording + ".json"), "file and sidecar written");
        t.expect(metric(s, "counts4") > 1e5, "the stream panel saw the tags");
        return true;
    }, 30);
    step("screenshot: stream panel after recording", [=, &t] {
        if (t.frames == 5) t.shot("09_stream_done.png");
        return t.frames > 7;
    });
    step("the recording in a new Countrate panel (file source): exact counts", [=, &a, &t] {
        if (t.frames == 1) app::g_open_panel_with_file(Kind::countrate, recording);
        if (t.frames == 3) a.panels.back()->start(ctx(), true);
        if (t.frames < 5 || a.panels.back()->running()) return false;
        // Every tag of the file: compare with the sidecar's per-channel event counts.
        const auto info = dt::read_saved_stream(recording);
        std::ifstream side(recording + ".json");
        const std::string text((std::istreambuf_iterator<char>(side)), std::istreambuf_iterator<char>());
        const auto at = text.find("\"events\": [");
        unsigned long long e[4] = {};
        if (at != std::string::npos) std::sscanf(text.c_str() + at + 11, "%llu, %llu, %llu, %llu", &e[0], &e[1], &e[2], &e[3]);
        char buf[200];
        std::snprintf(buf, sizeof buf, "file counts %.0f / %.0f / %.0f / %.0f = recorded %llu / %llu / %llu / %llu",
                      metric(*a.panels.back(), "counts1"), metric(*a.panels.back(), "counts2"), metric(*a.panels.back(), "counts3"),
                      metric(*a.panels.back(), "counts4"), e[0], e[1], e[2], e[3]);
        bool same = e[3] > 0;
        for (int c = 0; c < 4; ++c) same = same && metric(*a.panels.back(), "counts" + std::to_string(c + 1)) == static_cast<double>(e[c]);
        t.expect(same, buf);
        t.expect(info.ref_bits == 24 && info.stop_bits == 18 && info.ddr, "sidecar: release-2 frames");
        t.expect(a.panels.back()->error().empty(), "no error: " + a.panels.back()->error());
        return true;
    }, 30);
    step("the recording in a new Histogram panel: the 31.55 ns peak", [=, &a, &t] {
        if (t.frames == 1) {
            app::g_open_panel_with_file(Kind::histogram, recording);
            app::KeyValues kv{{"a", "1"}, {"b", "4"}, {"binwidth_ps", "10"}, {"n_bins", "5000"}, {"fit", "1"}};
            auto& p = *a.panels.back();
            p.load(kv, "");
            p.use_file = true;
            p.file_path = recording;
        }
        if (t.frames == 3) a.panels.back()->start(ctx(), true);
        if (t.frames < 5 || a.panels.back()->running()) return false;
        if (t.frames < 12) return false;   // the fit runs when the panel is drawn
        const auto& p = *a.panels.back();
        char buf[160];
        std::snprintf(buf, sizeof buf, "peak %.0f ps, fit centre %.1f ps, %.0f pairs", metric(p, "peak_ps"), metric(p, "fit_centre_ps"),
                      metric(p, "total"));
        t.expect(std::fabs(metric(p, "fit_centre_ps") - 31550) < 6 && metric(p, "total") > 1000, buf);
        t.shot("10_histogram_from_file.png");
        return true;
    }, 30);
    step("save raw tags with a timed run (Correlation, 0.3 s)", [=, &t] {
        auto& c = panel(3);
        if (t.frames == 1) {
            c.stop(ctx());
            c.save_raw = true;
            c.run_s = 0.3;
            c.start(ctx(), true);
            t.expect(c.recorder() != nullptr, "recording while it runs");
        }
        if (t.frames < 3 || c.running()) return false;
        t.expect(std::fabs(c.capture_s() - 0.3) < 1e-9, "captured exactly 0.3 s of tag time");
        t.expect(c.recordings().size() == 1, "one raw recording");
        std::error_code ec;
        t.expect(!c.recordings().empty() && fs::file_size(c.recordings().front(), ec) > 10000, "raw file written");
        c.save_raw = false;
        c.run_s = 0;
        return true;
    }, 30);
    step("CSV export of every type", [=, &a, &t] {
        static std::vector<std::string> paths;
        if (t.frames == 1) {
            paths.clear();
            for (std::size_t i = 0; i < 5; ++i) {
                const std::string path = t.dir + "/export_" + std::to_string(i) + ".csv";
                ui::g_force_csv[panel(i).csv_key()] = path;
                paths.push_back(path);
                a.focus = panel(i).window_name();
            }
        }
        // The panels draw only when visible: show each in turn.
        const int k = t.frames / 3;
        if (k < 5) {
            a.focus = panel(static_cast<std::size_t>(k)).window_name();
            return false;
        }
        for (const auto& p : paths) {
            std::ifstream in(p);
            std::string line;
            int lines = 0;
            while (std::getline(in, line)) ++lines;
            t.expect(lines > 5, "CSV " + p + " has data (" + std::to_string(lines) + " lines)");
        }
        return true;
    });
    step("2D projection: the arrow turns the angle by 1 degree", [=, &a, &t] {
        auto& h = panel(4);
        if (t.frames == 1) {
            app::KeyValues kv{{"start", "4"}, {"stop_x", "1"}, {"stop_y", "2"}, {"bw_x", "100"}, {"bw_y", "100"}, {"n_x", "400"},
                              {"n_y", "400"}, {"log_z", "1"}, {"projection", "1"}, {"proj_deg", "-45"}};
            h.load(kv, "");
            a.focus = h.window_name();
        }
        if (t.frames == 8) {
            const auto r = ui::g_probes["proj_up_" + std::to_string(h.id())];
            ImGuiIO& io = ImGui::GetIO();
            io.AddMousePosEvent((r.first.x + r.second.x) / 2, (r.first.y + r.second.y) / 2);
            io.AddMouseButtonEvent(0, true);
            io.AddMouseButtonEvent(0, false);
        }
        if (t.frames == 14) ImGui::GetIO().AddMousePosEvent(-1, -1);
        if (t.frames == 18) t.shot("11_hist2d_projection.png");
        if (t.frames < 20) return false;
        t.expect(std::fabs(metric(h, "proj_deg") + 44) < 1e-6, "angle -44 after one click (got " + std::to_string(metric(h, "proj_deg")) + ")");
        t.expect(metric(h, "proj_total") > 0 && std::fabs(metric(h, "proj_total") - metric(h, "total")) < 1e-6 * metric(h, "total") + 1e-6,
                 "projection keeps the total");
        return true;
    });
    step("grid layout", [=, &a, &t] {
        if (t.frames == 1) a.pending = Layout::grid;
        if (t.frames == 10) t.shot("12_grid.png");
        if (t.frames < 12) return false;
        for (auto& p : a.panels) {
            ImGuiWindow* w = ImGui::FindWindowByName(p->window_name().c_str());
            t.expect(w && w->DockNode && w->Viewport == ImGui::GetMainViewport(), std::string(p->title) + " docked in the main window");
        }
        return true;
    });
    step("separate windows layout (each measurement in its own OS window)", [=, &a, &t] {
        if (t.frames == 1) a.pending = Layout::windows;
        if (t.frames == 10) t.shot("13_windows_main.png");
        if (t.frames < 12) return false;
        for (auto& p : a.panels) {
            ImGuiWindow* w = ImGui::FindWindowByName(p->window_name().c_str());
            t.expect(w && !w->DockNode && w->Viewport != ImGui::GetMainViewport() && w->Viewport->PlatformHandle != nullptr,
                     std::string(p->title) + " in its own platform window");
        }
        return true;
    });
    step("back to tabs", [=, &a, &t] {
        if (t.frames == 1) a.pending = Layout::tabs;
        if (t.frames < 8) return false;
        for (auto& p : a.panels) {
            ImGuiWindow* w = ImGui::FindWindowByName(p->window_name().c_str());
            t.expect(w && w->DockNode && w->Viewport == ImGui::GetMainViewport(), std::string(p->title) + " docked again");
        }
        return true;
    });
    step("stop all; trigger scan on channels 1 and 2", [=, &a, &t] {
        if (t.frames == 1) {
            for (auto& p : a.panels) p->stop(ctx());
            a.scan.open = true;
            a.focus = a.scan.name;
            a.pages.scan.channels = {{true, true, false, false}};
            a.pages.scan.points = 6;
            a.pages.scan.seconds = 0.05;
            a.pages.scan.from_v = 0.0;
            a.pages.scan.to_v = 1.0;
        }
        if (t.frames == 6) t.expect(a.dev.threshold_scan(a.pages.scan), "scan started");
        if (t.frames < 10 || !idle()) return false;
        t.expect(v().scan_message == "complete" && v().scan_points.size() == 12, "12 points: " + v().scan_message);
        bool shape = v().scan_points.size() == 12;
        if (shape) {   // chatter at 0 V, the detector rate in between, nothing above the 0.6 V pulses
            shape = v().scan_points[0].rate_hz > 1e5 && std::fabs(v().scan_points[2].rate_hz - 1.003e5) < 2e4 &&
                    v().scan_points[5].rate_hz < 1e3;
        }
        t.expect(shape, "scan shape (noise, pulses, nothing above the pulse height)");
        t.expect(std::fabs(tdcctl::threshold_volts(v().state->board.dac_codes[0]) - 0.05) < 1e-3, "triggers restored");
        return true;
    }, 60);
    step("screenshot: scan, comparator", [=, &a, &t] {
        if (t.frames == 5) t.shot("14_scan.png");
        if (t.frames == 6) a.focus = a.cmp.name;
        if (t.frames == 12) t.shot("15_comparator.png");
        return t.frames > 14;
    });
    step("session round trip", [=, &a, &t] {
        save_session(a);
        const auto kv = app::read_kv(session_path());
        t.expect(app::kv_num(kv, "panels", 0) == static_cast<double>(a.panels.size()), "every panel in the session file");
        t.expect(app::kv_get(kv, "panel.4.kind") == "histogram2d" && app::kv_get(kv, "panel.4.projection") == "1", "panel settings saved");
        const auto ch = app::read_kv(app::config_dir() + "/channels.ini");
        t.expect(app::kv_get(ch, "trigger_v.3") == "0.30000", "channel settings persisted");
        return true;
    });
}

}  // namespace

int main(int argc, char** argv) {
    Args args;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--simulate") args.simulate = true;
        else if (a == "--serial" && i + 1 < argc) args.serial = argv[++i];
        else if (a == "--data" && i + 1 < argc) args.data = argv[++i];
        else if (a == "--selftest" && i + 1 < argc) args.selftest = argv[++i];
        else if (a == "--snapshot" && i + 1 < argc) args.snapshot = argv[++i];
        else if (a == "--reset-layout") args.reset_layout = true;
        else {
            std::printf("usage: dantagger [--simulate] [--serial S] [--data DIR] [--reset-layout]\n"
                        "       dantagger --selftest DIR     (simulated FPGA; screenshots in DIR; exit 0 = PASS)\n"
                        "       dantagger --snapshot DIR     (the real board, STOP + STATUS only; screenshots)\n");
            return a == "--help" || a == "-h" ? 0 : 2;
        }
    }
    const bool headless = !args.selftest.empty() || !args.snapshot.empty();
    if (!args.selftest.empty()) {
        // Keep the user's settings, layout and calibration state out of the test.
        args.simulate = true;
        fs::create_directories(args.selftest);
        const std::string dir = fs::absolute(args.selftest).string();
        args.selftest = dir;
        setenv("XDG_CONFIG_HOME", (dir + "/config").c_str(), 1);
        setenv("XDG_STATE_HOME", (dir + "/state").c_str(), 1);
        fs::remove_all(dir + "/config");
        fs::remove_all(dir + "/state");
        args.data = dir + "/data";
    }
    ui::set_headless(headless);

    glfwSetErrorCallback([](int code, const char* text) { std::fprintf(stderr, "GLFW error %d: %s\n", code, text); });
    if (!glfwInit()) {
        std::fprintf(stderr, "cannot initialise the display (is an X11 desktop available?)\n");
        return 1;
    }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);
    if (headless) glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    glfwWindowHintString(GLFW_X11_CLASS_NAME, "Dantagger");   // for a desktop entry's StartupWMClass
    glfwWindowHintString(GLFW_X11_INSTANCE_NAME, "dantagger");
    GLFWwindow* window = glfwCreateWindow(1600, 1000, args.simulate ? "Dantagger  [SIMULATED]" : "Dantagger", nullptr, nullptr);
    if (!window) {
        std::fprintf(stderr, "cannot open a window with OpenGL 3.0\n");
        glfwTerminate();
        return 1;
    }
    set_icon(window);
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);
    gl_viewport = gl_proc<PfnViewport>("glViewport");
    gl_clear_color = gl_proc<PfnClearColor>("glClearColor");
    gl_clear = gl_proc<PfnClear>("glClear");
    gl_read_pixels = gl_proc<PfnReadPixels>("glReadPixels");
    gl_pixel_storei = gl_proc<PfnPixelStorei>("glPixelStorei");

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    static std::string ini_path;
    ini_path = app::config_dir() + "/imgui.ini";
    if (args.reset_layout) fs::remove(ini_path);
    io.IniFilename = headless ? nullptr : ini_path.c_str();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_ViewportsEnable;
    io.ConfigWindowsMoveFromTitleBarOnly = true;
    float xscale = 1, yscale = 1;
    glfwGetWindowContentScale(window, &xscale, &yscale);
    const float scale = std::max(1.0f, xscale);
    ui::load_fonts(scale);
    ui::apply_theme(scale);
    ImGui::GetStyle().WindowRounding = 0;   // platform windows
    ImGui::GetStyle().Colors[ImGuiCol_WindowBg].w = 1.0f;
    ImPlotStyle& ps = ImPlot::GetStyle();
    ps.PlotPadding = ImVec2(10, 10);
    ps.LineWeight = 1.5f;
    ps.Colors[ImPlotCol_PlotBg] = ImVec4(0.075f, 0.086f, 0.110f, 1.0f);
    ps.Colors[ImPlotCol_FrameBg] = ImVec4(0, 0, 0, 0);
    ps.Colors[ImPlotCol_PlotBorder] = ImVec4(0.20f, 0.23f, 0.29f, 1.0f);
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 130");
    if (headless) ImGui::GetPlatformIO().Platform_ShowWindow = [](ImGuiViewport*) {};   // test windows stay hidden

    int code = 0;
    {
        app::Device device({args.simulate, args.serial, ""});
        App a(device);
        a.data_dir = args.data;
        if (!headless) load_session(a);
        if (a.data_dir.empty()) a.data_dir = app::default_data_dir();
        std::error_code ec;
        fs::create_directories(a.data_dir, ec);
        app::g_open_panel_with_file = [&a](Kind kind, const std::string& file) {
            auto& p = a.add_panel(kind);
            p.use_file = true;
            p.file_path = file;
        };
        const bool first_layout = !fs::exists(ini_path) || headless;
        if (first_layout) a.pending = Layout::tabs;
        LogView log;
        SelfTest test;
        test.dir = !args.selftest.empty() ? args.selftest : args.snapshot;
        app::AppContext* ctx_ptr = nullptr;
        if (!args.selftest.empty()) build_selftest(test, a, ctx_ptr, window);
        int snap_frames = 0;
        device.connect();

        while (!glfwWindowShouldClose(window)) {
            bool any_running = false;
            for (auto& p : a.panels) any_running |= p->running();
            const auto v = device.view();
            if (v.busy || any_running || headless) glfwPollEvents();
            else glfwWaitEventsTimeout(0.25);
            log.add(device.take_log());
            app::AppContext ctx{device, v, a.data_dir};
            ctx_ptr = &ctx;
            for (auto& p : a.panels) p->update(ctx);

            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();

            // ---- the main window: menu, sidebar, rate bar, dock space, log ----
            const ImGuiViewport* vp = ImGui::GetMainViewport();
            ImGui::SetNextWindowPos(vp->WorkPos);
            ImGui::SetNextWindowSize(vp->WorkSize);
            ImGui::SetNextWindowViewport(vp->ID);
            ImGui::Begin("##root", nullptr,
                         ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
                             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_MenuBar);
            if (ImGui::BeginMenuBar()) {
                if (ImGui::BeginMenu("File")) {
                    ImGui::TextDisabled("Data folder: %s", a.data_dir.c_str());
                    if (ImGui::MenuItem("Choose the data folder…")) {
                        auto dir = pfd::select_folder("Data folder for saved tag streams and exports", a.data_dir).result();
                        if (!dir.empty()) a.data_dir = dir;
                    }
                    if (ImGui::MenuItem("Open a saved tag stream…")) {
                        const auto files = pfd::open_file("Open a saved tag stream", a.data_dir,
                                                          {"Tag streams (*.bin)", "*.bin", "All files", "*"}).result();
                        if (!files.empty()) app::g_open_panel_with_file(Kind::histogram, files.front());
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem("Quit")) glfwSetWindowShouldClose(window, GLFW_TRUE);
                    ImGui::EndMenu();
                }
                if (ImGui::BeginMenu("Measurements")) {
                    if (ImGui::BeginMenu("New")) {
                        new_panel_menu(a);
                        ImGui::EndMenu();
                    }
                    if (ImGui::MenuItem("Start all"))
                        for (auto& p : a.panels)
                            if (!p->running() && p->startable(ctx)) p->start(ctx, true);
                    if (ImGui::MenuItem("Stop all"))
                        for (auto& p : a.panels)
                            if (p->running()) p->stop(ctx);
                    ImGui::EndMenu();
                }
                if (ImGui::BeginMenu("Layout")) {
                    if (ImGui::MenuItem("Tabs")) a.pending = Layout::tabs;
                    if (ImGui::MenuItem("Grid")) a.pending = Layout::grid;
                    if (ImGui::MenuItem("Separate windows")) a.pending = Layout::windows;
                    ImGui::EndMenu();
                }
                if (ImGui::BeginMenu("Board")) {
                    if (ImGui::MenuItem(v.connected ? "Reconnect" : "Connect")) device.connect();
                    if (ImGui::MenuItem("Disconnect", nullptr, false, v.connected)) device.disconnect();
                    ImGui::Separator();
                    if (ImGui::MenuItem("Set up the board", nullptr, false, v.connected && !v.busy)) device.setup(false);
                    if (ImGui::MenuItem("Recalibrate", nullptr, false, v.connected && !v.busy)) device.setup(true);
                    ImGui::EndMenu();
                }
                if (ImGui::BeginMenu("View")) {
                    for (PageWin* p : {&a.cmp, &a.tdc, &a.scan})
                        if (ImGui::MenuItem(p->label, nullptr, p->open)) {
                            p->open = !p->open;
                            if (p->open) a.focus = p->name;
                        }
                    ImGui::MenuItem("Log", nullptr, &a.show_log);
                    ImGui::EndMenu();
                }
                ImGui::EndMenuBar();
            }
            draw_sidebar(a, ctx, 280 * scale);
            ImGui::SameLine(0, 14 * scale);
            ImGui::BeginGroup();
            rate_bar(a, ctx);
            const float log_h = a.show_log ? 140 * scale : 0;
            const ImVec2 dock_size(ImGui::GetContentRegionAvail().x, ImGui::GetContentRegionAvail().y - log_h);
            a.dock_id = ImGui::GetID("MainDock");
            if (a.pending != Layout::none) {
                apply_layout(a, a.pending, dock_size);
                a.pending = Layout::none;
            }
            ImGui::DockSpace(a.dock_id, dock_size, ImGuiDockNodeFlags_None);
            if (a.show_log) {
                ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.078f, 0.090f, 0.114f, 1.0f));
                ImGui::BeginChild("##log", ImVec2(0, log_h - 4), ImGuiChildFlags_Border);
                log.draw();
                ImGui::EndChild();
                ImGui::PopStyleColor();
            }
            ImGui::EndGroup();
            ImGui::End();

            // ---- setup pages ----
            auto page = [&](PageWin& p, const std::function<void()>& body) {
                if (!p.open) return;
                ImGui::SetNextWindowDockID(a.dock_id, ImGuiCond_FirstUseEver);
                if (ImGui::Begin(p.name, &p.open)) body();
                ImGui::End();
            };
            page(a.cmp, [&] { app::page_comparator(ctx, a.pages); });
            page(a.tdc, [&] { app::page_tdc(ctx, a.pages); });
            page(a.scan, [&] { app::page_scan(ctx, a.pages); });

            // ---- measurement panels ----
            for (std::size_t i = 0; i < a.panels.size(); ++i) {
                auto& p = *a.panels[i];
                if (!p.open) continue;
                if (a.undock_frames[i] > 0) {   // into its own window, cascaded over the screen
                    --a.undock_frames[i];
                    ImGui::SetNextWindowDockID(0, ImGuiCond_Always);
                    const float off = 40.0f * static_cast<float>(i % 8);
                    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + 80 + off, vp->Pos.y + 80 + off), ImGuiCond_Always);
                    ImGui::SetNextWindowSize(ImVec2(1150 * scale, 720 * scale), ImGuiCond_Always);
                } else if (a.undock_frames[i] < 0) {   // dock request
                    ++a.undock_frames[i];
                    ImGui::SetNextWindowDockID(a.dock_id, ImGuiCond_Always);
                } else {
                    ImGui::SetNextWindowDockID(a.dock_id, ImGuiCond_FirstUseEver);
                }
                if (a.own_window[i]) {
                    ImGuiWindowClass wc;
                    wc.ViewportFlagsOverrideSet = ImGuiViewportFlags_NoAutoMerge;
                    ImGui::SetNextWindowClass(&wc);
                }
                if (ImGui::Begin(p.window_name().c_str(), &p.open)) p.draw(ctx);
                ImGui::End();
            }
            for (std::size_t i = 0; i < a.panels.size();) {   // closed panels: stop and forget
                if (!a.panels[i]->open) {
                    if (a.panels[i]->running()) a.panels[i]->stop(ctx);
                    a.panels.erase(a.panels.begin() + static_cast<std::ptrdiff_t>(i));
                    a.own_window.erase(a.own_window.begin() + static_cast<std::ptrdiff_t>(i));
                    a.undock_frames.erase(a.undock_frames.begin() + static_cast<std::ptrdiff_t>(i));
                } else {
                    ++i;
                }
            }
            if (!a.focus.empty()) {
                ImGui::SetWindowFocus(a.focus.c_str());
                a.focus.clear();
            }

            if (!args.selftest.empty()) test.tick();
            if (!args.snapshot.empty()) {
                ++snap_frames;
                if (snap_frames == 2) a.focus = a.cmp.name;
                if (snap_frames > 20 && v.state && device.idle()) {
                    static int phase = 0;
                    if (phase == 0) {
                        test.shot("snapshot_comparator.png");
                        phase = 1;
                    } else if (phase == 1) {
                        a.focus = a.tdc.name;
                        phase = 2;
                        snap_frames = 15;
                    } else if (phase == 2) {
                        test.shot("snapshot_tdc.png");
                        phase = 3;
                    } else {
                        test.done = true;
                    }
                }
                if (snap_frames > 600) {
                    std::fprintf(stderr, "snapshot: not connected (see the log)\n");
                    for (const auto& l : log.lines) std::fprintf(stderr, "  %s %s\n", l.time.c_str(), l.text.c_str());
                    test.failures.push_back("not connected");
                    test.done = true;
                }
            }

            ImGui::Render();
            int w = 0, h = 0;
            glfwGetFramebufferSize(window, &w, &h);
            gl_viewport(0, 0, w, h);
            gl_clear_color(0.067f, 0.078f, 0.098f, 1.0f);
            gl_clear(kGlColorBufferBit);
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
            if (test.want_shot) {
                screenshot(window, (fs::path(test.dir) / test.shot_name).string());
                test.want_shot = false;
            }
            if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
                GLFWwindow* backup = glfwGetCurrentContext();
                ImGui::UpdatePlatformWindows();
                ImGui::RenderPlatformWindowsDefault();
                glfwMakeContextCurrent(backup);
                // Measurements in their own windows get the icon too (once per window).
                static std::vector<void*> iconified;
                for (ImGuiViewport* pv : ImGui::GetPlatformIO().Viewports)
                    if (pv->PlatformHandle && pv != ImGui::GetMainViewport() &&
                        std::find(iconified.begin(), iconified.end(), pv->PlatformHandle) == iconified.end()) {
                        set_icon(static_cast<GLFWwindow*>(pv->PlatformHandle));
                        iconified.push_back(pv->PlatformHandle);
                    }
                if (iconified.size() > 256) iconified.clear();
            }
            glfwSwapBuffers(window);
            if (test.done) break;
        }

        if (!headless) save_session(a);
        {
            app::AppContext ctx{device, device.view(), a.data_dir};
            for (auto& p : a.panels)
                if (p->running()) p->stop(ctx);
        }
        a.panels.clear();
        app::g_open_panel_with_file = nullptr;
        if (!args.selftest.empty()) {
            std::printf("Dantagger app selftest %s (%zu steps)\n", test.failures.empty() && test.at >= test.steps.size() ? "PASS" : "FAIL",
                        test.steps.size());
            for (const auto& f : test.failures) std::printf("  FAIL: %s\n", f.c_str());
            if (!test.failures.empty()) {
                std::printf("last log lines:\n");
                const std::size_t from = log.lines.size() > 25 ? log.lines.size() - 25 : 0;
                for (std::size_t i = from; i < log.lines.size(); ++i)
                    std::printf("  %s %s\n", log.lines[i].time.c_str(), log.lines[i].text.c_str());
            }
            code = test.failures.empty() && test.at >= test.steps.size() ? 0 : 1;
        }
        if (!args.snapshot.empty()) code = test.failures.empty() ? 0 : 1;
    }
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return code;
}
