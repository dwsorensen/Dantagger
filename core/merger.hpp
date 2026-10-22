#pragma once

// Raw per-channel events -> one time-ordered tag stream.
//
// Per channel: software dead time (an event closer than `deadtime` to the
// previous accepted event of that channel is dropped, non-extending), then
// the software input delay.  Then a 4-way merge up to the watermark: every
// raw event still to come is >= watermark, so every delayed event still to
// come is >= watermark + min(delay); tags below that limit are final.
// A run's timeline starts at its raw base; tags a negative delay moves before
// that are dropped.
// Equal times are ordered by channel number (as the Swabian library does).
// Between runs the timeline is shifted forward if needed so it never goes
// back.

#include "common.hpp"

#include <array>
#include <vector>

namespace dt {

class Merger {
public:
    // Only between runs.
    void configure(const std::array<timestamp_t, kNumChannels>& delays,
                   const std::array<timestamp_t, kNumChannels>& deadtimes);

    // Adds the chunk; fills `out` with every tag that became final, as the
    // block [out.begin, out.end).  Returns false if the block would be empty
    // and covers no time.
    bool push(const RawChunk& chunk, TagBlock& out);

    timestamp_t position() const { return limit_; }
    bool in_run() const { return in_run_; }
    std::uint64_t deadtime_dropped() const { return deadtime_dropped_; }

private:
    struct Lane {
        std::vector<timestamp_t> buf;
        std::size_t head{0};
        timestamp_t last_accepted{kUnset};
        void compact() {
            if (head > 0 && (head == buf.size() || head * 2 > buf.size())) {
                buf.erase(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(head));
                head = 0;
            }
        }
    };
    void merge_until(timestamp_t limit, TagBlock& out);

    std::array<timestamp_t, kNumChannels> delay_{};
    std::array<timestamp_t, kNumChannels> deadtime_{};
    timestamp_t min_delay_{0}, max_delay_{0};
    std::array<Lane, kNumChannels> lanes_{};
    std::vector<Special> specials_;
    timestamp_t limit_{0};      // everything below is emitted
    timestamp_t shift_{0};      // added to raw times of the current run
    timestamp_t run_start_{0};  // timeline start of the current run
    bool in_run_{false};
    bool started_{false};
    bool pending_run_begin_{false};
    std::uint64_t deadtime_dropped_{0};
    std::uint64_t before_start_dropped_{0};
};

}  // namespace dt
