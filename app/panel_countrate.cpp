// Countrate panel (Swabian Countrate): the average rate of each channel over
// the capture, counts / capture duration, plus a rate history sampled while
// it runs.

#include "panels.hpp"

#include <implot.h>

#include <algorithm>
#include <cmath>
#include <deque>
#include <fstream>

namespace app {

namespace {

class CountratePanel final : public Panel {
public:
    explicit CountratePanel(int id) : Panel(Kind::countrate, id) {}

protected:
    std::shared_ptr<dt::Measurement> make() override {
        std::vector<int> chans;
        for (int c = 1; c <= 4; ++c)
            if ((edit_mask_ >> (c - 1)) & 1U) chans.push_back(c);
        return std::make_shared<dt::Countrate>(chans);
    }
    bool edited_differs() const override { return edit_mask_ != mask_; }
    void adopt_edited() override { mask_ = edit_mask_; }
    unsigned channel_mask() const override { return edit_mask_; }
    float settings_width() const override { return 360.0f; }

    void settings_ui(AppContext&) override {
        ui::row_label("Channels", 100);
        channel_checks("##ch", &edit_mask_);
        ImGui::PushTextWrapPos(0);
        ui::dim_text("Rate = counts / capture duration over the whole capture (Swabian Countrate.getData). The history "
                     "below samples the rate between two looks at the data (about 10 per second).");
        ImGui::PopTextWrapPos();
    }

    void cleared() override {
        history_.clear();
        have_prev_ = false;
    }

    void pull(dt::Measurement& m) override {
        auto& cr = static_cast<dt::Countrate&>(m);
        chans_ = cr.channels();
        rates_ = cr.rates();
        totals_ = cr.counts_total();
        const double t = static_cast<double>(cr.capture_duration) * 1e-12;
        if (!have_prev_ || t < prev_t_ || prev_totals_.size() != totals_.size()) {
            have_prev_ = true;
            prev_t_ = t;
            prev_totals_ = totals_;
            return;
        }
        if (t - prev_t_ < 0.05) return;
        Sample s;
        s.t = t;
        for (std::size_t i = 0; i < totals_.size(); ++i) {
            const int c = chans_[i];
            s.rate[static_cast<std::size_t>(c - 1)] = static_cast<double>(totals_[i] - prev_totals_[i]) / (t - prev_t_);
            s.has[static_cast<std::size_t>(c - 1)] = true;
        }
        history_.push_back(s);
        while (history_.size() > 200000) history_.pop_front();
        prev_t_ = t;
        prev_totals_ = totals_;
    }

    void view_ui(AppContext& ctx) override {
        if (chans_.empty()) {
            ui::dim_text("Start to measure. %s", use_file ? "The rates of the file's whole capture." : "");
            return;
        }
        const double cap = capture_s();
        // ---- tiles: as many per row as fit, the big font when there is room ----
        const float avail = ImGui::GetContentRegionAvail().x;
        const int n = static_cast<int>(chans_.size());
        const float gap = ImGui::GetStyle().ItemSpacing.x;
        const int per_row = std::max(1, std::min(n, static_cast<int>((avail + gap) / (165.0f + gap))));
        const float tile_w = (avail - gap * static_cast<float>(per_row - 1)) / static_cast<float>(per_row);
        ImFont* number_font = tile_w >= 215.0f ? ui::fonts.big : ui::fonts.heading;
        for (int i = 0; i < n; ++i) {
            const int c = chans_[static_cast<std::size_t>(i)];
            if (i % per_row != 0) ImGui::SameLine();
            ImGui::PushID(c);
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ui::c_panel());
            ImGui::BeginChild("##tile", ImVec2(tile_w, 0),
                              ImGuiChildFlags_Border | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
            const ImVec2 p = ImGui::GetWindowPos();
            ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + 4, p.y + ImGui::GetWindowHeight()),
                                                      ImGui::GetColorU32(ui::channel_colour(c)), 8.0f, ImDrawFlags_RoundCornersLeft);
            ImGui::TextColored(ui::channel_colour(c), "Channel %d", c);
            ImGui::PushFont(number_font);
            const double r = rates_[static_cast<std::size_t>(i)];
            ImGui::TextUnformatted(std::isnan(r) ? "—" : ui::fmt_rate(r).c_str());
            ImGui::PopFont();
            ImGui::PushFont(ui::fonts.small);
            ui::dim_text("%s counts", ui::fmt_count(static_cast<std::uint64_t>(totals_[static_cast<std::size_t>(i)])).c_str());
            if (!history_.empty() && history_.back().has[static_cast<std::size_t>(c - 1)]) {
                // spread of the sampled rates over the shown history
                double sum = 0, sum2 = 0;
                int k = 0;
                for (const auto& s : visible()) {
                    sum += s->rate[static_cast<std::size_t>(c - 1)];
                    sum2 += s->rate[static_cast<std::size_t>(c - 1)] * s->rate[static_cast<std::size_t>(c - 1)];
                    ++k;
                }
                const double mean = k ? sum / k : 0;
                const double sd = k > 1 ? std::sqrt(std::max(0.0, (sum2 - k * mean * mean) / (k - 1))) : 0;
                ui::dim_text("now %s · spread %.2g %%", ui::fmt_rate(history_.back().rate[static_cast<std::size_t>(c - 1)]).c_str(),
                             mean > 0 ? 100 * sd / mean : 0.0);
            } else {
                ui::dim_text(" ");
            }
            ImGui::PopFont();
            ImGui::EndChild();
            ImGui::PopStyleColor();
            ImGui::PopID();
        }
        // ---- totals and warnings ----
        double sum = 0;
        for (double r : rates_) sum += std::isnan(r) ? 0 : r;
        ui::dim_text("Capture %.3f s · all channels %s%s", cap, ui::fmt_rate(sum).c_str(),
                     use_file ? " · from the saved tag file" : "");
        if (running() && cap > 1.0)
            for (std::size_t i = 0; i < chans_.size(); ++i)
                if (totals_[i] == 0)
                    ui::warn_text("Channel %d counts nothing: its trigger may be above the signal (or below the "
                                  "baseline with too much hysteresis), or nothing is connected.", chans_[i]);
        if (!use_file && ctx.dev.rates().snapshot().dropped > 0)
            ui::warn_text("The FPGA dropped events in this session (see the rate bar): rates over those intervals are "
                          "lower bounds.");

        // ---- history ----
        ImGui::Spacing();
        static const char* spans[] = {"10 s", "1 min", "10 min", "all"};
        static const double span_s[] = {10, 60, 600, 1e18};
        ImGui::SetNextItemWidth(90);
        ImGui::Combo("history", &span_, spans, 4);
        ImGui::SameLine(0, 18);
        ImGui::Checkbox("log y", &log_y_);
        ImGui::SameLine(0, 18);
        ImGui::Checkbox("auto-fit", &autofit_);
        ui::toolbar_next(ImGui::CalcTextSize("Export CSV…").x + 20);
        const std::string path = ui::csv_button("cr" + std::to_string(id()), "Export CSV…", default_csv(ctx, "countrate"));
        if (!path.empty()) export_csv(ctx, path);
        span_seconds_ = span_s[span_];

        const auto vis = visible();
        if (ImPlot::BeginPlot("##crh", ImVec2(-1, std::max(180.0f, ImGui::GetContentRegionAvail().y - 4)))) {
            const ImPlotAxisFlags af = autofit_ ? ImPlotAxisFlags_AutoFit : ImPlotAxisFlags_None;
            ImPlot::SetupAxes("capture time (s)", "rate (1/s)", af, ImPlotAxisFlags_None);
            if (log_y_) ImPlot::SetupAxisScale(ImAxis_Y1, ImPlotScale_Log10);
            {   // y: the shown rates with a margin
                std::vector<double> all;
                for (int c : chans_)
                    for (const auto* s : vis) all.push_back(log_y_ ? std::max(s->rate[static_cast<std::size_t>(c - 1)], 0.5) : s->rate[static_cast<std::size_t>(c - 1)]);
                ui::setup_y_padded(all.data(), all.size(), log_y_, autofit_);
            }
            ImPlot::SetupLegend(ImPlotLocation_North, ImPlotLegendFlags_Outside | ImPlotLegendFlags_Horizontal);
            std::vector<double> xs, ys;
            for (int c : chans_) {
                xs.clear();
                ys.clear();
                for (const auto* s : vis) {
                    xs.push_back(s->t);
                    ys.push_back(log_y_ ? std::max(s->rate[static_cast<std::size_t>(c - 1)], 0.5) : s->rate[static_cast<std::size_t>(c - 1)]);
                }
                char name[16];
                std::snprintf(name, sizeof name, "ch %d", c);
                ImPlot::SetNextLineStyle(ui::channel_colour(c), 1.8f);
                if (!xs.empty()) ImPlot::PlotLine(name, xs.data(), ys.data(), static_cast<int>(xs.size()));
            }
            if (ImPlot::IsPlotHovered() && !vis.empty()) {   // the sample under the mouse
                const double mx = ImPlot::GetPlotMousePos().x;
                const auto it = std::min_element(vis.begin(), vis.end(), [&](const Sample* a, const Sample* b) {
                    return std::fabs(a->t - mx) < std::fabs(b->t - mx);
                });
                std::string tip;
                char line[96];
                std::snprintf(line, sizeof line, "t = %.3f s", (*it)->t);
                tip += line;
                for (int c : chans_) {
                    std::snprintf(line, sizeof line, "\nch %d  %s", c, ui::fmt_rate((*it)->rate[static_cast<std::size_t>(c - 1)]).c_str());
                    tip += line;
                }
                ImGui::SetTooltip("%s", tip.c_str());
            }
            ImPlot::EndPlot();
        }
    }

    std::map<std::string, double> metrics() const override {
        std::map<std::string, double> m;
        for (std::size_t i = 0; i < chans_.size(); ++i) {
            m["rate" + std::to_string(chans_[i])] = rates_[i];
            m["counts" + std::to_string(chans_[i])] = static_cast<double>(totals_[i]);
        }
        m["history"] = static_cast<double>(history_.size());
        return m;
    }
    std::string csv_key() const override { return "cr" + std::to_string(id()); }

    void save_settings(KeyValues& kv, const std::string& p) const override {
        kv.emplace_back(p + "channels", std::to_string(edit_mask_));
    }
    void load_settings(const KeyValues& kv, const std::string& p) override {
        edit_mask_ = static_cast<unsigned>(kv_num(kv, p + "channels", 0xF)) & 0xFU;
        if (!edit_mask_) edit_mask_ = 0xF;
    }

private:
    struct Sample {
        double t{0};
        std::array<double, 4> rate{};
        std::array<bool, 4> has{};
    };

    std::vector<const Sample*> visible() const {
        std::vector<const Sample*> out;
        if (history_.empty()) return out;
        const double from = history_.back().t - span_seconds_;
        for (const auto& s : history_)
            if (s.t >= from) out.push_back(&s);
        return out;
    }

    void export_csv(AppContext& ctx, const std::string& path) {
        std::ofstream out(path);
        out << csv_preamble(ctx);
        out << "# Countrate over the capture (counts / capture duration):";
        for (std::size_t i = 0; i < chans_.size(); ++i)
            out << " ch" << chans_[i] << " " << rates_[i] << " /s (" << totals_[i] << " counts);";
        out << "\n# history: rate between consecutive samples\ncapture_s";
        for (int c : chans_) out << ",rate_ch" << c << "_hz";
        out << "\n";
        for (const auto& s : history_) {
            out << s.t;
            for (int c : chans_) out << "," << s.rate[static_cast<std::size_t>(c - 1)];
            out << "\n";
        }
        log(ctx, out ? Level::ok : Level::error, (out ? "exported " : "cannot write ") + path);
    }

    unsigned edit_mask_{0xF}, mask_{0};
    std::vector<int> chans_;
    std::vector<double> rates_;
    std::vector<std::int64_t> totals_;
    std::deque<Sample> history_;
    bool have_prev_{false};
    double prev_t_{0};
    std::vector<std::int64_t> prev_totals_;
    int span_{1};
    double span_seconds_{60};
    bool log_y_{false};
    bool autofit_{true};
};

}  // namespace

std::unique_ptr<Panel> make_countrate_panel(int id) { return std::make_unique<CountratePanel>(id); }

}  // namespace app
