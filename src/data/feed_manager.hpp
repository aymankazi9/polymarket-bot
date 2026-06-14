#pragma once
#include "tick.hpp"
#include "ring_buffer.hpp"
#include "clock_sync.hpp"
#include "../../constants.hpp"

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace data {

// Manages all market-data WebSocket connections and writes normalised Ticks
// to the shared ring buffer consumed by the Bayesian engine on Thread 2.
//
// Connections:
//   Binance combined stream — depth20@100ms + aggTrade + markPrice@1s
//   Coinbase Advanced Trade — ticker channel for BTC-USD
//   Polymarket CLOB         — market channel for configured token IDs
//
// io_uring note: Boost.Asio 1.78+ supports io_uring transparently when
// compiled with -DBOOST_ASIO_HAS_IO_URING.  No code changes are required;
// the io_context will use io_uring automatically on Linux 5.1+.
//
// Threading:
//   run() is blocking — it calls ioc_.run() and must be called from Thread 1.
//   All other methods are safe to call from other threads (they only read atomics).

using TickBuffer = SPSCRingBuffer<Tick, 4096>;

class FeedManager {
public:
    explicit FeedManager(TickBuffer& ring);
    ~FeedManager();

    // Start all WebSocket connections and block until stop() is called.
    // poly_token_ids: YES and NO token IDs for each active market.
    // Reconnects automatically on disconnection.
    void run(const std::vector<std::string>& poly_token_ids,
             const std::string& binance_ws_host   = "stream.binance.com",
             const std::string& binance_ws_port   = "9443",
             const std::string& coinbase_ws_host  = "advanced-trade-ws.coinbase.com",
             const std::string& coinbase_ws_port  = "443",
             const std::string& polymarket_ws_host = "ws-subscriptions-clob.polymarket.com",
             const std::string& polymarket_ws_port = "443");

    void stop() noexcept;

    // Read-only accessors — safe to call from any thread.
    FeedFaultLevel fault_level(Source s) const noexcept;
    int64_t        last_received_us(Source s) const noexcept;
    int64_t        clock_offset_us(Source s)  const noexcept;

    // Auxiliary signals for the Bayesian engine (from Binance markPrice stream)
    double last_funding_rate() const noexcept;
    double last_mark_price()   const noexcept;

private:
    // Message dispatch — parsing helpers are free functions in feed_manager.cpp.
    // Each dispatch_* is called from a coroutine lambda inside run().
    void dispatch_binance_msg   (const std::string& raw, int64_t local_us);
    void dispatch_coinbase_msg  (const std::string& raw, int64_t local_us);
    void dispatch_polymarket_msg(const std::string& raw, int64_t local_us);

    int64_t now_us() const noexcept;
    void    record_received(Source s) noexcept;
    void    push_tick(Tick t) noexcept;

    TickBuffer&  ring_;
    ClockSync    clock_sync_;
    std::vector<std::string> poly_token_ids_;

    std::atomic<bool> running_{false};

    // Per-source state — written by Thread 1, read by Thread 3/4
    alignas(64) std::atomic<int64_t>        last_received_us_[SOURCE_COUNT]{};
    alignas(64) std::atomic<FeedFaultLevel> fault_level_[SOURCE_COUNT]{};

    // Binance auxiliary signals — written by Thread 1, read by Thread 2
    std::atomic<double> last_funding_rate_{0.0};
    std::atomic<double> last_mark_price_{0.0};

    // io_context lives on the stack of run() to keep it in Thread 1's scope
};

} // namespace data
