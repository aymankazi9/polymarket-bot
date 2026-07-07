#pragma once
#include "clob_client.hpp"
#include "coinbase_client.hpp"
#include "position_manager.hpp"
#include "../signal/shared_state.hpp"
#include "../wallet/key_manager.hpp"
#include "../wallet/clob_auth.hpp"
#include "../wallet/nonce_manager.hpp"
#include "../types/amount.hpp"
#include "../../constants.hpp"
#include <atomic>
#include <chrono>
#include <string>

namespace execution {

// Fires a two-leg IOC taker entry: Polymarket CLOB + Coinbase perp hedge.
//
// Flow (CONTEXT.md §5.2):
//   1. Compute Kelly size + Coinbase hedge quantity.
//   2. Build and sign Polymarket limit-IOC order.
//   3. Submit Polymarket order → get order ID.
//   4. Immediately submit Coinbase IOC hedge.
//   5. Poll both for fill within ~1s.
//   6. If either leg unfilled: cancel/market-unwind the filled leg, return FAILED.
//   7. If both filled: populate EntryData, return SUCCESS.
//
// Thread 3 calls fire() from inside the OrderStateMachine loop.
// ClobClient and CoinbaseClient are owned by OrderStateMachine and shared.

struct TakerConfig {
    std::string  token_id;       // YES token ID (decimal string)
    double       strike_price;   // for hedge direction decision
    bool         is_above;       // true → long YES = short BTC perp
};

enum class TakerResult { SUCCESS, SKIPPED, LEG_FAIL, ERROR };

class TakerArm {
public:
    TakerArm(ClobClient&                 clob,
             CoinbaseClient&             coinbase,
             const wallet::KeyManager&   km,
             const wallet::ClobAuth&     auth,
             wallet::NonceManager&       nonce_mgr,
             signals::SharedState&       ss,
             TakerConfig                 config);

    // Returns SUCCESS and populates `entry_out` on a clean two-leg fill.
    TakerResult fire(Amount bankroll,
                     Amount current_exposure,
                     PositionManager::EntryData& entry_out);

    // Minimum interval between taker entries on the same market.
    bool in_cooldown() const noexcept;
    void reset_cooldown() noexcept;

private:
    ClobClient&                 clob_;
    CoinbaseClient&             coinbase_;
    const wallet::KeyManager&   km_;
    const wallet::ClobAuth&     auth_;
    wallet::NonceManager&       nonce_mgr_;
    signals::SharedState&        ss_;
    TakerConfig                 config_;
    std::chrono::steady_clock::time_point last_entry_{};
};

} // namespace execution
