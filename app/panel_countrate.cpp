// Countrate panel (Swabian Countrate): the average rate of each channel over
// the capture, counts / capture duration, plus a rate history sampled while
// it runs.
//
// Coincidence countrates (user, 2026-11-16): "Add coincidence countrate" adds
// a tracker of the coincidences per second between chosen channels within a
// window (ps), plotted with the channel rates.  Each is a Coincidence virtual
// channel (core/virtual_channels.hpp, Swabian Coincidence) counted by the
// same Countrate measurement; its channels need not be among the shown ones.
// A coincidence is a tag on one of the chosen channels with a tag on every
// other chosen channel less than the window before it (the same time
// included).  The measurement always counts channels 1-4 as well, so each
// coincidence tile can show the accidental rate its inputs' rates imply.

#include "panels.hpp"

#include "tagger.hpp"
#include "virtual_channels.hpp"

#include <implot.h>

#include <algorithm>
#include <cmath>
#include <deque>
#include <fstream>

namespace app {

namespace {

constexpr int kMaxCoinc = 8;                       // per panel (27 virtual channels per board in all)
constexpr int kSeries = dt::kNumChannels + kMaxCoinc;

struct CoincSpec {
    unsigned mask{0x3};       // bit c-1 = channel c; at least two
    double window_ps{1000};   // Swabian's default coincidence window
    bool operator==(const CoincSpec& o) const { return mask == o.mask && window_ps == o.window_ps; }
    bool operator!=(const CoincSpec& o) const { return !(*this == o); }
};

std::vector<int> mask_channels(unsigned mask) {
    std::vector<int> out;
    for (int c = 1; c <= dt::kNumChannels; ++c)
        if ((mask >> (c - 1)) & 1U) out.push_back(c);
    return out;
}

std::string coinc_channels_text(unsigned mask) {   // "1+2+4"
    std::string s;
    for (int c : mask_channels(mask)) s += (s.empty() ? "" : "+") + std::to_string(c);
    return s;
}

// Coincidence colours: none of them a channel colour (blue, green, amber, violet).
ImVec4 coinc_colour(int i) {
    static const ImVec4 k[kMaxCoinc] = {ImVec4(1.00f, 0.42f, 0.62f, 1.0f), ImVec4(0.30f, 0.86f, 0.90f, 1.0f),
                                        ImVec4(0.96f, 0.92f, 0.40f, 1.0f), ImVec4(1.00f, 0.55f, 0.35f, 1.0f),
                                        ImVec4(0.62f, 0.95f, 0.45f, 1.0f), ImVec4(0.95f, 0.60f, 0.95f, 1.0f),
                                        ImVec4(0.65f, 0.78f, 1.00f, 1.0f), ImVec4(0.88f, 0.88f, 0.88f, 1.0f)};
    return k[std::clamp(i, 0, kMaxCoinc - 1)];
}

// Accidental coincidences per second of independent Poisson inputs with the
// given rates (Hz) and window (s): a tag of channel i (rate R_i) makes one
// when every other channel had a tag in the window before it.
double accidental_rate(const std::vector<double>& rates, double window_s) {
    double sum = 0;
    for (std::size_t i = 0; i < rates.size(); ++i) {
        double p = rates[i];
        for (std::size_t j = 0; j < rates.size(); ++j)
            if (j != i) p *= -std::expm1(-rates[j] * window_s);
        sum += p;
    }
    return sum;
}

class CountratePanel final : public Panel {
public:
    explicit CountratePanel(int id) : Panel(Kind::countrate, id) {}
    ~CountratePanel() override { release_virtuals(); }

protected:
    std::shared_ptr<dt::Measurement> make(dt::Tagger& tagger) override {
        if (edit_mask_ == 0 && edit_coinc_.empty()) throw std::invalid_argument("choose a channel or add a coincidence countrate");
        for (std::size_t i = 0; i < edit_coinc_.size(); ++i)
            if (__builtin_popcount(edit_coinc_[i].mask) < 2)
                throw std::invalid_argument("coincidence " + std::to_string(i + 1) + ": choose at least two channels");
        // Channels 1-4 always (the tiles show the chosen ones; the accidental
        // estimates use the inputs' rates), then one virtual channel per coincidence.
        std::vector<int> chans{1, 2, 3, 4};
        std::vector<std::shared_ptr<dt::Coincidence>> made;
        auto& engine = tagger.engine();
        try {
            for (const auto& spec : edit_coinc_) {
                auto v = std::make_shared<dt::Coincidence>(mask_channels(spec.mask),
                                                           static_cast<dt::timestamp_t>(std::llround(spec.window_ps)));
                chans.push_back(engine.add_virtual(v));
                made.push_back(std::move(v));
            }
            auto m = std::make_shared<dt::Countrate>(chans);
            pending_virtuals_ = std::move(made);
            return m;
        } catch (...) {
            for (const auto& v : made) engine.remove_virtual(v.get());
            throw;
        }
    }
    bool edited_differs() const override { return edit_mask_ != mask_ || edit_coinc_ != coinc_; }
    void adopt_edited() override {
        release_virtuals();
        virtuals_ = std::move(pending_virtuals_);
        pending_virtuals_.clear();
        virtual_tagger_ = tagger();
        mask_ = edit_mask_;
        coinc_ = edit_coinc_;
    }
    unsigned channel_mask() const override {
        unsigned m = edit_mask_;
        for (const auto& c : edit_coinc_) m |= c.mask;
        return m;
    }
    float settings_width() const override { return 430.0f; }

    void settings_ui(AppContext&) override {
        ui::row_label("Channels", 100);
        channel_checks("##ch", &edit_mask_, edit_coinc_.empty() ? 1 : 0);
        if (edit_mask_ == 0) {
            ImGui::SameLine();
            ImGui::PushFont(ui::fonts.small);
            ImGui::AlignTextToFramePadding();
            ui::dim_text("(coincidences only)");
            ImGui::PopFont();
        }
        ImGui::PushTextWrapPos(0);
        ui::dim_text("Rate = counts / capture duration over the whole capture (Swabian Countrate.getData). The history "
                     "below samples the rate between two looks at the data (about 10 per second).");
        ImGui::PopTextWrapPos();

        ImGui::Spacing();
        ImGui::SeparatorText("Coincidence countrates");
        int remove = -1;
        for (int i = 0; i < static_cast<int>(edit_coinc_.size()); ++i) {
            auto& spec = edit_coinc_[static_cast<std::size_t>(i)];
            ImGui::PushID(i);
            if (ImGui::Button("x")) remove = i;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Remove this coincidence countrate");
            ImGui::SameLine(0, 6);
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(coinc_colour(i), "C%d", i + 1);
            ImGui::SameLine(0, 8);
            channel_checks("##cch", &spec.mask, 2);
            ImGui::SameLine(0, 10);
            ImGui::AlignTextToFramePadding();
            ui::dim_text("<");
            ImGui::SameLine(0, 4);
            ui::time_input("##win", &spec.window_ps, 1, 1e12, 64);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Coincidence window: the other channels' tags must be less than this before");
            ImGui::PopID();
        }
        if (remove >= 0) {
            edit_coinc_.erase(edit_coinc_.begin() + remove);
            if (edit_coinc_.empty() && edit_mask_ == 0) edit_mask_ = 0xF;
        }
        const bool full = static_cast<int>(edit_coinc_.size()) >= kMaxCoinc;
        if (full) ImGui::BeginDisabled();
        if (ImGui::Button("Add coincidence countrate")) {
            CoincSpec spec;   // channels 1 + 2 within 1 ns, or the last one's settings
            if (!edit_coinc_.empty()) spec = edit_coinc_.back();
            edit_coinc_.push_back(spec);
        }
        ui::probe("addcoinc_" + std::to_string(id()));
        if (full) ImGui::EndDisabled();
        ImGui::PushFont(ui::fonts.small);
        ImGui::PushTextWrapPos(0);
        ui::dim_text("A coincidence: a tag on one of the chosen channels while every other chosen channel had a tag less "
                     "than the window before it (the same time included), as the Swabian Coincidence virtual channel. "
                     "Plotted with the channel rates; its channels need not be shown above. The input delays (Channels) "
                     "apply first, so they can line the channels up.");
        ImGui::PopTextWrapPos();
        ImGui::PopFont();
    }

    void cleared() override {
        history_.clear();
        have_prev_ = false;
    }

    void pull(dt::Measurement& m) override {
        auto& cr = static_cast<dt::Countrate&>(m);
        rates_ = cr.rates();
        totals_ = cr.counts_total();
        series_ = make_series();
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
        for (std::size_t i = 0; i < totals_.size() && i < s.rate.size(); ++i)
            s.rate[i] = static_cast<double>(totals_[i] - prev_totals_[i]) / (t - prev_t_);
        history_.push_back(s);
        while (history_.size() > 200000) history_.pop_front();
        prev_t_ = t;
        prev_totals_ = totals_;
    }

    void view_ui(AppContext& ctx) override {
        if (series_.empty() || rates_.empty()) {
            ui::dim_text("Start to measure. %s", use_file ? "The rates of the file's whole capture." : "");
            return;
        }
        const double cap = capture_s();
        // ---- tiles: as many per row as fit, the big font when there is room ----
        const float avail = ImGui::GetContentRegionAvail().x;
        const int n = static_cast<int>(series_.size());
        const float gap = ImGui::GetStyle().ItemSpacing.x;
        const int per_row = std::max(1, std::min(n, static_cast<int>((avail + gap) / (165.0f + gap))));
        const float tile_w = (avail - gap * static_cast<float>(per_row - 1)) / static_cast<float>(per_row);
        ImFont* number_font = tile_w >= 215.0f ? ui::fonts.big : ui::fonts.heading;
        const auto vis = visible();
        for (int i = 0; i < n; ++i) {
            const Series& se = series_[static_cast<std::size_t>(i)];
            if (i % per_row != 0) ImGui::SameLine();
            ImGui::PushID(i);
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ui::c_panel());
            ImGui::BeginChild("##tile", ImVec2(tile_w, 0),
                              ImGuiChildFlags_Border | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
            const ImVec2 p = ImGui::GetWindowPos();
            ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + 4, p.y + ImGui::GetWindowHeight()),
                                                      ImGui::GetColorU32(se.colour), 8.0f, ImDrawFlags_RoundCornersLeft);
            ImGui::TextColored(se.colour, "%s", se.title.c_str());
            if (se.coinc >= 0) {
                ImGui::SameLine();
                ImGui::PushFont(ui::fonts.small);
                ui::dim_text("< %s", ui::fmt_time_ps(se.window_ps).c_str());
                ImGui::PopFont();
            }
            ImGui::PushFont(number_font);
            const double r = rates_[static_cast<std::size_t>(se.pos)];
            ImGui::TextUnformatted(std::isnan(r) ? "—" : ui::fmt_rate(r).c_str());
            ImGui::PopFont();
            ImGui::PushFont(ui::fonts.small);
            ui::dim_text("%s counts", ui::fmt_count(static_cast<std::uint64_t>(totals_[static_cast<std::size_t>(se.pos)])).c_str());
            if (!history_.empty()) {
                // spread of the sampled rates over the shown history
                double sum = 0, sum2 = 0;
                int k = 0;
                for (const auto* s : vis) {
                    const double x = s->rate[static_cast<std::size_t>(se.pos)];
                    sum += x;
                    sum2 += x * x;
                    ++k;
                }
                const double mean = k ? sum / k : 0;
                const double sd = k > 1 ? std::sqrt(std::max(0.0, (sum2 - k * mean * mean) / (k - 1))) : 0;
                ui::dim_text("now %s · spread %.2g %%", ui::fmt_rate(history_.back().rate[static_cast<std::size_t>(se.pos)]).c_str(),
                             mean > 0 ? 100 * sd / mean : 0.0);
            } else {
                ui::dim_text(" ");
            }
            if (se.coinc >= 0) {
                const double acc = accidental(se.coinc);
                if (std::isnan(acc)) ui::dim_text(" ");
                else ui::dim_text("accidental ≈ %s", ui::fmt_rate(acc).c_str());
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Coincidences per second that channels %s would make by chance, if their tags were\n"
                                      "independent (Poisson) at their measured rates, with this window. Far more\n"
                                      "coincidences than this means the channels are correlated.",
                                      coinc_channels_text(coinc_[static_cast<std::size_t>(se.coinc)].mask).c_str());
            }
            ImGui::PopFont();
            ImGui::EndChild();
            ImGui::PopStyleColor();
            ImGui::PopID();
        }
        // ---- totals and warnings ----
        double sum = 0;
        int shown = 0;
        for (const auto& se : series_)
            if (se.coinc < 0) {
                const double r = rates_[static_cast<std::size_t>(se.pos)];
                sum += std::isnan(r) ? 0 : r;
                ++shown;
            }
        if (shown > 1)
            ui::dim_text("Capture %.3f s · all shown channels %s%s", cap, ui::fmt_rate(sum).c_str(),
                         use_file ? " · from the saved tag file" : "");
        else
            ui::dim_text("Capture %.3f s%s", cap, use_file ? " · from the saved tag file" : "");
        if (running() && cap > 1.0) {
            for (const auto& se : series_) {
                if (se.coinc < 0 && totals_[static_cast<std::size_t>(se.pos)] == 0)
                    ui::warn_text("Channel %d counts nothing: its trigger may be above the signal (or below the "
                                  "baseline with too much hysteresis), or nothing is connected.", se.channel);
                if (se.coinc >= 0 && totals_[static_cast<std::size_t>(se.pos)] == 0) {
                    const auto& spec = coinc_[static_cast<std::size_t>(se.coinc)];
                    bool inputs = true;
                    for (int c : mask_channels(spec.mask)) inputs = inputs && totals_[static_cast<std::size_t>(c - 1)] > 0;
                    if (inputs)
                        ui::warn_text("Coincidence %s counts nothing although its channels count: their tags may be "
                                      "further apart than %s. A Correlation panel shows the offset between two "
                                      "channels; their input delays (Channels) can line them up.",
                                      coinc_channels_text(spec.mask).c_str(), ui::fmt_time_ps(spec.window_ps).c_str());
                }
            }
        }
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
        bool hardware = false, coincidences = false;
        for (const auto& se : series_) (se.coinc < 0 ? hardware : coincidences) = true;
        if (hardware && coincidences) {
            ui::toolbar_next(ImGui::CalcTextSize("coincidence axis (?)").x + ImGui::GetFrameHeight() + 18);
            ImGui::Checkbox("coincidence axis", &own_axis_);
            ui::help("Coincidences on their own axis (right), so a few per second show next to channels at MHz. "
                     "Off: every rate on one axis.");
        }
        const bool two_axes = hardware && coincidences && own_axis_;
        ui::toolbar_next(ImGui::CalcTextSize("Export CSV…").x + 20);
        const std::string path = ui::csv_button("cr" + std::to_string(id()), "Export CSV…", default_csv(ctx, "countrate"));
        if (!path.empty()) export_csv(ctx, path);
        span_seconds_ = span_s[span_];

        auto value = [&](const Sample* s, const Series& se) {
            const double x = s->rate[static_cast<std::size_t>(se.pos)];
            return log_y_ ? std::max(x, 0.5) : x;
        };
        if (ImPlot::BeginPlot("##crh", ImVec2(-1, std::max(180.0f, ImGui::GetContentRegionAvail().y - 4)))) {
            const ImPlotAxisFlags af = autofit_ ? ImPlotAxisFlags_AutoFit : ImPlotAxisFlags_None;
            ImPlot::SetupAxes("capture time (s)", two_axes ? "channel rate (1/s)" : "rate (1/s)", af, ImPlotAxisFlags_None);
            if (two_axes) ImPlot::SetupAxis(ImAxis_Y2, "coincidences (1/s)", ImPlotAxisFlags_AuxDefault);
            if (log_y_) ImPlot::SetupAxisScale(ImAxis_Y1, ImPlotScale_Log10);
            if (log_y_ && two_axes) ImPlot::SetupAxisScale(ImAxis_Y2, ImPlotScale_Log10);
            for (int axis : {0, 1}) {   // y: the shown rates with a margin (axis 1: the coincidences on the right)
                if (axis == 1 && !two_axes) break;
                std::vector<double> all;
                for (const auto& se : series_)
                    if (!two_axes || (se.coinc >= 0) == (axis == 1))
                        for (const auto* s : vis) all.push_back(value(s, se));
                ui::setup_y_padded(all.data(), all.size(), log_y_, autofit_, axis == 1 ? ImAxis_Y2 : ImAxis_Y1);
            }
            ImPlot::SetupLegend(ImPlotLocation_North, ImPlotLegendFlags_Outside | ImPlotLegendFlags_Horizontal);
            std::vector<double> xs, ys;
            for (const auto& se : series_) {
                xs.clear();
                ys.clear();
                for (const auto* s : vis) {
                    xs.push_back(s->t);
                    ys.push_back(value(s, se));
                }
                ImPlot::SetAxes(ImAxis_X1, two_axes && se.coinc >= 0 ? ImAxis_Y2 : ImAxis_Y1);
                ImPlot::SetNextLineStyle(se.colour, se.coinc >= 0 ? 2.2f : 1.8f);
                if (!xs.empty()) ImPlot::PlotLine(se.legend.c_str(), xs.data(), ys.data(), static_cast<int>(xs.size()));
            }
            if (ImPlot::IsPlotHovered() && !vis.empty()) {   // the sample under the mouse
                const double mx = ImPlot::GetPlotMousePos().x;
                const auto it = std::min_element(vis.begin(), vis.end(), [&](const Sample* a, const Sample* b) {
                    return std::fabs(a->t - mx) < std::fabs(b->t - mx);
                });
                std::string tip;
                char line[128];
                std::snprintf(line, sizeof line, "t = %.3f s", (*it)->t);
                tip += line;
                for (const auto& se : series_) {
                    std::snprintf(line, sizeof line, "\n%-14s %s", se.legend.c_str(),
                                  ui::fmt_rate((*it)->rate[static_cast<std::size_t>(se.pos)]).c_str());
                    tip += line;
                }
                ImGui::SetTooltip("%s", tip.c_str());
            }
            ImPlot::EndPlot();
        }
    }

    std::map<std::string, double> metrics() const override {
        std::map<std::string, double> m;
        for (const auto& se : series_) {
            const double rate = rates_[static_cast<std::size_t>(se.pos)];
            const double counts = static_cast<double>(totals_[static_cast<std::size_t>(se.pos)]);
            if (se.coinc < 0) {
                m["rate" + std::to_string(se.channel)] = rate;
                m["counts" + std::to_string(se.channel)] = counts;
            } else {
                const std::string k = std::to_string(se.coinc + 1);
                m["coinc_rate" + k] = rate;
                m["coinc_counts" + k] = counts;
                m["coinc_accidental" + k] = accidental(se.coinc);
                m["coinc_channel" + k] = se.channel;
            }
        }
        m["coincidences"] = static_cast<double>(coinc_.size());
        m["edit_coincidences"] = static_cast<double>(edit_coinc_.size());
        m["history"] = static_cast<double>(history_.size());
        return m;
    }
    std::string csv_key() const override { return "cr" + std::to_string(id()); }

    void save_settings(KeyValues& kv, const std::string& p) const override {
        kv.emplace_back(p + "channels", std::to_string(edit_mask_));
        kv.emplace_back(p + "coinc", std::to_string(edit_coinc_.size()));
        kv.emplace_back(p + "coinc_axis", own_axis_ ? "1" : "0");
        for (std::size_t i = 0; i < edit_coinc_.size(); ++i) {
            const std::string k = p + "coinc." + std::to_string(i) + ".";
            kv.emplace_back(k + "channels", std::to_string(edit_coinc_[i].mask));
            kv.emplace_back(k + "window_ps", std::to_string(std::llround(edit_coinc_[i].window_ps)));
        }
    }
    void load_settings(const KeyValues& kv, const std::string& p) override {
        edit_coinc_.clear();
        own_axis_ = kv_get(kv, p + "coinc_axis") != "0";
        const int n = std::clamp(static_cast<int>(kv_num(kv, p + "coinc", 0)), 0, kMaxCoinc);
        for (int i = 0; i < n; ++i) {
            const std::string k = p + "coinc." + std::to_string(i) + ".";
            CoincSpec spec;
            spec.mask = static_cast<unsigned>(kv_num(kv, k + "channels", 0x3)) & 0xFU;
            if (__builtin_popcount(spec.mask) < 2) spec.mask = 0x3;
            spec.window_ps = std::clamp(std::round(kv_num(kv, k + "window_ps", 1000)), 1.0, 1e12);
            edit_coinc_.push_back(spec);
        }
        edit_mask_ = static_cast<unsigned>(kv_num(kv, p + "channels", 0xF)) & 0xFU;
        if (!edit_mask_ && edit_coinc_.empty()) edit_mask_ = 0xF;
    }

private:
    struct Sample {
        double t{0};
        std::array<double, kSeries> rate{};   // by position in the measurement: inputs 1-4, then the coincidences
    };
    struct Series {
        int pos{0};             // in the measurement's channel list
        int channel{0};         // input 1-4, or the virtual channel number
        int coinc{-1};          // index into coinc_ (coincidences)
        double window_ps{0};
        std::string title, legend;
        ImVec4 colour;
    };

    std::vector<Series> make_series() const {
        std::vector<Series> out;
        for (int c = 1; c <= dt::kNumChannels; ++c) {
            if (!((mask_ >> (c - 1)) & 1U)) continue;
            Series s;
            s.pos = c - 1;
            s.channel = c;
            s.title = "Channel " + std::to_string(c);
            s.legend = "ch " + std::to_string(c);
            s.colour = ui::channel_colour(c);
            out.push_back(s);
        }
        for (std::size_t i = 0; i < coinc_.size() && i < virtuals_.size(); ++i) {
            Series s;
            s.pos = dt::kNumChannels + static_cast<int>(i);
            s.channel = virtuals_[i]->channel();
            s.coinc = static_cast<int>(i);
            s.window_ps = coinc_[i].window_ps;
            s.title = "Coincidence " + coinc_channels_text(coinc_[i].mask);
            s.legend = coinc_channels_text(coinc_[i].mask) + " < " + ui::fmt_time_ps(coinc_[i].window_ps);
            s.colour = coinc_colour(static_cast<int>(i));
            out.push_back(s);
        }
        return out;
    }

    // Accidental coincidences/s of coincidence i from its inputs' average rates (NaN before data).
    double accidental(int i) const {
        if (i < 0 || static_cast<std::size_t>(i) >= coinc_.size() || rates_.size() < static_cast<std::size_t>(dt::kNumChannels))
            return std::nan("");
        std::vector<double> r;
        for (int c : mask_channels(coinc_[static_cast<std::size_t>(i)].mask)) {
            const double x = rates_[static_cast<std::size_t>(c - 1)];
            if (std::isnan(x)) return std::nan("");
            r.push_back(x);
        }
        return accidental_rate(r, coinc_[static_cast<std::size_t>(i)].window_ps * 1e-12);
    }

    void release_virtuals() {
        if (virtual_tagger_ && !virtual_tagger_->closed())
            for (const auto& v : virtuals_) virtual_tagger_->engine().remove_virtual(v.get());
        virtuals_.clear();
        virtual_tagger_.reset();
    }

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
        for (const auto& se : series_)
            out << " " << (se.coinc < 0 ? "ch" + std::to_string(se.channel) : "coincidence " + coinc_channels_text(coinc_[static_cast<std::size_t>(se.coinc)].mask))
                << " " << rates_[static_cast<std::size_t>(se.pos)] << " /s (" << totals_[static_cast<std::size_t>(se.pos)] << " counts);";
        out << "\n";
        for (const auto& se : series_) {
            if (se.coinc < 0) continue;
            const auto& spec = coinc_[static_cast<std::size_t>(se.coinc)];
            out << "# coincidence " << se.coinc + 1 << ": channels " << coinc_channels_text(spec.mask) << ", window "
                << std::llround(spec.window_ps) << " ps (a tag on one channel with a tag on every other less than the "
                << "window before it, Swabian Coincidence); accidental estimate " << accidental(se.coinc) << " /s\n";
        }
        out << "# history: rate between consecutive samples\ncapture_s";
        for (const auto& se : series_) {
            if (se.coinc < 0) out << ",rate_ch" << se.channel << "_hz";
            else out << ",rate_coinc" << se.coinc + 1 << "_ch" << coinc_channels_text(coinc_[static_cast<std::size_t>(se.coinc)].mask) << "_hz";
        }
        out << "\n";
        for (const auto& s : history_) {
            out << s.t;
            for (const auto& se : series_) out << "," << s.rate[static_cast<std::size_t>(se.pos)];
            out << "\n";
        }
        log(ctx, out ? Level::ok : Level::error, (out ? "exported " : "cannot write ") + path);
    }

    unsigned edit_mask_{0xF}, mask_{0};
    std::vector<CoincSpec> edit_coinc_, coinc_;
    std::vector<std::shared_ptr<dt::Coincidence>> virtuals_, pending_virtuals_;   // coinc_[i] <-> virtuals_[i]
    std::shared_ptr<dt::Tagger> virtual_tagger_;
    std::vector<Series> series_;
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
    bool own_axis_{true};   // coincidences on the right-hand axis (when channels are shown too)
};

}  // namespace

std::unique_ptr<Panel> make_countrate_panel(int id) { return std::make_unique<CountratePanel>(id); }

}  // namespace app
