#include "taker_arm.hpp"
#include "../risk/sizing.hpp"
#include "../wallet/eip712.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <thread>

namespace execution {

using namespace std::chrono;

TakerArm::TakerArm(ClobClient&                 clob,
                    CoinbaseClient&             coinbase,
                    const wallet::KeyManager&   km,
                    const wallet::ClobAuth&     auth,
                    wallet::NonceManager&       nonce_mgr,
                    signals::SharedState&       ss,
                    TakerConfig                 config)
    : clob_(clob), coinbase_(coinbase), km_(km), auth_(auth)
    , nonce_mgr_(nonce_mgr), ss_(ss), config_(std::move(config))
{}

bool TakerArm::in_cooldown() const noexcept {
    if (last_entry_.time_since_epoch().count() == 0) return false;
    auto elapsed = duration_cast<milliseconds>(steady_clock::now() - last_entry_).count();
    return elapsed < static_cast<long long>(constants::MIN_TAKER_INTERVAL_S * 1000.0);
}

void TakerArm::reset_cooldown() noexcept {
    last_entry_ = {};
}

TakerResult TakerArm::fire(Amount bankroll,
                             Amount current_exposure,
                             PositionManager::EntryData& entry_out)
{
    double p_true   = ss_.p_true.load(std::memory_order_acquire);
    double p_market = ss_.p_market.load(std::memory_order_acquire);
    double btc_mid  = ss_.btc_mid.load(std::memory_order_relaxed);

    // Size the Polymarket leg
    Amount usdc_size = risk::kelly_size_usdc(p_true, p_market, bankroll, current_exposure);
    if (usdc_size < Amount::from_double(1.0)) return TakerResult::SKIPPED;

    // Hedge: short BTC perp (bot is long YES = long BTC direction on above markets)
    double hedge_qty = risk::hedge_btc_qty(usdc_size, btc_mid, bankroll);
    if (hedge_qty < 0.001) return TakerResult::SKIPPED;  // below minimum order size

    // Build Polymarket IOC order
    double poly_ask = ss_.poly_best_ask.load(std::memory_order_relaxed);
    if (poly_ask <= 0.0 || poly_ask >= 1.0) return TakerResult::ERROR;

    // poly_units() gives exact 6-decimal integer — no floating-point conversion for USDC
    uint64_t maker_raw = static_cast<uint64_t>(usdc_size.poly_units());
    // shares = USDC / price — division by double is unavoidable here
    uint64_t taker_raw = (poly_ask > 0.0)
        ? static_cast<uint64_t>(static_cast<double>(usdc_size.poly_units()) / poly_ask)
        : 0;
    if (taker_raw == 0) return TakerResult::ERROR;

    int64_t now_s = duration_cast<seconds>(system_clock::now().time_since_epoch()).count();

    wallet::Order order;
    order.salt        = wallet::random_salt();
    order.maker       = km_.credentials().wallet_address;
    order.signer      = order.maker;
    order.taker       = {};  // zero address
    order.tokenId     = parse_token_id(config_.token_id);
    order.makerAmount = wallet::u64_to_uint256(maker_raw);
    order.takerAmount = wallet::u64_to_uint256(taker_raw);
    order.expiration  = wallet::u64_to_uint256(
                            static_cast<uint64_t>(now_s + constants::IOC_EXPIRY_SECONDS));
    order.nonce       = wallet::u64_to_uint256(nonce_mgr_.next());
    order.feeRateBps  = wallet::u64_to_uint256(constants::TAKER_FEE_RATE_BPS);
    order.side        = 0;  // BUY YES
    order.signatureType = 0;

    auto digest = wallet::hash_order(order);
    auto sig    = km_.sign(digest);

    // ---- Fire Polymarket leg ----
    auto poly_result = clob_.submit_order(order, sig, auth_, "IOC");
    if (!poly_result.success) {
        std::fprintf(stderr, "taker: poly submit failed: %s\n", poly_result.error.c_str());
        return TakerResult::ERROR;
    }

    // ---- Fire Coinbase hedge leg (as close to simultaneous as possible) ----
    // is_above=true → long YES = long BTC risk → hedge with SHORT BTC perp
    std::string hedge_side = config_.is_above ? "SELL" : "BUY";
    double hedge_price = config_.is_above
        ? btc_mid * (1.0 - 0.001)  // sell slightly below mid for IOC fill
        : btc_mid * (1.0 + 0.001);

    auto cb_result = coinbase_.submit_order(hedge_side, hedge_qty, hedge_price);

    // ---- Handle leg outcomes ----
    bool poly_filled     = !poly_result.order_id.empty();
    bool coinbase_filled = cb_result.success && cb_result.executed_qty >= hedge_qty * 0.99;

    if (!poly_filled && !coinbase_filled) {
        return TakerResult::LEG_FAIL;
    }

    if (poly_filled && !coinbase_filled) {
        // Unwind poly leg with a market sell (cancel IOC order has already expired; post SELL)
        std::fprintf(stderr, "taker: legging! poly filled but coinbase failed. unwinding poly.\n");
        wallet::Order unwind = order;
        unwind.salt        = wallet::random_salt();
        unwind.side        = 1;  // SELL
        unwind.makerAmount = order.takerAmount;
        unwind.takerAmount = order.makerAmount;
        unwind.nonce       = wallet::u64_to_uint256(nonce_mgr_.next());
        unwind.expiration  = wallet::u64_to_uint256(
                                 static_cast<uint64_t>(now_s + 5));
        auto ud = wallet::hash_order(unwind);
        auto us = km_.sign(ud);
        clob_.submit_order(unwind, us, auth_, "FOK");
        return TakerResult::LEG_FAIL;
    }

    if (!poly_filled && coinbase_filled) {
        std::fprintf(stderr, "taker: legging! coinbase filled but poly failed. unwinding coinbase.\n");
        coinbase_.cancel_order(cb_result.order_id);
        // Close coinbase hedge with a market order in the opposite direction
        std::string close_side = config_.is_above ? "BUY" : "SELL";
        coinbase_.submit_order(close_side, cb_result.executed_qty, 0.0);  // 0.0 → market IOC
        return TakerResult::LEG_FAIL;
    }

    // Both legs filled — populate entry data
    double fill_price_poly = (taker_raw > 0)
        ? static_cast<double>(maker_raw) / static_cast<double>(taker_raw)
        : poly_ask;

    entry_out.entry_price_poly   = fill_price_poly;
    entry_out.shares             = static_cast<double>(taker_raw) / 1e6;
    entry_out.hedge_entry_btc    = cb_result.avg_price > 0 ? cb_result.avg_price : btc_mid;
    entry_out.hedge_qty_btc      = cb_result.executed_qty;
    entry_out.initial_edge_value = Amount::from_double((p_true - p_market) * usdc_size.to_double());
    entry_out.poly_order_id      = poly_result.order_id;
    entry_out.coinbase_order_id  = cb_result.order_id;
    entry_out.is_yes_long        = true;

    last_entry_ = steady_clock::now();
    return TakerResult::SUCCESS;
}

} // namespace execution
