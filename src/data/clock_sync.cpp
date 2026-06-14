#include "clock_sync.hpp"
#include <cstring>

namespace data {

ClockSync::ClockSync(double alpha) : alpha_(alpha) {
    for (int i = 0; i < SOURCE_COUNT; ++i) {
        offset_ema_[i].store(0.0, std::memory_order_relaxed);
        initialised_[i].store(false, std::memory_order_relaxed);
    }
}

void ClockSync::update(Source source, int64_t local_us, int64_t exchange_us) noexcept {
    int idx = static_cast<int>(source);
    double sample = static_cast<double>(local_us - exchange_us);

    if (!initialised_[idx].load(std::memory_order_relaxed)) {
        // First sample: seed the EMA directly
        offset_ema_[idx].store(sample, std::memory_order_relaxed);
        initialised_[idx].store(true, std::memory_order_relaxed);
    } else {
        double prev = offset_ema_[idx].load(std::memory_order_relaxed);
        double next = alpha_ * sample + (1.0 - alpha_) * prev;
        offset_ema_[idx].store(next, std::memory_order_relaxed);
    }
}

int64_t ClockSync::offset_us(Source source) const noexcept {
    int idx = static_cast<int>(source);
    if (!initialised_[idx].load(std::memory_order_relaxed))
        return 0;
    return static_cast<int64_t>(
        offset_ema_[idx].load(std::memory_order_relaxed));
}

void ClockSync::reset(Source source) noexcept {
    int idx = static_cast<int>(source);
    offset_ema_[idx].store(0.0, std::memory_order_relaxed);
    initialised_[idx].store(false, std::memory_order_relaxed);
}

} // namespace data
