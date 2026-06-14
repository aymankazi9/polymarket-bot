#pragma once
#include "../constants.hpp"
#include <algorithm>
#include <cmath>

// Header-only position sizing.  No state, no allocation — pure functions.
// CONTEXT.md §7: fractional Kelly with hard caps.

namespace risk {

// USDC to spend on the Polymarket leg.
// Returns 0 when edge ≤ 0 or sizing would be below any practical minimum.
inline double kelly_size_usdc(double p_true,
                               double p_market,
                               double bankroll_usdc,
                               double current_exposure_usdc) noexcept {
    double edge = p_true - p_market;
    if (edge <= 0.0) return 0.0;
    double odds = (1.0 - p_market) / std::max(p_market, 1e-9);
    double f    = (edge / odds) * constants::KELLY_FRACTION;
    double size = f * bankroll_usdc;
    // Hard caps: per-trade max, and room left under total exposure cap
    double room = std::max(0.0, constants::MAX_TOTAL_EXPOSURE_USDC - current_exposure_usdc);
    return std::clamp(size, 0.0, std::min(constants::MAX_TRADE_USDC, room));
}

// BTC quantity for the Binance perp hedge, given the Polymarket USDC notional.
// Capped at MAX_HEDGE_FRACTION × bankroll.
inline double hedge_btc_qty(double usdc_notional,
                              double btc_price,
                              double bankroll_usdc) noexcept {
    if (btc_price <= 0.0 || usdc_notional <= 0.0) return 0.0;
    double max_usdc = constants::MAX_HEDGE_FRACTION * bankroll_usdc;
    double capped   = std::min(usdc_notional, max_usdc);
    return capped / btc_price;
}

} // namespace risk
