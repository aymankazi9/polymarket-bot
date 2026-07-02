#pragma once
#include "../signal/shared_state.hpp"
#include "../../constants.hpp"
#include "../types/amount.hpp"
#include <string>

// Tracks one open two-leg position (Polymarket + Binance perp) and evaluates
// exit conditions on every evaluation tick.  CONTEXT.md §5.4.
//
// PnL is computed in USDC across both legs:
//   poly_pnl   = (current_exit_price - entry_price_poly) * shares
//   binance_pnl = (hedge_entry_btc   - current_btc_mid)  * hedge_qty_btc
//
// Exit conditions (evaluated in priority order):
//   1. Hard stop:     combined_pnl < HARD_STOP_MULTIPLE * initial_edge_value
//   2. Trailing stop: arms when pnl >= TRAILING_PROFIT_TRIGGER * initial_edge_value;
//                     fires when pnl retraces TRAILING_STOP_FRACTION from peak
//   3. Early exit:    combined_pnl >= 0 (break-even or better after entry fees are sunk)

namespace execution {

class PositionManager {
public:
    enum class ExitReason { NONE, HARD_STOP, TRAILING_STOP, EARLY_EXIT, RESOLUTION };

    struct EntryData {
        double      entry_price_poly;   // USDC per share / probability (0-1, not monetary)
        double      shares;             // YES token quantity (not monetary)
        double      hedge_entry_btc;    // BTC perp entry price (not monetary)
        double      hedge_qty_btc;      // BTC perp quantity (not monetary)
        Amount      initial_edge_value; // edge × usdc_spent at entry (USDC)
        std::string poly_order_id;
        std::string binance_order_id;
        bool        is_yes_long;        // true = long YES, false = long NO

        EntryData() : entry_price_poly(0.0), shares(0.0), hedge_entry_btc(0.0),
                      hedge_qty_btc(0.0), initial_edge_value(Amount::zero()),
                      is_yes_long(false) {}
    };

    PositionManager() = default;

    void open(const EntryData& data) noexcept;
    void close() noexcept;
    bool is_open() const noexcept { return open_; }

    // Read current SharedState and evaluate exit conditions.
    ExitReason evaluate(const signal::SharedState& ss) noexcept;

    Amount combined_pnl_usdc(const signal::SharedState& ss) const noexcept;

    const EntryData& data() const noexcept { return data_; }

private:
    bool      open_             = false;
    EntryData data_{};
    Amount    peak_pnl_         = Amount::zero();
    bool      trailing_active_  = false;
};

} // namespace execution
