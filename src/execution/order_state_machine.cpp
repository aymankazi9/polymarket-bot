#include "order_state_machine.hpp"
#include "../risk/sizing.hpp"
#include "../wallet/eip712.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

namespace execution {

using namespace std::chrono;

OrderStateMachine::OrderStateMachine(signal::SharedState&      ss,
                                      const wallet::KeyManager& km,
                                      wallet::ClobAuth&         auth,
                                      wallet::NonceManager&     nonce_mgr,
                                      OSMConfig                 config)
    : ss_(ss), km_(km), auth_(auth), nonce_mgr_(nonce_mgr)
    , config_(std::move(config))
    , clob_(config_.clob_base_url)
    , binance_(km.credentials().binance_api_key,
               km.credentials().binance_secret,
               config_.binance_base_url)
    , taker_arm_(clob_, binance_, km_, auth_, nonce_mgr_, ss_,
                 TakerConfig{config_.token_id, config_.strike_price, config_.is_above})
    , maker_arm_(clob_, km_, auth_, nonce_mgr_, ss_,
                 MakerConfig{config_.token_id,
                              config_.maker_half_spread_usdc,
                              config_.maker_size_usdc})
{}

void OrderStateMachine::run() noexcept {
    while (!stop_flag_.load(std::memory_order_relaxed)) {
        if (flatten_flag_.load(std::memory_order_relaxed)) {
            close_position(/*market_order=*/true);
            flatten_flag_.store(false, std::memory_order_relaxed);
        }
        evaluate();
        std::this_thread::sleep_for(milliseconds(1));
    }
}

void OrderStateMachine::stop() noexcept {
    stop_flag_.store(true, std::memory_order_relaxed);
}

void OrderStateMachine::emergency_flatten() noexcept {
    flatten_flag_.store(true, std::memory_order_relaxed);
}

void OrderStateMachine::evaluate() noexcept {
    // Abort all trading when market has expired
    if (time_remaining_s() < 0.0) {
        if (pos_mgr_.is_open()) close_position(/*market_order=*/true);
        return;
    }

    switch (state_.load(std::memory_order_relaxed)) {
        case OSMState::IDLE:          handle_idle();          break;
        case OSMState::TAKER_EVAL:    handle_taker_eval();    break;
        case OSMState::TAKER_PEND:    break;  // poll handled in handle_taker_eval
        case OSMState::MAKER_QUOTED:  handle_maker_quoted();  break;
        case OSMState::POSITION_OPEN: handle_position_open(); break;
        case OSMState::CLOSING:       handle_closing();       break;
    }
}

double OrderStateMachine::time_remaining_s() const noexcept {
    auto now_us = duration_cast<microseconds>(
        system_clock::now().time_since_epoch()).count();
    return (config_.resolution_us - now_us) / 1e6;
}

double OrderStateMachine::compute_e_min_taker() const noexcept {
    // Dynamic half-spread: use live Polymarket spread when available.
    double best_bid = ss_.poly_best_bid.load(std::memory_order_relaxed);
    double best_ask = ss_.poly_best_ask.load(std::memory_order_relaxed);
    double live_half = (best_ask - best_bid) / 2.0;
    double half_spread = (live_half > 0.001 && live_half < 0.5)
        ? live_half
        : constants::HALF_SPREAD_DEFAULT_CENTS / 100.0;

    return (constants::FEE_TAKER_CENTS
          + half_spread * 100.0
          + constants::FRICTION_SETTLEMENT_CENTS
          + constants::ALPHA_BUFFER_CENTS) / 100.0;
}

void OrderStateMachine::handle_idle() noexcept {
    // Close to expiry — ensure hedge is wound down at T-30s
    if (time_remaining_s() < constants::HEDGE_CLOSE_BEFORE_EXPIRY_S) {
        if (pos_mgr_.is_open()) close_position(/*market_order=*/false);
        return;
    }

    if (ss_.open_position_count.load(std::memory_order_relaxed)
            >= constants::MAX_OPEN_POSITIONS) return;

    double p_true   = ss_.p_true.load(std::memory_order_acquire);
    double p_market = ss_.p_market.load(std::memory_order_acquire);
    double edge     = p_true - p_market;

    double e_min = compute_e_min_taker();

    if (edge >= e_min && !taker_arm_.in_cooldown()) {
        transition(OSMState::TAKER_EVAL);
        return;
    }

    // Maker arm: regime SIDEWAYS and small positive edge
    auto regime = ss_.regime.load(std::memory_order_relaxed);
    if (regime == signal::MarketRegime::SIDEWAYS && std::fabs(edge) < e_min) {
        double fair = p_true;
        if (maker_arm_.quote(fair)) {
            transition(OSMState::MAKER_QUOTED);
        }
    }
}

void OrderStateMachine::handle_taker_eval() noexcept {
    double bankroll = ss_.bankroll_usdc.load(std::memory_order_relaxed);
    double exposure = ss_.total_exposure_usdc.load(std::memory_order_relaxed);

    PositionManager::EntryData entry{};
    auto result = taker_arm_.fire(bankroll, exposure, entry);

    switch (result) {
        case TakerResult::SUCCESS:
            pos_mgr_.open(entry);
            ss_.total_exposure_usdc.fetch_add(
                entry.shares * entry.entry_price_poly,
                std::memory_order_relaxed);
            ss_.open_position_count.fetch_add(1, std::memory_order_relaxed);
            transition(OSMState::POSITION_OPEN);
            break;

        case TakerResult::SKIPPED:
        case TakerResult::LEG_FAIL:
        case TakerResult::ERROR:
            transition(OSMState::IDLE);
            break;
    }
}

void OrderStateMachine::handle_position_open() noexcept {
    auto reason = pos_mgr_.evaluate(ss_);
    if (reason != PositionManager::ExitReason::NONE) {
        std::fprintf(stderr, "osm: exit triggered, reason=%d\n", (int)reason);
        bool market = (reason == PositionManager::ExitReason::HARD_STOP);
        close_position(market);
    }
}

void OrderStateMachine::handle_maker_quoted() noexcept {
    // Re-check taker edge — if it's now >= E_min, cancel quotes and fire taker
    double p_true   = ss_.p_true.load(std::memory_order_acquire);
    double p_market = ss_.p_market.load(std::memory_order_acquire);
    double edge     = p_true - p_market;
    if (edge >= compute_e_min_taker() && !taker_arm_.in_cooldown()) {
        maker_arm_.cancel_all();
        transition(OSMState::TAKER_EVAL);
        return;
    }

    PositionManager::EntryData entry{};
    auto status = maker_arm_.manage(entry);
    switch (status) {
        case MakerStatus::FILLED_BID:
        case MakerStatus::FILLED_ASK:
            pos_mgr_.open(entry);
            ss_.total_exposure_usdc.fetch_add(
                entry.shares * entry.entry_price_poly,
                std::memory_order_relaxed);
            ss_.open_position_count.fetch_add(1, std::memory_order_relaxed);
            transition(OSMState::POSITION_OPEN);
            break;

        case MakerStatus::TIMED_OUT:
            transition(OSMState::IDLE);
            break;

        case MakerStatus::ACTIVE:
        case MakerStatus::REPRICED:
            break;
    }
}

void OrderStateMachine::handle_closing() noexcept {
    // Wait for close orders to confirm; for simplicity poll the poly order
    if (!close_poly_order_id_.empty()) {
        auto s = clob_.get_status(close_poly_order_id_);
        if (s.state == ClobClient::OrderState::FILLED ||
            s.state == ClobClient::OrderState::CANCELLED) {
            // Account for fill in shared state
            double usdc_released = pos_mgr_.data().shares
                                 * pos_mgr_.data().entry_price_poly;
            ss_.total_exposure_usdc.fetch_sub(usdc_released,
                                              std::memory_order_relaxed);
            ss_.open_position_count.fetch_sub(1, std::memory_order_relaxed);
            pos_mgr_.close();
            close_poly_order_id_.clear();
            close_binance_order_id_.clear();
            transition(OSMState::IDLE);
        }
    } else {
        // No tracking — just clear state
        double usdc_released = pos_mgr_.data().shares
                             * pos_mgr_.data().entry_price_poly;
        ss_.total_exposure_usdc.fetch_sub(usdc_released,
                                          std::memory_order_relaxed);
        ss_.open_position_count.fetch_sub(1, std::memory_order_relaxed);
        pos_mgr_.close();
        transition(OSMState::IDLE);
    }
}

void OrderStateMachine::close_position(bool market_order) noexcept {
    if (!pos_mgr_.is_open()) {
        transition(OSMState::IDLE);
        return;
    }

    const auto& d = pos_mgr_.data();

    // Close Polymarket leg: submit SELL IOC at current best bid (or slightly below)
    double close_price = ss_.poly_best_bid.load(std::memory_order_relaxed);
    if (close_price <= 0.0) close_price = 0.01;

    uint64_t maker_raw = static_cast<uint64_t>(d.shares * 1e6);
    uint64_t taker_raw = static_cast<uint64_t>(d.shares * close_price * 1e6);

    if (maker_raw > 0 && taker_raw > 0) {
        int64_t exp = duration_cast<seconds>(
            system_clock::now().time_since_epoch()).count()
            + constants::IOC_EXPIRY_SECONDS;

        wallet::Order close_order;
        close_order.salt          = wallet::random_salt();
        close_order.maker         = km_.credentials().wallet_address;
        close_order.signer        = close_order.maker;
        close_order.taker         = {};
        close_order.tokenId       = parse_token_id(config_.token_id);
        close_order.makerAmount   = wallet::u64_to_uint256(maker_raw);
        close_order.takerAmount   = wallet::u64_to_uint256(taker_raw);
        close_order.expiration    = wallet::u64_to_uint256(
                                        static_cast<uint64_t>(exp));
        close_order.nonce         = wallet::u64_to_uint256(nonce_mgr_.next());
        close_order.feeRateBps    = wallet::u64_to_uint256(constants::TAKER_FEE_RATE_BPS);
        close_order.side          = 1;  // SELL
        close_order.signatureType = 0;

        auto digest = wallet::hash_order(close_order);
        auto sig    = km_.sign(digest);
        auto result = clob_.submit_order(close_order, sig, auth_,
                                          market_order ? "IOC" : "IOC");
        close_poly_order_id_ = result.order_id;
    }

    // Close Binance hedge
    if (d.hedge_qty_btc >= 0.001) {
        std::string close_side = config_.is_above ? "BUY" : "SELL";
        double btc_price = ss_.btc_mid.load(std::memory_order_relaxed);
        auto bn = binance_.submit_order(close_side, d.hedge_qty_btc, btc_price);
        close_binance_order_id_ = bn.order_id;
    }

    transition(OSMState::CLOSING);
}

void OrderStateMachine::transition(OSMState next) noexcept {
    state_.store(next, std::memory_order_relaxed);
}

} // namespace execution
