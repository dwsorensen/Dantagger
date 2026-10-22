#pragma once

// Warnings the user must not miss (overflows, readout saturation, device
// failures).  The core only queues them; the Python layer prints them as
// banners whenever control passes through it (waits poll every 50 ms), and
// prints a session summary at freeTimeTagger / exit.

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace dt {

class WarningLog {
public:
    // kind: a short key ("overflow", "saturation", "stream_full", "device"...).
    void push(const std::string& kind, const std::string& text) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.size() < 10000) queue_.push_back({kind, text});
        ++counts_[index(kind)];
    }
    std::vector<std::pair<std::string, std::string>> take() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::pair<std::string, std::string>> out(queue_.begin(), queue_.end());
        queue_.clear();
        return out;
    }
    // Running totals for the session summary.
    void add_dropped(std::uint64_t n) {
        std::lock_guard<std::mutex> lock(mutex_);
        dropped_ += n;
        ++overflow_intervals_;
    }
    std::uint64_t dropped() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return dropped_;
    }
    std::uint64_t overflow_intervals() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return overflow_intervals_;
    }
    std::uint64_t count(const std::string& kind) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return counts_[index(kind)];
    }

private:
    static std::size_t index(const std::string& kind) {
        if (kind == "overflow") return 0;
        if (kind == "saturation") return 1;
        if (kind == "stream_full") return 2;
        if (kind == "device") return 3;
        return 4;
    }
    mutable std::mutex mutex_;
    std::deque<std::pair<std::string, std::string>> queue_;
    std::uint64_t counts_[5]{};
    std::uint64_t dropped_{0};
    std::uint64_t overflow_intervals_{0};
};

}  // namespace dt
