#pragma once

// Incremental START/STOP histogram (Swabian Time Tagger semantics, see
// run_file.hpp) for live acquisition and for files.  Events are added in time
// order; runs are separate segments (their times are unrelated) whose counts
// add up until clear().
//
// Raw delays d = STOP - START are binned over [base, base + 3R), R = bin width
// x bins, base = -centre - R.  The histogram shown for a shift (STOP delay -
// START delay) covers d + shift in [0, R), so any shift within centre +- R is
// shown instantly (and exactly: the bins keep their width, their edges move
// with the shift).  Further shifts need a new accumulator centred there.
//
// Bidirectional display: the shown window starts at an origin instead of 0,
// [origin, origin + R) with origin = -(bins / 2) x bin width, so pairs whose
// STOP comes before their START land in negative bins.  Showing [origin,
// origin + R) at `shift` is the same as showing [0, R) at `shift - origin`, so
// the toggle needs no new accumulator (Histogram mode bins both directions;
// StartStop pairs a STOP only with an earlier START and has no negative side).

#include "tdcctl/run_file.hpp"

#include <cstdint>
#include <deque>
#include <vector>

namespace tdcctl {

struct HistogramSettings {
    HistogramMode mode{HistogramMode::histogram};
    int start_channel{0};               // 0..3
    int stop_channel{1};
    std::int64_t binwidth_ps{1000};
    int n_bins{1000};
    bool operator==(const HistogramSettings& o) const {
        return mode == o.mode && start_channel == o.start_channel && stop_channel == o.stop_channel &&
               binwidth_ps == o.binwidth_ps && n_bins == o.n_bins;
    }
    bool operator!=(const HistogramSettings& o) const { return !(*this == o); }
};

// The left edge of the shown window (0, or the bidirectional origin above).
inline std::int64_t histogram_origin(const HistogramSettings& s, bool bidirectional) {
    if (!bidirectional || s.mode != HistogramMode::histogram) return 0;
    return -static_cast<std::int64_t>(s.n_bins / 2) * s.binwidth_ps;
}

class HistogramAccumulator {
public:
    static constexpr int kMaxBins = 1'000'000;
    static constexpr std::size_t kMaxPairing = 200'000;   // buffered clicks per role (bounds the work per event)

    // Throws ArgumentError for invalid settings.
    HistogramAccumulator(const HistogramSettings& settings, std::int64_t centre_shift_ps);

    const HistogramSettings& settings() const { return s_; }
    std::int64_t centre() const { return centre_; }
    std::int64_t range_ps() const { return range_; }
    bool covers(std::int64_t shift_ps, std::int64_t origin_ps = 0) const;

    // A new run: pairing state cleared, counts kept.
    void begin_segment();
    // One event; within a segment in nondecreasing time, and at equal times
    // events of the START channel first.
    void add(int channel, std::int64_t t_ps);
    void clear();

    struct Totals {
        std::uint64_t starts{0};
        std::uint64_t stops{0};
        std::uint64_t pairs{0};          // pairs binned in the accumulated window
        std::uint64_t outside{0};        // StartStop pairs outside it
        std::uint64_t late{0};           // events older than the previous one (processed anyway)
        std::uint64_t segments{0};
        bool saturated{false};           // the pairing buffer overflowed (rate x range too large)
    };
    const Totals& totals() const { return t_; }

    // The histogram shown for `shift_ps` over [origin, origin + R), at most
    // `max_points` steps (bins merged for display); x in ps, with one closing edge.
    struct Shown {
        std::vector<double> xs_ps;
        std::vector<double> ys;
        int merged{1};
        std::uint64_t peak{0};           // largest single bin
        double peak_x_ps{0};
        std::uint64_t total{0};          // counts in the shown bins
    };
    Shown shown(std::int64_t shift_ps, std::size_t max_points, std::int64_t origin_ps = 0) const;
    // Every shown bin at full resolution: left edge (ps) and count.
    void shown_bins(std::int64_t shift_ps, std::vector<std::int64_t>& left_ps, std::vector<std::uint64_t>& counts,
                    std::int64_t origin_ps = 0) const;

private:
    void bin(std::int64_t raw);

    HistogramSettings s_;
    std::int64_t centre_;
    std::int64_t range_;
    std::int64_t base_;
    std::int64_t top_;
    std::vector<std::uint64_t> raw_;
    Totals t_;
    // segment state
    std::deque<std::int64_t> starts_;
    std::deque<std::int64_t> stops_;
    bool have_last_{false};
    std::int64_t last_start_{0};
    bool any_{false};
    std::int64_t last_t_{0};
};

// ---- 2D histogram (Swabian Time Tagger Histogram2D) ------------------------------
// One START channel and two STOP channels, one per axis.  Each axis is
// single-start, single-stop: the first STOP1 and the first STOP2 after a START
// give the bin (d1, d2), and the START is used up.  The Time Tagger manual
// leaves the remaining cases open; here a START that comes before both STOPs
// replaces the waiting one (counted as abandoned), as in StartStop, and a pair
// outside the accumulated window is counted as outside.  A click on a channel
// with several roles acts as STOP before it becomes a START (it never pairs
// with itself); STOP1 = STOP2 fills both axes at once.
//
// Each axis keeps the 1D scheme: raw delays over [base_k, base_k + 3 R_k),
// base_k = -centre_k - R_k, and the histogram shown for shift_k (STOPk delay -
// START delay) covers d_k + shift_k in [0, R_k).  Only one direction exists
// (StartStop), so there is no bidirectional origin.

struct Histogram2DSettings {
    int start_channel{0};               // 0..3
    int stop1_channel{1};               // axis 1 (x)
    int stop2_channel{2};               // axis 2 (y)
    std::int64_t binwidth1_ps{10000};
    std::int64_t binwidth2_ps{10000};
    int n_bins1{100};
    int n_bins2{100};
    bool operator==(const Histogram2DSettings& o) const {
        return start_channel == o.start_channel && stop1_channel == o.stop1_channel && stop2_channel == o.stop2_channel &&
               binwidth1_ps == o.binwidth1_ps && binwidth2_ps == o.binwidth2_ps && n_bins1 == o.n_bins1 && n_bins2 == o.n_bins2;
    }
    bool operator!=(const Histogram2DSettings& o) const { return !(*this == o); }
};

class Histogram2DAccumulator {
public:
    static constexpr int kMaxBinsPerAxis = 4096;
    static constexpr std::int64_t kMaxCells = 512 * 512;   // n_bins1 x n_bins2 (the store holds 9 x as many)

    // Throws ArgumentError for invalid settings.
    Histogram2DAccumulator(const Histogram2DSettings& settings, std::int64_t centre1_ps, std::int64_t centre2_ps);

    const Histogram2DSettings& settings() const { return s_; }
    std::int64_t centre1() const { return centre1_; }
    std::int64_t centre2() const { return centre2_; }
    bool covers(std::int64_t shift1_ps, std::int64_t shift2_ps) const;

    // A new run: the waiting START dropped, counts kept.
    void begin_segment();
    // One event; within a segment in nondecreasing time, and at equal times
    // events of the START channel first.
    void add(int channel, std::int64_t t_ps);
    void clear();

    struct Totals {
        std::uint64_t starts{0};
        std::uint64_t stops1{0};
        std::uint64_t stops2{0};
        std::uint64_t pairs{0};          // (d1, d2) binned in the accumulated window
        std::uint64_t outside{0};        // complete pairs outside it
        std::uint64_t abandoned{0};      // STARTs replaced by the next START before both STOPs came
        std::uint64_t late{0};           // events older than the previous one (processed anyway)
        std::uint64_t segments{0};
    };
    const Totals& totals() const { return t_; }

    // The histogram shown for (shift1, shift2) over [0, R1) x [0, R2), at most
    // `max_per_axis` cells per axis (bins merged for display).  values is
    // row-major, row j = axis-2 cell j counted from the bottom (smallest d2).
    struct Shown {
        int cols{0};                     // axis 1
        int rows{0};                     // axis 2
        int merged1{1};
        int merged2{1};
        std::vector<double> values;
        double x0_ps{0}, x1_ps{0};       // axis-1 extent of the cells
        double y0_ps{0}, y1_ps{0};
        std::uint64_t peak{0};           // largest single bin
        double peak_x_ps{0};             // its lower-left corner
        double peak_y_ps{0};
        std::uint64_t total{0};          // counts in the shown bins
    };
    Shown shown(std::int64_t shift1_ps, std::int64_t shift2_ps, int max_per_axis) const;
    // Every shown bin at full resolution: lower-left corner (ps) and count,
    // axis 1 fastest.
    void shown_bins(std::int64_t shift1_ps, std::int64_t shift2_ps, std::vector<std::int64_t>& left1_ps,
                    std::vector<std::int64_t>& left2_ps, std::vector<std::uint64_t>& counts) const;

private:
    // First stored column / row of the shown window and how many; the shown
    // window is whole store bins because the edges move with the shift.
    void window(std::int64_t shift, std::int64_t base, std::int64_t width, int n, int& first, int& count) const;

    Histogram2DSettings s_;
    std::int64_t centre1_, centre2_;
    std::int64_t range1_, range2_;
    std::int64_t base1_, base2_;
    std::vector<std::uint64_t> raw_;     // (3 n_bins2) rows of (3 n_bins1)
    Totals t_;
    // segment state
    bool have_start_{false};
    std::int64_t start_t_{0};
    bool have1_{false}, have2_{false};
    std::int64_t d1_{0}, d2_{0};
    bool any_{false};
    std::int64_t last_t_{0};
};

// Projection of 2D bins onto the axis at `angle_deg` (-90 ... 90) from the x
// axis: u = x cos(angle) + y sin(angle), so 0 sums over y (onto x,
// horizontal), 90 sums over x (onto y, vertical), 45 gives (x + y) / sqrt(2)
// and -45 (x - y) / sqrt(2).  Each bin's counts are spread over its projected
// footprint (a trapezoid, as for counts uniform within the bin), so the curve
// changes smoothly with the angle and the total is kept; at 0 and 90 it is the
// plain sum.  Projection bins are max(w1 |cos|, w2 |sin|) wide and span every
// given bin, also empty ones.  Inputs as from shown_bins (lower-left corners).
struct Projection {
    double angle_deg{0};
    double u0_ps{0};                     // left edge of the first projection bin
    double bin_ps{0};
    std::vector<double> counts;
    double total{0};
};
Projection project_2d(const std::vector<std::int64_t>& left1_ps, const std::vector<std::int64_t>& left2_ps,
                      const std::vector<std::uint64_t>& counts, std::int64_t binwidth1_ps, std::int64_t binwidth2_ps,
                      double angle_deg);

}  // namespace tdcctl
