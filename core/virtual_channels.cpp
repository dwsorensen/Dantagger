#include "virtual_channels.hpp"

#include <algorithm>
#include <stdexcept>

namespace dt {

Coincidence::Coincidence(std::vector<int> channels, timestamp_t window) : window_(window) {
    if (channels.empty()) throw std::invalid_argument("Coincidence: no channels given");
    slot_.fill(-1);
    for (int c : channels) {
        check_tag_channel(c);
        if (slot_[static_cast<std::size_t>(c)] >= 0) continue;   // a repeated channel counts once (as Swabian)
        slot_[static_cast<std::size_t>(c)] = static_cast<std::int8_t>(channels_.size());
        channels_.push_back(c);
    }
    set_window(window);
    last_.assign(channels_.size(), kUnset);
}

void Coincidence::set_window(timestamp_t window) {
    if (window < 0) throw std::invalid_argument("Coincidence: the coincidence window cannot be less than zero");
    window_ = window;
}

void Coincidence::on_gap() { std::fill(last_.begin(), last_.end(), kUnset); }

void Coincidence::process(const TagBlock& block, std::vector<std::size_t>& out) {
    const std::size_t n = block.time.size();
    const timestamp_t* const t = block.time.data();
    const std::int8_t* const ch = block.channel.data();
    const std::size_t k = channels_.size();
    const timestamp_t w = window_;
    timestamp_t* const last = last_.data();
    const auto& specials = block.specials;
    std::size_t sp = 0;
    // An overflow forgets the past; a marker applies to the tags at and after its time.
    timestamp_t next_special = specials.empty() ? kNever : specials[0].time;
    for (std::size_t i = 0; i < n; ++i) {
        const timestamp_t ti = t[i];
        while (ti >= next_special) {
            if (specials[sp].type == TagType::OverflowBegin) on_gap();
            ++sp;
            next_special = sp < specials.size() ? specials[sp].time : kNever;
        }
        const int s = slot_[static_cast<std::size_t>(ch[i])];
        if (s < 0) continue;
        last[s] = ti;
        bool all = true;
        for (std::size_t j = 0; j < k; ++j) {
            const timestamp_t l = last[j];
            if (l == kUnset || ti - l >= w) {   // the tag itself: ti - ti = 0 < w unless w == 0
                all = false;
                break;
            }
        }
        if (all) out.push_back(i);
    }
    for (; sp < specials.size(); ++sp)
        if (specials[sp].type == TagType::OverflowBegin) on_gap();
}

}  // namespace dt
