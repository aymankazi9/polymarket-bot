#pragma once
#include <cstdint>
#include "src/types/amount.hpp"

// All trading constants. Every threshold, limit, and tunable parameter lives here.
// Tune by recompile only — no runtime overrides, no env-var fallbacks.

namespace constants {

// ---------------------------------------------------------------------------
// RPC endpoints  (CONTEXT_ADDENDUM A1.2)
// Primary: Alchemy free tier — 300M compute units/month on Polygon, permanent.
// Fallback: public polygon-rpc.com — rate-limited, used only if primary fails.
// ---------------------------------------------------------------------------
// Replace <YOUR_KEY> with your Alchemy project key before building.
inline constexpr char RPC_PRIMARY[]  = "https://polygon-mainnet.g.alchemy.com/v2/GDGDDWXXD03eSte0IseXw";
inline constexpr char RPC_FALLBACK[] = "https://polygon-rpc.com";
constexpr int         RPC_TIMEOUT_MS = 3000;  // retry on fallback after 3s

// ---------------------------------------------------------------------------
// CEX instrument  (CONTEXT_ADDENDUM A6)
// Coinbase Advanced Trade — US CFTC-regulated nano BTC perpetual futures, USDC-margined.
// Hedge orders: POST https://api.coinbase.com/api/v3/brokerage/orders
//
// IMPORTANT: verify COINBASE_HEDGE_SYMBOL via authenticated
//   GET /api/v3/brokerage/cfm/products
// before going live.  "BTC-PERP" is the expected product_id for the US CFM
// nano Bitcoin perpetual; "BTC-PERP-INTX" is the International Exchange product
// (non-CFTC, non-US) and must NOT be used.
// ---------------------------------------------------------------------------
inline constexpr char COINBASE_HEDGE_SYMBOL[] = "BTC-PERP";  // TODO: verify
inline constexpr char COINBASE_REST_BASE[]    = "https://api.coinbase.com";
// Market data feed remains on Binance WebSocket (deeper BTC book, better signal).
inline constexpr char BINANCE_FUTURES_WS[]    = "wss://fstream.binance.com";

// ---------------------------------------------------------------------------
// Bayesian signal engine
// ---------------------------------------------------------------------------
constexpr double T_DEGREES_OF_FREEDOM    = 4.0;    // Student-t fat-tail param; tune in backtest
constexpr double PRIOR_RESET_INTERVAL_S  = 30.0;   // reset P_prior to live CLOB mid every 30s
constexpr double OB_IMBALANCE_MULTIPLIER = 0.3;    // vol-scaling weight for OB imbalance signal

// 2-D calibration lookup table (secondary model, populated offline from backtest)
constexpr int    STRIKE_DISTANCE_BINS     = 200;   // -100bps to +100bps, 1bp steps
constexpr int    TIME_REMAINING_BINS      = 60;    // 300s to 0s, 5s steps
constexpr double STRIKE_DISTANCE_MIN_BPS  = -100.0;
constexpr double STRIKE_DISTANCE_MAX_BPS  = +100.0;
constexpr double TIME_REMAINING_MAX_S     = 300.0;
constexpr double TIME_BIN_WIDTH_S         = 5.0;

// Auxiliary likelihood multipliers: max ±10% adjustment per signal
constexpr double MAX_AUX_LOG_LIKELIHOOD_ADJ = 0.10;

// ---------------------------------------------------------------------------
// Market regime thresholds  (realized vol as decimal fraction, NOT percent)
// ---------------------------------------------------------------------------
constexpr double VOL_SIDEWAYS_THRESHOLD = 0.0015;  // <0.15% → SIDEWAYS (maker eligible)
constexpr double VOL_TRENDING_THRESHOLD = 0.0050;  // <0.50% → TRENDING (taker only, reduced size)
constexpr double VOL_VOLATILE_THRESHOLD = 0.0150;  // <1.50% → VOLATILE (taker only, min size or skip)
// >=VOL_VOLATILE_THRESHOLD → SPIKE (circuit breaker may fire)

constexpr double REALIZED_VOL_WINDOW_S = 60.0;     // rolling window for vol computation

// ---------------------------------------------------------------------------
// Edge thresholds  (cents per share == fractions of $0.01 USDC)
// ---------------------------------------------------------------------------
constexpr double FEE_TAKER_CENTS           = 0.02;
constexpr double HALF_SPREAD_DEFAULT_CENTS = 0.50;
constexpr double FRICTION_SETTLEMENT_CENTS = 0.10;
constexpr double ALPHA_BUFFER_CENTS        = 0.75;  // target net profit; tune 0.5–1.5 in backtest
constexpr double E_MIN_TAKER_CENTS         = FEE_TAKER_CENTS
                                           + HALF_SPREAD_DEFAULT_CENTS
                                           + FRICTION_SETTLEMENT_CENTS
                                           + ALPHA_BUFFER_CENTS;  // 1.37¢ default

// ---------------------------------------------------------------------------
// Position sizing
// ---------------------------------------------------------------------------
constexpr double KELLY_FRACTION           = 0.25;   // quarter-Kelly; increase only after 200+ live trades
const     Amount MAX_TRADE_USDC           = Amount::from_double(200.0);
constexpr double MAX_HEDGE_FRACTION       = 0.05;   // 5% of bankroll per Binance hedge
const     Amount MAX_TOTAL_EXPOSURE_USDC  = Amount::from_double(400.0);
const     Amount MAX_MAKER_QUOTE_USDC     = Amount::from_double(100.0);  // per side
constexpr int    KELLY_LIVE_TRADE_MIN     = 200;    // trades before raising Kelly fraction

// ---------------------------------------------------------------------------
// Execution
// ---------------------------------------------------------------------------
constexpr double MAKER_QUOTE_MAX_AGE_MS       = 1500.0;  // cancel and re-evaluate after 1500ms
constexpr double MAKER_REQUOTE_THRESHOLD_BPS  = 0.5;     // requote immediately on mid move > 0.5bps
constexpr int    IOC_EXPIRY_SECONDS           = 10;      // IOC/FOK orders: expiration = now + 10s
constexpr double MIN_TAKER_INTERVAL_S         = 30.0;    // minimum gap between taker entries per market
constexpr int    MAX_OPEN_POSITIONS           = 3;       // across all markets simultaneously

// Fee rates in bps for order submission (verify against live API)
constexpr uint32_t TAKER_FEE_RATE_BPS = 200;
constexpr uint32_t MAKER_FEE_RATE_BPS = 0;

// ---------------------------------------------------------------------------
// Position management
// ---------------------------------------------------------------------------
constexpr double HARD_STOP_MULTIPLE       = -1.5;  // close if PnL < -1.5 × initial_edge_value
constexpr double TRAILING_PROFIT_TRIGGER  =  0.5;  // trailing stop activates at +0.5 × initial_edge
constexpr double TRAILING_STOP_FRACTION   =  0.30; // retrace 30% from peak → exit
constexpr double HEDGE_CLOSE_BEFORE_EXPIRY_S = 30.0; // close Coinbase hedge at T-30s before resolution

// ---------------------------------------------------------------------------
// Feed fault tiers
// ---------------------------------------------------------------------------
constexpr double FEED_DEGRADED_THRESHOLD_S    = 2.0;  // 0–2s gap: DEGRADED
constexpr double FEED_FALLBACK_THRESHOLD_S    = 5.0;  // 2–5s gap: REST FALLBACK
// >5s gap: EMERGENCY FLATTEN (break into slices below)
constexpr int    FEED_FLATTEN_SLICE_COUNT     = 5;
constexpr double FEED_FLATTEN_SLICE_INTERVAL_S = 6.0; // one slice per 6s → 30s total
constexpr double FEED_REST_POLL_INTERVAL_MS   = 500.0;
constexpr double MAKER_SPREAD_DEGRADED_MULT   = 2.0;  // widen maker spread 2× when DEGRADED

// ---------------------------------------------------------------------------
// Circuit breakers  (Thread 4 — risk watchdog)
// ---------------------------------------------------------------------------
constexpr int    CB_MAX_CONSECUTIVE_LOSSES  = 4;
constexpr double CB_LOSS_WINDOW_S           = 900.0;   // 15 minutes
constexpr double CB_BTC_SPIKE_THRESHOLD     = 0.015;   // 1.5% BTC move in window
constexpr double CB_BTC_SPIKE_WINDOW_S      = 60.0;
constexpr double CB_NAV_DRAWDOWN_THRESHOLD  = 0.08;    // 8% of NAV
constexpr double CB_NAV_DRAWDOWN_WINDOW_S   = 3600.0;  // 1 hour

// ---------------------------------------------------------------------------
// Risk guardrails  (always active; not circuit breakers)
// ---------------------------------------------------------------------------
constexpr double CEX_MARGIN_FLOOR_FRACTION = 0.5; // halt new entries if bankroll < 50% of initial

// ---------------------------------------------------------------------------
// Infrastructure
// ---------------------------------------------------------------------------
constexpr double HEARTBEAT_INTERVAL_MS      = 500.0;
constexpr double WATCHDOG_STALE_THRESHOLD_S = 2.0;
constexpr int    METRICS_PORT               = 9090;

// ---------------------------------------------------------------------------
// Chain / protocol
// ---------------------------------------------------------------------------
constexpr uint64_t POLYGON_CHAIN_ID = 137;

namespace addresses {
// Polygon mainnet. Used to construct EIP-712 domain separator.
// Do NOT use these as runtime strings — see eip712.cpp for the byte arrays.
inline constexpr char CTF_EXCHANGE[]     = "0x4bFb41d5B3570DeFd03C39a9A4D8dE6Bd8B8982E";
inline constexpr char NEG_RISK_ADAPTER[] = "0xd91E80cF2E7be2e162c6513ceD06f1dD0dA35296";
inline constexpr char USDC_POLYGON[]     = "0x2791Bca1f2de4661ED88A30C99A7a9449Aa84174";
} // namespace addresses

// ---------------------------------------------------------------------------
// Go-live ramp  (deploy with reduced caps, double after 48h of good behaviour)
// ---------------------------------------------------------------------------
namespace ramp {
const Amount INITIAL_MAX_TRADE_USDC          = Amount::from_double(20.0);
const Amount INITIAL_MAX_TOTAL_EXPOSURE_USDC = Amount::from_double(40.0);
} // namespace ramp

} // namespace constants
