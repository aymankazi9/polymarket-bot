#include "regime.hpp"
#include <cmath>

namespace signals {

void RollingVolState::update(int64_t timestamp_us, double price) noexcept {
    if (price <= 0.0) return;

    if (last_price_ > 0.0) {
        double ret = std::log(price / last_price_);

        // If the ring is full, evict the oldest entry before writing
        if (count_ == N) {
            const Entry& old = buf_[read_];
            running_sum_  -= old.log_return;
            running_sum2_ -= old.log_return * old.log_return;
            read_ = (read_ + 1) & MASK;
            --count_;
        }

        buf_[write_] = {timestamp_us, ret};
        write_ = (write_ + 1) & MASK;
        running_sum_  += ret;
        running_sum2_ += ret * ret;
        ++count_;
    }

    last_price_ = price;

    // Expire entries older than REALIZED_VOL_WINDOW_S
    int64_t cutoff_us = timestamp_us
        - static_cast<int64_t>(constants::REALIZED_VOL_WINDOW_S * 1e6);

    while (count_ > 0 && buf_[read_].ts_us < cutoff_us) {
        const double r = buf_[read_].log_return;
        running_sum_  -= r;
        running_sum2_ -= r * r;
        read_ = (read_ + 1) & MASK;
        --count_;
    }
}

double RollingVolState::realized_vol() const noexcept {
    if (count_ < 2) return 0.0;

    double mean    = running_sum_ / count_;
    double variance = running_sum2_ / count_ - mean * mean;
    if (variance <= 0.0) return 0.0;

    // Returns per-sample std-dev; caller interprets as 60s fractional vol
    return std::sqrt(variance);
}

MarketRegime classify_regime(double vol) noexcept {
    if (vol < constants::VOL_SIDEWAYS_THRESHOLD) return MarketRegime::SIDEWAYS;
    if (vol < constants::VOL_TRENDING_THRESHOLD) return MarketRegime::TRENDING;
    if (vol < constants::VOL_VOLATILE_THRESHOLD) return MarketRegime::VOLATILE;
    return MarketRegime::SPIKE;
}

} // namespace signals
