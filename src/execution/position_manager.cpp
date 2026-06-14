#include "position_manager.hpp"
#include <algorithm>
#include <cmath>

namespace execution {

void PositionManager::open(const EntryData& d) noexcept {
    data_            = d;
    peak_pnl_        = 0.0;
    trailing_active_ = false;
    open_            = true;
}

void PositionManager::close() noexcept {
    open_            = false;
    trailing_active_ = false;
    peak_pnl_        = 0.0;
}

double PositionManager::combined_pnl_usdc(const signal::SharedState& ss) const noexcept {
    if (!open_) return 0.0;

    // Exit price for the Polymarket leg: best_bid if long YES, best_ask if long NO
    double poly_exit;
    if (data_.is_yes_long)
        poly_exit = ss.poly_best_bid.load(std::memory_order_relaxed);
    else
        poly_exit = 1.0 - ss.poly_best_ask.load(std::memory_order_relaxed);

    double poly_pnl    = (poly_exit - data_.entry_price_poly) * data_.shares;
    double binance_pnl = (data_.hedge_entry_btc - ss.btc_mid.load(std::memory_order_relaxed))
                       * data_.hedge_qty_btc;
    return poly_pnl + binance_pnl;
}

PositionManager::ExitReason PositionManager::evaluate(
    const signal::SharedState& ss) noexcept
{
    if (!open_) return ExitReason::NONE;

    double pnl = combined_pnl_usdc(ss);

    // 1. Hard stop
    if (pnl < constants::HARD_STOP_MULTIPLE * data_.initial_edge_value)
        return ExitReason::HARD_STOP;

    // 2. Trailing stop
    if (!trailing_active_ &&
        pnl >= constants::TRAILING_PROFIT_TRIGGER * data_.initial_edge_value)
    {
        trailing_active_ = true;
    }
    if (trailing_active_) {
        peak_pnl_ = std::max(peak_pnl_, pnl);
        if (peak_pnl_ > 0.0 &&
            pnl < peak_pnl_ * (1.0 - constants::TRAILING_STOP_FRACTION))
        {
            return ExitReason::TRAILING_STOP;
        }
    }

    // 3. Early exit: break-even or better (after sunk entry cost)
    if (pnl >= 0.0)
        return ExitReason::EARLY_EXIT;

    return ExitReason::NONE;
}

} // namespace execution
