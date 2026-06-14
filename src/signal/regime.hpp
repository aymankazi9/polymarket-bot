#pragma once
#include "shared_state.hpp"
#include "../../constants.hpp"
#include <cstdint>

// Market regime classifier used by Thread 2 (Bayesian engine hot-path).
//
// RollingVolState maintains a time-windowed circular buffer of log-returns
// with O(1) incremental updates via running sum / sum-of-squares.
//
// classify_regime maps the current realized vol to a MarketRegime using the
// thresholds in constants.hpp. Thread 2 calls this after each vol update and
// stores the result in SharedState::regime.

namespace signal {

class RollingVolState {
public:
    // Feed one price sample.  Call once per Tick processed by Thread 2.
    // timestamp_us: wall-clock microseconds (clock-synced).
    // price: BTC mid price from the Tick.
    void update(int64_t timestamp_us, double price) noexcept;

    // Current realized vol (std-dev of log-returns in the rolling window).
    // Returns 0.0 if fewer than 2 samples remain in the window.
    double realized_vol() const noexcept;

    int sample_count() const noexcept { return count_; }

private:
    // 2048-entry ring (power-of-2): covers >200s at 10 ticks/s
    static constexpr int N    = 2048;
    static constexpr int MASK = N - 1;

    struct Entry {
        int64_t ts_us;
        double  log_return;
    };

    Entry  buf_[N]{};
    int    write_  = 0;     // next slot to write
    int    read_   = 0;     // oldest occupied slot
    int    count_  = 0;
    double last_price_    = 0.0;
    double running_sum_   = 0.0;
    double running_sum2_  = 0.0;
};

// Map realized vol (decimal fraction) to a MarketRegime.
MarketRegime classify_regime(double realized_vol_60s) noexcept;

} // namespace signal
