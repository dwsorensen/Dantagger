// 2D Histogram panel (Swabian HistogramND with two stop channels): one START,
// the first click on each axis' STOP channel after it, shown as a heatmap
// with the TDC Control app's 2D page features (log colour, colour maps,
// hover readout, display merging, the 1D projection at an angle, CSV).

#include "panels.hpp"

#include "tdcctl/histogram.hpp"

#include <implot.h>

#include <algorithm>
#include <cmath>
#include <fstream>

namespace app {

namespace {

constexpr int kMaxCellsPerAxis = 400;            // display cells per axis (bins merged beyond)
constexpr int kMaxBinsPerAxis = 4096;
constexpr std::int64_t kMaxBins = 1024 * 1024;   // in all (the Python library allows more; the app stays responsive)
const char* kMapNames[] = {"viridis", "plasma", "hot", "jet", "greys"};
const ImPlotColormap kMaps[] = {ImPlotColormap_Viridis, ImPlotColormap_Plasma, ImPlotColormap_Hot, ImPlotColormap_Jet,
                                ImPlotColormap_Greys};
constexpr double kPi = 3.14159265358979323846;

class Histogram2DPanel final : public Panel {
public:
    explicit Histogram2DPanel(int id) : Panel(Kind::histogram2d, id) {}

protected:
    struct Settings {
        int start{4}, stop_x{1}, stop_y{2};
        double bw_x{100}, bw_y{100};
        int n_x{400}, n_y{400};
        bool operator==(const Settings& o) const {
            return start == o.start && stop_x == o.stop_x && stop_y == o.stop_y && bw_x == o.bw_x && bw_y == o.bw_y &&
                   n_x == o.n_x && n_y == o.n_y;
        }
    };

    std::shared_ptr<dt::Measurement> make() override {
        if (static_cast<std::int64_t>(edit_.n_x) * edit_.n_y > kMaxBins)
            throw std::invalid_argument("at most " + ui::fmt_count(kMaxBins) + " bins in all (e.g. 1024 x 1024)");
        return std::make_shared<dt::HistogramND>(edit_.start, std::vector<int>{edit_.stop_x, edit_.stop_y},
                                                 std::vector<dt::timestamp_t>{static_cast<dt::timestamp_t>(edit_.bw_x),
                                                                              static_cast<dt::timestamp_t>(edit_.bw_y)},
                                                 std::vector<int>{edit_.n_x, edit_.n_y});
    }
    bool edited_differs() const override { return !(edit_ == applied_); }
    void adopt_edited() override { applied_ = edit_; }
    unsigned channel_mask() const override {
        return (1U << (edit_.start - 1)) | (1U << (edit_.stop_x - 1)) | (1U << (edit_.stop_y - 1));
    }
    float settings_width() const override { return 430.0f; }

    void settings_ui(AppContext&) override {
        const float lw = 100;
        ui::row_label("Start", lw);
        channel_combo("##start", &edit_.start);
        ui::help("As the Swabian HistogramND (used by the tomography scripts): for each START, the first click of the "
                 "x-axis STOP and the first of the y-axis STOP after it give the bin; every axis must be in range, then "
                 "the START is used up. A START that comes before both STOPs replaces the waiting one (counted as "
                 "abandoned). x STOP = y STOP gives the diagonal.");
        ui::row_label("x axis STOP", lw);
        channel_combo("##sx", &edit_.stop_x);
        ImGui::SameLine(0, 14);
        ImGui::TextUnformatted("y axis STOP");
        ImGui::SameLine();
        channel_combo("##sy", &edit_.stop_y);
        ui::row_label("Bin width x", lw);
        ui::time_input("bwx", &edit_.bw_x, 1, 1e12, 95);
        ui::row_label("Bin width y", lw);
        ui::time_input("bwy", &edit_.bw_y, 1, 1e12, 95);
        ui::row_label("Bins", lw);
        ImGui::SetNextItemWidth(95);
        ImGui::InputInt("##nx", &edit_.n_x, 0, 0);
        ImGui::SameLine();
        ImGui::TextUnformatted("×");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(95);
        ImGui::InputInt("##ny", &edit_.n_y, 0, 0);
        edit_.n_x = std::clamp(edit_.n_x, 1, kMaxBinsPerAxis);
        edit_.n_y = std::clamp(edit_.n_y, 1, kMaxBinsPerAxis);
        ui::dim_text("range %s × %s", ui::fmt_time_ps(edit_.bw_x * edit_.n_x).c_str(), ui::fmt_time_ps(edit_.bw_y * edit_.n_y).c_str());
        if (static_cast<std::int64_t>(edit_.n_x) * edit_.n_y > kMaxBins)
            ImGui::TextColored(ui::c_warn(), "%s bins in all: at most %s.",
                               ui::fmt_count(static_cast<std::uint64_t>(edit_.n_x) * static_cast<std::uint64_t>(edit_.n_y)).c_str(),
                               ui::fmt_count(kMaxBins).c_str());
        ImGui::PushTextWrapPos(0);
        ui::dim_text("At most %d bins per axis and %s in all. To move a peak into the window, give the START channel an "
                     "input delay (Channels).", kMaxBinsPerAxis, ui::fmt_count(kMaxBins).c_str());
        ImGui::PopTextWrapPos();
    }

    void pull(dt::Measurement& m) override {
        auto& h = static_cast<dt::HistogramND&>(m);
        data_ = h.data();
        nx_ = h.n_bins()[0];
        ny_ = h.n_bins()[1];
        bwx_ = h.binwidths()[0];
        bwy_ = h.binwidths()[1];
        start_ = h.start_channel();
        sx_ = h.stop_channels()[0];
        sy_ = h.stop_channels()[1];
        stats_ = h.stats();
        build_cells();
    }

    void view_ui(AppContext& ctx) override {
        if (data_.empty()) {
            ui::dim_text("Start to fill the 2D histogram (%d × %d bins).", edit_.n_x, edit_.n_y);
            return;
        }
        const double range_x = static_cast<double>(bwx_) * nx_, range_y = static_cast<double>(bwy_) * ny_;
        const int unit = ui::resolve_unit(unit_, std::max(range_x, range_y));
        const double scale = 1.0 / ui::kUnitPs[unit];

        // ---- numbers ----
        ImGui::PushTextWrapPos(0);
        ui::dim_text("HistogramND · START ch %d, x STOP ch %d, y STOP ch %d · %d × %d bins of %s × %s", start_, sx_, sy_, nx_,
                     ny_, ui::fmt_time_ps(static_cast<double>(bwx_)).c_str(), ui::fmt_time_ps(static_cast<double>(bwy_)).c_str());
        ui::dim_text("starts %s · x stops %s · y stops %s · pairs %s · outside %s · abandoned %s · peak %s at (%s, %s)",
                     ui::fmt_count(stats_.starts).c_str(), ui::fmt_count(stats_.stop_tags[static_cast<std::size_t>(sx_)]).c_str(),
                     ui::fmt_count(stats_.stop_tags[static_cast<std::size_t>(sy_)]).c_str(), ui::fmt_count(total_).c_str(),
                     ui::fmt_count(stats_.outside).c_str(), ui::fmt_count(stats_.abandoned).c_str(),
                     ui::fmt_count(peak_).c_str(), ui::fmt_time_ps(peak_x_).c_str(), ui::fmt_time_ps(peak_y_).c_str());
        ImGui::PopTextWrapPos();
        if (stats_.starts > 0 && stats_.stop_tags[static_cast<std::size_t>(sx_)] > 0 &&
            stats_.stop_tags[static_cast<std::size_t>(sy_)] > 0 && total_ == 0)
            ui::warn_text("STARTs and both STOPs arrive but no pair falls in the window (%s × %s after the START): widen "
                          "the bins or delay the START channel (Channels).",
                          ui::fmt_time_ps(range_x).c_str(), ui::fmt_time_ps(range_y).c_str());

        // ---- toolbar ----
        ImGui::SetNextItemWidth(80);
        ImGui::Combo("unit", &unit_, ui::kUnits, 6);
        ImGui::SameLine(0, 18);
        ImGui::Checkbox("log colour", &log_z_);
        ui::help("Colours by log10(1 + counts), so faint structure shows next to a strong peak.");
        ImGui::SameLine(0, 18);
        ImGui::Checkbox("auto-fit", &autofit_);
        ui::help("Keeps the axes on the full range; switch off to zoom and pan with the mouse.");
        ui::toolbar_next(100 + ImGui::CalcTextSize("colours").x);
        ImGui::SetNextItemWidth(100);
        ImGui::Combo("colours", &colormap_, kMapNames, 5);
        ui::toolbar_next(ImGui::CalcTextSize("Export CSV…").x + 20);
        const std::string path = ui::csv_button("h2" + std::to_string(id()), "Export CSV…", default_csv(ctx, "hist2d"));
        if (!path.empty()) export_csv(ctx, path);
        if (mx_ > 1 || my_ > 1) {
            ui::toolbar_next(300);
            ui::dim_text("display: %d × %d bins per cell (CSV has every bin)", mx_, my_);
        }
        ui::toolbar_next(ImGui::CalcTextSize("1D projection (?)").x + ImGui::GetFrameHeight());
        ImGui::Checkbox("1D projection", &projection_);
        ui::help("Projects the 2D histogram onto the axis at the angle from the x axis and shows it next to the heatmap: "
                 "u = x cos(angle) + y sin(angle). 0° sums over y (onto x, horizontal), 90° sums over x (onto y, "
                 "vertical), -45° gives (x - y) / sqrt 2, the difference of the two STOPs. Each bin's counts are spread "
                 "over its projected footprint, so the curve changes smoothly with the angle. The line on the heatmap is "
                 "the axis.");

        std::string proj_path;
        if (projection_) {
            const float arrow_step = 1.0f;
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("angle");
            ImGui::SameLine();
            ImGui::PushButtonRepeat(true);   // held arrows sweep the angle
            if (ImGui::ArrowButton("##deg_down", ImGuiDir_Left)) proj_deg_ -= arrow_step;
            ImGui::SameLine(0, 4);
            ImGui::SetNextItemWidth(200);
            ImGui::SliderFloat("##deg", &proj_deg_, -90.0f, 90.0f, "%.1f°");
            if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {   // keyboard arrows while over the slider
                if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) proj_deg_ -= arrow_step;
                if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) proj_deg_ += arrow_step;
            }
            ImGui::SameLine(0, 4);
            if (ImGui::ArrowButton("##deg_up", ImGuiDir_Right)) proj_deg_ += arrow_step;
            ui::probe("proj_up_" + std::to_string(id()));
            ImGui::PopButtonRepeat();
            proj_deg_ = std::clamp(proj_deg_, -90.0f, 90.0f);
            ui::help("Drag the slider, or hold the arrows (1° steps; the left/right keys too while the mouse is over the "
                     "slider). Ctrl+click the slider to type an angle. 0° horizontal (onto x), 90° vertical (onto y).");
            ImGui::SameLine(0, 18);
            const double rad = proj_deg_ * (kPi / 180.0);
            ui::dim_text("u = %.3f x %c %.3f y", std::cos(rad), std::sin(rad) < 0 ? '-' : '+', std::fabs(std::sin(rad)));
            ImGui::SameLine(0, 18);
            ImGui::Checkbox("log y##proj", &proj_log_y_);
            ui::toolbar_next(ImGui::CalcTextSize("Export projection CSV…").x + 20);
            proj_path = ui::csv_button("h2p" + std::to_string(id()), "Export projection CSV…", default_csv(ctx, "projection"));
            if (proj_version_ != data_version_ || proj_done_deg_ != proj_deg_) {
                try {
                    std::vector<std::int64_t> l1, l2;
                    std::vector<std::uint64_t> n;
                    full_bins(l1, l2, n);
                    proj_ = tdcctl::project_2d(l1, l2, n, bwx_, bwy_, proj_deg_);
                } catch (const std::exception& bad) {
                    proj_ = tdcctl::Projection{};
                    log(ctx, Level::error, std::string("projection: ") + bad.what());
                }
                proj_version_ = data_version_;
                proj_done_deg_ = proj_deg_;
            }
        }

        // ---- heatmap rows top-down (ImPlot draws row 0 at the top); colour scale 0 .. largest cell ----
        cells_.assign(values_.size(), 0.0);
        double vmax = 0;
        for (int r = 0; r < rows_; ++r)
            for (int c = 0; c < cols_; ++c) {
                const double x = values_[static_cast<std::size_t>(r) * static_cast<std::size_t>(cols_) + static_cast<std::size_t>(c)];
                const double y = log_z_ ? std::log10(1.0 + x) : x;
                cells_[static_cast<std::size_t>(rows_ - 1 - r) * static_cast<std::size_t>(cols_) + static_cast<std::size_t>(c)] = y;
                vmax = std::max(vmax, y);
            }
        vmax = std::max(vmax, log_z_ ? std::log10(2.0) : 1.0);
        const char* zlabel = log_z_ ? "log10(1 + counts)" : (mx_ * my_ > 1 ? "counts per cell" : "counts per bin");
        const ImPlotColormap cmap = kMaps[std::clamp(colormap_, 0, 4)];
        const float bar_w = 90;
        const float plot_h = std::max(220.0f, ImGui::GetContentRegionAvail().y - 4);
        const float gap = ImGui::GetStyle().ItemSpacing.x;
        const float proj_w = projection_ ? std::floor(ImGui::GetContentRegionAvail().x * 0.40f) : 0.0f;
        const float map_w = ImGui::GetContentRegionAvail().x - bar_w - gap - (projection_ ? proj_w + gap : 0.0f);
        const double x1 = static_cast<double>(bwx_) * cols_ * mx_, y1 = static_cast<double>(bwy_) * rows_ * my_;
        ImPlot::PushColormap(cmap);
        if (ImPlot::BeginPlot("##hist2d", ImVec2(map_w, plot_h), ImPlotFlags_NoLegend)) {
            char xlabel[64], ylabel[64];
            std::snprintf(xlabel, sizeof(xlabel), "ch %d - ch %d (%s)", sx_, start_, ui::kUnits[unit]);
            std::snprintf(ylabel, sizeof(ylabel), "ch %d - ch %d (%s)", sy_, start_, ui::kUnits[unit]);
            ImPlot::SetupAxes(xlabel, ylabel);
            const ImPlotCond cond = autofit_ ? ImPlotCond_Always : ImPlotCond_Once;
            ImPlot::SetupAxisLimits(ImAxis_X1, 0, range_x * scale, cond);
            ImPlot::SetupAxisLimits(ImAxis_Y1, 0, range_y * scale, cond);
            if (cols_ > 0 && rows_ > 0) {
                ImPlot::PlotHeatmap("counts", cells_.data(), rows_, cols_, 0, vmax, nullptr, ImPlotPoint(0, 0),
                                    ImPlotPoint(x1 * scale, y1 * scale));
                if (ImPlot::IsPlotHovered()) {   // the cell under the mouse
                    const ImPlotPoint mp = ImPlot::GetPlotMousePos();
                    const double cw = x1 / cols_, ch = y1 / rows_;
                    const int c = static_cast<int>(std::floor(mp.x / scale / cw));
                    const int r = static_cast<int>(std::floor(mp.y / scale / ch));
                    if (c >= 0 && c < cols_ && r >= 0 && r < rows_) {
                        const double n = values_[static_cast<std::size_t>(r) * static_cast<std::size_t>(cols_) + static_cast<std::size_t>(c)];
                        ImGui::SetTooltip("x %s ... %s\ny %s ... %s\n%s counts", ui::fmt_time_ps(c * cw).c_str(),
                                          ui::fmt_time_ps((c + 1) * cw).c_str(), ui::fmt_time_ps(r * ch).c_str(),
                                          ui::fmt_time_ps((r + 1) * ch).c_str(), ui::fmt_count(static_cast<std::uint64_t>(n)).c_str());
                    }
                }
            }
            if (projection_) {   // the projection axis through the middle of the window
                const double rad = proj_deg_ * (kPi / 180.0);
                const double c = std::cos(rad), sn = std::sin(rad);
                const double mxp = range_x / 2, myp = range_y / 2, len = std::hypot(range_x, range_y);
                const double xs[2] = {(mxp - len * c) * scale, (mxp + len * c) * scale};
                const double ys[2] = {(myp - len * sn) * scale, (myp + len * sn) * scale};
                ImPlot::SetNextLineStyle(ImVec4(1, 1, 1, 0.85f), 1.5f);
                ImPlot::PlotLine("##axis", xs, ys, 2);
                const double reach = 0.42 * std::min(std::fabs(c) > 1e-9 ? range_x / std::fabs(c) : 1e300,
                                                     std::fabs(sn) > 1e-9 ? range_y / std::fabs(sn) : 1e300);
                ImPlot::PushStyleColor(ImPlotCol_InlayText, ImVec4(1, 1, 1, 0.95f));
                ImPlot::PlotText("u", (mxp + reach * c) * scale, (myp + reach * sn) * scale, ImVec2(-8, -10));
                ImPlot::PopStyleColor();
            }
            ImPlot::EndPlot();
        }
        ImGui::SameLine();
        ImPlot::ColormapScale(zlabel, 0, vmax, ImVec2(bar_w, plot_h), log_z_ ? "%.2g" : "%g");
        ImPlot::PopColormap();

        if (projection_) {
            ImGui::SameLine();
            if (ImPlot::BeginPlot("##proj", ImVec2(proj_w, plot_h), ImPlotFlags_NoLegend)) {
                char xlabel[64];
                std::snprintf(xlabel, sizeof(xlabel), "u at %.1f° (%s)", proj_deg_, ui::kUnits[unit]);
                ImPlot::SetupAxes(xlabel, "counts per bin");
                if (proj_log_y_) ImPlot::SetupAxisScale(ImAxis_Y1, ImPlotScale_Log10);
                double peak = 0;
                for (double c : proj_.counts) peak = std::max(peak, c);
                const double u1 = proj_.u0_ps + static_cast<double>(proj_.counts.size()) * proj_.bin_ps;
                const ImPlotCond cond = autofit_ ? ImPlotCond_Always : ImPlotCond_Once;
                if (!proj_.counts.empty()) ImPlot::SetupAxisLimits(ImAxis_X1, proj_.u0_ps * scale, u1 * scale, cond);
                ImPlot::SetupAxisLimits(ImAxis_Y1, proj_log_y_ ? 0.8 : 0.0, std::max(1.0, peak * 1.08), cond);
                if (!proj_.counts.empty()) {
                    pxs_.resize(proj_.counts.size() + 1);
                    pys_.resize(proj_.counts.size() + 1);
                    for (std::size_t k = 0; k < proj_.counts.size(); ++k) {
                        pxs_[k] = (proj_.u0_ps + static_cast<double>(k) * proj_.bin_ps) * scale;
                        pys_[k] = proj_.counts[k];
                    }
                    pxs_.back() = u1 * scale;
                    pys_.back() = pys_[pys_.size() - 2];
                    ImPlot::PushStyleColor(ImPlotCol_Line, ui::c_accent());
                    ImPlot::PushStyleColor(ImPlotCol_Fill, ImVec4(ui::c_accent().x, ui::c_accent().y, ui::c_accent().z, 0.28f));
                    ImPlot::PlotStairs("projection", pxs_.data(), pys_.data(), static_cast<int>(pxs_.size()), ImPlotStairsFlags_Shaded);
                    ImPlot::PopStyleColor(2);
                    if (proj_.u0_ps < 0 && u1 > 0) {
                        const double zero = 0.0;
                        ImPlot::PushStyleColor(ImPlotCol_Line, ImVec4(ui::c_dim().x, ui::c_dim().y, ui::c_dim().z, 0.7f));
                        ImPlot::PlotInfLines("##zero", &zero, 1);
                        ImPlot::PopStyleColor();
                    }
                    if (ImPlot::IsPlotHovered()) {
                        const double u = ImPlot::GetPlotMousePos().x / scale;
                        const auto k = static_cast<long>(std::floor((u - proj_.u0_ps) / proj_.bin_ps));
                        if (k >= 0 && k < static_cast<long>(proj_.counts.size()))
                            ImGui::SetTooltip("u %s ... %s\n%.1f counts",
                                              ui::fmt_time_ps(proj_.u0_ps + static_cast<double>(k) * proj_.bin_ps).c_str(),
                                              ui::fmt_time_ps(proj_.u0_ps + static_cast<double>(k + 1) * proj_.bin_ps).c_str(),
                                              proj_.counts[static_cast<std::size_t>(k)]);
                    }
                }
                ImPlot::EndPlot();
            }
        }
        if (!proj_path.empty()) export_projection(ctx, proj_path);
    }

    std::map<std::string, double> metrics() const override {
        return {{"total", static_cast<double>(total_)},           {"peak", static_cast<double>(peak_)},
                {"peak_x_ps", peak_x_ + 0.5 * static_cast<double>(bwx_)}, {"peak_y_ps", peak_y_ + 0.5 * static_cast<double>(bwy_)},
                {"starts", static_cast<double>(stats_.starts)}, {"abandoned", static_cast<double>(stats_.abandoned)},
                {"outside", static_cast<double>(stats_.outside)}, {"proj_deg", proj_deg_},
                {"proj_total", proj_.total}};
    }
    std::string csv_key() const override { return "h2" + std::to_string(id()); }

    void save_settings(KeyValues& kv, const std::string& p) const override {
        kv.emplace_back(p + "start", std::to_string(edit_.start));
        kv.emplace_back(p + "stop_x", std::to_string(edit_.stop_x));
        kv.emplace_back(p + "stop_y", std::to_string(edit_.stop_y));
        kv.emplace_back(p + "bw_x", std::to_string(static_cast<long long>(edit_.bw_x)));
        kv.emplace_back(p + "bw_y", std::to_string(static_cast<long long>(edit_.bw_y)));
        kv.emplace_back(p + "n_x", std::to_string(edit_.n_x));
        kv.emplace_back(p + "n_y", std::to_string(edit_.n_y));
        kv.emplace_back(p + "log_z", log_z_ ? "1" : "0");
        kv.emplace_back(p + "colormap", std::to_string(colormap_));
        kv.emplace_back(p + "projection", projection_ ? "1" : "0");
        kv.emplace_back(p + "proj_deg", std::to_string(proj_deg_));
    }
    void load_settings(const KeyValues& kv, const std::string& p) override {
        edit_.start = std::clamp(static_cast<int>(kv_num(kv, p + "start", 4)), 1, 4);
        edit_.stop_x = std::clamp(static_cast<int>(kv_num(kv, p + "stop_x", 1)), 1, 4);
        edit_.stop_y = std::clamp(static_cast<int>(kv_num(kv, p + "stop_y", 2)), 1, 4);
        edit_.bw_x = std::clamp(kv_num(kv, p + "bw_x", 100), 1.0, 1e12);
        edit_.bw_y = std::clamp(kv_num(kv, p + "bw_y", 100), 1.0, 1e12);
        edit_.n_x = std::clamp(static_cast<int>(kv_num(kv, p + "n_x", 400)), 1, kMaxBinsPerAxis);
        edit_.n_y = std::clamp(static_cast<int>(kv_num(kv, p + "n_y", 400)), 1, kMaxBinsPerAxis);
        log_z_ = kv_get(kv, p + "log_z") == "1";
        colormap_ = std::clamp(static_cast<int>(kv_num(kv, p + "colormap", 0)), 0, 4);
        projection_ = kv_get(kv, p + "projection") == "1";
        proj_deg_ = static_cast<float>(std::clamp(kv_num(kv, p + "proj_deg", 0), -90.0, 90.0));
    }

private:
    // Display cells (merged bins): values_ row-major, row 0 = smallest y.
    void build_cells() {
        mx_ = (nx_ + kMaxCellsPerAxis - 1) / kMaxCellsPerAxis;
        my_ = (ny_ + kMaxCellsPerAxis - 1) / kMaxCellsPerAxis;
        cols_ = (nx_ + mx_ - 1) / mx_;
        rows_ = (ny_ + my_ - 1) / my_;
        values_.assign(static_cast<std::size_t>(cols_) * static_cast<std::size_t>(rows_), 0.0);
        total_ = 0;
        peak_ = 0;
        for (int ix = 0; ix < nx_; ++ix)
            for (int iy = 0; iy < ny_; ++iy) {
                const auto v = static_cast<std::uint64_t>(data_[static_cast<std::size_t>(ix) * static_cast<std::size_t>(ny_) + static_cast<std::size_t>(iy)]);
                if (!v) continue;
                total_ += v;
                if (v > peak_) {
                    peak_ = v;
                    peak_x_ = static_cast<double>(ix) * static_cast<double>(bwx_);
                    peak_y_ = static_cast<double>(iy) * static_cast<double>(bwy_);
                }
                values_[static_cast<std::size_t>(iy / my_) * static_cast<std::size_t>(cols_) + static_cast<std::size_t>(ix / mx_)] += static_cast<double>(v);
            }
    }

    // Every bin: lower-left corner (ps) and count, x fastest (as tdcctl::project_2d takes them).
    void full_bins(std::vector<std::int64_t>& l1, std::vector<std::int64_t>& l2, std::vector<std::uint64_t>& n) const {
        const auto total = static_cast<std::size_t>(nx_) * static_cast<std::size_t>(ny_);
        l1.resize(total);
        l2.resize(total);
        n.resize(total);
        std::size_t k = 0;
        for (int iy = 0; iy < ny_; ++iy)
            for (int ix = 0; ix < nx_; ++ix, ++k) {
                l1[k] = ix * bwx_;
                l2[k] = iy * bwy_;
                n[k] = static_cast<std::uint64_t>(data_[static_cast<std::size_t>(ix) * static_cast<std::size_t>(ny_) + static_cast<std::size_t>(iy)]);
            }
    }

    std::string describe() const {
        char buf[300];
        std::snprintf(buf, sizeof buf,
                      "# HistogramND (first STOP per axis after each START): START ch%d, x STOP ch%d, y STOP ch%d, bin width "
                      "%lld x %lld ps, %d x %d bins; starts %llu, pairs %llu, outside %llu, abandoned %llu\n",
                      start_, sx_, sy_, static_cast<long long>(bwx_), static_cast<long long>(bwy_), nx_, ny_,
                      static_cast<unsigned long long>(stats_.starts), static_cast<unsigned long long>(total_),
                      static_cast<unsigned long long>(stats_.outside), static_cast<unsigned long long>(stats_.abandoned));
        return buf;
    }

    void export_csv(AppContext& ctx, const std::string& path) {
        std::ofstream out(path);
        out << csv_preamble(ctx) << describe() << "x_bin_left_ps,y_bin_left_ps,count\n";
        for (int ix = 0; ix < nx_; ++ix)
            for (int iy = 0; iy < ny_; ++iy)
                out << ix * bwx_ << "," << iy * bwy_ << ","
                    << data_[static_cast<std::size_t>(ix) * static_cast<std::size_t>(ny_) + static_cast<std::size_t>(iy)] << "\n";
        log(ctx, out ? Level::ok : Level::error, (out ? "exported " : "cannot write ") + path);
    }

    void export_projection(AppContext& ctx, const std::string& path) {
        std::ofstream out(path);
        char buf[200];
        std::snprintf(buf, sizeof(buf),
                      "# 1D projection at %.2f deg: u = x cos + y sin (x = x STOP - START, y = y STOP - START), each bin "
                      "spread over its footprint\n",
                      proj_.angle_deg);
        out << csv_preamble(ctx) << describe() << buf << "u_bin_left_ps,counts\n";
        for (std::size_t k = 0; k < proj_.counts.size(); ++k) {
            std::snprintf(buf, sizeof(buf), "%.3f,%.6g\n", proj_.u0_ps + static_cast<double>(k) * proj_.bin_ps, proj_.counts[k]);
            out << buf;
        }
        log(ctx, out ? Level::ok : Level::error, (out ? "exported " : "cannot write ") + path);
    }

    Settings edit_, applied_{0, 0, 0, 0, 0, 0, 0};
    std::vector<std::int32_t> data_;
    int nx_{0}, ny_{0};
    dt::timestamp_t bwx_{1}, bwy_{1};
    int start_{0}, sx_{0}, sy_{0};
    dt::HistogramND::Stats stats_;
    std::uint64_t total_{0}, peak_{0};
    double peak_x_{0}, peak_y_{0};
    int mx_{1}, my_{1}, cols_{0}, rows_{0};
    std::vector<double> values_, cells_;
    // view
    int unit_{0};
    bool log_z_{false};
    bool autofit_{true};
    int colormap_{0};
    bool projection_{false};
    float proj_deg_{0};
    bool proj_log_y_{false};
    tdcctl::Projection proj_;
    std::uint64_t proj_version_{~0ULL};
    float proj_done_deg_{1000};
    std::vector<double> pxs_, pys_;
};

}  // namespace

std::unique_ptr<Panel> make_histogram2d_panel(int id) { return std::make_unique<Histogram2DPanel>(id); }

}  // namespace app
