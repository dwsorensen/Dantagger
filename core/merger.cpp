#include "merger.hpp"

#include <algorithm>

namespace dt {

void Merger::configure(const std::array<timestamp_t, kNumChannels>& delays,
                       const std::array<timestamp_t, kNumChannels>& deadtimes) {
    delay_ = delays;
    deadtime_ = deadtimes;
    min_delay_ = *std::min_element(delay_.begin(), delay_.end());
    max_delay_ = *std::max_element(delay_.begin(), delay_.end());
}

bool Merger::push(const RawChunk& chunk, TagBlock& out) {
    out.reset();
    if (chunk.run_begin) {
        for (auto& lane : lanes_) {
            lane.buf.clear();
            lane.head = 0;
            lane.last_accepted = kUnset;
        }
        specials_.clear();
        // The run's timeline starts at its base (shifted forward if the
        // previous run reached further); a negative delay that moves a tag
        // before the start drops it (no window can contain it).
        const timestamp_t base = chunk.run_base;
        if (!started_) {
            shift_ = 0;
            limit_ = base;
            started_ = true;
        } else {
            shift_ = std::max<timestamp_t>(0, limit_ - base);
            limit_ = std::max(limit_, base + shift_);
        }
        run_start_ = limit_;
        in_run_ = true;
        pending_run_begin_ = true;
    }
    if (!in_run_) return false;

    for (int c = 0; c < kNumChannels; ++c) {
        const auto& ev = chunk.events[c];
        if (ev.empty()) continue;
        Lane& lane = lanes_[static_cast<std::size_t>(c)];
        const timestamp_t d = delay_[static_cast<std::size_t>(c)] + shift_;
        const timestamp_t dead = deadtime_[static_cast<std::size_t>(c)];
        const timestamp_t floor = limit_;   // already emitted: never go back
        const timestamp_t start = run_start_;
        lane.buf.reserve(lane.buf.size() + ev.size());
        timestamp_t last = lane.last_accepted;
        for (timestamp_t t : ev) {
            if (dead > 0 && last != kUnset && t - last < dead) {
                ++deadtime_dropped_;
                continue;
            }
            last = t;
            const timestamp_t td = t + d;
            if (td < start) {
                ++before_start_dropped_;
                continue;
            }
            lane.buf.push_back(std::max(td, floor));
        }
        lane.last_accepted = last;
    }
    for (Special s : chunk.specials) {
        s.time = std::max(s.time + shift_ + min_delay_, limit_);
        auto it = std::upper_bound(specials_.begin(), specials_.end(), s.time,
                                   [](timestamp_t v, const Special& x) { return v < x.time; });
        specials_.insert(it, s);
    }

    timestamp_t new_limit = chunk.watermark + shift_ + min_delay_;
    if (chunk.run_end) {
        new_limit = std::max(new_limit, chunk.watermark + shift_ + max_delay_);
        for (const auto& lane : lanes_)
            if (lane.head < lane.buf.size()) new_limit = std::max(new_limit, lane.buf.back() + 1);
        for (const auto& s : specials_) new_limit = std::max(new_limit, s.time + 1);
    }
    new_limit = std::max(new_limit, limit_);
    out.begin = limit_;
    merge_until(new_limit, out);
    out.end = new_limit;
    limit_ = new_limit;
    out.run_begin = pending_run_begin_;
    pending_run_begin_ = false;
    if (chunk.run_end) in_run_ = false;
    return out.end > out.begin || !out.time.empty() || out.run_begin || !out.specials.empty();
}

void Merger::merge_until(timestamp_t limit, TagBlock& out) {
    const timestamp_t* p[kNumChannels];
    const timestamp_t* e[kNumChannels];
    std::size_t total = 0;
    for (int c = 0; c < kNumChannels; ++c) {
        auto& lane = lanes_[static_cast<std::size_t>(c)];
        p[c] = lane.buf.data() + lane.head;
        e[c] = lane.buf.data() + lane.buf.size();
        total += static_cast<std::size_t>(e[c] - p[c]);
    }
    out.time.reserve(total);
    out.channel.reserve(total);
    while (true) {
        int best = -1;
        for (int c = 0; c < kNumChannels; ++c)
            if (p[c] < e[c] && *p[c] < limit && (best < 0 || *p[c] < *p[best])) best = c;
        if (best < 0) break;
        timestamp_t sec_t = limit;
        int sec_c = -1;   // a tie at `limit` is never emitted
        for (int c = 0; c < kNumChannels; ++c) {
            if (c == best || p[c] >= e[c]) continue;
            if (*p[c] < sec_t || (*p[c] == sec_t && sec_c >= 0 && c < sec_c)) {
                sec_t = *p[c];
                sec_c = c;
            }
        }
        const timestamp_t* q = p[best];
        const timestamp_t* const qe = e[best];
        const timestamp_t* const q0 = q;
        if (sec_c < 0 || best < sec_c) {
            while (q < qe && *q <= sec_t && (*q < sec_t || sec_c >= 0)) ++q;
            // (*q == sec_t is allowed only when the other lane has a higher channel)
        } else {
            while (q < qe && *q < sec_t) ++q;
        }
        const auto n = static_cast<std::size_t>(q - q0);
        out.time.insert(out.time.end(), q0, q);
        out.channel.insert(out.channel.end(), n, static_cast<std::int8_t>(best + 1));
        p[best] = q;
    }
    for (int c = 0; c < kNumChannels; ++c) {
        auto& lane = lanes_[static_cast<std::size_t>(c)];
        lane.head = static_cast<std::size_t>(p[c] - lane.buf.data());
        lane.compact();
    }
    std::size_t k = 0;
    while (k < specials_.size() && specials_[k].time < limit) ++k;
    if (k > 0) {
        out.specials.insert(out.specials.end(), specials_.begin(), specials_.begin() + static_cast<std::ptrdiff_t>(k));
        specials_.erase(specials_.begin(), specials_.begin() + static_cast<std::ptrdiff_t>(k));
    }
}

}  // namespace dt
