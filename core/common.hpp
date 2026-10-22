#pragma once

// Shared types of the Dantagger core.  Times are picoseconds on one
// monotonic timeline per open board (int64, as Swabian's timestamp_t).
// Channels are 1..4 (STOP1..STOP4); inside the core they are int8.

#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace dt {

using timestamp_t = std::int64_t;
using channel_t = std::int32_t;

constexpr int kNumChannels = 4;                     // STOP1..STOP4 = channels 1..4
constexpr channel_t kChannelUnused = -134217728;    // Swabian CHANNEL_UNUSED
constexpr timestamp_t kNever = std::numeric_limits<timestamp_t>::max();
constexpr timestamp_t kUnset = std::numeric_limits<timestamp_t>::min();

// Swabian Tag::Type values (TimeTagStreamBuffer.getEventTypes()).
enum class TagType : std::uint8_t { TimeTag = 0, Error = 1, OverflowBegin = 2, OverflowEnd = 3, MissedEvents = 4 };

struct Special {
    timestamp_t time{0};
    TagType type{TagType::Error};
    channel_t channel{kChannelUnused};
    std::uint32_t missed{0};
};

// Time-ordered tags covering the timeline interval [begin, end): every tag
// with a time in that interval is in the block.  `run_begin` marks the first
// block after a gap in the stream (a new FPGA run).
struct TagBlock {
    timestamp_t begin{0};
    timestamp_t end{0};
    bool run_begin{false};
    std::vector<timestamp_t> time;
    std::vector<std::int8_t> channel;
    std::vector<Special> specials;   // time-ordered; begin <= time < end

    void reset() {
        time.clear();
        channel.clear();
        specials.clear();
        run_begin = false;
    }
    std::size_t size() const { return time.size(); }
};

// Raw input of one backend step, before dead time, delays and merging.
// Per channel the times are increasing.  Every later event of this run has a
// raw time >= `watermark`.
struct RawChunk {
    std::vector<timestamp_t> events[kNumChannels];
    std::vector<Special> specials;
    timestamp_t watermark{0};
    bool run_begin{false};
    timestamp_t run_base{0};       // raw time at which the run begins (run_begin only)
    bool run_end{false};           // last chunk of the run: flush everything up to watermark
    std::size_t bytes() const {
        std::size_t n = 0;
        for (const auto& e : events) n += e.size() * sizeof(timestamp_t);
        return n + specials.size() * sizeof(Special) + 64;
    }
};

// TDC board comparator: safety window and the values `reset()` applies.
constexpr double kTriggerMinVolts = -0.100;
constexpr double kTriggerMaxVolts = 1.500;
constexpr double kHysteresisMinOhms = 75.0;
constexpr double kHysteresisMaxOhms = 5075.0;
constexpr double kDefaultTriggerVolts = 0.010;    // user, 2026-10-21
constexpr double kDefaultHysteresisOhms = 1000.0;

class SettingsBusyError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class DeviceError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

inline void check_channel(int channel) {
    if (channel < 1 || channel > kNumChannels)
        throw std::invalid_argument("channel " + std::to_string(channel) + " does not exist (the TDC board has channels 1-4)");
}

}  // namespace dt
