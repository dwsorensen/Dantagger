#include "tdcctl/gauss_fit.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace tdcctl {

namespace {

constexpr double kFwhmPerSigma = 2.3548200450309493;   // 2 sqrt(2 ln 2)

using Vec4 = std::array<double, 4>;
using Mat4 = std::array<std::array<double, 4>, 4>;

// Solves m x = b (Gauss-Jordan, partial pivoting); false if singular.
bool solve4(Mat4 m, Vec4 b, Vec4& x) {
    for (int c = 0; c < 4; ++c) {
        int piv = c;
        for (int r = c + 1; r < 4; ++r)
            if (std::abs(m[r][c]) > std::abs(m[piv][c])) piv = r;
        if (!(std::abs(m[piv][c]) > 1e-300)) return false;
        std::swap(m[c], m[piv]);
        std::swap(b[c], b[piv]);
        for (int r = 0; r < 4; ++r) {
            if (r == c) continue;
            const double f = m[r][c] / m[c][c];
            for (int k = c; k < 4; ++k) m[r][k] -= f * m[c][k];
            b[r] -= f * b[c];
        }
    }
    for (int c = 0; c < 4; ++c) x[c] = b[c] / m[c][c];
    return true;
}

bool invert4(const Mat4& m, Mat4& inv) {
    for (int c = 0; c < 4; ++c) {
        Vec4 e{};
        e[c] = 1;
        Vec4 col;
        if (!solve4(m, e, col)) return false;
        for (int r = 0; r < 4; ++r) inv[r][c] = col[r];
    }
    return true;
}

// A fit point: `bins` merged bins centred at x (relative to the centre guess)
// holding `sum` counts.
struct Point {
    double x;
    double sum;
    double bins;
};

// Expected counts of a point for p = {A, mu, sigma, B} (per-bin model x bins).
double expected(const Point& q, const Vec4& p, double* ex_out = nullptr) {
    const double d = (q.x - p[1]) / p[2];
    const double ex = std::exp(-0.5 * d * d);
    if (ex_out) *ex_out = ex;
    return q.bins * (p[0] * ex + p[3]);
}

// Poisson deviance 2 sum(M - Y + Y ln(Y / M)); infinite if a point with
// counts is expected to have none.
double deviance(const std::vector<Point>& pts, const Vec4& p) {
    double s = 0;
    for (const auto& q : pts) {
        const double m = expected(q, p);
        if (q.sum > 0) {
            if (!(m > 0)) return INFINITY;
            s += m - q.sum + q.sum * std::log(q.sum / m);
        } else {
            if (m < 0) return INFINITY;
            s += m;
        }
    }
    return 2 * s;
}

// Peak start values from the bins [i0, i1): rebinned by 1, 2, 4, ... until the
// tallest (rebinned) bin stands clear of the noise.  The half-maximum walk
// stops at the second bin in a row below half, so one noisy bin does not end it.
bool start_values(const std::vector<std::int64_t>& left, const std::vector<std::uint64_t>& counts, double w,
                  std::size_t i0, std::size_t i1, double& x0, double& a0, double& sigma0, double& b0) {
    const std::size_t n = i1 - i0;
    for (std::size_t k = 1; n / k >= 8; k *= 2) {
        std::vector<double> c;
        c.reserve(n / k + 1);
        for (std::size_t i = i0; i < i1; i += k) {
            double s = 0;
            for (std::size_t j = i; j < std::min(i1, i + k); ++j) s += static_cast<double>(counts[j]);
            c.push_back(s * static_cast<double>(k) / static_cast<double>(std::min(i1, i + k) - i));   // a short last bin scaled up
        }
        std::size_t imax = 0;
        for (std::size_t i = 1; i < c.size(); ++i)
            if (c[i] > c[imax]) imax = i;
        std::vector<double> sorted = c;
        std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(sorted.size() / 4), sorted.end());
        const double bk = sorted[sorted.size() / 4];
        const double ak = c[imax] - bk;
        if (!(ak > 3 * std::sqrt(std::max(c[imax], 1.0)) + 3)) continue;   // not clear of the noise yet
        const double half = bk + 0.5 * ak;
        std::size_t l = imax, r = imax;
        while (l >= 1 && (c[l - 1] > half || (l >= 2 && c[l - 2] > half))) --l;
        while (r + 1 < c.size() && (c[r + 1] > half || (r + 2 < c.size() && c[r + 2] > half))) ++r;
        const double width_bins = static_cast<double>(r - l + 1);
        if (k > 1 && width_bins < 3) {   // coarser bins hide the shape: use the finer estimate's scale
            sigma0 = std::max(0.5 * w, width_bins * static_cast<double>(k) * w / kFwhmPerSigma / 2);
        } else {
            sigma0 = std::max(0.5 * w, width_bins * static_cast<double>(k) * w / kFwhmPerSigma);
        }
        // centre: the tallest original bin inside the tallest rebinned bin
        const std::size_t f0 = i0 + imax * k, f1 = std::min(i1, f0 + k);
        std::size_t best = f0;
        for (std::size_t j = f0; j < f1; ++j)
            if (counts[j] > counts[best]) best = j;
        x0 = static_cast<double>(left[best]) + 0.5 * w;
        a0 = ak / static_cast<double>(k);
        b0 = bk / static_cast<double>(k);
        return true;
    }
    return false;
}


// The envelope's support points: the vertices of the upper hull of
// (bin centre, ln(counts - floor)) over the bins of [i0, i1) that stand clear
// of the background floor (counts > floor + 5 sqrt(floor) + 5: a stray tail
// bin on the hull would cut out every top between it and the peak).  A Gaussian
// on a flat floor is a downward parabola in ln(counts - floor), so the tooth
// tops that no line between other tops passes over are the points its
// envelope touches; lower teeth (and the weaker half of a doublet) fall under
// the hull, and stray counts in the tails are left out.
std::vector<Point> hull_tops(const std::vector<std::int64_t>& left, const std::vector<std::uint64_t>& counts, double w,
                             std::size_t i0, std::size_t i1, double floor) {
    std::vector<Point> hull;   // x absolute, sum = counts, bins = 1
    const double clear = floor + 5 * std::sqrt(std::max(floor, 1.0)) + 5;
    auto ly = [floor](const Point& q) { return std::log(q.sum - floor); };
    for (std::size_t i = i0; i < i1; ++i) {
        if (static_cast<double>(counts[i]) <= clear) continue;
        const Point q{static_cast<double>(left[i]) + 0.5 * w, static_cast<double>(counts[i]), 1.0};
        // Drop the last vertex while it lies on or under the line from the one before it to q.
        while (hull.size() >= 2) {
            const Point& a = hull[hull.size() - 2];
            const Point& b = hull.back();
            const double cross = (b.x - a.x) * (ly(q) - ly(a)) - (ly(b) - ly(a)) * (q.x - a.x);
            if (cross >= 0) hull.pop_back();
            else break;
        }
        hull.push_back(q);
    }
    return hull;
}
}  // namespace

double GaussFit::fwhm_ps() const { return kFwhmPerSigma * sigma_ps; }
double GaussFit::fwhm_err_ps() const { return kFwhmPerSigma * sigma_err_ps; }

double GaussFit::eval(double x) const {
    const double d = (x - centre_ps) / sigma_ps;
    return amplitude * std::exp(-0.5 * d * d) + background;
}

GaussFit fit_gaussian(const std::vector<std::int64_t>& left, const std::vector<std::uint64_t>& counts,
                      std::int64_t bw, double lo, double hi, FitTarget target) {
    GaussFit f;
    const double w = static_cast<double>(bw);
    // Bins inside the range.
    std::size_t i0 = 0, i1 = std::min(left.size(), counts.size());
    while (i0 < i1 && static_cast<double>(left[i0]) + 0.5 * w < lo) ++i0;
    while (i1 > i0 && static_cast<double>(left[i1 - 1]) + 0.5 * w >= hi) --i1;
    if (i1 - i0 < 5) {
        f.why = "fewer than 5 bins in view";
        return f;
    }
    std::uint64_t total = 0;
    for (std::size_t i = i0; i < i1; ++i) total += counts[i];
    if (total < 10) {
        f.why = "fewer than 10 counts in view";
        return f;
    }
    double x0 = 0, a0 = 0, sigma0 = 0, b0 = 0;
    std::size_t j0 = i0, j1 = i1;
    std::vector<Point> pts;
    bool fix_background = false;   // envelope: the background is the measured floor, not fitted
    if (target == FitTarget::envelope) {
        // Floor: the median of the bins in view (0 between a comb's teeth; the background beside a smooth peak).
        std::vector<std::uint64_t> sorted(counts.begin() + static_cast<std::ptrdiff_t>(i0),
                                          counts.begin() + static_cast<std::ptrdiff_t>(i1));
        std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(sorted.size() / 2), sorted.end());
        b0 = static_cast<double>(sorted[sorted.size() / 2]);
        fix_background = true;
        // Fit through the tooth tops on the envelope.
        pts = hull_tops(left, counts, w, i0, i1, b0);
        if (pts.size() < 3) {
            f.why = "fewer than 3 peaks stand out in view";
            return f;
        }
        f.lo_ps = pts.front().x - 0.5 * w;
        f.hi_ps = pts.back().x + 0.5 * w;
        // Start: the tallest top, and the width from the tops' spread above the floor.
        double s0 = 0, sx = 0, sxx = 0, top = 0;
        for (const auto& q : pts) {
            const double e = q.sum - b0;
            s0 += e;
            sx += e * q.x;
            if (q.sum > top) {
                top = q.sum;
                x0 = q.x;
            }
        }
        const double mean = sx / s0;
        for (const auto& q : pts) sxx += (q.sum - b0) * (q.x - mean) * (q.x - mean);
        sigma0 = std::max({0.5 * w, std::sqrt(sxx / s0), 0.25 * (f.hi_ps - f.lo_ps) / 2});
        a0 = top - b0;
        for (auto& q : pts) q.x -= x0;
        f.teeth = static_cast<int>(pts.size());
        f.points = f.teeth;
    } else {
        if (!start_values(left, counts, w, i0, i1, x0, a0, sigma0, b0)) {
            f.why = "no peak stands out of the noise";
            return f;
        }
        // Fit window: centre guess +- 8 sigma guess (within the range).
        const double wlo = std::max(lo, x0 - 8 * sigma0), whi = std::min(hi, x0 + 8 * sigma0);
        while (j0 < j1 && static_cast<double>(left[j0]) + 0.5 * w < wlo) ++j0;
        while (j1 > j0 && static_cast<double>(left[j1 - 1]) + 0.5 * w >= whi) --j1;
        const auto centre_bin = static_cast<std::size_t>(std::lround((x0 - 0.5 * w - static_cast<double>(left[i0])) / w)) + i0;
        if (j1 - j0 < 9) {   // a peak a few bins wide: take a few bins either side
            j0 = centre_bin >= i0 + 4 ? centre_bin - 4 : i0;
            j1 = std::min(i1, centre_bin + 5);
        }
    }
    if (pts.empty()) {   // bins, merged to <= kMaxFitPoints points
        const std::size_t n = j1 - j0;
        const std::size_t k = (n + kMaxFitPoints - 1) / kMaxFitPoints;
        pts.reserve(n / k + 1);
        for (std::size_t i = j0; i < j1; i += k) {
            const std::size_t e = std::min(j1, i + k);
            double sum = 0;
            for (std::size_t j = i; j < e; ++j) sum += static_cast<double>(counts[j]);
            const double xc = 0.5 * (static_cast<double>(left[i]) + static_cast<double>(left[e - 1]) + w) - x0;
            pts.push_back({xc, sum, static_cast<double>(e - i)});
        }
        f.merged = static_cast<int>(k);
        f.points = static_cast<int>(pts.size());
        f.lo_ps = static_cast<double>(left[j0]);
        f.hi_ps = static_cast<double>(left[j1 - 1]) + w;
    }
    if (pts.size() < (target == FitTarget::envelope ? 3U : 5U)) {
        f.why = "fewer than 5 points to fit";
        return f;
    }
    if (!(a0 > 0)) {
        f.why = "no peak above the background";
        return f;
    }

    // Poisson maximum likelihood by Levenberg-Marquardt on the deviance
    // (Fisher scoring: information sum M'M'/M, score sum (Y - M) M'/M).
    Vec4 p{a0, 0.0, sigma0, fix_background ? b0 : std::max(b0, 1e-3)};
    double dev = deviance(pts, p);
    if (!std::isfinite(dev) && !fix_background) {
        p[3] = std::max(1.0, b0);
        dev = deviance(pts, p);
    }
    double lambda = 1e-3;
    Mat4 info{};
    bool converged = false;
    int it = 0;
    for (; it < 200 && !converged; ++it) {
        Vec4 g{};
        for (auto& row : info) row.fill(0);
        for (const auto& q : pts) {
            double ex = 0;
            const double m = std::max(expected(q, p, &ex), 1e-9);
            const double d = (q.x - p[1]) / p[2];
            const Vec4 jac{q.bins * ex, q.bins * p[0] * ex * d / p[2], q.bins * p[0] * ex * d * d / p[2], q.bins};
            for (int a = 0; a < 4; ++a) {
                g[a] += (q.sum - m) / m * jac[a];
                for (int b = 0; b <= a; ++b) info[a][b] += jac[a] * jac[b] / m;
            }
        }
        for (int a = 0; a < 4; ++a)
            for (int b = a + 1; b < 4; ++b) info[a][b] = info[b][a];
        if (fix_background) {   // B stays at the floor: no gradient, an identity row
            g[3] = 0;
            for (int a = 0; a < 4; ++a) info[3][a] = info[a][3] = 0;
            info[3][3] = 1;
        }
        bool improved = false;
        while (lambda < 1e12) {
            Mat4 m = info;
            for (int a = 0; a < 4; ++a) m[a][a] *= 1 + lambda;
            Vec4 step;
            if (!solve4(m, g, step)) {
                lambda *= 10;
                continue;
            }
            Vec4 trial{p[0] + step[0], p[1] + step[1], std::abs(p[2] + step[2]), p[3] + step[3]};
            if (!(trial[2] > 1e-3 * w)) trial[2] = 1e-3 * w;
            const double c = deviance(pts, trial);
            if (std::isfinite(c) && c <= dev) {
                converged = (dev - c) <= 1e-9 * std::max(dev, 1.0);
                p = trial;
                dev = c;
                lambda = std::max(lambda / 10, 1e-12);
                improved = true;
                break;
            }
            lambda *= 10;
        }
        if (!improved) break;   // no step lowers the deviance: at the minimum
    }
    f.iterations = it;
    const int dof = static_cast<int>(pts.size()) - (fix_background ? 3 : 4);
    f.dof = std::max(0, dof);
    f.chi2_dof = dof > 0 ? dev / dof : 0;
    Mat4 cov;
    if (!(p[0] > 0) || !invert4(info, cov)) {
        f.why = "no peak: the counts in view are not peaked";
        return f;
    }
    const double scale = std::max(1.0, f.chi2_dof);   // widen the errors when the model does not describe the data
    f.amplitude = p[0];
    f.centre_ps = p[1] + x0;
    f.sigma_ps = p[2];
    f.background = p[3];
    f.amplitude_err = std::sqrt(std::max(0.0, cov[0][0] * scale));
    f.centre_err_ps = std::sqrt(std::max(0.0, cov[1][1] * scale));
    f.sigma_err_ps = std::sqrt(std::max(0.0, cov[2][2] * scale));
    f.background_err = fix_background ? 0.0 : std::sqrt(std::max(0.0, cov[3][3] * scale));
    f.ok = std::isfinite(f.centre_ps) && std::isfinite(f.sigma_ps) && f.centre_ps >= f.lo_ps && f.centre_ps <= f.hi_ps;
    if (!f.ok) f.why = "the fit diverged";
    return f;
}

}  // namespace tdcctl
