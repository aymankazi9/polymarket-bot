#pragma once
#include "clob_client.hpp"
#include "position_manager.hpp"
#include "../signal/shared_state.hpp"
#include "../wallet/key_manager.hpp"
#include "../wallet/clob_auth.hpp"
#include "../wallet/nonce_manager.hpp"
#include "../types/amount.hpp"
#include "../constants.hpp"
#include <chrono>
#include <string>

namespace execution {

// Posts and manages passive maker quotes on the Polymarket CLOB.
// CONTEXT.md §5.3.
//
// Activation: regime == SIDEWAYS && edge < E_min_taker
//
// Lifecycle:
//   quote()    — cancel stale quotes if any, post fresh bid and ask, start timer
//   manage()   — called every ~1ms; handles requote, timeout, and fill detection
//   cancel()   — cancel both active quotes (called on state transition)
//
// No Binance hedge for maker positions (cost exceeds rebate, per CONTEXT.md).

struct MakerConfig {
    std::string  token_id;      // YES token ID (decimal string)
    double       half_spread;   // half-spread as price fraction per share (e.g. 0.005 = 0.5¢)
    Amount       size_usdc;     // per-side quote size in USDC
};

enum class MakerStatus {
    ACTIVE,       // quotes are live
    FILLED_BID,   // bid side filled — transition to POSITION_OPEN
    FILLED_ASK,   // ask side filled — transition to POSITION_OPEN
    TIMED_OUT,    // max_quote_age exceeded — go back to IDLE
    REPRICED,     // significant mid move; new quotes submitted
};

class MakerArm {
public:
    MakerArm(ClobClient&               clob,
             const wallet::KeyManager& km,
             const wallet::ClobAuth&   auth,
             wallet::NonceManager&     nonce_mgr,
             signal::SharedState&      ss,
             MakerConfig               config);

    // Submit initial quotes around fair_price.
    // Returns false if submission fails.
    bool quote(double fair_price);

    // Evaluate quotes; returns status.  Call from Thread 3 main loop.
    MakerStatus manage(PositionManager::EntryData& entry_out);

    // Cancel all live quotes.  Safe to call when no quotes are active.
    void cancel_all();

    bool has_active_quotes() const noexcept {
        return !bid_order_id_.empty() || !ask_order_id_.empty();
    }

private:
    bool submit_side(uint8_t side, double price, Amount size_usdc,
                     std::string& order_id_out);
    bool check_fill(const std::string& order_id, double entry_price,
                    bool is_bid, PositionManager::EntryData& entry_out);

    ClobClient&               clob_;
    const wallet::KeyManager& km_;
    const wallet::ClobAuth&   auth_;
    wallet::NonceManager&     nonce_mgr_;
    signal::SharedState&      ss_;
    MakerConfig               config_;

    std::string  bid_order_id_;
    std::string  ask_order_id_;
    double       quoted_mid_    = 0.0;
    double       quoted_fair_   = 0.0;
    std::chrono::steady_clock::time_point quote_time_{};
};

} // namespace execution
