#pragma once
#include "shared_state.hpp"
#include "regime.hpp"
#include "../data/ring_buffer.hpp"
#include "../data/tick.hpp"
#include "../../constants.hpp"
#include <atomic>
#include <cstdint>
#include <string>

// Forward-declare to keep Boost.Beast/Asio headers out of this header.
namespace data { class FeedManager; }

namespace signal {

// Configuration for one active Polymarket prediction market.
struct MarketConfig {
    std::string token_id;       // YES token ID (used to route Polymarket ticks)
    double      strike_price;   // BTC threshold price
    int64_t     resolution_us;  // expected resolution time (Unix microseconds)
    bool        is_above;       // true  → YES wins if BTC ends above strike
                                // false → YES wins if BTC ends below strike
};

// Thread 2 hot-path.  Consumes Ticks from the SPSC ring, applies Bayesian
// updates, and writes results to SharedState atomics.
//
// Prior is seeded from p_market (Polymarket CLOB mid) and resets to it every
// PRIOR_RESET_INTERVAL_S seconds.  Between resets the prior evolves tick by tick.
//
// Auxiliary log-likelihood adjustments (§3.4):
//   - Binance funding rate: positive funding → bullish → +adj on YES for above markets
//   - OB imbalance: bid-heavy book → bullish → +adj
//   - Coinbase spot delta: cross-venue confirmation
// Each contribution is capped at ±MAX_AUX_LOG_LIKELIHOOD_ADJ (0.10).
class BayesianEngine {
public:
    BayesianEngine(data::TickBuffer&        ring,
                   SharedState&             state,
                   const data::FeedManager& feed,
                   MarketConfig             config);

    void run()    noexcept;  // blocks Thread 2; loops until stop()
    void stop()   noexcept;  // safe to call from any thread

private:
    void   process_tick(const data::Tick& tick)  noexcept;
    void   run_bayesian_update(const data::Tick& tick) noexcept;
    double compute_aux_log_adj(const data::Tick& tick) noexcept;

    data::TickBuffer&        ring_;
    SharedState&             state_;
    const data::FeedManager& feed_;
    MarketConfig             config_;

    RollingVolState   vol_state_;
    std::atomic<bool> stop_flag_{false};

    double  p_prior_       = 0.5;
    double  btc_mid_       = 0.0;
    double  prev_cb_mid_   = 0.0;  // last Coinbase mid, for cross-venue delta
    int64_t last_reset_us_ = 0;    // timestamp of last prior reset
};

} // namespace signal
