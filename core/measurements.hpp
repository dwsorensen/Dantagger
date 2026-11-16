#pragma once

// The measurement classes the SDE scripts use, with Swabian semantics as
// established against the Swabian library itself (tests/oracle, GAMEPLAN.md):
//
// * Correlation(ch1, ch2, bw, n): histogram of t(ch1) - t(ch2) over all pairs;
//   bin k covers [origin + k*bw, origin + (k+1)*bw), origin = -(n*bw)/2
//   (integer division); getIndex() = left edges; channel_1 == channel_2 is an
//   auto-correlation that includes the self-pairs at 0.
// * Histogram(click, start, bw, n): multiple start, multiple stop; bins
//   [k*bw, (k+1)*bw) of t(click) - t(start) >= 0; start == click counts the
//   self-pair in bin 0.
// * HistogramND(start, stops, bws, ns): single start (a new start replaces an
//   unfinished one), first stop per axis; a count needs every axis in range;
//   the start is then used up; row-major flat data.
// * Countrate: counts / capture duration (full window, NaN before any).
// * Counter(channels, bw, n): bins [k*bw, (k+1)*bw) of capture time, only
//   completed bins are stored (circular buffer of n); rolling getData puts
//   the most recent last.
// * TimeTagStream: an in-memory copy of the tags (never written to disk).
// Equal timestamps on different channels are ordered by channel number.
// Every class also counts virtual channels (Coincidence, channels 5..31).

#include "measurement.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace dt {

// Division by a positive constant for 0 <= x < 2^52 (double reciprocal with
// a one-step correction): a few cycles instead of a 64-bit idiv.
struct FastDiv {
    std::int64_t d{1};
    double inv{1.0};
    FastDiv() = default;
    explicit FastDiv(std::int64_t divisor) : d(divisor), inv(1.0 / static_cast<double>(divisor)) {}
    inline std::int64_t operator()(std::int64_t x) const {
        std::int64_t q = static_cast<std::int64_t>(static_cast<double>(x) * inv);
        const std::int64_t r = x - q * d;
        if (r < 0) --q;
        else if (r >= d) ++q;
        return q;
    }
};

// Times of one channel inside a sliding range: a vector with a moving head.
class TimeWindow {
public:
    void push(timestamp_t t) { v_.push_back(t); }
    // Drops every entry < limit.
    void drop_before(timestamp_t limit) {
        while (head_ < v_.size() && v_[head_] < limit) ++head_;
        if (head_ > 4096 && head_ * 2 > v_.size()) {
            v_.erase(v_.begin(), v_.begin() + static_cast<std::ptrdiff_t>(head_));
            head_ = 0;
        }
    }
    const timestamp_t* begin() const { return v_.data() + head_; }
    const timestamp_t* end() const { return v_.data() + v_.size(); }
    void clear() {
        v_.clear();
        head_ = 0;
    }

private:
    std::vector<timestamp_t> v_;
    std::size_t head_{0};
};

class Countrate final : public Measurement {
public:
    explicit Countrate(std::vector<int> channels);
    void process(const timestamp_t* t, const std::int8_t* ch, std::size_t n, timestamp_t begin, timestamp_t end) override;
    void clear_data() override;
    const char* name() const override { return "Countrate"; }
    std::vector<double> rates() const;                 // Hz; NaN before any capture
    std::vector<std::int64_t> counts_total() const;
    const std::vector<int>& channels() const { return channels_; }

private:
    std::vector<int> channels_;
    std::array<std::int64_t, kChannelSlots> counts_{};
};

class Counter final : public Measurement {
public:
    Counter(std::vector<int> channels, timestamp_t binwidth, int n_values);
    void process(const timestamp_t* t, const std::int8_t* ch, std::size_t n, timestamp_t begin, timestamp_t end) override;
    void clear_data() override;
    const char* name() const override { return "Counter"; }
    // channels x n_values, row-major.
    std::vector<std::int32_t> data(bool rolling) const;
    std::vector<double> data_normalized(bool rolling) const;
    std::vector<timestamp_t> index() const;
    std::vector<std::uint64_t> total_counts() const;
    int n_channels() const { return static_cast<int>(channels_.size()); }
    int n_values() const { return n_values_; }
    const std::vector<int>& channels() const { return channels_; }
    timestamp_t binwidth() const { return binwidth_; }
    std::int64_t completed() const { return completed_; }   // bins completed since clear (the newest min(n_values, completed) are valid)

private:
    void complete_until(std::int64_t bin);
    std::vector<int> channels_;
    timestamp_t binwidth_;
    int n_values_;
    std::array<std::int64_t, kChannelSlots> current_{};   // integrating bin, per channel number
    std::vector<std::int32_t> ring_;                         // [slot][n_values]
    std::vector<std::uint64_t> total_;
    std::int64_t completed_{0};                              // bins completed since clear
};

class Histogram final : public Measurement {
public:
    Histogram(int click_channel, int start_channel, timestamp_t binwidth, int n_bins);
    void process(const timestamp_t* t, const std::int8_t* ch, std::size_t n, timestamp_t begin, timestamp_t end) override;
    void clear_data() override;
    void on_gap() override { starts_.clear(); }
    const char* name() const override { return "Histogram"; }
    std::vector<std::int32_t> data() const;
    std::vector<timestamp_t> index() const;
    // Diagnostics for the app (since clear).
    std::uint64_t starts_seen() const { return starts_seen_; }
    std::uint64_t clicks_seen() const { return clicks_seen_; }
    std::size_t buffered_starts() const { return static_cast<std::size_t>(starts_.end() - starts_.begin()); }
    int click_channel() const { return click_; }
    int start_channel() const { return start_; }
    timestamp_t binwidth() const { return binwidth_; }

private:
    int click_, start_;
    std::uint64_t starts_seen_{0}, clicks_seen_{0};
    timestamp_t binwidth_, range_;
    FastDiv div_;
    TimeWindow starts_;
    std::vector<std::int64_t> hist_;
};

class Correlation final : public Measurement {
public:
    Correlation(int channel_1, int channel_2, timestamp_t binwidth, int n_bins);
    void process(const timestamp_t* t, const std::int8_t* ch, std::size_t n, timestamp_t begin, timestamp_t end) override;
    void clear_data() override;
    void on_gap() override {
        buf1_.clear();
        buf2_.clear();
    }
    const char* name() const override { return "Correlation"; }
    std::vector<std::int32_t> data() const;
    std::vector<double> data_normalized() const;
    std::vector<timestamp_t> index() const;
    // Diagnostics for the app (since clear).
    std::int64_t n1() const { return n1_; }
    std::int64_t n2() const { return n2_; }
    int channel_1() const { return c1_; }
    int channel_2() const { return c2_; }
    timestamp_t binwidth() const { return binwidth_; }

private:
    int c1_, c2_;
    timestamp_t binwidth_, origin_, hi_;
    int n_bins_;
    FastDiv div_;
    TimeWindow buf1_, buf2_;
    std::vector<std::int64_t> hist_;
    std::int64_t n1_{0}, n2_{0};
};

class HistogramND final : public Measurement {
public:
    HistogramND(int start_channel, std::vector<int> stop_channels, std::vector<timestamp_t> binwidths,
                std::vector<int> n_bins);
    void process(const timestamp_t* t, const std::int8_t* ch, std::size_t n, timestamp_t begin, timestamp_t end) override;
    void clear_data() override;
    void on_gap() override { active_ = false; }
    const char* name() const override { return "HistogramND"; }
    std::vector<std::int32_t> data() const;
    std::vector<timestamp_t> index(int dim) const;
    const std::vector<int>& n_bins() const { return n_bins_; }
    const std::vector<timestamp_t>& binwidths() const { return binwidths_; }
    const std::vector<int>& stop_channels() const { return stops_; }
    int start_channel() const { return start_; }
    // Diagnostics for the app (since clear): STARTs, tags of each stop
    // channel (indexed by channel), STARTs replaced by the next START before
    // every axis had its stop ("abandoned"), and STARTs whose first stop on
    // some axis fell beyond that axis' range ("outside").
    struct Stats {
        std::uint64_t starts{0};
        std::array<std::uint64_t, kChannelSlots> stop_tags{};
        std::uint64_t abandoned{0};
        std::uint64_t outside{0};
    };
    const Stats& stats() const { return stats_; }

private:
    Stats stats_;
    int start_;
    std::vector<int> stops_;
    std::vector<timestamp_t> binwidths_, ranges_;
    std::vector<int> n_bins_;
    std::vector<std::int64_t> strides_;
    std::vector<FastDiv> divs_;
    std::array<std::uint32_t, kChannelSlots> axes_of_{};   // bit mask of axes per channel number
    std::uint32_t all_{0};
    bool active_{false};
    timestamp_t start_time_{0};
    std::uint32_t got_{0};
    std::int64_t flat_{0};
    std::vector<std::int64_t> hist_;
};

struct StreamBuffer {
    std::vector<timestamp_t> timestamps;
    std::vector<std::int32_t> channels;
    std::vector<std::uint8_t> types;
    std::vector<std::uint16_t> missed;
    timestamp_t t_start{0};
    timestamp_t t_get_data{0};
    bool has_overflows{false};
    std::size_t lost{0};   // tags dropped because the buffer was full (n_max_events)
};

class TimeTagStream final : public Measurement {
public:
    TimeTagStream(std::size_t n_max_events, std::vector<int> channels);
    void process(const timestamp_t* t, const std::int8_t* ch, std::size_t n, timestamp_t begin, timestamp_t end) override;
    void on_special(const Special& s) override;
    void clear_data() override;
    void on_window_start(timestamp_t t) override;
    const char* name() const override { return "TimeTagStream"; }
    // Returns the buffer and starts an empty one (Swabian getData()).
    StreamBuffer take();
    std::size_t counts() const { return buf_.timestamps.size(); }
    std::size_t n_max() const { return n_max_; }

private:
    std::size_t n_max_;
    std::array<bool, kChannelSlots> wanted_{};
    StreamBuffer buf_;
    bool started_{false};
};

}  // namespace dt
