#pragma once
#include "../constants.hpp"
#include "../types/amount.hpp"
#include <algorithm>
#include <cmath>

// Header-only position sizing.  No state, no allocation — pure functions.
// CONTEXT.md §7: fractional Kelly with hard caps.

namespace risk {

// USDC to spend on the Polymarket leg.
// Returns Amount::zero() when edge ≤ 0 or sizing is below any practical minimum.
inline Amount kelly_size_usdc(double p_true,
                               double p_market,
                               Amount bankroll,
                               Amount current_exposure) noexcept {
    double edge = p_true - p_market;
    if (edge <= 0.0) return Amount::zero();
    double odds = (1.0 - p_market) / std::max(p_market, 1e-9);
    double f    = (edge / odds) * constants::KELLY_FRACTION;
    double size = f * bankroll.to_double();
    // Hard caps: per-trade max, and room left under total exposure cap
    Amount room = (constants::MAX_TOTAL_EXPOSURE_USDC - current_exposure).abs();
    if (current_exposure >= constants::MAX_TOTAL_EXPOSURE_USDC)
        return Amount::zero();
    double capped = std::clamp(size,
                               0.0,
                               std::min(constants::MAX_TRADE_USDC.to_double(),
                                        room.to_double()));
    return Amount::from_double(capped);
}

// BTC quantity for the Binance perp hedge, given the Polymarket USDC notional.
// Capped at MAX_HEDGE_FRACTION × bankroll.  Returns raw double (BTC quantity).
inline double hedge_btc_qty(Amount usdc_notional,
                              double btc_price,
                              Amount bankroll) noexcept {
    if (btc_price <= 0.0 || usdc_notional.is_zero()) return 0.0;
    double max_usdc = constants::MAX_HEDGE_FRACTION * bankroll.to_double();
    double capped   = std::min(usdc_notional.to_double(), max_usdc);
    return capped / btc_price;
}

} // namespace risk
