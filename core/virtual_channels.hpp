#pragma once

// Virtual channels (Swabian virtual channels): tags computed from other
// channels' tags and put into the tag stream before any measurement sees it,
// so every measurement can count them like an input.  The engine runs each
// registered virtual channel over every tag block, in the order they were
// registered, so a virtual channel may take an earlier one as an input.  A
// virtual channel's tag has the time of the tag that made it and follows it
// directly in the stream, as in the Swabian library (a later virtual
// channel's tag therefore comes before an earlier one's made by the same
// tag; tests/oracle checks the order through chained coincidences).
//
// Coincidence (Swabian Coincidence with CoincidenceTimestamp.Last, checked
// against Swabian 2.22.6 with its virtual tagger, tests/oracle): the group is
// the set of distinct channels given.  A tag at time t on a group channel
// makes a coincidence tag at t when every other group channel's latest tag
// lies within (t - window, t]: strictly less than `window` before it, the
// same time included.  A tag is never used up (two quick tags on one channel
// after one tag on the other make two coincidences); a group of one channel
// copies it; window 0 never fires.  Forgets the past at a new FPGA run and at
// an overflow.

#include "common.hpp"

#include <array>
#include <vector>

namespace dt {

class VirtualChannel {
public:
    virtual ~VirtualChannel() = default;

    // ---- engine thread, engine mutex held ----
    // `block`: time-ordered tags, including those of virtual channels
    // registered earlier.  Appends the positions in `block` of the tags that
    // make a tag of this channel (increasing; the new tag gets that time and
    // goes right after it) to `out`.
    virtual void process(const TagBlock& block, std::vector<std::size_t>& out) = 0;
    // A discontinuity (new run, overflow, restart): forget earlier tags.
    virtual void on_gap() = 0;
    // The channels it reads (checked when it is registered).
    virtual std::vector<int> inputs() const = 0;
    virtual const char* name() const = 0;

    // Its channel number once registered with an engine (0 before).
    int channel() const { return channel_; }
    // Swabian start()/stop(): a stopped virtual channel makes no tags (engine mutex).
    bool enabled{true};

private:
    friend class Engine;
    int channel_{0};
};

class Coincidence final : public VirtualChannel {
public:
    // channels: 1..kMaxChannel (repeats count once); window in ps, >= 0.
    Coincidence(std::vector<int> channels, timestamp_t window);
    void process(const TagBlock& block, std::vector<std::size_t>& out) override;
    void on_gap() override;
    std::vector<int> inputs() const override { return channels_; }
    const char* name() const override { return "Coincidence"; }

    const std::vector<int>& channels() const { return channels_; }   // distinct, in the order given
    timestamp_t window() const { return window_; }
    void set_window(timestamp_t window);   // engine mutex

private:
    std::vector<int> channels_;
    timestamp_t window_;
    std::array<std::int8_t, kChannelSlots> slot_{};   // index into channels_ / last_, -1 if not in the group
    std::vector<timestamp_t> last_;                   // latest tag per group channel (kUnset: none yet)
};

}  // namespace dt
