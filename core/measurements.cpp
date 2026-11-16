#include "measurements.hpp"

#include "engine.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace dt {

std::unique_lock<std::mutex> Measurement::guard() const { return std::unique_lock<std::mutex>(engine->mutex()); }

namespace {

constexpr std::int64_t kMaxRange = std::int64_t{1} << 52;   // FastDiv domain (~75 minutes)

std::int32_t sat32(std::int64_t v) {
    return v > std::numeric_limits<std::int32_t>::max() ? std::numeric_limits<std::int32_t>::max()
                                                        : static_cast<std::int32_t>(v);
}

void check_bins(timestamp_t binwidth, std::int64_t n_bins, const char* what) {
    if (binwidth <= 0) throw std::invalid_argument(std::string(what) + ": binwidth must be > 0 ps");
    if (n_bins <= 0) throw std::invalid_argument(std::string(what) + ": n_bins must be > 0");
    if (n_bins > 100000000) throw std::invalid_argument(std::string(what) + ": n_bins is too large");
    if (binwidth > kMaxRange / n_bins)
        throw std::invalid_argument(std::string(what) + ": binwidth * n_bins exceeds 2^52 ps");
}

}  // namespace

// ---------------------------------------------------------------------------
// Countrate
// ---------------------------------------------------------------------------
Countrate::Countrate(std::vector<int> channels) : channels_(std::move(channels)) {
    if (channels_.empty()) throw std::invalid_argument("Countrate: no channels given");
    for (int c : channels_) check_tag_channel(c);
}

void Countrate::process(const timestamp_t*, const std::int8_t* ch, std::size_t n, timestamp_t, timestamp_t) {
    // Four interleaved counter sets keep consecutive increments independent.
    std::int64_t c[4][kChannelSlots] = {};
    std::size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        ++c[0][ch[i]];
        ++c[1][ch[i + 1]];
        ++c[2][ch[i + 2]];
        ++c[3][ch[i + 3]];
    }
    for (; i < n; ++i) ++c[0][ch[i]];
    for (int k = 1; k <= kMaxChannel; ++k) counts_[static_cast<std::size_t>(k)] += c[0][k] + c[1][k] + c[2][k] + c[3][k];
}

void Countrate::clear_data() { counts_.fill(0); }

std::vector<double> Countrate::rates() const {
    std::vector<double> out;
    for (int c : channels_) {
        out.push_back(capture_duration > 0
                          ? static_cast<double>(counts_[static_cast<std::size_t>(c)]) / (static_cast<double>(capture_duration) * 1e-12)
                          : std::numeric_limits<double>::quiet_NaN());
    }
    return out;
}

std::vector<std::int64_t> Countrate::counts_total() const {
    std::vector<std::int64_t> out;
    for (int c : channels_) out.push_back(counts_[static_cast<std::size_t>(c)]);
    return out;
}

// ---------------------------------------------------------------------------
// Counter
// ---------------------------------------------------------------------------
Counter::Counter(std::vector<int> channels, timestamp_t binwidth, int n_values)
    : channels_(std::move(channels)), binwidth_(binwidth), n_values_(n_values) {
    if (channels_.empty()) throw std::invalid_argument("Counter: no channels given");
    for (int c : channels_) check_tag_channel(c);
    if (binwidth_ <= 0) throw std::invalid_argument("Counter: binwidth must be > 0 ps");
    if (n_values_ <= 0 || n_values_ > 100000000) throw std::invalid_argument("Counter: n_values must be 1 .. 1e8");
    ring_.assign(channels_.size() * static_cast<std::size_t>(n_values_), 0);
    total_.assign(channels_.size(), 0);
}

void Counter::complete_until(std::int64_t bin) {
    if (bin <= completed_) return;
    const auto n = static_cast<std::int64_t>(n_values_);
    // The integrating bin.
    {
        const auto pos = static_cast<std::size_t>(completed_ % n);
        for (std::size_t s = 0; s < channels_.size(); ++s) {
            const std::int64_t v = current_[static_cast<std::size_t>(channels_[s])];
            ring_[s * static_cast<std::size_t>(n_values_) + pos] = sat32(v);
            total_[s] += static_cast<std::uint64_t>(v);
        }
        // Only this counter's channels (other channels' entries are never read):
        // clearing all kChannelSlots per bin cost 3x with 1 us bins.
        for (int c : channels_) current_[static_cast<std::size_t>(c)] = 0;
        ++completed_;
    }
    // Empty bins up to `bin`.
    if (bin - completed_ >= n) {
        std::fill(ring_.begin(), ring_.end(), 0);
        completed_ = bin;
        return;
    }
    for (; completed_ < bin; ++completed_) {
        const auto pos = static_cast<std::size_t>(completed_ % n);
        for (std::size_t s = 0; s < channels_.size(); ++s) ring_[s * static_cast<std::size_t>(n_values_) + pos] = 0;
    }
}

void Counter::process(const timestamp_t* t, const std::int8_t* ch, std::size_t n, timestamp_t begin, timestamp_t end) {
    const std::int64_t base = capture_duration - begin;   // capture time = t + base
    std::int64_t next_edge = (completed_ + 1) * binwidth_;
    for (std::size_t i = 0; i < n; ++i) {
        const std::int64_t tau = t[i] + base;
        if (tau >= next_edge) {
            complete_until(tau / binwidth_);
            next_edge = (completed_ + 1) * binwidth_;
        }
        ++current_[static_cast<std::size_t>(ch[i])];
    }
    const std::int64_t tau_end = end + base;
    if (tau_end >= next_edge) complete_until(tau_end / binwidth_);
}

void Counter::clear_data() {
    std::fill(ring_.begin(), ring_.end(), 0);
    std::fill(total_.begin(), total_.end(), 0);
    current_.fill(0);
    completed_ = 0;
}

std::vector<std::int32_t> Counter::data(bool rolling) const {
    const auto n = static_cast<std::int64_t>(n_values_);
    std::vector<std::int32_t> out(ring_.size(), 0);
    const std::int64_t m = std::min(completed_, n);
    for (std::size_t s = 0; s < channels_.size(); ++s) {
        const std::size_t row = s * static_cast<std::size_t>(n_values_);
        for (std::int64_t j = 0; j < m; ++j) {
            const std::int64_t bin = completed_ - m + j;
            const auto pos = static_cast<std::size_t>(bin % n);
            const auto dst = rolling ? static_cast<std::size_t>(n - m + j) : pos;
            out[row + dst] = ring_[row + pos];
        }
    }
    return out;
}

std::vector<double> Counter::data_normalized(bool rolling) const {
    const auto n = static_cast<std::int64_t>(n_values_);
    std::vector<double> out(ring_.size(), std::numeric_limits<double>::quiet_NaN());
    const std::int64_t m = std::min(completed_, n);
    const double scale = 1.0 / (static_cast<double>(binwidth_) * 1e-12);
    for (std::size_t s = 0; s < channels_.size(); ++s) {
        const std::size_t row = s * static_cast<std::size_t>(n_values_);
        for (std::int64_t j = 0; j < m; ++j) {
            const std::int64_t bin = completed_ - m + j;
            const auto pos = static_cast<std::size_t>(bin % n);
            const auto dst = rolling ? static_cast<std::size_t>(n - m + j) : pos;
            out[row + dst] = ring_[row + pos] * scale;
        }
    }
    return out;
}

std::vector<timestamp_t> Counter::index() const {
    std::vector<timestamp_t> out(static_cast<std::size_t>(n_values_));
    for (int k = 0; k < n_values_; ++k) out[static_cast<std::size_t>(k)] = k * binwidth_;
    return out;
}

std::vector<std::uint64_t> Counter::total_counts() const { return total_; }

// ---------------------------------------------------------------------------
// Histogram
// ---------------------------------------------------------------------------
Histogram::Histogram(int click_channel, int start_channel, timestamp_t binwidth, int n_bins)
    : click_(click_channel), start_(start_channel), binwidth_(binwidth) {
    check_tag_channel(click_);
    check_tag_channel(start_);
    check_bins(binwidth, n_bins, "Histogram");
    range_ = binwidth * n_bins;
    div_ = FastDiv(binwidth);
    hist_.assign(static_cast<std::size_t>(n_bins), 0);
}

void Histogram::process(const timestamp_t* t, const std::int8_t* ch, std::size_t n, timestamp_t, timestamp_t) {
    const std::int8_t click = static_cast<std::int8_t>(click_), start = static_cast<std::int8_t>(start_);
    std::int64_t* hist = hist_.data();
    std::size_t pushes = 0;
    std::uint64_t starts = 0, clicks = 0;   // diagnostics in locals (members would alias the histogram writes)
    for (std::size_t i = 0; i < n; ++i) {
        const std::int8_t c = ch[i];
        const timestamp_t tt = t[i];
        if (c == start) {
            ++starts;
            starts_.push(tt);
            if ((++pushes & 1023U) == 0) starts_.drop_before(tt - range_ + 1);   // bound memory with a silent click channel
        }
        if (c == click) {
            ++clicks;
            starts_.drop_before(tt - range_ + 1);   // keep starts with tt - s < range
            for (const timestamp_t* p = starts_.begin(); p != starts_.end(); ++p) ++hist[div_(tt - *p)];
        }
    }
    starts_seen_ += starts;
    clicks_seen_ += clicks;
}

void Histogram::clear_data() {
    std::fill(hist_.begin(), hist_.end(), 0);
    starts_seen_ = clicks_seen_ = 0;
}

std::vector<std::int32_t> Histogram::data() const {
    std::vector<std::int32_t> out(hist_.size());
    for (std::size_t i = 0; i < hist_.size(); ++i) out[i] = sat32(hist_[i]);
    return out;
}

std::vector<timestamp_t> Histogram::index() const {
    std::vector<timestamp_t> out(hist_.size());
    for (std::size_t k = 0; k < out.size(); ++k) out[k] = static_cast<timestamp_t>(k) * binwidth_;
    return out;
}

// ---------------------------------------------------------------------------
// Correlation
// ---------------------------------------------------------------------------
Correlation::Correlation(int channel_1, int channel_2, timestamp_t binwidth, int n_bins)
    : c1_(channel_1), c2_(channel_2), binwidth_(binwidth), n_bins_(n_bins) {
    check_tag_channel(c1_);
    check_tag_channel(c2_);
    check_bins(binwidth, n_bins, "Correlation");
    origin_ = -(binwidth * n_bins) / 2;
    hi_ = origin_ + binwidth * n_bins;
    div_ = FastDiv(binwidth);
    hist_.assign(static_cast<std::size_t>(n_bins), 0);
}

void Correlation::process(const timestamp_t* t, const std::int8_t* ch, std::size_t n, timestamp_t, timestamp_t) {
    std::int64_t* hist = hist_.data();
    const timestamp_t origin = origin_, hi = hi_;
    const std::int8_t c1 = static_cast<std::int8_t>(c1_), c2 = static_cast<std::int8_t>(c2_);
    if (c1_ == c2_) {
        // Auto-correlation: every pair in both orders, plus each tag with itself.
        const timestamp_t keep = std::max(hi, 1 - origin);
        const std::int64_t self_bin = div_(-origin);
        for (std::size_t i = 0; i < n; ++i) {
            if (ch[i] != c1) continue;
            const timestamp_t tt = t[i];
            buf1_.drop_before(tt - keep + 1);
            for (const timestamp_t* p = buf1_.begin(); p != buf1_.end(); ++p) {
                const timestamp_t dd = tt - *p;
                if (dd < hi) ++hist[div_(dd - origin)];
                if (-dd >= origin) ++hist[div_(-dd - origin)];
            }
            ++hist[self_bin];
            buf1_.push(tt);
            ++n1_;
            ++n2_;
        }
        return;
    }
    std::size_t pushes = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const std::int8_t c = ch[i];
        const timestamp_t tt = t[i];
        if (c == c2) {
            buf1_.drop_before(tt + origin);   // keep s - tt >= origin
            for (const timestamp_t* p = buf1_.begin(); p != buf1_.end(); ++p) ++hist[div_(*p - tt - origin)];
            buf2_.push(tt);
            ++n2_;
            if ((++pushes & 1023U) == 0) buf2_.drop_before(tt - hi + 1);
        } else if (c == c1) {
            buf2_.drop_before(tt - hi + 1);   // keep tt - s < hi
            for (const timestamp_t* p = buf2_.begin(); p != buf2_.end(); ++p) ++hist[div_(tt - *p - origin)];
            buf1_.push(tt);
            ++n1_;
            if ((++pushes & 1023U) == 0) buf1_.drop_before(tt + origin);
        }
    }
}

void Correlation::clear_data() {
    std::fill(hist_.begin(), hist_.end(), 0);
    n1_ = n2_ = 0;
}

std::vector<std::int32_t> Correlation::data() const {
    std::vector<std::int32_t> out(hist_.size());
    for (std::size_t i = 0; i < hist_.size(); ++i) out[i] = sat32(hist_[i]);
    return out;
}

std::vector<double> Correlation::data_normalized() const {
    std::vector<double> out(hist_.size(), std::numeric_limits<double>::quiet_NaN());
    if (n1_ == 0 || n2_ == 0 || capture_duration <= 0) return out;
    const double scale = static_cast<double>(capture_duration) /
                         (static_cast<double>(binwidth_) * static_cast<double>(n1_) * static_cast<double>(n2_));
    for (std::size_t i = 0; i < hist_.size(); ++i) out[i] = static_cast<double>(hist_[i]) * scale;
    return out;
}

std::vector<timestamp_t> Correlation::index() const {
    std::vector<timestamp_t> out(hist_.size());
    for (std::size_t k = 0; k < out.size(); ++k) out[k] = origin_ + static_cast<timestamp_t>(k) * binwidth_;
    return out;
}

// ---------------------------------------------------------------------------
// HistogramND
// ---------------------------------------------------------------------------
HistogramND::HistogramND(int start_channel, std::vector<int> stop_channels, std::vector<timestamp_t> binwidths,
                         std::vector<int> n_bins)
    : start_(start_channel), stops_(std::move(stop_channels)), binwidths_(std::move(binwidths)), n_bins_(std::move(n_bins)) {
    check_tag_channel(start_);
    if (stops_.empty() || stops_.size() > 8) throw std::invalid_argument("HistogramND: 1 to 8 stop channels");
    if (binwidths_.size() != stops_.size() || n_bins_.size() != stops_.size())
        throw std::invalid_argument("HistogramND: stop_channels, binwidths and n_bins must have the same length");
    std::int64_t total = 1;
    for (std::size_t k = 0; k < stops_.size(); ++k) {
        check_tag_channel(stops_[k]);
        check_bins(binwidths_[k], n_bins_[k], "HistogramND");
        ranges_.push_back(binwidths_[k] * n_bins_[k]);
        divs_.emplace_back(binwidths_[k]);
        axes_of_[static_cast<std::size_t>(stops_[k])] |= 1U << k;
        total *= n_bins_[k];
        if (total > 400000000) throw std::invalid_argument("HistogramND: too many bins in total");
    }
    strides_.assign(stops_.size(), 1);
    for (std::size_t k = stops_.size() - 1; k > 0; --k) strides_[k - 1] = strides_[k] * n_bins_[k];
    all_ = (1U << stops_.size()) - 1U;
    hist_.assign(static_cast<std::size_t>(total), 0);
}

void HistogramND::process(const timestamp_t* t, const std::int8_t* ch, std::size_t n, timestamp_t, timestamp_t) {
    const std::int8_t start = static_cast<std::int8_t>(start_);
    // Diagnostics in locals (stored once per call): members would alias the histogram writes.
    std::uint64_t starts = 0, abandoned = 0, outside = 0;
    std::uint64_t stop_tags[kChannelSlots] = {};
    for (std::size_t i = 0; i < n; ++i) {
        const std::int8_t c = ch[i];
        const timestamp_t tt = t[i];
        if (c == start) {
            ++starts;
            abandoned += active_ ? 1U : 0U;
            active_ = true;
            start_time_ = tt;
            got_ = 0;
            flat_ = 0;
        }
        std::uint32_t axes = axes_of_[static_cast<std::size_t>(c)];
        if (axes == 0) continue;
        ++stop_tags[static_cast<std::size_t>(c)];
        if (!active_) continue;
        axes &= ~got_;
        const timestamp_t d = tt - start_time_;
        while (axes != 0) {
            const int k = __builtin_ctz(axes);
            axes &= axes - 1U;
            if (d >= ranges_[static_cast<std::size_t>(k)]) {   // can never count for this start
                active_ = false;
                ++outside;
                break;
            }
            flat_ += divs_[static_cast<std::size_t>(k)](d) * strides_[static_cast<std::size_t>(k)];
            got_ |= 1U << k;
        }
        if (active_ && got_ == all_) {
            ++hist_[static_cast<std::size_t>(flat_)];
            active_ = false;
        }
    }
    stats_.starts += starts;
    stats_.abandoned += abandoned;
    stats_.outside += outside;
    for (int c = 1; c <= kMaxChannel; ++c) stats_.stop_tags[static_cast<std::size_t>(c)] += stop_tags[c];
}

void HistogramND::clear_data() {
    std::fill(hist_.begin(), hist_.end(), 0);
    active_ = false;
    stats_ = Stats{};
}

std::vector<std::int32_t> HistogramND::data() const {
    std::vector<std::int32_t> out(hist_.size());
    for (std::size_t i = 0; i < hist_.size(); ++i) out[i] = sat32(hist_[i]);
    return out;
}

std::vector<timestamp_t> HistogramND::index(int dim) const {
    if (dim < 0 || dim >= static_cast<int>(stops_.size())) throw std::out_of_range("HistogramND.getIndex: no such dimension");
    std::vector<timestamp_t> out(static_cast<std::size_t>(n_bins_[static_cast<std::size_t>(dim)]));
    for (std::size_t k = 0; k < out.size(); ++k) out[k] = static_cast<timestamp_t>(k) * binwidths_[static_cast<std::size_t>(dim)];
    return out;
}

// ---------------------------------------------------------------------------
// TimeTagStream
// ---------------------------------------------------------------------------
TimeTagStream::TimeTagStream(std::size_t n_max_events, std::vector<int> channels) : n_max_(n_max_events) {
    if (n_max_ == 0) throw std::invalid_argument("TimeTagStream: n_max_events must be > 0");
    for (int c : channels) {
        check_tag_channel(c);
        wanted_[static_cast<std::size_t>(c)] = true;
    }
}

void TimeTagStream::on_window_start(timestamp_t t) {
    if (!started_) {
        buf_.t_start = t;
        started_ = true;
    }
}

void TimeTagStream::process(const timestamp_t* t, const std::int8_t* ch, std::size_t n, timestamp_t, timestamp_t) {
    for (std::size_t i = 0; i < n; ++i) {
        if (!wanted_[static_cast<std::size_t>(ch[i])]) continue;
        if (buf_.timestamps.size() >= n_max_) {
            ++buf_.lost;
            continue;
        }
        buf_.timestamps.push_back(t[i]);
        buf_.channels.push_back(ch[i]);
        buf_.types.push_back(0);
        buf_.missed.push_back(0);
    }
}

void TimeTagStream::on_special(const Special& s) {
    buf_.has_overflows = true;
    if (buf_.timestamps.size() >= n_max_) {
        ++buf_.lost;
        return;
    }
    buf_.timestamps.push_back(s.time);
    buf_.channels.push_back(s.channel);
    buf_.types.push_back(static_cast<std::uint8_t>(s.type));
    buf_.missed.push_back(static_cast<std::uint16_t>(std::min<std::uint32_t>(s.missed, 65535)));
}

void TimeTagStream::clear_data() {
    buf_ = StreamBuffer{};
    buf_.t_start = last_time;
}

StreamBuffer TimeTagStream::take() {
    StreamBuffer out = std::move(buf_);
    out.t_get_data = last_time;
    buf_ = StreamBuffer{};
    buf_.t_start = last_time;
    return out;
}

}  // namespace dt
