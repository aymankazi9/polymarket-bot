#pragma once
#include "clob_client.hpp"
#include "binance_client.hpp"
#include "taker_arm.hpp"
#include "maker_arm.hpp"
#include "position_manager.hpp"
#include "../signal/shared_state.hpp"
#include "../constants.hpp"
#include "../wallet/key_manager.hpp"
#include "../wallet/clob_auth.hpp"
#include "../wallet/nonce_manager.hpp"
#include <atomic>
#include <string>

// Per-market order state machine. Thread 3 runs one OSM per active market.
//
// States (CONTEXT.md §5.1):
//   IDLE        — no position, no quotes; evaluating edge on each tick
//   TAKER_EVAL  — about to fire taker; momentary transition state
//   TAKER_PEND  — taker orders submitted; waiting for fill confirmation
//   MAKER_QUOTED — passive bid/ask live on the book
//   POSITION_OPEN — two-leg position active; monitoring exit conditions
//   CLOSING     — close orders submitted; waiting for confirmation
//
// The machine reads from SharedState (written by Thread 2) and
// writes back bankroll / exposure / position_count updates.

namespace execution {

enum class OSMState {
    IDLE,
    TAKER_EVAL,
    TAKER_PEND,
    MAKER_QUOTED,
    POSITION_OPEN,
    CLOSING,
};

struct OSMConfig {
    // Market identity
    std::string  token_id;     // YES token decimal string
    double       strike_price;
    int64_t      resolution_us;
    bool         is_above;     // true → YES wins if BTC > strike

    // REST endpoints (defaults point at Polymarket / Binance mainnet)
    std::string clob_base_url    = "https://clob.polymarket.com";
    std::string binance_base_url = "https://fapi.binance.com";

    // Maker arm parameters
    double maker_half_spread_usdc = 0.005;  // 0.5¢
    double maker_size_usdc        = constants::MAX_MAKER_QUOTE_USDC;
};

class OrderStateMachine {
public:
    OrderStateMachine(signal::SharedState&      ss,
                      const wallet::KeyManager& km,
                      wallet::ClobAuth&         auth,
                      wallet::NonceManager&     nonce_mgr,
                      OSMConfig                 config);

    // Thread 3 entry point.  Blocks until stop() is called.
    void run()  noexcept;
    void stop() noexcept;

    // Kill-switch: close all open positions and halt.  Called by Thread 4.
    void emergency_flatten() noexcept;

    OSMState state() const noexcept { return state_.load(std::memory_order_relaxed); }

private:
    void evaluate() noexcept;

    // State handlers
    void handle_idle()         noexcept;
    void handle_taker_eval()   noexcept;
    void handle_position_open()noexcept;
    void handle_maker_quoted() noexcept;
    void handle_closing()      noexcept;

    void transition(OSMState next) noexcept;
    void close_position(bool market_order) noexcept;

    double time_remaining_s() const noexcept;
    double compute_e_min_taker() const noexcept;

    signal::SharedState&      ss_;
    const wallet::KeyManager& km_;
    wallet::ClobAuth&         auth_;
    wallet::NonceManager&     nonce_mgr_;
    OSMConfig                 config_;

    // Owned sub-components
    ClobClient    clob_;
    BinanceClient binance_;
    TakerArm      taker_arm_;
    MakerArm      maker_arm_;
    PositionManager pos_mgr_;

    std::atomic<OSMState> state_{OSMState::IDLE};
    std::atomic<bool>     stop_flag_{false};
    std::atomic<bool>     flatten_flag_{false};

    // Closing leg tracking
    std::string close_poly_order_id_;
    std::string close_binance_order_id_;
};

} // namespace execution
