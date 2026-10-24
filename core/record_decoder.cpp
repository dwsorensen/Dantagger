#include "record_decoder.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace dt {

namespace {
constexpr std::uint64_t kMask48 = (std::uint64_t{1} << 48) - 1;
constexpr double kSafeFraction = 0.45;              // of a wrap: the largest prediction error accepted
}  // namespace

RecordDecoder::RecordDecoder(DecoderConfig config) { set_config(config); }

void RecordDecoder::set_config(const DecoderConfig& config) {
    if (config.divisions == 0 || config.lsb_ps <= 0 || config.ref_bits < 1 || config.ref_bits > 24 ||
        config.stop_bits < 14 || config.stop_bits > 20 || config.coarse_ps <= 0 || config.latency_spread_ps < 0 ||
        config.coarse_uncertainty_ps < 0)
        throw std::invalid_argument("invalid decoder configuration");
    cfg_ = config;
    ref_period_ps_ = static_cast<double>(cfg_.divisions) * static_cast<double>(cfg_.lsb_ps);
    wrap_refs_ = std::ldexp(1.0, cfg_.ref_bits);
    wrap_ps_ = (std::int64_t{1} << cfg_.ref_bits) * static_cast<std::int64_t>(cfg_.divisions) * cfg_.lsb_ps;
    inv_wrap_ps_ = 1.0 / static_cast<double>(wrap_ps_);
    coarse_ps_ = cfg_.coarse_ps;
    spread_ps_ = static_cast<double>(cfg_.latency_spread_ps + cfg_.coarse_uncertainty_ps);
    margin_ps_ = kMarginPs + cfg_.coarse_uncertainty_ps;
    // Fast path (ratio taken as 1) while spread + 2e-4 * dc stays below the safe fraction of a wrap.
    fast_dc_limit_ = std::max<std::int64_t>(
        1, static_cast<std::int64_t>((kSafeFraction * static_cast<double>(wrap_ps_) - spread_ps_) / 2e-4 /
                                     static_cast<double>(coarse_ps_)));
}

void RecordDecoder::begin_run() {
    started_ = false;
    ended_ = false;
    have_event_ = false;
    for (bool& h : have_ch_) h = false;
}

timestamp_t RecordDecoder::marker_time() const {
    if (!have_event_) return static_cast<timestamp_t>(coarse_) * coarse_ps_;
    const double dc = (static_cast<double>(coarse_) - static_cast<double>(last_event_coarse_)) * coarse_ps_;
    return last_raw_ + static_cast<timestamp_t>(dc * ratio_);
}

void RecordDecoder::update_watermark() { (void)watermark(); }

void RecordDecoder::feed(std::uint64_t raw, RawChunk& chunk) {
    if ((raw >> 63) != 0) {
        ++stats_.markers;
        const unsigned type = static_cast<unsigned>((raw >> 60) & 7U);
        const auto info = static_cast<std::uint32_t>((raw >> 48) & 0xFFFU);
        const std::uint64_t c48 = raw & kMask48;
        if (type == 0) {   // START
            started_ = true;
            ended_ = false;
            coarse_ = c48;
            run_base_ = static_cast<timestamp_t>(coarse_) * coarse_ps_;
            watermark_ = run_base_;
            chunk.run_begin = true;
            chunk.run_base = run_base_;
            chunk.watermark = run_base_;
            return;
        }
        if (!started_) throw std::runtime_error("the first record of a run is not a START marker");
        // 48-bit coarse never wraps in practice (32 days); keep it monotonic.
        if (c48 + 64 >= (coarse_ & kMask48)) coarse_ = (coarse_ & ~kMask48) | c48;
        if (type == 2) {   // OVERFLOW
            stats_.dropped += info;
            const timestamp_t begin = watermark_;
            const timestamp_t end = std::max(marker_time(), begin);
            chunk.specials.push_back({begin, TagType::OverflowBegin, kChannelUnused, 0});
            chunk.specials.push_back({end, TagType::MissedEvents, kChannelUnused, info});
            chunk.specials.push_back({end, TagType::OverflowEnd, kChannelUnused, 0});
        } else if (type == 3) {   // END
            ended_ = true;
        }
        update_watermark();
        chunk.watermark = watermark_;
        return;
    }

    // Event.
    if (!started_) throw std::runtime_error("an event record before the START marker");
    const int channel = static_cast<int>((raw >> 61) & 3U);
    const auto coarse_low = static_cast<std::uint16_t>((raw >> 44) & 0xFFFFU);
    const auto ref = static_cast<std::int64_t>((raw >> 20) & 0xFFFFFFU);
    const auto stop = static_cast<std::int64_t>(raw & 0xFFFFFU);
    const auto diff = static_cast<std::int16_t>(static_cast<std::uint16_t>(coarse_low - static_cast<std::uint16_t>(coarse_ & 0xFFFFU)));
    coarse_ = static_cast<std::uint64_t>(static_cast<std::int64_t>(coarse_) + diff);
    if (stop >= static_cast<std::int64_t>(cfg_.divisions)) {
        ++stats_.corrupt;
        return;
    }
    ++stats_.events;
    const std::int64_t divisions = static_cast<std::int64_t>(cfg_.divisions);
    const timestamp_t t_cand = (ref * divisions + stop) * cfg_.lsb_ps;   // GPX2 time modulo one index wrap
    timestamp_t tgpx;
    bool anchor = !have_event_;
    if (have_event_) {
        const std::int64_t dc = static_cast<std::int64_t>(coarse_) - static_cast<std::int64_t>(last_event_coarse_);
        if (dc >= -64 && dc < fast_dc_limit_) {
            // Fast path (consecutive events close in time): the clock ratio's
            // error over dc is far below half a wrap, so predict with ratio 1.
            const timestamp_t predicted = last_tgpx_ + dc * coarse_ps_;
            const timestamp_t x = predicted - t_cand + wrap_ps_ / 2;
            std::int64_t k = static_cast<std::int64_t>(std::floor(static_cast<double>(x) * inv_wrap_ps_));
            const timestamp_t r = x - k * wrap_ps_;
            if (r < 0) --k;
            else if (r >= wrap_ps_) ++k;
            tgpx = t_cand + k * wrap_ps_;
        } else {
            const double dcp = static_cast<double>(dc) * coarse_ps_;
            const double predicted = static_cast<double>(last_tgpx_) + dcp * ratio_;
            const double k = std::round((predicted - static_cast<double>(t_cand)) / static_cast<double>(wrap_ps_));
            tgpx = t_cand + static_cast<std::int64_t>(k) * wrap_ps_;
            const double uncertainty = spread_ps_ + std::fabs(dcp) * ratio_unc_;
            if (uncertainty > kSafeFraction * static_cast<double>(wrap_ps_)) {
                anchor = true;
                ++stats_.reanchors;
            }
        }
    } else {
        tgpx = t_cand;
    }
    if (anchor) {
        // Compact records: the event happened up to coarse_uncertainty_ps after
        // its (marker) coarse time; anchoring at the latest possible time keeps
        // events that completed later but happened earlier after the run start.
        timestamp_t target = static_cast<timestamp_t>(coarse_) * coarse_ps_ + cfg_.coarse_uncertainty_ps - kNominalLatencyPs;
        target = std::max(target, std::max(run_base_, watermark()));
        if (have_event_) target = std::max(target, last_raw_);
        K_ = target - tgpx;
        // A new segment for the clock-ratio measurement: tgpx may sit a wrap
        // away from the previous segment's (that is why this event re-anchors).
        anchor_coarse_ = coarse_;
        anchor_tgpx_ = tgpx;
        have_event_ = true;
    } else if ((stats_.events & 1023U) == 0) {
        // Clock ratio from the run's first event to this one (>= 50 ms apart).
        const double span = (static_cast<double>(coarse_) - static_cast<double>(anchor_coarse_)) * coarse_ps_;
        if (span > 5e10) {
            const double unc = 3.0 * spread_ps_ / span + 1e-9;
            if (unc < ratio_unc_) {
                ratio_ = static_cast<double>(tgpx - anchor_tgpx_) / span;
                ratio_unc_ = unc;
            }
        }
    }
    timestamp_t t = tgpx + K_;
    if (have_ch_[channel] && t < last_raw_ch_[channel]) {   // the GPX2 keeps per-channel order; never emit backwards
        ++stats_.order_fixes;
        t = last_raw_ch_[channel];
    }
    have_ch_[channel] = true;
    last_raw_ch_[channel] = t;
    last_tgpx_ = tgpx;
    last_event_coarse_ = coarse_;
    last_raw_ = t;
    chunk.events[channel].push_back(t);
}

timestamp_t RecordDecoder::watermark() const {
    timestamp_t w;
    if (!have_event_) {
        w = static_cast<timestamp_t>(coarse_) * coarse_ps_ - margin_ps_;
    } else {
        const double dc = std::fabs((static_cast<double>(coarse_) - static_cast<double>(last_event_coarse_)) * coarse_ps_);
        w = marker_time() - margin_ps_ - static_cast<timestamp_t>(dc * ratio_unc_);
    }
    w = std::max(w, run_base_);
    watermark_ = std::max(watermark_, w);
    return watermark_;
}

}  // namespace dt
