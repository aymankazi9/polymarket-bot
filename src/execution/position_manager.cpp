#include "position_manager.hpp"
#include <algorithm>
#include <cmath>

namespace execution {

void PositionManager::open(const EntryData& d) noexcept {
    data_            = d;
    peak_pnl_        = Amount::zero();
    trailing_active_ = false;
    open_            = true;
}

void PositionManager::close() noexcept {
    open_            = false;
    trailing_active_ = false;
    peak_pnl_        = Amount::zero();
}

Amount PositionManager::combined_pnl_usdc(const signals::SharedState& ss) const noexcept {
    if (!open_) return Amount::zero();

    // Exit price for the Polymarket leg: best_bid if long YES, best_ask if long NO
    double poly_exit;
    if (data_.is_yes_long)
        poly_exit = ss.poly_best_bid.load(std::memory_order_relaxed);
    else
        poly_exit = 1.0 - ss.poly_best_ask.load(std::memory_order_relaxed);

    double poly_pnl     = (poly_exit - data_.entry_price_poly) * data_.shares;
    double coinbase_pnl = (data_.hedge_entry_btc - ss.btc_mid.load(std::memory_order_relaxed))
                        * data_.hedge_qty_btc;
    return Amount::from_double(poly_pnl + coinbase_pnl);
}

PositionManager::ExitReason PositionManager::evaluate(
    const signals::SharedState& ss) noexcept
{
    if (!open_) return ExitReason::NONE;

    Amount pnl = combined_pnl_usdc(ss);

    // 1. Hard stop
    Amount hard_stop = Amount::from_double(
        constants::HARD_STOP_MULTIPLE * data_.initial_edge_value.to_double());
    if (pnl < hard_stop)
        return ExitReason::HARD_STOP;

    // 2. Trailing stop
    Amount trigger = Amount::from_double(
        constants::TRAILING_PROFIT_TRIGGER * data_.initial_edge_value.to_double());
    if (!trailing_active_ && pnl >= trigger) {
        trailing_active_ = true;
    }
    if (trailing_active_) {
        if (pnl > peak_pnl_) peak_pnl_ = pnl;
        if (peak_pnl_.is_positive()) {
            Amount floor = Amount::from_double(
                peak_pnl_.to_double() * (1.0 - constants::TRAILING_STOP_FRACTION));
            if (pnl < floor)
                return ExitReason::TRAILING_STOP;
        }
    }

    // 3. Early exit: break-even or better (after sunk entry cost)
    if (pnl >= Amount::zero())
        return ExitReason::EARLY_EXIT;

    return ExitReason::NONE;
}

} // namespace execution
