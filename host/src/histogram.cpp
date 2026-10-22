#include "tdcctl/histogram.hpp"

#include <algorithm>
#include <cmath>

namespace tdcctl {

HistogramAccumulator::HistogramAccumulator(const HistogramSettings& settings, std::int64_t centre_shift_ps)
    : s_(settings), centre_(centre_shift_ps) {
    if (s_.start_channel < 0 || s_.start_channel >= kChannels || s_.stop_channel < 0 || s_.stop_channel >= kChannels)
        throw ArgumentError("channels must be 1..4");
    if (s_.binwidth_ps < 1 || s_.binwidth_ps > 1'000'000'000'000LL) throw ArgumentError("bin width must be 1 ps ... 1 s");
    if (s_.n_bins < 1 || s_.n_bins > kMaxBins) throw ArgumentError("number of bins must be 1 ... 1,000,000");
    if (s_.binwidth_ps > 1'000'000'000'000'000'000LL / s_.n_bins) throw ArgumentError("histogram range too large");
    if (centre_ < -2'000'000'000'000LL || centre_ > 2'000'000'000'000LL) throw ArgumentError("START/STOP delays must be within +-1 s");
    range_ = s_.binwidth_ps * s_.n_bins;
    base_ = -centre_ - range_;
    top_ = base_ + 3 * range_;
    raw_.assign(static_cast<std::size_t>(3 * s_.n_bins), 0);
}

bool HistogramAccumulator::covers(std::int64_t shift_ps, std::int64_t origin_ps) const {
    const std::int64_t s = shift_ps - origin_ps;
    return s >= centre_ - range_ && s <= centre_ + range_;
}

void HistogramAccumulator::begin_segment() {
    starts_.clear();
    stops_.clear();
    have_last_ = false;
    any_ = false;
    last_t_ = 0;
    ++t_.segments;
}

void HistogramAccumulator::clear() {
    std::fill(raw_.begin(), raw_.end(), 0);
    const auto segments = t_.segments;
    t_ = Totals{};
    t_.segments = segments;
}

void HistogramAccumulator::bin(std::int64_t raw) {
    ++raw_[static_cast<std::size_t>((raw - base_) / s_.binwidth_ps)];
    ++t_.pairs;
}

void HistogramAccumulator::add(int channel, std::int64_t t) {
    const bool is_stop = channel == s_.stop_channel;
    const bool is_start = channel == s_.start_channel;
    if (!is_stop && !is_start) return;
    if (any_ && t < last_t_) ++t_.late;
    last_t_ = any_ ? std::max(last_t_, t) : t;
    any_ = true;
    const bool same = s_.start_channel == s_.stop_channel;

    if (s_.mode == HistogramMode::start_stop) {
        if (is_stop) {
            ++t_.stops;
            if (have_last_) {
                const std::int64_t raw = t - last_start_;
                if (raw >= base_ && raw < top_) bin(raw);
                else ++t_.outside;
                have_last_ = false;
            }
        }
        if (is_start) {
            ++t_.starts;
            last_start_ = t;
            have_last_ = true;
        }
        return;
    }

    // Histogram (multiple start, multiple stop).  Each pair is binned when
    // its later click arrives; a click acts as STOP before it becomes a
    // START, so on one channel it never pairs with itself.
    if (is_stop) {
        ++t_.stops;
        // raw = t - s >= 0 against earlier STARTs: s in (t - top, t - max(0, base)].
        if (top_ > 0) {
            while (!starts_.empty() && t - starts_.front() >= top_) starts_.pop_front();
            const std::int64_t lo = std::max<std::int64_t>(0, base_);
            for (auto it = starts_.rbegin(); it != starts_.rend(); ++it) {
                const std::int64_t raw = t - *it;
                if (raw < lo) continue;
                bin(raw);
            }
        }
    }
    if (is_start) {
        ++t_.starts;
        // raw = p - t < 0 against earlier STOPs: p in [t + base, t) (equal times
        // too on one channel: two clicks at the same time pair both ways).
        if (base_ < 0) {
            while (!stops_.empty() && stops_.front() - t < base_) stops_.pop_front();
            for (auto it = stops_.rbegin(); it != stops_.rend(); ++it) {
                const std::int64_t raw = *it - t;
                if (raw > 0 || (raw == 0 && !same)) continue;
                if (raw >= top_) continue;
                bin(raw);
            }
        }
        if (top_ > 0) {
            starts_.push_back(t);
            if (starts_.size() > kMaxPairing) {
                starts_.pop_front();
                t_.saturated = true;
            }
        }
    }
    if (is_stop && base_ < 0) {
        stops_.push_back(t);
        if (stops_.size() > kMaxPairing) {
            stops_.pop_front();
            t_.saturated = true;
        }
    }
}

void HistogramAccumulator::shown_bins(std::int64_t shift, std::vector<std::int64_t>& left,
                                      std::vector<std::uint64_t>& counts, std::int64_t origin) const {
    left.clear();
    counts.clear();
    for (std::size_t j = 0; j < raw_.size(); ++j) {
        const std::int64_t x = base_ + static_cast<std::int64_t>(j) * s_.binwidth_ps + shift;
        if (x + s_.binwidth_ps <= origin || x >= origin + range_) continue;
        left.push_back(x);
        counts.push_back(raw_[j]);
    }
}

HistogramAccumulator::Shown HistogramAccumulator::shown(std::int64_t shift, std::size_t max_points,
                                                       std::int64_t origin) const {
    std::vector<std::int64_t> left;
    std::vector<std::uint64_t> counts;
    shown_bins(shift, left, counts, origin);
    Shown out;
    const std::size_t n = counts.size();
    if (n == 0) return out;
    const std::size_t k = std::max<std::size_t>(1, (n + max_points - 1) / std::max<std::size_t>(1, max_points));
    out.merged = static_cast<int>(k);
    for (std::size_t i = 0; i < n; ++i) {
        out.total += counts[i];
        if (counts[i] > out.peak) {
            out.peak = counts[i];
            out.peak_x_ps = static_cast<double>(left[i]);
        }
    }
    out.xs_ps.reserve(n / k + 2);
    out.ys.reserve(n / k + 2);
    for (std::size_t i = 0; i < n; i += k) {
        std::uint64_t sum = 0;
        for (std::size_t j = i; j < std::min(n, i + k); ++j) sum += counts[j];
        out.xs_ps.push_back(static_cast<double>(left[i]));
        out.ys.push_back(static_cast<double>(sum));
    }
    out.xs_ps.push_back(static_cast<double>(left.back() + s_.binwidth_ps));
    out.ys.push_back(out.ys.back());
    return out;
}

}  // namespace tdcctl

namespace tdcctl {

// ---- Histogram2DAccumulator ----------------------------------------------------------
Histogram2DAccumulator::Histogram2DAccumulator(const Histogram2DSettings& settings, std::int64_t centre1_ps,
                                               std::int64_t centre2_ps)
    : s_(settings), centre1_(centre1_ps), centre2_(centre2_ps) {
    for (int c : {s_.start_channel, s_.stop1_channel, s_.stop2_channel})
        if (c < 0 || c >= kChannels) throw ArgumentError("channels must be 1..4");
    for (auto w : {s_.binwidth1_ps, s_.binwidth2_ps})
        if (w < 1 || w > 1'000'000'000'000LL) throw ArgumentError("bin width must be 1 ps ... 1 s");
    for (int n : {s_.n_bins1, s_.n_bins2})
        if (n < 1 || n > kMaxBinsPerAxis) throw ArgumentError("number of bins per axis must be 1 ... 4096");
    if (static_cast<std::int64_t>(s_.n_bins1) * s_.n_bins2 > kMaxCells)
        throw ArgumentError("at most 262,144 bins in all (e.g. 512 x 512)");
    if (s_.binwidth1_ps > 1'000'000'000'000'000LL / s_.n_bins1 || s_.binwidth2_ps > 1'000'000'000'000'000LL / s_.n_bins2)
        throw ArgumentError("histogram range too large");
    for (auto c : {centre1_, centre2_})
        if (c < -2'000'000'000'000LL || c > 2'000'000'000'000LL) throw ArgumentError("START/STOP delays must be within +-1 s");
    range1_ = s_.binwidth1_ps * s_.n_bins1;
    range2_ = s_.binwidth2_ps * s_.n_bins2;
    base1_ = -centre1_ - range1_;
    base2_ = -centre2_ - range2_;
    raw_.assign(static_cast<std::size_t>(9) * static_cast<std::size_t>(s_.n_bins1) * static_cast<std::size_t>(s_.n_bins2), 0);
}

bool Histogram2DAccumulator::covers(std::int64_t shift1, std::int64_t shift2) const {
    return shift1 >= centre1_ - range1_ && shift1 <= centre1_ + range1_ && shift2 >= centre2_ - range2_ &&
           shift2 <= centre2_ + range2_;
}

void Histogram2DAccumulator::begin_segment() {
    have_start_ = false;
    have1_ = have2_ = false;
    any_ = false;
    last_t_ = 0;
    ++t_.segments;
}

void Histogram2DAccumulator::clear() {
    std::fill(raw_.begin(), raw_.end(), 0);
    const auto segments = t_.segments;
    t_ = Totals{};
    t_.segments = segments;
}

void Histogram2DAccumulator::add(int channel, std::int64_t t) {
    const bool is_start = channel == s_.start_channel;
    const bool is_stop1 = channel == s_.stop1_channel;
    const bool is_stop2 = channel == s_.stop2_channel;
    if (!is_start && !is_stop1 && !is_stop2) return;
    if (any_ && t < last_t_) ++t_.late;
    last_t_ = any_ ? std::max(last_t_, t) : t;
    any_ = true;

    // STOP roles first: a click on the START channel never pairs with itself.
    if (is_stop1) {
        ++t_.stops1;
        if (have_start_ && !have1_) {
            d1_ = t - start_t_;
            have1_ = true;
        }
    }
    if (is_stop2) {
        ++t_.stops2;
        if (have_start_ && !have2_) {
            d2_ = t - start_t_;
            have2_ = true;
        }
    }
    if (have_start_ && have1_ && have2_) {
        const std::int64_t n1 = 3 * s_.n_bins1;
        if (d1_ >= base1_ && d1_ < base1_ + 3 * range1_ && d2_ >= base2_ && d2_ < base2_ + 3 * range2_) {
            const std::int64_t i = (d1_ - base1_) / s_.binwidth1_ps;
            const std::int64_t j = (d2_ - base2_) / s_.binwidth2_ps;
            ++raw_[static_cast<std::size_t>(j * n1 + i)];
            ++t_.pairs;
        } else {
            ++t_.outside;
        }
        have_start_ = false;
    }
    if (is_start) {
        ++t_.starts;
        if (have_start_) ++t_.abandoned;
        have_start_ = true;
        start_t_ = t;
        have1_ = have2_ = false;
    }
}

void Histogram2DAccumulator::window(std::int64_t shift, std::int64_t base, std::int64_t width, int n, int& first,
                                    int& count) const {
    const std::int64_t range = width * n;
    first = -1;
    count = 0;
    for (int j = 0; j < 3 * n; ++j) {
        const std::int64_t x = base + static_cast<std::int64_t>(j) * width + shift;
        if (x + width <= 0 || x >= range) continue;
        if (first < 0) first = j;
        ++count;
    }
}

void Histogram2DAccumulator::shown_bins(std::int64_t shift1, std::int64_t shift2, std::vector<std::int64_t>& left1,
                                        std::vector<std::int64_t>& left2, std::vector<std::uint64_t>& counts) const {
    left1.clear();
    left2.clear();
    counts.clear();
    int f1 = 0, c1 = 0, f2 = 0, c2 = 0;
    window(shift1, base1_, s_.binwidth1_ps, s_.n_bins1, f1, c1);
    window(shift2, base2_, s_.binwidth2_ps, s_.n_bins2, f2, c2);
    const std::size_t n1 = static_cast<std::size_t>(3 * s_.n_bins1);
    for (int j = f2; j < f2 + c2; ++j) {
        for (int i = f1; i < f1 + c1; ++i) {
            left1.push_back(base1_ + static_cast<std::int64_t>(i) * s_.binwidth1_ps + shift1);
            left2.push_back(base2_ + static_cast<std::int64_t>(j) * s_.binwidth2_ps + shift2);
            counts.push_back(raw_[static_cast<std::size_t>(j) * n1 + static_cast<std::size_t>(i)]);
        }
    }
}

Histogram2DAccumulator::Shown Histogram2DAccumulator::shown(std::int64_t shift1, std::int64_t shift2, int max_per_axis) const {
    Shown out;
    int f1 = 0, c1 = 0, f2 = 0, c2 = 0;
    window(shift1, base1_, s_.binwidth1_ps, s_.n_bins1, f1, c1);
    window(shift2, base2_, s_.binwidth2_ps, s_.n_bins2, f2, c2);
    if (c1 == 0 || c2 == 0) return out;
    const int most = std::max(1, max_per_axis);
    out.merged1 = (c1 + most - 1) / most;
    out.merged2 = (c2 + most - 1) / most;
    out.cols = (c1 + out.merged1 - 1) / out.merged1;
    out.rows = (c2 + out.merged2 - 1) / out.merged2;
    out.values.assign(static_cast<std::size_t>(out.cols) * static_cast<std::size_t>(out.rows), 0.0);
    out.x0_ps = static_cast<double>(base1_ + static_cast<std::int64_t>(f1) * s_.binwidth1_ps + shift1);
    out.y0_ps = static_cast<double>(base2_ + static_cast<std::int64_t>(f2) * s_.binwidth2_ps + shift2);
    out.x1_ps = out.x0_ps + static_cast<double>(out.cols) * out.merged1 * static_cast<double>(s_.binwidth1_ps);
    out.y1_ps = out.y0_ps + static_cast<double>(out.rows) * out.merged2 * static_cast<double>(s_.binwidth2_ps);
    const std::size_t n1 = static_cast<std::size_t>(3 * s_.n_bins1);
    for (int j = 0; j < c2; ++j) {
        for (int i = 0; i < c1; ++i) {
            const std::uint64_t v = raw_[static_cast<std::size_t>(f2 + j) * n1 + static_cast<std::size_t>(f1 + i)];
            if (v == 0) continue;
            out.total += v;
            out.values[static_cast<std::size_t>(j / out.merged2) * static_cast<std::size_t>(out.cols) +
                       static_cast<std::size_t>(i / out.merged1)] += static_cast<double>(v);
            if (v > out.peak) {
                out.peak = v;
                out.peak_x_ps = out.x0_ps + static_cast<double>(i) * static_cast<double>(s_.binwidth1_ps);
                out.peak_y_ps = out.y0_ps + static_cast<double>(j) * static_cast<double>(s_.binwidth2_ps);
            }
        }
    }
    return out;
}

// ---- project_2d ------------------------------------------------------------------------
namespace {

// CDF of U(0, p) + U(0, q), p >= q >= 0: the share of a projected bin below t.
double footprint_cdf(double t, double p, double q) {
    if (t <= 0) return 0;
    if (t >= p + q) return 1;
    if (q <= 0) return t / p;
    if (t <= q) return t * t / (2 * p * q);
    if (t <= p) return (2 * t - q) / (2 * p);
    const double r = p + q - t;
    return 1 - r * r / (2 * p * q);
}

}  // namespace

Projection project_2d(const std::vector<std::int64_t>& left1, const std::vector<std::int64_t>& left2,
                      const std::vector<std::uint64_t>& counts, std::int64_t w1, std::int64_t w2, double angle_deg) {
    if (!(angle_deg >= -90 && angle_deg <= 90)) throw ArgumentError("projection angle must be -90 ... 90 degrees");
    if (w1 < 1 || w2 < 1) throw ArgumentError("bin widths must be at least 1 ps");
    if (left1.size() != counts.size() || left2.size() != counts.size()) throw ArgumentError("bins and counts differ in length");
    Projection out;
    out.angle_deg = angle_deg;
    const double rad = angle_deg * (3.14159265358979323846 / 180.0);
    double c = std::cos(rad), s = std::sin(rad);
    if (std::fabs(c) < 1e-12) c = 0;   // exact at 0 and +-90
    if (std::fabs(s) < 1e-12) s = 0;
    const double a = static_cast<double>(w1) * c;                  // c >= 0 on -90 ... 90
    const double b = static_cast<double>(w2) * std::fabs(s);
    const double p = std::max(a, b), q = std::min(a, b);
    const double len = a + b;
    out.bin_ps = p;
    if (counts.empty()) return out;
    // Lower end of each bin's footprint: its lowest corner.
    auto lower = [&](std::size_t i) {
        return static_cast<double>(left1[i]) * c + static_cast<double>(left2[i]) * s + std::min(0.0, static_cast<double>(w2) * s);
    };
    double lo = lower(0), hi = lo;
    for (std::size_t i = 0; i < counts.size(); ++i) {
        const double l = lower(i);
        lo = std::min(lo, l);
        hi = std::max(hi, l + len);
    }
    out.u0_ps = lo;
    const auto n = static_cast<std::size_t>(std::max(1.0, std::ceil((hi - lo) / p - 1e-9)));
    out.counts.assign(n, 0.0);
    for (std::size_t i = 0; i < counts.size(); ++i) {
        if (counts[i] == 0) continue;
        const double v = static_cast<double>(counts[i]);
        const double t0 = lower(i) - lo;                         // footprint [t0, t0 + len) from u0
        const auto k0 = static_cast<std::size_t>(std::max(0.0, std::floor(t0 / p)));
        double below = 0;
        for (std::size_t k = k0; k < n; ++k) {
            const double edge = static_cast<double>(k + 1) * p - t0;
            const double f = k + 1 == n ? 1.0 : footprint_cdf(edge, p, q);
            out.counts[k] += v * (f - below);
            below = f;
            if (f >= 1) break;
        }
        out.total += v;
    }
    return out;
}

}  // namespace tdcctl
