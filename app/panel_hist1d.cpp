// Histogram and Correlation panels (Swabian Histogram, Correlation): a 1D
// histogram of time differences with the TDC Control app's plot (units, log
// y, auto-fit, hover readout, live Gaussian fit of the tallest peak or of the
// envelope of a comb, CSV export with the fit in the header).

#include "panels.hpp"

#include "tdcctl/gauss_fit.hpp"

#include <implot.h>

#include <algorithm>
#include <cmath>
#include <fstream>

namespace app {

namespace {

constexpr int kMaxShownSteps = 4000;   // display steps (bins merged beyond)

class Hist1DPanel : public Panel {
public:
    using Panel::Panel;

protected:
    struct Settings {
        int a{1}, b{2};                   // Histogram: click, start; Correlation: channel 1, channel 2
        double binwidth_ps{10.0};
        int n_bins{4000};
        bool operator==(const Settings& o) const { return a == o.a && b == o.b && binwidth_ps == o.binwidth_ps && n_bins == o.n_bins; }
    };

    bool edited_differs() const override { return !(edit_ == applied_); }
    void adopt_edited() override { applied_ = edit_; }
    unsigned channel_mask() const override { return (1U << (edit_.a - 1)) | (1U << (edit_.b - 1)); }

    // ---- per type ----
    virtual const char* a_label() const = 0;
    virtual const char* b_label() const = 0;
    virtual const char* x_label() const = 0;            // without the unit
    virtual void type_notes() = 0;                      // under the settings
    virtual void summary_ui() = 0;                       // the counts line(s) above the plot
    virtual bool zero_line() const { return false; }
    virtual double y_scale() const { return 1.0; }       // display scale (Correlation normalised)
    virtual const char* y_label() const { return "counts per bin"; }
    virtual void extra_toolbar() {}
    virtual std::string csv_kind_line() const = 0;

    void settings_ui(AppContext&) override {
        const float lw = 110;
        ui::row_label(a_label(), lw);
        channel_combo("##a", &edit_.a);
        ImGui::SameLine(0, 14);
        ImGui::TextUnformatted(b_label());
        ImGui::SameLine();
        channel_combo("##b", &edit_.b);
        ui::row_label("Bin width", lw);
        ui::time_input("bw", &edit_.binwidth_ps, 1, 1e12, 110);
        ui::row_label("Bins", lw);
        ImGui::SetNextItemWidth(110);
        ImGui::InputInt("##n", &edit_.n_bins, 0, 0);
        edit_.n_bins = std::clamp(edit_.n_bins, 1, 2'000'000);
        ImGui::SameLine();
        ui::dim_text("range %s", ui::fmt_time_ps(edit_.binwidth_ps * edit_.n_bins).c_str());
        ImGui::PushTextWrapPos(0);
        type_notes();
        ImGui::PopTextWrapPos();
    }

    void cleared() override { fit_version_ = ~0ULL; }

    // Bins of the shown data: left edges (ps), counts.
    std::vector<std::int64_t> left_;
    std::vector<std::uint64_t> counts_;
    std::int64_t bw_{1};

    void view_ui(AppContext& ctx) override {
        if (counts_.empty()) {
            ui::dim_text("Start to histogram. Range %s in %d bins.", ui::fmt_time_ps(edit_.binwidth_ps * edit_.n_bins).c_str(),
                         edit_.n_bins);
            return;
        }
        const double range = static_cast<double>(bw_) * static_cast<double>(counts_.size());
        const double x0 = static_cast<double>(left_.front());
        const int unit = ui::resolve_unit(unit_, range);
        const double scale = 1.0 / ui::kUnitPs[unit];

        // ---- numbers ----
        ImGui::PushTextWrapPos(0);
        summary_ui();
        {
            std::uint64_t total = 0, in_view = 0, peak = 0;
            std::size_t peak_i = 0;
            double sum = 0, sum2 = 0;
            const double lo = view_hi_ > view_lo_ ? view_lo_ : -1e300, hi = view_hi_ > view_lo_ ? view_hi_ : 1e300;
            for (std::size_t i = 0; i < counts_.size(); ++i) {
                total += counts_[i];
                const double x = static_cast<double>(left_[i]) + 0.5 * static_cast<double>(bw_);
                if (x < lo || x >= hi) continue;
                in_view += counts_[i];
                sum += x * static_cast<double>(counts_[i]);
                sum2 += x * x * static_cast<double>(counts_[i]);
                if (counts_[i] > peak) {
                    peak = counts_[i];
                    peak_i = i;
                }
            }
            const double mean = in_view ? sum / static_cast<double>(in_view) : 0;
            const double rms = in_view ? std::sqrt(std::max(0.0, sum2 / static_cast<double>(in_view) - mean * mean)) : 0;
            ui::dim_text("in histogram %s · in view %s · peak %s at %s · mean %s · rms %s", ui::fmt_count(total).c_str(),
                         ui::fmt_count(in_view).c_str(), ui::fmt_count(peak).c_str(),
                         ui::fmt_time_ps(static_cast<double>(left_[peak_i])).c_str(), ui::fmt_time_ps(mean).c_str(),
                         ui::fmt_time_ps(rms).c_str());
        }
        ImGui::PopTextWrapPos();
        // ---- fit ----
        if (fit_on_ && (fit_version_ != data_version_ || fit_lo_ != view_lo_ || fit_hi_ != view_hi_ || fit_target_done_ != fit_target_)) {
            const double lo = view_hi_ > view_lo_ ? view_lo_ : -1e300, hi = view_hi_ > view_lo_ ? view_hi_ : 1e300;
            fit_ = tdcctl::fit_gaussian(left_, counts_, bw_, lo, hi,
                                        fit_target_ == 1 ? tdcctl::FitTarget::envelope : tdcctl::FitTarget::tallest_peak);
            fit_version_ = data_version_;
            fit_lo_ = view_lo_;
            fit_hi_ = view_hi_;
            fit_target_done_ = fit_target_;
        }
        if (fit_on_) {
            const auto& f = fit_;
            if (f.ok) {
                ImGui::BeginGroup();
                char what[96] = "";
                if (fit_target_ == 1) std::snprintf(what, sizeof(what), " (envelope through %d tops)", f.teeth);
                ImGui::TextColored(ui::c_fit(), "Gaussian fit%s: FWHM %s · centre %s", what,
                                   ui::fmt_time_err(f.fwhm_ps(), f.fwhm_err_ps()).c_str(),
                                   ui::fmt_time_err(f.centre_ps, f.centre_err_ps).c_str());
                const double bg = std::fabs(f.background) < 1e-6 * f.amplitude ? 0.0 : f.background;
                char quality[96];
                if (f.dof > 0) std::snprintf(quality, sizeof(quality), "chi2/dof %.2f", f.chi2_dof);
                else std::snprintf(quality, sizeof(quality), "exact through %d points (no chi2; errors from counts only)", f.points);
                char bg_err[32] = " (floor)";
                if (f.background_err > 0) std::snprintf(bg_err, sizeof(bg_err), " ± %.2g", f.background_err);
                ui::dim_text("sigma %s · background %.3g%s per bin · %s", ui::fmt_time_err(f.sigma_ps, f.sigma_err_ps).c_str(), bg,
                             bg_err, quality);
                if (f.fwhm_ps() < 1.5 * static_cast<double>(bw_) && fit_target_ == 0)
                    ImGui::TextColored(ui::c_warn(), "The peak is about one bin wide: its width is not resolved. Use narrower bins.");
                ImGui::EndGroup();
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("A exp(-(x - centre)^2 / 2 sigma^2) + background, Poisson maximum likelihood over %s ... %s\n"
                                      "(%d points%s). Amplitude %.4g ± %.2g per bin.\n"
                                      "Errors are 1 sigma, widened by sqrt(chi2/dof) when that is above 1. Zoom in (auto-fit off)\n"
                                      "to fit another peak.",
                                      ui::fmt_time_ps(f.lo_ps).c_str(), ui::fmt_time_ps(f.hi_ps).c_str(), f.points,
                                      f.merged > 1 ? (", " + std::to_string(f.merged) + " bins per point").c_str() : "",
                                      f.amplitude, f.amplitude_err);
            } else {
                ImGui::TextColored(ui::c_dim(), "Gaussian fit: %s", f.why.c_str());
            }
        }

        // ---- toolbar ----
        ImGui::SetNextItemWidth(80);
        ImGui::Combo("x unit", &unit_, ui::kUnits, 6);
        ImGui::SameLine(0, 18);
        ImGui::Checkbox("log y", &log_y_);
        ImGui::SameLine(0, 18);
        ImGui::Checkbox("auto-fit", &autofit_);
        ui::help("Keeps the axes on the full range and the tallest bin; switch off to zoom and pan with the mouse.");
        extra_toolbar();
        static const char* kTargets[] = {"tallest peak", "envelope of peaks"};
        const float target_w = ImGui::CalcTextSize(kTargets[1]).x + ImGui::GetFrameHeight() + 2 * ImGui::GetStyle().FramePadding.x;
        ui::toolbar_next(ImGui::CalcTextSize("Gaussian fit (?)").x + ImGui::GetFrameHeight() + (fit_on_ ? target_w + 10 : 0));
        ImGui::Checkbox("Gaussian fit", &fit_on_);
        if (fit_on_) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(target_w);
            ImGui::Combo("##fit_target", &fit_target_, kTargets, 2);
        }
        ui::help("Fits a Gaussian plus a flat background to the bins in view, live, and shows the FWHM.\n"
                 "Tallest peak: the tallest peak in view (switch auto-fit off and zoom to fit another).\n"
                 "Envelope of peaks: for a comb of narrow peaks (the GPX2's ~44 ps steps) the Gaussian through the tooth "
                 "tops that form the outline.");
        ui::toolbar_next(ImGui::CalcTextSize("Export CSV…").x + 20);
        const std::string path = ui::csv_button("h1" + std::to_string(id()), "Export CSV…", default_csv(ctx, kind_key(kind())));
        if (!path.empty()) export_csv(ctx, path);

        // ---- display steps (merged beyond kMaxShownSteps) ----
        const std::size_t n = counts_.size();
        const std::size_t merge = (n + kMaxShownSteps - 1) / kMaxShownSteps;
        xs_.clear();
        ys_.clear();
        const double ys = y_scale();
        for (std::size_t i = 0; i < n; i += merge) {
            std::uint64_t c = 0;
            for (std::size_t k = i; k < std::min(n, i + merge); ++k) c += counts_[k];
            xs_.push_back(static_cast<double>(left_[i]) * scale);
            const double y = static_cast<double>(c) * ys / static_cast<double>(merge);
            ys_.push_back(log_y_ ? std::max(y, 0.5 * ys) : y);
        }
        xs_.push_back((x0 + range) * scale);   // closing edge
        ys_.push_back(ys_.back());
        if (merge > 1) {
            ui::toolbar_next(260);
            ui::dim_text("display: %zu bins per step (CSV has every bin)", merge);
        }

        // ---- plot ----
        ImGui::PushID("plot");
        if (ImPlot::BeginPlot("##h1", ImVec2(-1, std::max(200.0f, ImGui::GetContentRegionAvail().y - 4)), ImPlotFlags_NoLegend)) {
            char xl[64];
            std::snprintf(xl, sizeof(xl), "%s (%s)", x_label(), ui::kUnits[unit]);
            ImPlot::SetupAxes(xl, y_label());
            if (log_y_) ImPlot::SetupAxisScale(ImAxis_Y1, ImPlotScale_Log10);
            const ImPlotCond cond = autofit_ ? ImPlotCond_Always : ImPlotCond_Once;
            ImPlot::SetupAxisLimits(ImAxis_X1, x0 * scale, (x0 + range) * scale, cond);
            ui::setup_y_padded(ys_.data(), ys_.size(), log_y_, autofit_);   // the shown steps with a margin
            ImPlot::PushStyleColor(ImPlotCol_Line, ui::c_accent());
            ImPlot::PushStyleColor(ImPlotCol_Fill, ImVec4(ui::c_accent().x, ui::c_accent().y, ui::c_accent().z, 0.28f));
            ImPlot::PlotStairs("counts", xs_.data(), ys_.data(), static_cast<int>(xs_.size()), ImPlotStairsFlags_Shaded);
            ImPlot::PopStyleColor(2);
            if (fit_on_ && fit_.ok) {   // the fitted curve, scaled like the display steps
                constexpr int kN = 600;
                std::vector<double> fx(kN), fy(kN);
                for (int i = 0; i < kN; ++i) {
                    const double x = fit_.lo_ps + (fit_.hi_ps - fit_.lo_ps) * i / (kN - 1);
                    fx[static_cast<std::size_t>(i)] = x * scale;
                    fy[static_cast<std::size_t>(i)] = fit_.eval(x) * ys;
                }
                ImPlot::SetNextLineStyle(ui::c_fit(), 2.0f);
                ImPlot::PlotLine("fit", fx.data(), fy.data(), kN);
            }
            if (zero_line()) {
                const double zero = 0.0;
                ImPlot::PushStyleColor(ImPlotCol_Line, ImVec4(ui::c_dim().x, ui::c_dim().y, ui::c_dim().z, 0.7f));
                ImPlot::PlotInfLines("##zero", &zero, 1);
                ImPlot::PopStyleColor();
            }
            if (ImPlot::IsPlotHovered()) {
                const double xp = ImPlot::GetPlotMousePos().x / scale;
                const auto k = static_cast<long long>(std::floor((xp - x0) / static_cast<double>(bw_)));
                if (k >= 0 && k < static_cast<long long>(n)) {
                    const auto kk = static_cast<std::size_t>(k);
                    ImGui::SetTooltip("%s ... %s\n%s counts%s", ui::fmt_time_ps(static_cast<double>(left_[kk])).c_str(),
                                      ui::fmt_time_ps(static_cast<double>(left_[kk] + bw_)).c_str(),
                                      ui::fmt_count(counts_[kk]).c_str(),
                                      ys != 1.0 ? ("\nnormalised " + std::to_string(static_cast<double>(counts_[kk]) * ys)).c_str() : "");
                }
            }
            const ImPlotRect lim = ImPlot::GetPlotLimits();
            view_lo_ = lim.X.Min / scale;
            view_hi_ = lim.X.Max / scale;
            ImPlot::EndPlot();
        }
        ImGui::PopID();
    }

    void export_csv(AppContext& ctx, const std::string& path) {
        std::ofstream out(path);
        out << csv_preamble(ctx) << csv_kind_line();
        if (fit_on_ && fit_.ok) {
            char buf[400];
            std::snprintf(buf, sizeof(buf),
                          "# Gaussian fit (%s, Poisson ML, %.0f ... %.0f ps): FWHM %.3f +- %.3f ps, centre %.3f +- %.3f ps, "
                          "sigma %.3f +- %.3f ps, amplitude %.4g +- %.3g /bin, background %.4g +- %.3g /bin, chi2/dof %.3f\n",
                          fit_target_ == 1 ? "envelope through the tooth tops" : "tallest peak", fit_.lo_ps, fit_.hi_ps,
                          fit_.fwhm_ps(), fit_.fwhm_err_ps(), fit_.centre_ps, fit_.centre_err_ps, fit_.sigma_ps,
                          fit_.sigma_err_ps, fit_.amplitude, fit_.amplitude_err, fit_.background, fit_.background_err,
                          fit_.chi2_dof);
            out << buf;
        }
        const double ys = y_scale();
        out << "bin_left_ps,count" << (ys != 1.0 ? ",normalized" : "") << "\n";
        for (std::size_t i = 0; i < counts_.size(); ++i) {
            out << left_[i] << "," << counts_[i];
            if (ys != 1.0) out << "," << static_cast<double>(counts_[i]) * ys;
            out << "\n";
        }
        log(ctx, out ? Level::ok : Level::error, (out ? "exported " : "cannot write ") + path);
    }

public:
    std::map<std::string, double> metrics() const override {
        std::map<std::string, double> m;
        std::uint64_t total = 0, peak = 0;
        std::size_t peak_i = 0;
        for (std::size_t i = 0; i < counts_.size(); ++i) {
            total += counts_[i];
            if (counts_[i] > peak) {
                peak = counts_[i];
                peak_i = i;
            }
        }
        m["total"] = static_cast<double>(total);
        m["peak"] = static_cast<double>(peak);
        m["peak_ps"] = counts_.empty() ? 0.0 : static_cast<double>(left_[peak_i]) + 0.5 * static_cast<double>(bw_);
        if (fit_on_ && fit_.ok) {
            m["fit_fwhm_ps"] = fit_.fwhm_ps();
            m["fit_centre_ps"] = fit_.centre_ps;
        }
        return m;
    }
    std::string csv_key() const override { return "h1" + std::to_string(id()); }

protected:
    void save_common(KeyValues& kv, const std::string& p) const {
        kv.emplace_back(p + "a", std::to_string(edit_.a));
        kv.emplace_back(p + "b", std::to_string(edit_.b));
        kv.emplace_back(p + "binwidth_ps", std::to_string(static_cast<long long>(edit_.binwidth_ps)));
        kv.emplace_back(p + "n_bins", std::to_string(edit_.n_bins));
        kv.emplace_back(p + "fit", fit_on_ ? std::to_string(fit_target_ + 1) : "0");
        kv.emplace_back(p + "log_y", log_y_ ? "1" : "0");
    }
    void load_common(const KeyValues& kv, const std::string& p) {
        edit_.a = std::clamp(static_cast<int>(kv_num(kv, p + "a", edit_.a)), 1, 4);
        edit_.b = std::clamp(static_cast<int>(kv_num(kv, p + "b", edit_.b)), 1, 4);
        edit_.binwidth_ps = std::clamp(kv_num(kv, p + "binwidth_ps", edit_.binwidth_ps), 1.0, 1e12);
        edit_.n_bins = std::clamp(static_cast<int>(kv_num(kv, p + "n_bins", edit_.n_bins)), 1, 2'000'000);
        const int fit = static_cast<int>(kv_num(kv, p + "fit", 0));
        fit_on_ = fit > 0;
        fit_target_ = fit == 2 ? 1 : 0;
        log_y_ = kv_get(kv, p + "log_y") == "1";
    }

    Settings edit_, applied_{0, 0, 0, 0};
    int unit_{0};
    bool log_y_{false};
    bool autofit_{true};
    bool fit_on_{false};
    int fit_target_{0};
    double view_lo_{0}, view_hi_{0};

private:
    tdcctl::GaussFit fit_;
    std::uint64_t fit_version_{~0ULL};
    double fit_lo_{0}, fit_hi_{0};
    int fit_target_done_{-1};
    std::vector<double> xs_, ys_;
};

// ---------------------------------------------------------------------------
class HistogramPanel final : public Hist1DPanel {
public:
    explicit HistogramPanel(int id) : Hist1DPanel(Kind::histogram, id) {
        edit_.a = 1;   // click
        edit_.b = 4;   // start
        edit_.binwidth_ps = 10;
        edit_.n_bins = 5000;
    }

protected:
    std::shared_ptr<dt::Measurement> make() override {
        return std::make_shared<dt::Histogram>(edit_.a, edit_.b, static_cast<dt::timestamp_t>(edit_.binwidth_ps), edit_.n_bins);
    }
    const char* a_label() const override { return "Click"; }
    const char* b_label() const override { return "Start"; }
    const char* x_label() const override { return "click - start"; }
    void type_notes() override {
        ui::dim_text("Multiple start, multiple stop (Swabian Histogram): every click is binned against every start in "
                     "the range before it; bins [k·w, (k+1)·w). Click = start pairs each tag with itself in bin 0.");
    }
    void pull(dt::Measurement& m) override {
        auto& h = static_cast<dt::Histogram&>(m);
        const auto d = h.data();
        const auto idx = h.index();
        counts_.assign(d.begin(), d.end());
        left_.assign(idx.begin(), idx.end());
        bw_ = h.binwidth();
        starts_ = h.starts_seen();
        clicks_ = h.clicks_seen();
        buffered_ = h.buffered_starts();
        click_ = h.click_channel();
        start_ = h.start_channel();
    }
    void summary_ui() override {
        std::uint64_t pairs = 0;
        for (auto c : counts_) pairs += c;
        ui::dim_text("Histogram · click ch %d, start ch %d · %s bins of %s · starts %s · clicks %s · pairs in range %s",
                     click_, start_, ui::fmt_count(counts_.size()).c_str(), ui::fmt_time_ps(static_cast<double>(bw_)).c_str(),
                     ui::fmt_count(starts_).c_str(), ui::fmt_count(clicks_).c_str(), ui::fmt_count(pairs).c_str());
        if (starts_ > 0 && clicks_ > 0 && pairs == 0)
            ui::warn_text("Starts and clicks arrive but no click falls within %s after a start: the delay between the "
                          "channels is probably larger (or the click comes first). Widen the range, or set an input "
                          "delay on the start channel (Channels).",
                          ui::fmt_time_ps(static_cast<double>(bw_) * static_cast<double>(counts_.size())).c_str());
        if (buffered_ > 100000)
            ui::warn_text("%s starts lie within one range of each click: every click is binned against all of them, which "
                          "costs CPU. A shorter range or a lower start rate keeps it fast.",
                          ui::fmt_count(buffered_).c_str());
    }
    std::string csv_kind_line() const override {
        return "# Histogram (multi start, multi stop): click ch" + std::to_string(click_) + ", start ch" + std::to_string(start_) +
               ", bin width " + std::to_string(bw_) + " ps, " + std::to_string(counts_.size()) + " bins; starts " +
               std::to_string(starts_) + ", clicks " + std::to_string(clicks_) + "\n";
    }
    void save_settings(KeyValues& kv, const std::string& p) const override { save_common(kv, p); }
    void load_settings(const KeyValues& kv, const std::string& p) override { load_common(kv, p); }

private:
    std::uint64_t starts_{0}, clicks_{0};
    std::size_t buffered_{0};
    int click_{0}, start_{0};
};

// ---------------------------------------------------------------------------
class CorrelationPanel final : public Hist1DPanel {
public:
    explicit CorrelationPanel(int id) : Hist1DPanel(Kind::correlation, id) {
        edit_.a = 1;
        edit_.b = 2;
        edit_.binwidth_ps = 10;
        edit_.n_bins = 2000;
    }

protected:
    std::shared_ptr<dt::Measurement> make() override {
        return std::make_shared<dt::Correlation>(edit_.a, edit_.b, static_cast<dt::timestamp_t>(edit_.binwidth_ps), edit_.n_bins);
    }
    const char* a_label() const override { return "Channel 1"; }
    const char* b_label() const override { return "Channel 2"; }
    const char* x_label() const override { return "t(ch 1) - t(ch 2)"; }
    bool zero_line() const override { return true; }
    double y_scale() const override { return normalized_ ? norm_ : 1.0; }
    const char* y_label() const override { return normalized_ ? "g2 (normalised)" : "counts per bin"; }
    void extra_toolbar() override {
        ImGui::SameLine(0, 18);
        ImGui::Checkbox("normalised", &normalized_);
        ui::help("Swabian Correlation.getDataNormalized: counts x capture duration / (bin width x N1 x N2), so "
                 "uncorrelated channels give 1 (g2).");
    }
    void type_notes() override {
        ui::dim_text("Every pair of a channel-1 and a channel-2 tag, both signs (Swabian Correlation): bins of t1 - t2 "
                     "from -(n·w)/2. The same channel twice is an auto-correlation (each tag with itself at 0).");
    }
    void pull(dt::Measurement& m) override {
        auto& c = static_cast<dt::Correlation&>(m);
        const auto d = c.data();
        const auto idx = c.index();
        counts_.assign(d.begin(), d.end());
        left_.assign(idx.begin(), idx.end());
        bw_ = c.binwidth();
        n1_ = c.n1();
        n2_ = c.n2();
        c1_ = c.channel_1();
        c2_ = c.channel_2();
        const double cap = static_cast<double>(c.capture_duration);
        norm_ = n1_ > 0 && n2_ > 0 && cap > 0 ? cap / (static_cast<double>(bw_) * static_cast<double>(n1_) * static_cast<double>(n2_)) : 0.0;
    }
    void summary_ui() override {
        std::uint64_t pairs = 0;
        for (auto c : counts_) pairs += c;
        ui::dim_text("Correlation · ch %d vs ch %d · %s bins of %s · ch %d tags %s · ch %d tags %s · pairs in range %s", c1_, c2_,
                     ui::fmt_count(counts_.size()).c_str(), ui::fmt_time_ps(static_cast<double>(bw_)).c_str(), c1_,
                     ui::fmt_count(static_cast<std::uint64_t>(n1_)).c_str(), c2_,
                     ui::fmt_count(static_cast<std::uint64_t>(n2_)).c_str(), ui::fmt_count(pairs).c_str());
        if (normalized_ && norm_ == 0) ui::dim_text("normalisation needs tags on both channels.");
        if (n1_ > 0 && n2_ > 0 && pairs == 0)
            ui::warn_text("Both channels have tags but no pair is within ±%s: the delay between them is larger than half the "
                          "range. Widen the range or set an input delay (Channels).",
                          ui::fmt_time_ps(static_cast<double>(bw_) * static_cast<double>(counts_.size()) / 2).c_str());
    }
    std::string csv_kind_line() const override {
        char buf[200];
        std::snprintf(buf, sizeof buf, "# Correlation: t(ch%d) - t(ch%d), bin width %lld ps, %zu bins; N1 %lld, N2 %lld, normalisation %.6g\n",
                      c1_, c2_, static_cast<long long>(bw_), counts_.size(), static_cast<long long>(n1_),
                      static_cast<long long>(n2_), norm_);
        return buf;
    }
    void save_settings(KeyValues& kv, const std::string& p) const override {
        save_common(kv, p);
        kv.emplace_back(p + "normalized", normalized_ ? "1" : "0");
    }
    void load_settings(const KeyValues& kv, const std::string& p) override {
        load_common(kv, p);
        normalized_ = kv_get(kv, p + "normalized") == "1";
    }

private:
    bool normalized_{false};
    double norm_{0};
    std::int64_t n1_{0}, n2_{0};
    int c1_{0}, c2_{0};
};

}  // namespace

std::unique_ptr<Panel> make_histogram_panel(int id) { return std::make_unique<HistogramPanel>(id); }
std::unique_ptr<Panel> make_correlation_panel(int id) { return std::make_unique<CorrelationPanel>(id); }

}  // namespace app
