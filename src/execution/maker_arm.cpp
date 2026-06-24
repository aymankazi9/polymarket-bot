#include "maker_arm.hpp"
#include "../wallet/eip712.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <thread>

namespace execution {

using namespace std::chrono;

MakerArm::MakerArm(ClobClient&               clob,
                    const wallet::KeyManager& km,
                    const wallet::ClobAuth&   auth,
                    wallet::NonceManager&     nonce_mgr,
                    signal::SharedState&      ss,
                    MakerConfig               config)
    : clob_(clob), km_(km), auth_(auth)
    , nonce_mgr_(nonce_mgr), ss_(ss), config_(std::move(config))
{}

bool MakerArm::submit_side(uint8_t side, double price, Amount size_usdc,
                             std::string& order_id_out)
{
    uint64_t maker_raw, taker_raw;
    if (side == 0) {
        // BUY: offer USDC, receive shares
        // poly_units() gives exact 6-decimal integer — no FP conversion for USDC amount
        maker_raw = static_cast<uint64_t>(size_usdc.poly_units());
        taker_raw = (price > 0)
            ? static_cast<uint64_t>(static_cast<double>(size_usdc.poly_units()) / price)
            : 0;
    } else {
        // SELL: offer shares, receive USDC
        maker_raw = (price > 0)
            ? static_cast<uint64_t>(static_cast<double>(size_usdc.poly_units()) / price)
            : 0;
        taker_raw = static_cast<uint64_t>(size_usdc.poly_units());
    }
    if (maker_raw == 0 || taker_raw == 0) return false;

    int64_t expiry = duration_cast<seconds>(
        system_clock::now().time_since_epoch()).count() + 1800;  // 30-min GTC

    wallet::Order order;
    order.salt          = wallet::random_salt();
    order.maker         = km_.credentials().wallet_address;
    order.signer        = order.maker;
    order.taker         = {};
    order.tokenId       = parse_token_id(config_.token_id);
    order.makerAmount   = wallet::u64_to_uint256(maker_raw);
    order.takerAmount   = wallet::u64_to_uint256(taker_raw);
    order.expiration    = wallet::u64_to_uint256(static_cast<uint64_t>(expiry));
    order.nonce         = wallet::u64_to_uint256(nonce_mgr_.next());
    order.feeRateBps    = wallet::u64_to_uint256(constants::MAKER_FEE_RATE_BPS);
    order.side          = side;
    order.signatureType = 0;

    auto digest = wallet::hash_order(order);
    auto sig    = km_.sign(digest);

    auto result = clob_.submit_order(order, sig, auth_, "GTC");
    if (!result.success) {
        std::fprintf(stderr, "maker: submit side=%d failed: %s\n",
                     side, result.error.c_str());
        return false;
    }
    order_id_out = result.order_id;
    return true;
}

bool MakerArm::quote(double fair_price) {
    cancel_all();

    double bid = fair_price - config_.half_spread;
    double ask = fair_price + config_.half_spread;

    if (bid <= 0.01 || ask >= 0.99) return false;  // too close to boundaries

    bool ok_bid = submit_side(0, bid, config_.size_usdc, bid_order_id_);
    bool ok_ask = submit_side(1, ask, config_.size_usdc, ask_order_id_);

    if (!ok_bid && !ok_ask) return false;
    if (!ok_bid) bid_order_id_.clear();
    if (!ok_ask) ask_order_id_.clear();

    quoted_fair_ = fair_price;
    quoted_mid_  = ss_.p_market.load(std::memory_order_relaxed);
    quote_time_  = steady_clock::now();
    return true;
}

void MakerArm::cancel_all() {
    if (!bid_order_id_.empty()) {
        clob_.cancel_order(bid_order_id_, auth_);
        bid_order_id_.clear();
    }
    if (!ask_order_id_.empty()) {
        clob_.cancel_order(ask_order_id_, auth_);
        ask_order_id_.clear();
    }
}

bool MakerArm::check_fill(const std::string& order_id, double entry_price,
                            bool is_bid, PositionManager::EntryData& entry_out)
{
    if (order_id.empty()) return false;
    auto s = clob_.get_status(order_id);
    if (s.state != ClobClient::OrderState::FILLED) return false;

    entry_out.entry_price_poly   = s.avg_price > 0 ? s.avg_price : entry_price;
    entry_out.shares             = config_.size_usdc.to_double() / entry_out.entry_price_poly;
    entry_out.hedge_entry_btc    = 0.0;  // no hedge on maker arm
    entry_out.hedge_qty_btc      = 0.0;
    // expected profit = half_spread (per share) × shares = USDC value
    entry_out.initial_edge_value = Amount::from_double(
        config_.half_spread * entry_out.shares);
    entry_out.poly_order_id      = order_id;
    entry_out.binance_order_id   = "";
    entry_out.is_yes_long        = is_bid;
    return true;
}

MakerStatus MakerArm::manage(PositionManager::EntryData& entry_out) {
    auto now  = steady_clock::now();
    auto aged = duration_cast<milliseconds>(now - quote_time_).count();

    // Timeout
    if (aged > static_cast<long long>(constants::MAKER_QUOTE_MAX_AGE_MS)) {
        cancel_all();
        return MakerStatus::TIMED_OUT;
    }

    // Check for significant mid move → requote
    double current_mid = ss_.p_market.load(std::memory_order_relaxed);
    double move_bps = std::fabs(current_mid - quoted_mid_) / quoted_mid_ * 10000.0;
    if (move_bps > constants::MAKER_REQUOTE_THRESHOLD_BPS) {
        double new_fair = ss_.p_true.load(std::memory_order_relaxed);
        bool ok = quote(new_fair);
        return ok ? MakerStatus::REPRICED : MakerStatus::TIMED_OUT;
    }

    // Check for fills (poll order status)
    double bid_price = quoted_fair_ - config_.half_spread;
    double ask_price = quoted_fair_ + config_.half_spread;

    if (check_fill(bid_order_id_, bid_price, true,  entry_out)) {
        ask_order_id_.clear();  // cancel ask side (best-effort; it will expire)
        cancel_all();
        return MakerStatus::FILLED_BID;
    }
    if (check_fill(ask_order_id_, ask_price, false, entry_out)) {
        bid_order_id_.clear();
        cancel_all();
        return MakerStatus::FILLED_ASK;
    }

    return MakerStatus::ACTIVE;
}

} // namespace execution
