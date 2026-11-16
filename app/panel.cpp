#include "panel.hpp"

#include "engine.hpp"
#include "tagger.hpp"

#include <portable-file-dialogs.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <sstream>

namespace app {

namespace {

struct KindInfo {
    const char* title;
    const char* key;
    const char* summary;
};
const KindInfo kKindInfo[kKinds] = {
    {"Countrate", "countrate", "average count rate per channel and of coincidences over the capture (Swabian Countrate)"},
    {"Counter", "counter", "counts per time bin, a rolling trace per channel (Swabian Counter)"},
    {"Histogram", "histogram", "START -> click time differences, multi start / multi stop (Swabian Histogram)"},
    {"Correlation", "correlation", "time differences between two channels, both signs (Swabian Correlation)"},
    {"2D Histogram", "histogram2d", "one START, first click per axis on two STOP channels (Swabian HistogramND)"},
    {"TimeTagStream", "stream", "stream the tags to disk for a time (Swabian TimeTagStream)"},
};

std::unique_ptr<pfd::open_file>& open_dialog(int id) {
    static std::map<int, std::unique_ptr<pfd::open_file>> dialogs;
    return dialogs[id];
}

}  // namespace

std::function<void(Kind kind, const std::string& file)> g_open_panel_with_file;

const char* kind_title(Kind k) { return kKindInfo[static_cast<int>(k)].title; }
const char* kind_key(Kind k) { return kKindInfo[static_cast<int>(k)].key; }
const char* kind_summary(Kind k) { return kKindInfo[static_cast<int>(k)].summary; }
bool kind_from_key(const std::string& key, Kind& out) {
    for (int i = 0; i < kKinds; ++i)
        if (key == kKindInfo[i].key) {
            out = static_cast<Kind>(i);
            return true;
        }
    return false;
}

Panel::Panel(Kind kind, int id) : kind_(kind), id_(id) {
    std::snprintf(title, sizeof title, "%s %d", kind_title(kind), id);
}

Panel::~Panel() {
    try {
        if (running_ && tagger_ && !tagger_->closed()) tagger_->engine().stop({m_.get()});
        finish_recording(nullptr);
        detach();
    } catch (...) {
    }
}

std::string Panel::window_name() const { return std::string(title) + "###panel" + std::to_string(id_); }

void Panel::log(AppContext& ctx, Level level, const std::string& text) const {
    ctx.dev.log(level, std::string(title) + ": " + text);
}

void Panel::detach() {
    if (m_ && tagger_ && !tagger_->closed()) tagger_->engine().remove(m_.get());
    if (file_backend_ && tagger_) tagger_->close();
    file_backend_ = nullptr;
    tagger_.reset();
}

void Panel::finish_recording(AppContext* ctx) {
    if (!recorder_) return;
    if (rec_backend_) rec_backend_->remove_tap(recorder_.get());
    recorder_->finish();
    const auto p = recorder_->progress();
    recordings_.push_back(p.path);
    if (ctx) {
        char buf[512];
        std::snprintf(buf, sizeof buf, "raw tag stream saved: %s (%s records, %.3f s of FPGA time%s)", p.path.c_str(),
                      ui::fmt_count(p.records).c_str(), p.seconds,
                      p.lost ? (", " + ui::fmt_count(p.lost) + " LOST: the disk was too slow").c_str() : "");
        log(*ctx, p.lost || !p.error.empty() ? Level::warn : Level::ok, buf);
        if (!p.error.empty()) log(*ctx, Level::error, p.error);
    }
    recorder_.reset();
    rec_backend_ = nullptr;
    rec_tagger_.reset();
}

bool Panel::startable(const AppContext& ctx, std::string* why) const {
    auto no = [&](const char* text) {
        if (why) *why = text;
        return false;
    };
    if (use_file) return file_path.empty() ? no("choose a saved tag stream") : true;
    if (!ctx.v.connected) return no("not connected");
    if (!ctx.v.set_up) return no("set up the board first (sidebar: Set up the board)");
    if (ctx.v.scanning || ctx.v.calibrating) return no("the board is busy (trigger scan or calibration)");
    return true;
}

void Panel::replace_measurement(AppContext& ctx) {
    auto live = ctx.dev.tagger();
    if (!live) throw std::runtime_error("not connected");
    auto m = make(*live);   // validates the settings first
    detach();
    tagger_ = live;
    m_ = std::move(m);
    adopt_edited();
    tagger_->engine().add(m_, false);
    generation_ = ctx.v.generation;
    cleared();
}

void Panel::start(AppContext& ctx, bool clear) {
    error_.clear();
    note_.clear();
    std::string why;
    if (!startable(ctx, &why)) {
        error_ = why;
        return;
    }
    try {
        const dt::timestamp_t duration = run_s > 0 ? static_cast<dt::timestamp_t>(std::llround(run_s * 1e12)) : -1;
        if (use_file) {
            auto fb = std::make_unique<dt::FileBackend>(file_path);
            dt::FileBackend* fbp = fb.get();
            auto replay = std::make_shared<dt::Tagger>(std::move(fb), dt::SettingsPolicy::restart);
            const auto& ch = ctx.dev.channels();
            for (int c = 1; c <= dt::kNumChannels; ++c) {   // the shared software settings apply to replays too
                replay->set_delay(c, ch.delay_ps[static_cast<std::size_t>(c - 1)]);
                replay->set_deadtime(c, ch.deadtime_ps[static_cast<std::size_t>(c - 1)]);
            }
            auto m = make(*replay);   // validates the settings (the replay closes again if they are wrong)
            detach();
            file_backend_ = fbp;
            tagger_ = std::move(replay);
            m_ = std::move(m);
            adopt_edited();
            tagger_->engine().add(m_, false);
            cleared();
            tagger_->engine().start({m_.get()}, duration, true);
        } else {
            if (!m_ || file_backend_ || tagger_ != ctx.dev.tagger() || tagger_->closed() || edited_differs())
                replace_measurement(ctx);
            else if (clear)
                cleared();
            if (save_raw || records_always()) {
                std::error_code ec;
                std::filesystem::create_directories(ctx.data_dir, ec);
                dt::StreamRecorder::Options o;
                o.path = (std::filesystem::path(ctx.data_dir) / ui::timestamped(ui::safe_name(title), ".bin")).string();
                o.max_seconds = run_s;
                o.channel_mask = record_mask();
                o.note = std::string(kind_title(kind_)) + " panel '" + title + "'";
                recorder_ = std::make_shared<dt::StreamRecorder>(o);
                rec_tagger_ = tagger_;
                rec_backend_ = ctx.dev.backend();
                if (rec_backend_) rec_backend_->add_tap(recorder_);
            }
            tagger_->engine().start({m_.get()}, duration, clear);
        }
        running_ = true;
        dirty_ = true;
        started_at_ = std::chrono::steady_clock::now();
    } catch (const std::exception& e) {
        error_ = e.what();
        finish_recording(&ctx);
        running_ = false;
    }
}

void Panel::stop(AppContext& ctx) {
    if (m_ && tagger_ && !tagger_->closed()) tagger_->engine().stop({m_.get()});
    finish_recording(&ctx);
    if (running_) note_ = "stopped";
    running_ = false;
    dirty_ = true;
}

void Panel::clear() {
    if (m_ && tagger_ && !tagger_->closed()) tagger_->engine().clear({m_.get()});
    cleared();
    dirty_ = true;
}

void Panel::do_pull() {
    {
        auto g = m_->guard();
        capture_ps_ = m_->capture_duration;
        pull(*m_);
    }
    ++data_version_;
    last_pull_ = std::chrono::steady_clock::now();
    dirty_ = false;
}

void Panel::update(AppContext& ctx) {
    if (!m_ || !tagger_) return;
    if (!file_backend_ && generation_ != ctx.v.generation) {   // the live tagger went away (disconnect / reconnect)
        if (running_) note_ = "the board disconnected";
        running_ = false;
        finish_recording(&ctx);
        generation_ = ctx.v.generation;
    }
    if (tagger_->closed() && !file_backend_) {
        running_ = false;
        return;
    }
    auto& engine = tagger_->engine();
    bool now = engine.any_running({m_.get()});
    if (file_backend_ && now && file_backend_->finished() && engine.drained()) {
        engine.stop({m_.get()});
        now = false;
        note_ = "file read to the end";
        if (!file_backend_->error().empty()) error_ = file_backend_->error();
    }
    if (running_ && !now) {
        if (note_.empty()) note_ = run_s > 0 ? "run time reached" : "stopped";
        if (const auto e = engine.error(); !e.empty()) error_ = e;
        finish_recording(&ctx);
        dirty_ = true;
    }
    running_ = now;
    if (running_ && recorder_ && recorder_->limit_reached() && records_always()) {
        // TimeTagStream: the FPGA-time limit of the recording ends the run.
        engine.stop({m_.get()});
        running_ = false;
        note_ = "recording time reached";
        finish_recording(&ctx);
        dirty_ = true;
    }
    if (dirty_ || (running_ && std::chrono::steady_clock::now() - last_pull_ > std::chrono::milliseconds(100))) do_pull();
}

std::string Panel::state_text() const {
    char buf[96];
    if (running_) {
        if (file_backend_) std::snprintf(buf, sizeof buf, "reading file %.0f %%", file_backend_->progress() * 100);
        else std::snprintf(buf, sizeof buf, "running · %.1f s", capture_s());
        return buf;
    }
    if (!error_.empty()) return "error";
    if (m_) {
        std::snprintf(buf, sizeof buf, "stopped · %.1f s", capture_s());
        return buf;
    }
    return "not started";
}

ImVec4 Panel::state_colour() const {
    if (running_) return ui::c_ok();
    if (!error_.empty()) return ui::c_bad();
    return ImVec4(0.35f, 0.39f, 0.46f, 1.0f);
}

std::string Panel::csv_preamble(const AppContext& ctx) const {
    std::ostringstream o;
    o << "# Dantagger app, " << kind_title(kind_) << " '" << title << "', exported " << ui::timestamped("", "").substr(1) << "\n";
    if (file_backend_) o << "# source: saved tag stream " << file_backend_->stream_info().path << "\n";
    else o << "# source: live stream" << (ctx.v.serial.empty() ? "" : ", board " + ctx.v.serial) << ", " << ctx.v.release << "\n";
    char buf[96];
    std::snprintf(buf, sizeof buf, "# capture duration %.6f s\n", capture_s());
    o << buf;
    const auto& ch = ctx.dev.channels();
    o << "# channel settings (shared):";
    for (int c = 0; c < dt::kNumChannels; ++c) {
        const auto i = static_cast<std::size_t>(c);
        std::snprintf(buf, sizeof buf, " ch%d trigger %.4f V hysteresis %.0f ohm delay %lld ps deadtime %lld ps;", c + 1,
                      ch.trigger_v[i], ch.hysteresis_ohm[i], static_cast<long long>(ch.delay_ps[i]),
                      static_cast<long long>(ch.deadtime_ps[i]));
        o << buf;
    }
    o << "\n";
    return o.str();
}

std::string Panel::default_csv(const AppContext& ctx, const std::string& what) const {
    return (std::filesystem::path(ctx.data_dir) / ui::timestamped(ui::safe_name(title) + (what.empty() ? "" : "_" + what), ".csv")).string();
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------
void Panel::header_ui(AppContext& ctx) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ui::c_panel());
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 8));
    ImGui::BeginChild("##hdr", ImVec2(0, 0),
                      ImGuiChildFlags_Border | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleVar();

    // ---- line 1: state and the run buttons ----
    ImGui::BeginGroup();
    ui::status_dot(ImGui::GetColorU32(state_colour()), 5);
    ImGui::SameLine(0, 6);
    ImGui::TextUnformatted(running_ ? (file_backend_ ? "READING FILE" : (recorder_ ? "RUNNING · REC" : "RUNNING"))
                                    : (m_ ? "STOPPED" : "READY"));
    ImGui::PushFont(ui::fonts.small);
    if (running_ && file_backend_)
        ui::dim_text("%.0f %% of %s records · %.3f s captured", file_backend_->progress() * 100,
                     ui::fmt_count(file_backend_->stream_info().records).c_str(), capture_s());
    else if (m_)
        ui::dim_text("%.3f s captured%s%s", capture_s(), note_.empty() ? "" : " · ", note_.c_str());
    else
        ui::dim_text("%s", kind_summary(kind_));
    ImGui::PopFont();
    ImGui::EndGroup();
    ImGui::SameLine(0, 24);
    ImGui::BeginGroup();
    std::string why;
    const bool can = startable(ctx, &why);
    if (!running_) {
        if (!can) ImGui::BeginDisabled();
        if (ui::accent_button(records_always() && !use_file ? "Start streaming" : "Start")) start(ctx, true);
        ui::probe(std::string("start_") + std::to_string(id_));
        if (m_ && !use_file && !records_always()) {
            ImGui::SameLine();
            if (ImGui::Button("Continue")) start(ctx, false);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Starts again without clearing: the data keeps adding up.");
        }
        if (!can) ImGui::EndDisabled();
    } else {
        if (ui::danger_button("Stop")) stop(ctx);
    }
    if (m_ && !records_always()) {
        ImGui::SameLine();
        if (ImGui::Button("Clear")) clear();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Discards the data (also while running: it starts adding up again).");
    }
    ImGui::EndGroup();
    if (!running_ && !can && !why.empty()) {
        ui::toolbar_next(ImGui::CalcTextSize(why.c_str()).x);
        ImGui::AlignTextToFramePadding();
        ImGui::PushFont(ui::fonts.small);
        ui::dim_text("%s", why.c_str());
        ImGui::PopFont();
    }

    // ---- line 2: source, saving, run time (wraps in narrow windows; a popup in short ones) ----
    const bool short_window = ImGui::GetWindowHeight() < 300.0f || compact_header_;
    if (short_window) {
        ImGui::SameLine();
        if (ImGui::Button("Source…")) ImGui::OpenPopup("##srcpop");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s%s · %s", use_file ? "saved tag file " : "live stream",
                              use_file ? std::filesystem::path(file_path).filename().string().c_str() : "",
                              run_s > 0 ? ("run for " + std::to_string(run_s).substr(0, 6) + " s").c_str() : "until Stop");
        if (!ImGui::BeginPopup("##srcpop")) {
            if (recorder_) {
                ImGui::SameLine();
                ImGui::AlignTextToFramePadding();
                ImGui::TextColored(ui::c_warn(), "REC");
            }
            if (!error_.empty()) ui::warn_text("%s", error_.c_str());
            ImGui::EndChild();
            ImGui::PopStyleColor();
            return;
        }
    }
    const bool lock = running_;
    if (lock) ImGui::BeginDisabled();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Source");
    ImGui::SameLine();
    int src = use_file ? 1 : 0;
    const char* sources[] = {"live stream", "saved tag file"};
    ImGui::SetNextItemWidth(140);
    if (ImGui::Combo("##src", &src, sources, 2)) use_file = src == 1;
    if (use_file) {
        ImGui::SameLine();
        auto& dlg = open_dialog(id_);
        if (ImGui::Button("Browse…") && !dlg)
            dlg = std::make_unique<pfd::open_file>("Open a saved tag stream", ctx.data_dir,
                                                   std::vector<std::string>{"Tag streams (*.bin)", "*.bin", "All files", "*"});
        if (dlg && dlg->ready(0)) {
            const auto files = dlg->result();
            if (!files.empty()) file_path = files.front();
            dlg.reset();
        }
        const std::string name = file_path.empty() ? "no file chosen" : std::filesystem::path(file_path).filename().string();
        ui::toolbar_next(ImGui::CalcTextSize(name.c_str()).x);
        ImGui::AlignTextToFramePadding();
        ImGui::PushFont(ui::fonts.small);
        ui::dim_text("%s", name.c_str());
        ImGui::PopFont();
        if (!file_path.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", file_path.c_str());
    } else if (!records_always()) {
        ui::toolbar_next(ImGui::CalcTextSize("save raw tags (?)").x + ImGui::GetFrameHeight());
        ImGui::Checkbox("save raw tags", &save_raw);
        ui::help("While this measurement runs, the unprocessed tag stream of the board (every FPGA record, all channels) "
                 "is written to the data folder as FILE.bin + FILE.bin.json. Any panel can read it back (Source: saved tag "
                 "file). Off by default; about 8 bytes per tag.");
    }
    const char* run_label = records_always() ? "Stream for" : "Run for";
    ui::toolbar_next(ImGui::CalcTextSize(run_label).x + 80 + ImGui::CalcTextSize("s (until Stop) (?)").x + 20);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(run_label);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80);
    ImGui::InputDouble("s##run", &run_s, 0, 0, "%.6g");
    run_s = std::clamp(run_s, 0.0, 1e6);
    if (run_s <= 0) {
        ImGui::SameLine(0, 4);
        ui::dim_text("(until Stop)");
    }
    ui::help(records_always() ? "Seconds of FPGA time to stream to disk; 0 = until you press Stop."
                              : "Seconds of tag time to capture (Swabian startFor); 0 = until you press Stop.");
    if (lock) ImGui::EndDisabled();
    if (recorder_) {
        const auto p = recorder_->progress();
        ImGui::PushFont(ui::fonts.small);
        const std::string text = std::filesystem::path(p.path).filename().string() + " · " +
                                 ui::fmt_bytes(static_cast<double>(p.bytes_written)) + " · " +
                                 std::to_string(p.seconds).substr(0, 5) + " s" +
                                 (p.lost ? " · " + ui::fmt_count(p.lost) + " LOST" : "");
        ui::toolbar_next(ImGui::CalcTextSize(text.c_str()).x + 60);
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(ui::c_warn(), "REC");
        ImGui::SameLine();
        ui::dim_text("%s", text.c_str());
        ImGui::PopFont();
    }
    if (short_window) ImGui::EndPopup();
    if (!error_.empty()) ui::warn_text("%s", error_.c_str());
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void Panel::draw(AppContext& ctx) {
    ImGui::PushID(id_);
    compact_header_ = ImGui::GetContentRegionAvail().y < 420.0f;
    header_ui(ctx);
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const bool narrow = avail.x < 760.0f;
    auto settings_column = [&] {
        if (ui::card_begin("##set", "Settings")) {
            settings_ui(ctx);
            if (m_ && edited_differs()) {
                ImGui::Spacing();
                if (running_) {
                    if (ui::accent_button("Apply (restarts, clears the data)", ImVec2(-1, 0))) {
                        try {
                            replace_measurement(ctx);
                            const dt::timestamp_t duration = run_s > 0 ? static_cast<dt::timestamp_t>(std::llround(run_s * 1e12)) : -1;
                            tagger_->engine().start({m_.get()}, duration, true);
                            dirty_ = true;
                        } catch (const std::exception& e) {
                            error_ = e.what();
                        }
                    }
                } else {
                    ImGui::PushFont(ui::fonts.small);
                    ui::dim_text("Changed: the next Start uses these settings (and clears the data).");
                    ImGui::PopFont();
                }
            }
        }
        ui::card_end();
        ImGui::SetNextItemOpen(channels_open_by_default(), ImGuiCond_Once);
        if (ImGui::CollapsingHeader("Channels · shared by all measurements")) channel_table(ctx, channel_mask());
    };
    if (narrow) {
        ImGui::BeginChild("##body", ImVec2(0, 0));
        ImGui::SetNextItemOpen(!m_, ImGuiCond_Once);   // a panel with data shows its plots first
        if (ImGui::CollapsingHeader("Settings & channels")) settings_column();
        const float h = std::max(ImGui::GetContentRegionAvail().y, 380.0f);
        ImGui::BeginChild("##view", ImVec2(0, h));
        view_ui(ctx);
        ImGui::EndChild();
        ImGui::EndChild();
    } else {
        const float left_w = std::min(settings_width(), avail.x * 0.38f);
        ImGui::BeginChild("##left", ImVec2(left_w, avail.y));
        settings_column();
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("##view", ImVec2(0, avail.y));
        view_ui(ctx);
        ImGui::EndChild();
    }
    ImGui::PopID();
}

// ---------------------------------------------------------------------------
// Session file
// ---------------------------------------------------------------------------
void Panel::save(KeyValues& kv, const std::string& p) const {
    kv.emplace_back(p + "kind", kind_key(kind_));
    kv.emplace_back(p + "title", title);
    kv.emplace_back(p + "use_file", use_file ? "1" : "0");
    kv.emplace_back(p + "file", file_path);
    kv.emplace_back(p + "run_s", std::to_string(run_s));
    kv.emplace_back(p + "save_raw", save_raw ? "1" : "0");
    save_settings(kv, p);
}

void Panel::load(const KeyValues& kv, const std::string& p) {
    const auto t = kv_get(kv, p + "title");
    if (!t.empty()) std::snprintf(title, sizeof title, "%s", t.c_str());
    use_file = kv_get(kv, p + "use_file") == "1";
    file_path = kv_get(kv, p + "file");
    run_s = kv_num(kv, p + "run_s", 0);
    save_raw = kv_get(kv, p + "save_raw") == "1";
    load_settings(kv, p);
}

// ---------------------------------------------------------------------------
// Shared widgets
// ---------------------------------------------------------------------------
bool channel_combo(const char* id, int* channel, float width) {
    static const char* names[] = {"1", "2", "3", "4"};
    int i = std::clamp(*channel, 1, 4) - 1;
    ImGui::SetNextItemWidth(width);
    const bool changed = ImGui::Combo(id, &i, names, 4);
    *channel = i + 1;
    return changed;
}

bool channel_checks(const char* id, unsigned* mask, int min_on) {
    ImGui::PushID(id);
    bool changed = false;
    for (int c = 0; c < 4; ++c) {
        if (c) ImGui::SameLine();
        bool on = ((*mask >> c) & 1U) != 0;
        char label[8];
        std::snprintf(label, sizeof label, "%d", c + 1);
        ImGui::PushStyleColor(ImGuiCol_CheckMark, ui::channel_colour(c + 1));
        if (ImGui::Checkbox(label, &on)) {
            const unsigned next = on ? (*mask | (1U << c)) : (*mask & ~(1U << c));
            if (__builtin_popcount(next) >= min_on) {
                *mask = next;
                changed = true;
            }
        }
        ImGui::PopStyleColor();
    }
    ImGui::PopID();
    return changed;
}

void channel_table(AppContext& ctx, unsigned highlight, bool comparator_only) {
    const auto& v = ctx.v;
    const auto& ch = ctx.dev.channels();
    const tdcctl::Response* r = v.state ? &*v.state : nullptr;
    if (v.comparator_differs && r) {
        ui::warn_text("The board's trigger levels / hysteresis differ from the saved values (rows marked !). Nothing was "
                      "changed on connect.");
        if (ImGui::SmallButton("Apply the saved values to the board")) ctx.dev.apply_saved_comparator();
        ImGui::SameLine();
        if (ImGui::SmallButton("Keep the board's values")) ctx.dev.adopt_board_comparator();
    }
    const bool compact = ImGui::GetContentRegionAvail().x < 640.0f;
    if (compact) {   // narrow columns: one block of labelled lines per channel
        const float lw = 92.0f, w = std::max(90.0f, std::min(150.0f, ImGui::GetContentRegionAvail().x - lw - 150.0f));
        for (int c = 1; c <= dt::kNumChannels; ++c) {
            const auto i = static_cast<std::size_t>(c - 1);
            ImGui::PushID(c);
            const bool used = ((highlight >> (c - 1)) & 1U) != 0;
            if (c > 1) ImGui::Separator();
            ImGui::TextColored(used ? ui::channel_colour(c) : ui::c_dim(), "Channel %d%s", c, used ? "" : "  (not used here)");
            ui::row_label("Trigger", lw);
            double trig = ch.trigger_v[i];
            if (ui::input_commit("##trig", &trig, 0, 0, "%.4f", w)) ctx.dev.set_trigger(c, trig);
            ImGui::SameLine();
            ImGui::PushFont(ui::fonts.small);
            ImGui::AlignTextToFramePadding();
            if (r) {
                const double now = tdcctl::threshold_volts(r->board.dac_codes[i]);
                const bool differs = std::fabs(now - ch.trigger_v[i]) > 0.0005;
                ImGui::TextColored(differs ? ui::c_warn() : ui::c_dim(), "V · %sboard %+.4f V", differs ? "! " : "", now);
            } else {
                ui::dim_text("V");
            }
            ImGui::PopFont();
            ui::row_label("Hysteresis", lw);
            double hys = ch.hysteresis_ohm[i];
            if (ui::input_commit("##hys", &hys, 0, 0, "%.0f", w)) ctx.dev.set_hysteresis(c, hys);
            ImGui::SameLine();
            ImGui::PushFont(ui::fonts.small);
            ImGui::AlignTextToFramePadding();
            if (r) {
                const double now = tdcctl::wiper_ohms(r->board.wiper(c - 1));
                const bool differs = std::fabs(now - ch.hysteresis_ohm[i]) > 25.0;
                ImGui::TextColored(differs ? ui::c_warn() : ui::c_dim(), "Ω · %sboard %.0f Ω ≈%.0f mV", differs ? "! " : "", now,
                                   ui::hysteresis_mv(now));
            } else {
                ui::dim_text("Ω ≈%.0f mV", ui::hysteresis_mv(ch.hysteresis_ohm[i]));
            }
            ImGui::PopFont();
            if (!comparator_only) {
                ui::row_label("Input delay", lw);
                double delay = static_cast<double>(ch.delay_ps[i]);
                bool done = false;
                ui::time_input("##delay", &delay, -1e12, 1e12, w, &done);
                if (done) ctx.dev.set_delay(c, static_cast<std::int64_t>(delay));
                ui::row_label("Dead time", lw);
                double dead = static_cast<double>(ch.deadtime_ps[i]);
                done = false;
                ui::time_input("##dead", &dead, 0, 1e12, w, &done);
                if (done) ctx.dev.set_deadtime(c, static_cast<std::int64_t>(dead));
            }
            ImGui::PopID();
        }
    }
    const int cols = comparator_only ? 3 : 5;
    const ImGuiTableFlags tf = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp;
    if (!compact && ImGui::BeginTable("##chtable", cols, tf)) {
        ImGui::TableSetupColumn("Ch", ImGuiTableColumnFlags_WidthFixed, 26);
        ImGui::TableSetupColumn("Trigger (V)", 0, 1.0f);
        ImGui::TableSetupColumn("Hysteresis (Ω) (?)", 0, 1.0f);
        if (!comparator_only) {
            ImGui::TableSetupColumn("Input delay", 0, 1.25f);
            ImGui::TableSetupColumn("Dead time", 0, 1.25f);
        }
        ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
        for (int col = 0; col < cols; ++col) {
            ImGui::TableSetColumnIndex(col);
            ImGui::TableHeader(ImGui::TableGetColumnName(col));
            if (col == 2 && ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32.0f);
                ImGui::TextUnformatted(ui::kHysteresisHelp);
                ImGui::PopTextWrapPos();
                ImGui::EndTooltip();
            }
            if (col == 3 && ImGui::IsItemHovered())
                ImGui::SetTooltip("Added to every tag of the channel (Swabian setInputDelay), in software and exactly;\n"
                                  "any sign, up to ±1 s. Applies to every measurement, live and file replays.");
            if (col == 4 && ImGui::IsItemHovered())
                ImGui::SetTooltip("Tags closer than this to the previous accepted tag of the channel are dropped\n"
                                  "(Swabian setDeadtime, non-extending, in software). The TDC-GPX2 itself needs 20 ns\n"
                                  "between pulses (50 / 100 ns in its 2x / 4x high-resolution modes).");
        }
        for (int c = 1; c <= dt::kNumChannels; ++c) {
            const auto i = static_cast<std::size_t>(c - 1);
            ImGui::PushID(c);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            const bool used = ((highlight >> (c - 1)) & 1U) != 0;
            ImGui::TextColored(used ? ui::channel_colour(c) : ui::c_dim(), "%d", c);
            // trigger
            ImGui::TableNextColumn();
            double trig = ch.trigger_v[i];
            // Enter, a -/+ step or leaving the field after typing applies it.
            if (ui::input_commit("##trig", &trig, 0.001, 0.01, "%.4f")) ctx.dev.set_trigger(c, trig);
            ui::probe("trigger_" + std::to_string(c));
            ImGui::PushFont(ui::fonts.small);
            if (r) {
                const double now = tdcctl::threshold_volts(r->board.dac_codes[i]);
                const bool differs = std::fabs(now - ch.trigger_v[i]) > 0.0005;
                ImGui::TextColored(differs ? ui::c_warn() : ui::c_dim(), "%sboard %+.4f V", differs ? "! " : "", now);
            } else {
                ui::dim_text("saved");
            }
            ImGui::PopFont();
            // hysteresis
            ImGui::TableNextColumn();
            double hys = ch.hysteresis_ohm[i];
            if (ui::input_commit("##hys", &hys, 25, 250, "%.0f")) ctx.dev.set_hysteresis(c, hys);
            ImGui::PushFont(ui::fonts.small);
            if (r) {
                const double now = tdcctl::wiper_ohms(r->board.wiper(c - 1));
                const bool differs = std::fabs(now - ch.hysteresis_ohm[i]) > 25.0;
                ImGui::TextColored(differs ? ui::c_warn() : ui::c_dim(), "%sboard %.0f Ω ≈%.0f mV", differs ? "! " : "", now,
                                   ui::hysteresis_mv(now));
            } else {
                ui::dim_text("≈%.0f mV", ui::hysteresis_mv(ch.hysteresis_ohm[i]));
            }
            ImGui::PopFont();
            if (!comparator_only) {
                ImGui::TableNextColumn();
                double delay = static_cast<double>(ch.delay_ps[i]);
                bool done = false;
                ui::time_input("##delay", &delay, -1e12, 1e12, 90, &done);
                if (done) ctx.dev.set_delay(c, static_cast<std::int64_t>(delay));
                ImGui::TableNextColumn();
                double dead = static_cast<double>(ch.deadtime_ps[i]);
                done = false;
                ui::time_input("##dead", &dead, 0, 1e12, 90, &done);
                if (done) ctx.dev.set_deadtime(c, static_cast<std::int64_t>(dead));
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::PushFont(ui::fonts.small);
    ImGui::PushTextWrapPos(0);
    ui::dim_text("Applied on Enter (or the -/+ steps, or leaving the field) and saved for the next session. While measurements run, each "
                 "change briefly stops the board's run, is applied and streaming resumes (the FPGA takes no comparator "
                 "commands while it streams); the measurements keep their data. Safe trigger range %+.3f … %+.3f V.",
                 dt::kTriggerMinVolts, dt::kTriggerMaxVolts);
    ImGui::PopTextWrapPos();
    ImGui::PopFont();
}

}  // namespace app
