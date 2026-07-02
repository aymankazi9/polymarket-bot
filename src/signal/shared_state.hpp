#pragma once
#include <atomic>
#include <cstdint>

// Shared state written by Thread 2 (Bayesian engine) and read by
// Threads 3 (order state machine) and 4 (risk watchdog).
//
// All fields are std::atomic.  On x86-64, double atomics are lock-free.
// Use memory_order_release on writes and memory_order_acquire on reads
// where causality matters; relaxed suffices for gauge-style reads.

namespace signals {

enum class MarketRegime : uint8_t {
    SIDEWAYS  = 0,  // realized_vol_60s < 0.15% — maker arm eligible
    TRENDING  = 1,  // < 0.50% — taker only, reduced size
    VOLATILE  = 2,  // < 1.50% — taker only, min size or skip
    SPIKE     = 3,  // >= 1.50% — circuit breaker may fire
};

struct SharedState {
    std::atomic<double>       p_true{0.5};          // model probability (Bayesian posterior)
    std::atomic<double>       p_market{0.5};         // latest Polymarket CLOB YES mid
    std::atomic<double>       btc_mid{0.0};          // latest BTC mid price
    std::atomic<double>       realized_vol_60s{0.0}; // 60s realized vol (decimal fraction)
    std::atomic<MarketRegime> regime{MarketRegime::SIDEWAYS};

    // Polymarket order book — written by BayesianEngine, read by OrderStateMachine
    std::atomic<double> poly_best_bid{0.5};
    std::atomic<double> poly_best_ask{0.5};

    // Accounting — written by OrderStateMachine / RiskWatchdog
    std::atomic<double> bankroll_usdc{0.0};
    std::atomic<double> total_exposure_usdc{0.0};
    std::atomic<int>    open_position_count{0};

    // Circuit breaker — Thread 4 sets true; Thread 3 reads and halts on true.
    // Manual reset required.
    std::atomic<bool>    kill_switch{false};

    // Loss streak tracking — Thread 3 writes, Thread 4 reads
    std::atomic<int>     consecutive_losses{0};
    std::atomic<int64_t> first_loss_streak_us{0};  // epoch micros of streak start

    // Not copyable — owns atomic members
    SharedState() = default;
    SharedState(const SharedState&) = delete;
    SharedState& operator=(const SharedState&) = delete;
};

} // namespace signals
