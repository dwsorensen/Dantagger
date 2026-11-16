// TimeTagStream panel (Swabian TimeTagStream): streams the tags to disk for a
// time (user, 2026-10-22: trigger/hysteresis configuration, "stream to disk
// for X seconds", start/stop).  What is written is the unprocessed record
// stream (FILE.bin + FILE.bin.json in the data folder), which every panel can
// read back; the TimeTagStream measurement shows the tags as they pass.  With
// a saved file as the source the panel inspects that file.

#include "panels.hpp"

#include <algorithm>
#include <cmath>
#include <deque>
#include <filesystem>

namespace app {

namespace {

class StreamPanel final : public Panel {
public:
    explicit StreamPanel(int id) : Panel(Kind::stream, id) { run_s = 1.0; }

protected:
    std::shared_ptr<dt::Measurement> make(dt::Tagger&) override {
        std::vector<int> chans;
        for (int c = 1; c <= 4; ++c)
            if ((edit_mask_ >> (c - 1)) & 1U) chans.push_back(c);
        return std::make_shared<dt::TimeTagStream>(4'000'000, chans);
    }
    bool edited_differs() const override { return edit_mask_ != mask_; }
    void adopt_edited() override { mask_ = edit_mask_; }
    unsigned channel_mask() const override { return edit_mask_; }
    bool records_always() const override { return true; }
    unsigned record_mask() const override { return edit_mask_; }
    bool channels_open_by_default() const override { return true; }
    float settings_width() const override { return 470.0f; }

    void settings_ui(AppContext& ctx) override {
        ui::row_label("Channels", 100);
        channel_checks("##ch", &edit_mask_);
        ImGui::PushTextWrapPos(0);
        ui::dim_text("Start streaming writes the board's unprocessed tag stream of these channels (every FPGA record, "
                     "markers included) to the data folder for the time set above, or until Stop:");
        ImGui::PushFont(ui::fonts.mono);
        ImGui::TextUnformatted(ctx.data_dir.c_str());
        ImGui::PopFont();
        ui::dim_text("FILE.bin holds 8 bytes per tag; FILE.bin.json how to decode it. Any panel reads it back with "
                     "Source: saved tag file. Set the trigger levels and hysteresis below.");
        ImGui::PopTextWrapPos();
    }

    void cleared() override {
        counts_.fill(0);
        tags_.clear();
        overflows_ = 0;
        lost_ = 0;
        have_first_ = false;
    }

    void pull(dt::Measurement& m) override {
        auto& s = static_cast<dt::TimeTagStream&>(m);
        const auto b = s.take();
        lost_ += b.lost;
        for (std::size_t i = 0; i < b.timestamps.size(); ++i) {
            if (b.types[i] != 0) {
                ++overflows_;
                continue;
            }
            const int c = b.channels[i];
            ++counts_[static_cast<std::size_t>(c - 1)];
            if (!have_first_) {
                first_t_ = b.timestamps[i];
                have_first_ = true;
            }
            last_t_ = b.timestamps[i];
        }
        // the newest tags for the table
        const std::size_t from = b.timestamps.size() > kShown * 4 ? b.timestamps.size() - kShown * 4 : 0;
        for (std::size_t i = from; i < b.timestamps.size(); ++i)
            if (b.types[i] == 0) tags_.push_back({b.timestamps[i], b.channels[i]});
        while (tags_.size() > kShown) tags_.pop_front();
    }

    void view_ui(AppContext&) override {
        // ---- recording ----
        if (auto rec = recorder()) {
            const auto p = rec->progress();
            if (ui::card_begin("##rec", "Recording")) {
                ImGui::TextColored(ui::c_warn(), "● REC");
                ImGui::SameLine();
                ImGui::PushFont(ui::fonts.mono);
                ImGui::TextUnformatted(p.path.c_str());
                ImGui::PopFont();
                const double limit = run_s;
                char overlay[96];
                std::snprintf(overlay, sizeof overlay, "%.2f s%s", p.seconds, limit > 0 ? (" of " + std::to_string(limit).substr(0, 6) + " s").c_str() : "");
                ImGui::ProgressBar(limit > 0 ? static_cast<float>(std::min(1.0, p.seconds / limit)) : 0.0f, ImVec2(-1, 0), overlay);
                const double elapsed = std::max(1e-3, p.seconds);
                ui::dim_text("%s records · %s written (%s/s) · %d run%s", ui::fmt_count(p.records).c_str(),
                             ui::fmt_bytes(static_cast<double>(p.bytes_written)).c_str(),
                             ui::fmt_bytes(static_cast<double>(p.bytes_written) / elapsed).c_str(), p.runs, p.runs == 1 ? "" : "s");
                ui::dim_text("tags written: ch1 %s · ch2 %s · ch3 %s · ch4 %s", ui::fmt_count(p.events[0]).c_str(),
                             ui::fmt_count(p.events[1]).c_str(), ui::fmt_count(p.events[2]).c_str(), ui::fmt_count(p.events[3]).c_str());
                if (p.lost) ui::warn_text("%s records LOST: the disk could not keep up.", ui::fmt_count(p.lost).c_str());
                if (p.fpga_dropped) ui::warn_text("The FPGA dropped %s events (its FIFO was full).", ui::fmt_count(p.fpga_dropped).c_str());
            }
            ui::card_end();
        }

        // ---- the source file (inspect mode) ----
        if (const auto* fb = file_backend()) {
            const auto& info = fb->stream_info();
            if (ui::card_begin("##file", "Saved tag stream")) {
                ImGui::PushFont(ui::fonts.mono);
                ImGui::TextUnformatted(info.path.c_str());
                ImGui::PopFont();
                if (info.have_sidecar) {
                    ui::dim_text("recorded %s · %s · %d + %d-bit frames%s · %.3f s of FPGA time · %s records",
                                 info.start.empty() ? "?" : info.start.c_str(),
                                 info.design_version == 0x54440003   ? (info.record_format == 2 ? "release 3, compact records" : "release 3")
                                 : info.design_version == 0x54440002 ? "release 2"
                                 : info.design_version == 0x54440001 ? "release 1"
                                                                     : "design ?",
                                 info.ref_bits, info.stop_bits, info.ddr ? " DDR" : "", info.seconds, ui::fmt_count(info.records).c_str());
                    if (info.have_comparator)
                        ui::dim_text("triggers %.3f / %.3f / %.3f / %.3f V · hysteresis %.0f / %.0f / %.0f / %.0f Ω", info.trigger_v[0],
                                     info.trigger_v[1], info.trigger_v[2], info.trigger_v[3], info.hysteresis_ohm[0],
                                     info.hysteresis_ohm[1], info.hysteresis_ohm[2], info.hysteresis_ohm[3]);
                    if (!info.note.empty()) ui::dim_text("%s", info.note.c_str());
                } else {
                    ui::warn_text("No sidecar (FILE.json): decoded with the release-1 defaults.");
                }
            }
            ui::card_end();
        }

        // ---- tags per channel ----
        if (ui::card_begin("##counts", "Tags")) {
            const double cap = capture_s();
            const ImGuiTableFlags tf = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchSame;
            if (ImGui::BeginTable("##cnt", 4, tf)) {
                for (const char* h : {"Channel", "Tags", "Rate", "Share"}) ImGui::TableSetupColumn(h);
                ImGui::TableHeadersRow();
                std::uint64_t all = 0;
                for (auto n : counts_) all += n;
                for (int c = 1; c <= 4; ++c) {
                    if (!((mask_ >> (c - 1)) & 1U)) continue;
                    const auto n = counts_[static_cast<std::size_t>(c - 1)];
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextColored(ui::channel_colour(c), "%d", c);
                    ImGui::PushFont(ui::fonts.mono);
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(ui::fmt_count(n).c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(cap > 0 ? ui::fmt_rate(static_cast<double>(n) / cap).c_str() : "—");
                    ImGui::TableNextColumn();
                    ImGui::Text("%.1f %%", all ? 100.0 * static_cast<double>(n) / static_cast<double>(all) : 0.0);
                    ImGui::PopFont();
                }
                ImGui::EndTable();
            }
            ui::dim_text("%.3f s captured%s%s", cap, overflows_ ? (" · " + ui::fmt_count(overflows_) + " overflow markers").c_str() : "",
                         lost_ ? (" · " + ui::fmt_count(lost_) + " tags not shown (preview buffer full)").c_str() : "");
        }
        ui::card_end();

        // ---- recordings of this session ----
        if (!recordings().empty() && ui::card_begin("##recs", "Recordings")) {
            for (std::size_t i = recordings().size(); i-- > 0;) {
                const auto& path = recordings()[i];
                ImGui::PushID(static_cast<int>(i));
                std::error_code ec;
                const auto size = std::filesystem::file_size(path, ec);
                ImGui::PushFont(ui::fonts.mono);
                ImGui::TextUnformatted(std::filesystem::path(path).filename().string().c_str());
                ImGui::PopFont();
                ImGui::SameLine();
                ui::dim_text("%s", ec ? "missing" : ui::fmt_bytes(static_cast<double>(size)).c_str());
                ImGui::AlignTextToFramePadding();
                ui::dim_text("open in a new");
                for (Kind k : {Kind::countrate, Kind::counter, Kind::histogram, Kind::correlation, Kind::histogram2d}) {
                    ImGui::SameLine();
                    if (ImGui::SmallButton(kind_title(k)) && g_open_panel_with_file) g_open_panel_with_file(k, path);
                }
                ImGui::PopID();
            }
        }
        if (!recordings().empty()) ui::card_end();

        // ---- the newest tags ----
        if (ui::card_begin("##tags", "Newest tags", std::max(160.0f, ImGui::GetContentRegionAvail().y - 4))) {
            const ImGuiTableFlags tf = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY |
                                       ImGuiTableFlags_SizingStretchProp;
            if (ImGui::BeginTable("##tagt", 4, tf, ImVec2(0, ImGui::GetContentRegionAvail().y))) {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn("Ch", 0, 0.4f);
                ImGui::TableSetupColumn("Time (ps)", 0, 1.6f);
                ImGui::TableSetupColumn("since previous", 0, 1.0f);
                ImGui::TableSetupColumn("since start", 0, 1.0f);
                ImGui::TableHeadersRow();
                for (std::size_t i = tags_.size(); i-- > 0;) {
                    const auto& t = tags_[i];
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextColored(ui::channel_colour(t.channel), "%d", t.channel);
                    ImGui::PushFont(ui::fonts.mono);
                    ImGui::TableNextColumn();
                    ImGui::Text("%lld", static_cast<long long>(t.time));
                    ImGui::TableNextColumn();
                    if (i > 0) ImGui::TextUnformatted(ui::fmt_time_ps(static_cast<double>(t.time - tags_[i - 1].time)).c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(ui::fmt_time_ps(static_cast<double>(t.time - first_t_)).c_str());
                    ImGui::PopFont();
                }
                ImGui::EndTable();
            }
        }
        ui::card_end();
    }

    std::map<std::string, double> metrics() const override {
        std::map<std::string, double> m;
        for (int c = 1; c <= 4; ++c) m["counts" + std::to_string(c)] = static_cast<double>(counts_[static_cast<std::size_t>(c - 1)]);
        m["shown_tags"] = static_cast<double>(tags_.size());
        return m;
    }

    void save_settings(KeyValues& kv, const std::string& p) const override {
        kv.emplace_back(p + "channels", std::to_string(edit_mask_));
    }
    void load_settings(const KeyValues& kv, const std::string& p) override {
        edit_mask_ = static_cast<unsigned>(kv_num(kv, p + "channels", 0xF)) & 0xFU;
        if (!edit_mask_) edit_mask_ = 0xF;
    }

private:
    static constexpr std::size_t kShown = 200;
    struct Tag {
        dt::timestamp_t time;
        int channel;
    };
    unsigned edit_mask_{0xF}, mask_{0xF};
    std::array<std::uint64_t, 4> counts_{};
    std::deque<Tag> tags_;
    std::uint64_t overflows_{0}, lost_{0};
    bool have_first_{false};
    dt::timestamp_t first_t_{0}, last_t_{0};
};

}  // namespace

std::unique_ptr<Panel> make_stream_panel(int id) { return std::make_unique<StreamPanel>(id); }

}  // namespace app
