#include "order_state_machine.hpp"
#include "../risk/sizing.hpp"
#include "../types/amount.hpp"
#include "../wallet/eip712.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

namespace execution {

using namespace std::chrono;

OrderStateMachine::OrderStateMachine(signals::SharedState&      ss,
                                      const wallet::KeyManager& km,
                                      wallet::ClobAuth&         auth,
                                      wallet::NonceManager&     nonce_mgr,
                                      OSMConfig                 config)
    : ss_(ss), km_(km), auth_(auth), nonce_mgr_(nonce_mgr)
    , config_(std::move(config))
    , clob_(config_.clob_base_url)
    , coinbase_(km.credentials().coinbase_key_id,
                km.credentials().coinbase_key_secret,
                config_.coinbase_base_url)
    , taker_arm_(clob_, coinbase_, km_, auth_, nonce_mgr_, ss_,
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
        // Exit when the market has been resolved and all positions cleared
        if (time_remaining_s() < -60.0 && !pos_mgr_.is_open()) break;
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
    double t_rem = time_remaining_s();

    // Internal fallback triggers — mirror what scanner callbacks do,
    // in case we hit a deadline between scanner poll intervals.
    if (t_rem <= 120.0 && !entries_stopped_.load(std::memory_order_relaxed))
        entries_stopped_.store(true, std::memory_order_release);
    if (t_rem <= 45.0 && !quotes_cancelled_.load(std::memory_order_relaxed))
        quotes_cancelled_.store(true, std::memory_order_release);
    if (t_rem <= 30.0 && !positions_closed_.load(std::memory_order_relaxed))
        positions_closed_.store(true, std::memory_order_release);
    if (t_rem <= 10.0 && !force_closed_.load(std::memory_order_relaxed))
        force_closed_.store(true, std::memory_order_release);

    // Abort all trading when market has expired
    if (t_rem < 0.0) {
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
    double best_bid  = ss_.poly_best_bid.load(std::memory_order_relaxed);
    double best_ask  = ss_.poly_best_ask.load(std::memory_order_relaxed);
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
    // Safety: if somehow IDLE with a stale open position, clean it up
    if (pos_mgr_.is_open()) {
        close_position(/*market_order=*/false);
        return;
    }

    // No new taker entries after T-120s
    if (entries_stopped_.load(std::memory_order_relaxed)) return;

    if (ss_.open_position_count.load(std::memory_order_relaxed)
            >= constants::MAX_OPEN_POSITIONS) return;

    double p_true   = ss_.p_true.load(std::memory_order_acquire);
    double p_market = ss_.p_market.load(std::memory_order_acquire);
    double edge     = p_true - p_market;
    double e_min    = compute_e_min_taker();

    if (edge >= e_min && !taker_arm_.in_cooldown()) {
        transition(OSMState::TAKER_EVAL);
        return;
    }

    // No new maker quotes after T-45s
    if (quotes_cancelled_.load(std::memory_order_relaxed)) return;

    auto regime = ss_.regime.load(std::memory_order_relaxed);
    if (regime == signals::MarketRegime::SIDEWAYS && std::fabs(edge) < e_min) {
        if (maker_arm_.quote(p_true)) {
            transition(OSMState::MAKER_QUOTED);
        }
    }
}

void OrderStateMachine::handle_taker_eval() noexcept {
    // SharedState stores monetary values as double atomics for lock-free access.
    // Convert to Amount at the boundary before passing into typed execution code.
    Amount bankroll = Amount::from_double(ss_.bankroll_usdc.load(std::memory_order_relaxed));
    Amount exposure = Amount::from_double(ss_.total_exposure_usdc.load(std::memory_order_relaxed));

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
    // T-10s: forced market close takes priority
    if (force_closed_.load(std::memory_order_relaxed)) {
        close_position(/*market_order=*/true);
        return;
    }

    // T-30s: close taker positions with limit order
    if (positions_closed_.load(std::memory_order_relaxed)) {
        close_position(/*market_order=*/false);
        return;
    }

    auto reason = pos_mgr_.evaluate(ss_);
    if (reason != PositionManager::ExitReason::NONE) {
        std::fprintf(stderr, "osm: exit triggered, reason=%d\n", (int)reason);
        bool market = (reason == PositionManager::ExitReason::HARD_STOP);
        close_position(market);
    }
}

void OrderStateMachine::handle_maker_quoted() noexcept {
    // T-45s: cancel all open maker quotes
    if (quotes_cancelled_.load(std::memory_order_relaxed)) {
        maker_arm_.cancel_all();
        // Poll once to capture any fill that raced with the cancel
        PositionManager::EntryData entry{};
        auto status = maker_arm_.manage(entry);
        if (status == MakerStatus::FILLED_BID || status == MakerStatus::FILLED_ASK) {
            handle_late_fill(entry);
        } else {
            transition(OSMState::IDLE);
        }
        return;
    }

    // Re-check taker edge — if >= E_min, cancel quotes and fire taker
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

// A5.2: fill arrived after T-45s quote cancellation.
// Do NOT hedge with Binance — too close to resolution for the hedge to be worth it.
// Exit immediately at break-even if the book allows; otherwise hold to resolution.
void OrderStateMachine::handle_late_fill(const PositionManager::EntryData& entry) noexcept {
    PositionManager::EntryData unhedged = entry;
    unhedged.hedge_qty_btc = 0.0;  // no Coinbase hedge for late fills

    pos_mgr_.open(unhedged);
    ss_.total_exposure_usdc.fetch_add(
        unhedged.shares * unhedged.entry_price_poly,
        std::memory_order_relaxed);
    ss_.open_position_count.fetch_add(1, std::memory_order_relaxed);

    // Break-even: best bid covers entry price plus taker exit fee
    double best_bid        = ss_.poly_best_bid.load(std::memory_order_relaxed);
    double break_even_exit = unhedged.entry_price_poly
                           + (constants::FEE_TAKER_CENTS / 100.0);

    if (best_bid >= break_even_exit) {
        std::fprintf(stderr, "osm[%s]: late fill, break-even exit available (bid=%.4f >= %.4f)\n",
                     config_.condition_id.c_str(), best_bid, break_even_exit);
        close_position(/*market_order=*/false);
    } else {
        std::fprintf(stderr, "osm[%s]: late fill, holding to resolution (bid=%.4f < %.4f)\n",
                     config_.condition_id.c_str(), best_bid, break_even_exit);
        transition(OSMState::POSITION_OPEN);
    }
}

void OrderStateMachine::handle_closing() noexcept {
    if (!close_poly_order_id_.empty()) {
        auto s = clob_.get_status(close_poly_order_id_);
        if (s.state == ClobClient::OrderState::FILLED ||
            s.state == ClobClient::OrderState::CANCELLED) {
            double usdc_released = pos_mgr_.data().shares
                                 * pos_mgr_.data().entry_price_poly;
            ss_.total_exposure_usdc.fetch_sub(usdc_released,
                                              std::memory_order_relaxed);
            ss_.open_position_count.fetch_sub(1, std::memory_order_relaxed);
            pos_mgr_.close();
            close_poly_order_id_.clear();
            close_coinbase_order_id_.clear();
            transition(OSMState::IDLE);
        }
    } else {
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

    // Close Coinbase hedge (skip if late fill — hedge_qty_btc was zeroed)
    if (d.hedge_qty_btc >= 0.001) {
        std::string close_side = config_.is_above ? "BUY" : "SELL";
        double btc_price = ss_.btc_mid.load(std::memory_order_relaxed);
        auto cb = coinbase_.submit_order(close_side, d.hedge_qty_btc, btc_price);
        close_coinbase_order_id_ = cb.order_id;
    }

    transition(OSMState::CLOSING);
}

void OrderStateMachine::transition(OSMState next) noexcept {
    state_.store(next, std::memory_order_relaxed);
}

} // namespace execution
