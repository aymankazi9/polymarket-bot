#include "bayesian_engine.hpp"
#include "likelihood.hpp"
#include "../data/feed_manager.hpp"

#include <algorithm>
#include <cmath>
#include <thread>

namespace signal {

BayesianEngine::BayesianEngine(data::TickBuffer&        ring,
                                SharedState&             state,
                                const data::FeedManager& feed,
                                MarketConfig             config)
    : ring_(ring), state_(state), feed_(feed), config_(std::move(config))
{}

void BayesianEngine::run() noexcept {
    data::Tick tick;
    while (!stop_flag_.load(std::memory_order_relaxed)) {
        if (ring_.pop(tick)) {
            process_tick(tick);
        } else {
            std::this_thread::yield();
        }
    }
}

void BayesianEngine::stop() noexcept {
    stop_flag_.store(true, std::memory_order_relaxed);
}

void BayesianEngine::process_tick(const data::Tick& tick) noexcept {
    using data::Source;

    if (tick.source == Source::COINBASE) {
        // Track cross-venue mid before triggering an update
        if (tick.mid > 0.0) {
            if (prev_cb_mid_ > 0.0 && btc_mid_ > 0.0)
                run_bayesian_update(tick);
            prev_cb_mid_ = tick.mid;
        }
        return;
    }

    if (tick.source == Source::POLYMARKET) {
        // Update the market mid and optionally reset the prior
        if (tick.mid > 0.0 && tick.mid < 1.0) {
            state_.p_market.store(tick.mid, std::memory_order_release);
            state_.poly_best_bid.store(tick.best_bid, std::memory_order_release);
            state_.poly_best_ask.store(tick.best_ask, std::memory_order_release);

            bool first_reset = (last_reset_us_ == 0);
            bool interval_elapsed =
                tick.timestamp_us - last_reset_us_ >=
                static_cast<int64_t>(constants::PRIOR_RESET_INTERVAL_S * 1e6);

            if (first_reset || interval_elapsed) {
                p_prior_       = tick.mid;
                last_reset_us_ = tick.timestamp_us;
            }
        }
        return;
    }

    // Source::BINANCE — primary price + vol feed
    if (tick.source == Source::BINANCE && tick.mid > 0.0) {
        btc_mid_ = tick.mid;
        state_.btc_mid.store(tick.mid, std::memory_order_release);

        vol_state_.update(tick.timestamp_us, tick.mid);
        double vol = vol_state_.realized_vol();
        state_.realized_vol_60s.store(vol, std::memory_order_release);
        state_.regime.store(classify_regime(vol), std::memory_order_release);

        run_bayesian_update(tick);
    }
}

void BayesianEngine::run_bayesian_update(const data::Tick& tick) noexcept {
    if (btc_mid_ <= 0.0 || config_.strike_price <= 0.0) return;

    double vol = state_.realized_vol_60s.load(std::memory_order_relaxed);
    if (vol <= 0.0) return;

    double time_remaining_s =
        (static_cast<double>(config_.resolution_us) -
         static_cast<double>(tick.timestamp_us)) / 1e6;
    if (time_remaining_s <= 0.0) return;

    // L_yes = P(data | YES resolves), L_no = P(data | NO resolves)
    double L_yes = compute_likelihood(btc_mid_, config_.strike_price, time_remaining_s,
                                      vol, tick.ob_imbalance,  config_.is_above);
    double L_no  = compute_likelihood(btc_mid_, config_.strike_price, time_remaining_s,
                                      vol, tick.ob_imbalance, !config_.is_above);

    // Apply auxiliary log-likelihood boost to L_yes (§3.4)
    double L_yes_eff = L_yes * std::exp(compute_aux_log_adj(tick));

    // Bayesian normalisation:  P(YES|data) = P_prior * L_yes / Z
    double Z = p_prior_ * L_yes_eff + (1.0 - p_prior_) * L_no;
    if (Z < 1e-10) return;

    double p_post = std::clamp(p_prior_ * L_yes_eff / Z, 0.01, 0.99);
    p_prior_ = p_post;
    state_.p_true.store(p_post, std::memory_order_release);
}

double BayesianEngine::compute_aux_log_adj(const data::Tick& tick) noexcept {
    constexpr double MAX = constants::MAX_AUX_LOG_LIKELIHOOD_ADJ;
    double total = 0.0;

    // 1. Funding rate: positive funding means longs paying → crowded long → bullish
    //    Typical range ±0.001 (±0.1% per 8h); scale so 0.001 → ±MAX.
    double fr = feed_.last_funding_rate();
    total += std::clamp(fr * 100.0, -MAX, MAX);

    // 2. OB imbalance: bid-heavy book signals buying pressure
    total += std::clamp(tick.ob_imbalance * MAX, -MAX, MAX);

    // 3. Coinbase spot delta vs last observed Coinbase mid (cross-venue confirmation)
    //    Scale: 0.1% relative move → ±MAX.
    if (prev_cb_mid_ > 0.0 && btc_mid_ > 0.0) {
        double rel_delta = (btc_mid_ - prev_cb_mid_) / prev_cb_mid_;
        total += std::clamp(rel_delta / 0.001 * MAX, -MAX, MAX);
    }

    // Flip sign on "below" markets: bullish BTC → bearish for YES
    return config_.is_above ? total : -total;
}

} // namespace signal
