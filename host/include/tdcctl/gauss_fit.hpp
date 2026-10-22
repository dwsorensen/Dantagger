#pragma once

// Gaussian fit of histogram bins: counts = A exp(-(x - mu)^2 / (2 sigma^2)) + B,
// x = bin centre.  Poisson maximum likelihood (Levenberg-Marquardt on the
// deviance), so low counts and empty bins do not bias it.
//
// The fit uses the bins inside [lo, hi) (e.g. the plot's view).  It starts at
// the tallest bin (rebinning coarser until the peak stands out of the noise),
// estimates the width from where the counts fall to half, and fits mu +- 8
// sigma of that estimate (bins merged to at most kMaxFitPoints), so a narrow
// peak in a wide histogram stays cheap.
//
// FitTarget::envelope fits the envelope of a comb of narrow peaks (the
// GPX2's ~44 ps steps, datasheet Figure 21), even an uneven one (doublets
// where the step grid jumps at a reference edge): the fit points are the
// tooth tops on the upper hull of (bin centre, ln(counts - floor)), floor =
// the median of the bins in view, among the bins clear of it.  A
// Gaussian on a flat floor is a downward parabola there, so these are the
// points its envelope touches; lower teeth fall under the hull.  The curve
// runs through the tops; the background is held at the floor.  The tops are
// single bins, so noise lifts them slightly (by about sqrt(counts)).

#include <cstdint>
#include <string>
#include <vector>

namespace tdcctl {

struct GaussFit {
    bool ok{false};
    std::string why;                    // when !ok
    double amplitude{0}, centre_ps{0}, sigma_ps{0}, background{0};
    double amplitude_err{0}, centre_err_ps{0}, sigma_err_ps{0}, background_err{0};
    double fwhm_ps() const;             // 2 sqrt(2 ln 2) sigma
    double fwhm_err_ps() const;
    double chi2_dof{0};                 // Poisson deviance / degrees of freedom (~ reduced chi-square)
    int dof{0};                         // degrees of freedom (0: the curve passes exactly through the points)
    double lo_ps{0}, hi_ps{0};          // the range the fit used
    int points{0};                      // fitted points (after merging)
    int merged{1};                      // bins per point
    int iterations{0};
    int teeth{0};                       // envelope fit: tooth tops fitted
    double eval(double x_ps) const;
};

constexpr int kMaxFitPoints = 4000;

enum class FitTarget { tallest_peak, envelope };

// left_ps: left edge of each bin (ascending, equal width binwidth_ps).
GaussFit fit_gaussian(const std::vector<std::int64_t>& left_ps, const std::vector<std::uint64_t>& counts,
                      std::int64_t binwidth_ps, double lo_ps, double hi_ps,
                      FitTarget target = FitTarget::tallest_peak);

}  // namespace tdcctl
