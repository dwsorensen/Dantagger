// Counter panel (Swabian Counter): counts per time bin of capture time, a
// rolling buffer of n_values bins per channel, shown as a trace.

#include "panels.hpp"

#include <implot.h>

#include <algorithm>
#include <cmath>
#include <fstream>

namespace app {

namespace {

class CounterPanel final : public Panel {
public:
    explicit CounterPanel(int id) : Panel(Kind::counter, id) {}

protected:
    struct Settings {
        unsigned mask{0x3};
        double binwidth_ps{1e10};   // 10 ms
        int n_values{1000};
        bool operator==(const Settings& o) const { return mask == o.mask && binwidth_ps == o.binwidth_ps && n_values == o.n_values; }
    };

    std::shared_ptr<dt::Measurement> make() override {
        std::vector<int> chans;
        for (int c = 1; c <= 4; ++c)
            if ((edit_.mask >> (c - 1)) & 1U) chans.push_back(c);
        return std::make_shared<dt::Counter>(chans, static_cast<dt::timestamp_t>(edit_.binwidth_ps), edit_.n_values);
    }
    bool edited_differs() const override { return !(edit_ == applied_); }
    void adopt_edited() override { applied_ = edit_; }
    unsigned channel_mask() const override { return edit_.mask; }
    float settings_width() const override { return 380.0f; }

    void settings_ui(AppContext&) override {
        const float lw = 110;
        ui::row_label("Channels", lw);
        channel_checks("##ch", &edit_.mask);
        ui::row_label("Bin width", lw);
        ui::time_input("bw", &edit_.binwidth_ps, 1e3, 1e13, 110);
        ui::row_label("Bins kept", lw);
        ImGui::SetNextItemWidth(110);
        ImGui::InputInt("##n", &edit_.n_values, 0, 0);
        edit_.n_values = std::clamp(edit_.n_values, 1, 10'000'000);
        ui::dim_text("buffer covers %s", ui::fmt_time_ps(edit_.binwidth_ps * edit_.n_values).c_str());
        ImGui::PushTextWrapPos(0);
        ui::dim_text("Bins are left-closed intervals of capture time; only completed bins are shown, the newest on the "
                     "right (Swabian Counter, rolling). Totals count every completed bin since the start.");
        ImGui::PopTextWrapPos();
    }

    void pull(dt::Measurement& m) override {
        auto& c = static_cast<dt::Counter&>(m);
        chans_ = c.channels();
        n_ = c.n_values();
        bw_ = c.binwidth();
        completed_ = c.completed();
        data_ = c.data(true);
        totals_ = c.total_counts();
    }

    void view_ui(AppContext& ctx) override {
        if (chans_.empty()) {
            ui::dim_text("Start to count. Bin width %s, %d bins.", ui::fmt_time_ps(edit_.binwidth_ps).c_str(), edit_.n_values);
            return;
        }
        const std::int64_t filled = std::min<std::int64_t>(completed_, n_);
        const double bw_s = static_cast<double>(bw_) * 1e-12;
        const std::size_t first = static_cast<std::size_t>(n_ - filled);   // rolling: valid bins at the end
        // ---- stats table ----
        const ImGuiTableFlags tf = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchSame;
        if (ImGui::BeginTable("##stats", 8, tf)) {
            for (const char* h : {"Ch", "Last", "Mean", "Std", "Min", "Max", "Var/mean", "Total"}) ImGui::TableSetupColumn(h);
            ImGui::TableHeadersRow();
            for (std::size_t s = 0; s < chans_.size(); ++s) {
                const int c = chans_[s];
                double sum = 0, sum2 = 0, lo = 1e300, hi = -1e300, last = 0;
                for (std::int64_t j = 0; j < filled; ++j) {
                    const double v = scale() * data_[s * static_cast<std::size_t>(n_) + first + static_cast<std::size_t>(j)];
                    sum += v;
                    sum2 += v * v;
                    lo = std::min(lo, v);
                    hi = std::max(hi, v);
                    last = v;
                }
                const double mean = filled ? sum / static_cast<double>(filled) : 0;
                const double var = filled > 1 ? std::max(0.0, (sum2 - static_cast<double>(filled) * mean * mean) / static_cast<double>(filled - 1)) : 0;
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextColored(ui::channel_colour(c), "%d", c);
                ImGui::PushFont(ui::fonts.mono);
                auto cell = [&](double v) {
                    ImGui::TableNextColumn();
                    if (filled == 0) ui::dim_text("—");
                    else if (normalized_) ImGui::TextUnformatted(ui::fmt_rate(v).c_str());
                    else ImGui::Text("%.6g", v);
                };
                cell(last);
                cell(mean);
                cell(std::sqrt(var));
                cell(lo);
                cell(hi);
                ImGui::TableNextColumn();
                // Var/mean of the counts: 1 for Poisson; > 1 means the rate itself fluctuates.
                const double cmean = normalized_ ? mean * bw_s : mean, cvar = normalized_ ? var * bw_s * bw_s : var;
                if (filled > 1 && cmean > 0) ImGui::Text("%.3g", cvar / cmean);
                else ui::dim_text("—");
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(ui::fmt_count(s < totals_.size() ? totals_[s] : 0).c_str());
                ImGui::PopFont();
            }
            ImGui::EndTable();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Over the %lld bins shown. Var/mean of the counts per bin is 1 for Poisson statistics;\n"
                              "above 1 the rate itself fluctuates (e.g. laser power, trigger near the noise).",
                              static_cast<long long>(filled));
        if (completed_ < n_)
            ui::dim_text("%lld of %d bins filled so far.", static_cast<long long>(completed_), n_);

        // ---- toolbar ----
        static const char* shows[] = {"counts per bin", "rate (1/s)"};
        int show = normalized_ ? 1 : 0;
        ImGui::SetNextItemWidth(130);
        if (ImGui::Combo("##show", &show, shows, 2)) normalized_ = show == 1;
        ImGui::SameLine(0, 18);
        ImGui::Checkbox("log y", &log_y_);
        ImGui::SameLine(0, 18);
        ImGui::Checkbox("auto-fit", &autofit_);
        ui::help("Keeps the axes on the data; switch off to zoom and pan with the mouse.");
        ui::toolbar_next(ImGui::CalcTextSize("Export CSV…").x + 20);
        const std::string path = ui::csv_button("ctr" + std::to_string(id()), "Export CSV…", default_csv(ctx, "counter"));
        if (!path.empty()) export_csv(ctx, path, filled, first);

        // ---- trace ----
        if (ImPlot::BeginPlot("##ctr", ImVec2(-1, std::max(180.0f, ImGui::GetContentRegionAvail().y - 4)))) {
            const ImPlotAxisFlags af = autofit_ ? ImPlotAxisFlags_AutoFit : ImPlotAxisFlags_None;
            ImPlot::SetupAxes("capture time (s)", normalized_ ? "rate (1/s)" : "counts per bin", af, af);
            if (log_y_) ImPlot::SetupAxisScale(ImAxis_Y1, ImPlotScale_Log10);
            ImPlot::SetupLegend(ImPlotLocation_NorthWest);
            xs_.resize(static_cast<std::size_t>(filled));
            ys_.resize(static_cast<std::size_t>(filled));
            const double t0 = static_cast<double>(completed_ - filled) * bw_s;
            for (std::int64_t j = 0; j < filled; ++j) xs_[static_cast<std::size_t>(j)] = t0 + static_cast<double>(j) * bw_s;
            for (std::size_t s = 0; s < chans_.size(); ++s) {
                for (std::int64_t j = 0; j < filled; ++j) {
                    const double v = scale() * data_[s * static_cast<std::size_t>(n_) + first + static_cast<std::size_t>(j)];
                    ys_[static_cast<std::size_t>(j)] = log_y_ ? std::max(v, 0.5 * scale()) : v;
                }
                char name[16];
                std::snprintf(name, sizeof name, "ch %d", chans_[s]);
                ImPlot::SetNextLineStyle(ui::channel_colour(chans_[s]), 1.6f);
                if (filled > 0) ImPlot::PlotStairs(name, xs_.data(), ys_.data(), static_cast<int>(filled));
            }
            if (ImPlot::IsPlotHovered() && filled > 0) {
                const double mx = ImPlot::GetPlotMousePos().x;
                const auto j = static_cast<std::int64_t>(std::floor((mx - t0) / bw_s));
                if (j >= 0 && j < filled) {
                    std::string tip;
                    char line[128];
                    std::snprintf(line, sizeof line, "bin %.6g ... %.6g s", t0 + static_cast<double>(j) * bw_s,
                                  t0 + static_cast<double>(j + 1) * bw_s);
                    tip += line;
                    for (std::size_t s = 0; s < chans_.size(); ++s) {
                        const double v = data_[s * static_cast<std::size_t>(n_) + first + static_cast<std::size_t>(j)];
                        std::snprintf(line, sizeof line, "\nch %d  %.0f counts  (%s)", chans_[s], v, ui::fmt_rate(v / bw_s).c_str());
                        tip += line;
                    }
                    ImGui::SetTooltip("%s", tip.c_str());
                }
            }
            ImPlot::EndPlot();
        }
    }

    std::map<std::string, double> metrics() const override {
        std::map<std::string, double> m;
        const std::int64_t filled = std::min<std::int64_t>(completed_, n_);
        m["filled"] = static_cast<double>(filled);
        for (std::size_t s = 0; s < chans_.size(); ++s) {
            double sum = 0;
            for (std::int64_t j = 0; j < filled; ++j) sum += data_[s * static_cast<std::size_t>(n_) + static_cast<std::size_t>(n_ - filled + j)];
            m["mean" + std::to_string(chans_[s])] = filled ? sum / static_cast<double>(filled) : 0;
            m["total" + std::to_string(chans_[s])] = s < totals_.size() ? static_cast<double>(totals_[s]) : 0;
        }
        return m;
    }
    std::string csv_key() const override { return "ctr" + std::to_string(id()); }

    void save_settings(KeyValues& kv, const std::string& p) const override {
        kv.emplace_back(p + "channels", std::to_string(edit_.mask));
        kv.emplace_back(p + "binwidth_ps", std::to_string(static_cast<long long>(edit_.binwidth_ps)));
        kv.emplace_back(p + "n_values", std::to_string(edit_.n_values));
        kv.emplace_back(p + "normalized", normalized_ ? "1" : "0");
    }
    void load_settings(const KeyValues& kv, const std::string& p) override {
        edit_.mask = static_cast<unsigned>(kv_num(kv, p + "channels", 3)) & 0xFU;
        if (!edit_.mask) edit_.mask = 3;
        edit_.binwidth_ps = std::clamp(kv_num(kv, p + "binwidth_ps", 1e10), 1e3, 1e13);
        edit_.n_values = std::clamp(static_cast<int>(kv_num(kv, p + "n_values", 1000)), 1, 10'000'000);
        normalized_ = kv_get(kv, p + "normalized") == "1";
    }

private:
    double scale() const { return normalized_ ? 1.0 / (static_cast<double>(bw_) * 1e-12) : 1.0; }

    void export_csv(AppContext& ctx, const std::string& path, std::int64_t filled, std::size_t first) {
        std::ofstream out(path);
        out << csv_preamble(ctx);
        out << "# Counter: bin width " << bw_ << " ps, " << n_ << " bins kept, " << completed_
            << " bins completed; counts per bin (rolling, oldest first)\nbin_start_s";
        for (int c : chans_) out << ",ch" << c;
        out << "\n";
        const double bw_s = static_cast<double>(bw_) * 1e-12;
        for (std::int64_t j = 0; j < filled; ++j) {
            out << static_cast<double>(completed_ - filled + j) * bw_s;
            for (std::size_t s = 0; s < chans_.size(); ++s) out << "," << data_[s * static_cast<std::size_t>(n_) + first + static_cast<std::size_t>(j)];
            out << "\n";
        }
        log(ctx, out ? Level::ok : Level::error, (out ? "exported " : "cannot write ") + path);
    }

    Settings edit_, applied_{0, 0, 0};
    std::vector<int> chans_;
    int n_{0};
    dt::timestamp_t bw_{1};
    std::int64_t completed_{0};
    std::vector<std::int32_t> data_;
    std::vector<std::uint64_t> totals_;
    std::vector<double> xs_, ys_;
    bool normalized_{false};
    bool log_y_{false};
    bool autofit_{true};
};

}  // namespace

std::unique_ptr<Panel> make_counter_panel(int id) { return std::make_unique<CounterPanel>(id); }

}  // namespace app
