#pragma once
#include <cstdint>

// Normalised market tick — the common unit written to the ring buffer by Thread 1
// and consumed by the Bayesian engine on Thread 2.
//
// Field semantics by source:
//   BINANCE / COINBASE: best_bid/ask/mid from order-book depth; ob_imbalance from
//                       top-5 levels; last_trade from aggTrade (0 on depth ticks).
//   POLYMARKET:         best_bid/ask/mid from CLOB YES-token best levels;
//                       ob_imbalance from top levels of the YES book;
//                       last_trade unused (0).
//
// Exact struct layout matches CONTEXT.md §4.2.

namespace data {

enum class Source : uint8_t {
    BINANCE    = 0,
    COINBASE   = 1,
    POLYMARKET = 2,
};

constexpr int SOURCE_COUNT = 3;

enum class FeedFaultLevel : uint8_t {
    NOMINAL           = 0,
    DEGRADED          = 1,  // gap 0–2s: widen maker spread, stop new taker
    REST_FALLBACK     = 2,  // gap 2–5s: poll REST, delta only
    EMERGENCY_FLATTEN = 3,  // gap >5s: TWAP exit, then halt
};

struct Tick {
    int64_t timestamp_us;   // microseconds since epoch (normalised to local clock)
    Source  source;
    double  best_bid;
    double  best_ask;
    double  mid;
    double  ob_imbalance;   // (bid_qty - ask_qty) / (bid_qty + ask_qty), top-5 levels
    double  last_trade;     // last matched price; 0 if not a trade event
};

} // namespace data
