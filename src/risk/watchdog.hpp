#pragma once
#include "../signal/shared_state.hpp"
#include "../../constants.hpp"
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// Thread 4 — Risk watchdog.
//
// Runs at SCHED_FIFO priority (falls back gracefully on EPERM).
// Evaluates all circuit-breaker conditions independently of Thread 3:
//
//   CB1 — Consecutive losses  : consecutive_losses >= CB_MAX_CONSECUTIVE_LOSSES
//                                AND streak started within CB_LOSS_WINDOW_S
//   CB2 — BTC volatility spike : |max_btc_60s - min_btc_60s| / min > CB_BTC_SPIKE_THRESHOLD
//   CB3 — Hourly NAV drawdown  : bankroll drops > CB_NAV_DRAWDOWN_THRESHOLD in CB_NAV_DRAWDOWN_WINDOW_S
//
// On trip: sets SharedState::kill_switch = true, calls all FlattenCallback functions,
// logs reason to stderr.  Manual reset required.
//
// Guardrail (not a circuit breaker):
//   kill_switch is set when bankroll < initial_bankroll * CEX_MARGIN_FLOOR_FRACTION.
//   Thread 3 checks this before opening new taker positions.

namespace risk {

class RiskWatchdog {
public:
    using FlattenCallback = std::function<void()>;

    RiskWatchdog(signals::SharedState&          ss,
                 std::vector<FlattenCallback>  flatten_all,
                 double                        initial_bankroll_usdc);

    // Thread 4 entry point.  Blocks until stop() is called.
    void run()  noexcept;
    void stop() noexcept;

private:
    void evaluate()          noexcept;
    void check_consecutive_losses() noexcept;
    void check_btc_spike()   noexcept;
    void check_nav_drawdown()noexcept;
    void record_btc(double price, int64_t ts_us) noexcept;
    void trip(const char* reason) noexcept;

    signals::SharedState&         ss_;
    std::vector<FlattenCallback> flatten_cbs_;
    std::atomic<bool>            stop_flag_{false};

    double  initial_bankroll_;
    bool    tripped_ = false;

    // Rolling 60s BTC price buffer (for spike check)
    static constexpr int BTC_BUF_N = 256;
    struct BtcEntry { int64_t ts_us; double price; };
    BtcEntry btc_buf_[BTC_BUF_N]{};
    int btc_write_ = 0, btc_read_ = 0, btc_count_ = 0;

    // Rolling 1h NAV tracking
    int64_t nav_window_start_us_  = 0;
    double  nav_window_start_usdc_ = 0.0;
};

} // namespace risk
