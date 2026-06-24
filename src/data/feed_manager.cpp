// io_uring acceleration:
//   Compile with -DBOOST_ASIO_HAS_IO_URING to make the io_context use
//   io_uring for all socket I/O on Linux 5.1+.  No code changes required;
//   Boost.Asio selects io_uring transparently when that define is set.

#include "feed_manager.hpp"

#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/asio/steady_timer.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace beast     = boost::beast;
namespace asio      = boost::asio;
namespace websocket = beast::websocket;
namespace ssl       = asio::ssl;
using tcp           = asio::ip::tcp;
using json          = nlohmann::json;

namespace data {

namespace {

// ---------------------------------------------------------------------------
// Time helpers
// ---------------------------------------------------------------------------

inline int64_t now_us_epoch() noexcept {
    using namespace std::chrono;
    return duration_cast<microseconds>(system_clock::now().time_since_epoch()).count();
}

// Parse a decimal string to double (avoids exceptions on bad input)
inline double parse_double(const std::string& s) noexcept {
    try { return std::stod(s); } catch (...) { return 0.0; }
}

// Parse ISO 8601 timestamp "2023-01-01T12:00:00.000000Z" to microseconds.
// Used for Coinbase's envelope timestamp field.
int64_t iso8601_to_us(const std::string& s) noexcept {
    std::tm tm{};
    const char* p = s.c_str();
    // Parse: YYYY-MM-DDTHH:MM:SS
    if (std::sscanf(p, "%4d-%2d-%2dT%2d:%2d:%2d",
                    &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
                    &tm.tm_hour, &tm.tm_min, &tm.tm_sec) != 6)
        return 0;
    tm.tm_year -= 1900;
    tm.tm_mon  -= 1;
    tm.tm_isdst = 0;

    // Fractional seconds
    double frac = 0.0;
    const char* dot = std::strchr(p, '.');
    if (dot) std::sscanf(dot, "%lf", &frac);

    time_t epoch = timegm(&tm);  // POSIX timegm (UTC); available on Linux
    if (epoch == static_cast<time_t>(-1)) return 0;
    return static_cast<int64_t>(epoch) * 1'000'000LL +
           static_cast<int64_t>(frac * 1e6);
}

// ---------------------------------------------------------------------------
// OB imbalance: (bid_qty - ask_qty) / (bid_qty + ask_qty) over top-5 levels
// Both arrays are assumed sorted best-first (bids descending, asks ascending).
// ---------------------------------------------------------------------------

template<typename LevelArray>
double compute_ob_imbalance(const LevelArray& bids, const LevelArray& asks,
                             int levels = 5) noexcept {
    double bid_qty = 0.0, ask_qty = 0.0;
    int    nb = std::min(static_cast<int>(bids.size()), levels);
    int    na = std::min(static_cast<int>(asks.size()), levels);

    for (int i = 0; i < nb; ++i)
        bid_qty += bids[i];
    for (int i = 0; i < na; ++i)
        ask_qty += asks[i];

    double total = bid_qty + ask_qty;
    if (total == 0.0) return 0.0;
    return (bid_qty - ask_qty) / total;
}

// ---------------------------------------------------------------------------
// Binance combined-stream parsers
//   Message envelope: {"stream": "name", "data": {...}}
// ---------------------------------------------------------------------------

bool parse_binance_depth(const json& data,
                          int64_t exchange_us, int64_t clock_offset_us,
                          Tick& out) {
    // data.e == "depthUpdate"
    // data.b: [["price", "qty"], ...] sorted bid descending
    // data.a: [["price", "qty"], ...] sorted ask ascending
    if (!data.contains("b") || !data.contains("a"))
        return false;

    const auto& bids = data["b"];
    const auto& asks = data["a"];
    if (bids.empty() && asks.empty())
        return false;

    int nb = static_cast<int>(bids.size());
    int na = static_cast<int>(asks.size());

    double bb = nb > 0 ? parse_double(bids[0][0].get<std::string>()) : 0.0;
    double ba = na > 0 ? parse_double(asks[0][0].get<std::string>()) : 0.0;

    // Fewer than 2 levels on either side = thin book; neutralise OB imbalance signal
    bool low_depth = (nb < 2 || na < 2);

    // Top-5 quantities for OB imbalance (only meaningful when !low_depth)
    std::array<double, 5> bqty{}, aqty{};
    for (int i = 0; i < std::min(5, nb); ++i)
        bqty[i] = parse_double(bids[i][1].get<std::string>());
    for (int i = 0; i < std::min(5, na); ++i)
        aqty[i] = parse_double(asks[i][1].get<std::string>());

    out.timestamp_us  = exchange_us + clock_offset_us;
    out.source        = Source::BINANCE;
    out.best_bid      = bb;
    out.best_ask      = ba;
    out.mid           = (bb + ba) / 2.0;
    out.ob_imbalance  = low_depth ? 0.0 : compute_ob_imbalance(bqty, aqty);
    out.last_trade    = 0.0;
    out.low_depth     = low_depth;
    return true;
}

bool parse_binance_trade(const json& data,
                          int64_t exchange_us, int64_t clock_offset_us,
                          Tick& out) {
    // data.e == "aggTrade"
    // data.p: price string
    if (!data.contains("p")) return false;
    double price = parse_double(data["p"].get<std::string>());
    if (price == 0.0) return false;

    out.timestamp_us  = exchange_us + clock_offset_us;
    out.source        = Source::BINANCE;
    out.best_bid      = 0.0;
    out.best_ask      = 0.0;
    out.mid           = 0.0;
    out.ob_imbalance  = 0.0;
    out.last_trade    = price;
    out.low_depth     = false;  // trade events carry no OB data; not a depth fault
    return true;
}

// Returns true and writes to funding_rate / mark_price atomics on success.
bool parse_binance_funding(const json& data,
                            std::atomic<double>& funding_rate_out,
                            std::atomic<double>& mark_price_out) noexcept {
    // data.e == "markPriceUpdate"
    // data.p: mark price string
    // data.r: funding rate string
    if (!data.contains("p") || !data.contains("r")) return false;
    double mp = parse_double(data["p"].get<std::string>());
    double fr = parse_double(data["r"].get<std::string>());
    if (mp == 0.0) return false;

    mark_price_out.store(mp, std::memory_order_relaxed);
    funding_rate_out.store(fr, std::memory_order_relaxed);
    return true;
}

// ---------------------------------------------------------------------------
// Coinbase Advanced Trade ticker channel
//   Envelope: {"channel":"ticker","timestamp":"ISO8601","events":[{"type":"snapshot"|"update","tickers":[...]}]}
//   Ticker fields: product_id, price, best_bid, best_ask, best_bid_quantity, best_ask_quantity
// ---------------------------------------------------------------------------

bool parse_coinbase_ticker(const json& j,
                            int64_t envelope_us, int64_t clock_offset_us,
                            Tick& out) {
    if (!j.contains("events")) return false;
    for (const auto& ev : j["events"]) {
        if (!ev.contains("tickers")) continue;
        for (const auto& t : ev["tickers"]) {
            if (!t.contains("product_id")) continue;
            std::string pid = t["product_id"].get<std::string>();
            if (pid != "BTC-USD") continue;

            double bb = t.contains("best_bid") ?
                parse_double(t["best_bid"].get<std::string>()) : 0.0;
            double ba = t.contains("best_ask") ?
                parse_double(t["best_ask"].get<std::string>()) : 0.0;
            double pr = t.contains("price") ?
                parse_double(t["price"].get<std::string>()) : 0.0;

            // OB imbalance from top-1 quantities (ticker channel doesn't give depth)
            double bbq = t.contains("best_bid_quantity") ?
                parse_double(t["best_bid_quantity"].get<std::string>()) : 0.0;
            double baq = t.contains("best_ask_quantity") ?
                parse_double(t["best_ask_quantity"].get<std::string>()) : 0.0;
            double total = bbq + baq;
            double obi   = (total > 0.0) ? (bbq - baq) / total : 0.0;

            out.timestamp_us = envelope_us + clock_offset_us;
            out.source       = Source::COINBASE;
            out.best_bid     = bb;
            out.best_ask     = ba;
            out.mid          = (bb > 0.0 && ba > 0.0) ? (bb + ba) / 2.0 : pr;
            out.ob_imbalance = obi;
            out.last_trade   = pr;   // price = last matched trade
            out.low_depth    = false;  // ticker channel provides 1 level by design; not a fault
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Polymarket CLOB market channel
//   book:          {"event_type":"book","asset_id":"...","bids":[{"price":"0.48","size":"30"},...],
//                   "asks":[...],"timestamp":"ms_epoch_string"}
//   price_change:  {"event_type":"price_change","price_changes":[{"asset_id":"...",
//                   "best_bid":"0.5","best_ask":"1","price":"0.5","side":"BUY","size":"200"}],
//                   "timestamp":"ms_epoch_string"}
// ---------------------------------------------------------------------------

bool parse_polymarket_book(const json& j,
                            int64_t clock_offset_us,
                            Tick& out) {
    if (!j.contains("bids") || !j.contains("asks")) return false;

    int64_t exchange_us = 0;
    if (j.contains("timestamp"))
        exchange_us = std::stoll(j["timestamp"].get<std::string>()) * 1000LL;

    const auto& bids = j["bids"];
    const auto& asks = j["asks"];

    int nb = static_cast<int>(bids.size());
    int na = static_cast<int>(asks.size());

    // Fewer than 2 levels on either side = thin market; neutralise imbalance signal
    bool low_depth = (nb < 2 || na < 2);

    double bb = nb > 0 ? parse_double(bids[0]["price"].get<std::string>()) : 0.0;
    double ba = na > 0 ? parse_double(asks[0]["price"].get<std::string>()) : 0.0;

    // OB imbalance from top-5 size levels (only meaningful when !low_depth)
    std::array<double, 5> bsizes{}, asizes{};
    for (int i = 0; i < std::min(5, nb); ++i)
        bsizes[i] = parse_double(bids[i]["size"].get<std::string>());
    for (int i = 0; i < std::min(5, na); ++i)
        asizes[i] = parse_double(asks[i]["size"].get<std::string>());

    out.timestamp_us = exchange_us + clock_offset_us;
    out.source       = Source::POLYMARKET;
    out.best_bid     = bb;
    out.best_ask     = ba;
    out.mid          = (bb > 0.0 && ba > 0.0) ? (bb + ba) / 2.0 : 0.0;
    out.ob_imbalance = low_depth ? 0.0 : compute_ob_imbalance(bsizes, asizes);
    out.last_trade   = 0.0;
    out.low_depth    = low_depth;
    return true;
}

bool parse_polymarket_price_change(const json& j,
                                    int64_t clock_offset_us,
                                    Tick& out) {
    if (!j.contains("price_changes")) return false;
    const auto& changes = j["price_changes"];
    if (changes.empty()) return false;

    // Use the first change in the array; BUY side gives best bid, SELL gives best ask.
    // price_change events carry best_bid and best_ask directly.
    int64_t exchange_us = 0;
    if (j.contains("timestamp"))
        exchange_us = std::stoll(j["timestamp"].get<std::string>()) * 1000LL;

    double bb = 0.0, ba = 0.0;
    for (const auto& c : changes) {
        if (c.contains("best_bid")) bb = parse_double(c["best_bid"].get<std::string>());
        if (c.contains("best_ask")) ba = parse_double(c["best_ask"].get<std::string>());
        if (bb > 0.0 || ba > 0.0) break;  // first change with book info is enough
    }

    if (bb == 0.0 && ba == 0.0) return false;

    out.timestamp_us = exchange_us + clock_offset_us;
    out.source       = Source::POLYMARKET;
    out.best_bid     = bb;
    out.best_ask     = ba;
    out.mid          = (bb > 0.0 && ba > 0.0) ? (bb + ba) / 2.0 : (bb + ba);
    out.ob_imbalance = 0.0;    // delta event; no depth info
    out.last_trade   = 0.0;
    out.low_depth    = false;  // price_change carries no OB snapshot; not a depth fault
    return true;
}

// ---------------------------------------------------------------------------
// WS type aliases
// ---------------------------------------------------------------------------

using WsStream = websocket::stream<beast::ssl_stream<beast::tcp_stream>>;

// Build and return an SSL-enabled WebSocket stream connected to host:port
// and upgraded to WS with the given path.  Throws on any failure.
WsStream connect_wss(asio::io_context& ioc,
                     ssl::context& ssl_ctx,
                     const std::string& host,
                     const std::string& port,
                     const std::string& path,
                     asio::yield_context yield) {
    tcp::resolver resolver{ioc};
    auto endpoints = resolver.async_resolve(host, port, yield);

    WsStream ws{ioc, ssl_ctx};
    beast::get_lowest_layer(ws).async_connect(endpoints, yield);

    // SNI hostname (required by many TLS servers)
    if (!SSL_set_tlsext_host_name(
            ws.next_layer().native_handle(), host.c_str()))
        throw std::runtime_error("SSL_set_tlsext_host_name failed");

    ws.next_layer().async_handshake(ssl::stream_base::client, yield);

    ws.set_option(websocket::stream_base::timeout::suggested(
        beast::role_type::client));
    ws.set_option(websocket::stream_base::decorator([](websocket::request_type& req) {
        req.set(beast::http::field::user_agent, "polymarket-bot/1.0");
    }));
    ws.async_handshake(host, path, yield);
    return ws;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// FeedManager
// ---------------------------------------------------------------------------

FeedManager::FeedManager(TickBuffer& ring) : ring_(ring) {
    for (int i = 0; i < SOURCE_COUNT; ++i) {
        last_received_us_[i].store(0, std::memory_order_relaxed);
        fault_level_[i].store(FeedFaultLevel::NOMINAL, std::memory_order_relaxed);
    }
}

FeedManager::~FeedManager() { stop(); }

void FeedManager::stop() noexcept {
    running_.store(false, std::memory_order_release);
}

FeedFaultLevel FeedManager::fault_level(Source s) const noexcept {
    return fault_level_[static_cast<int>(s)].load(std::memory_order_acquire);
}

int64_t FeedManager::last_received_us(Source s) const noexcept {
    return last_received_us_[static_cast<int>(s)].load(std::memory_order_relaxed);
}

int64_t FeedManager::clock_offset_us(Source s) const noexcept {
    return clock_sync_.offset_us(s);
}

bool FeedManager::both_cex_dead() const noexcept {
    return fault_level(Source::BINANCE)  == FeedFaultLevel::EMERGENCY_FLATTEN
        && fault_level(Source::COINBASE) == FeedFaultLevel::EMERGENCY_FLATTEN;
}

double FeedManager::last_funding_rate() const noexcept {
    return last_funding_rate_.load(std::memory_order_relaxed);
}

double FeedManager::last_mark_price() const noexcept {
    return last_mark_price_.load(std::memory_order_relaxed);
}

int64_t FeedManager::now_us() const noexcept { return now_us_epoch(); }

void FeedManager::record_received(Source s) noexcept {
    last_received_us_[static_cast<int>(s)].store(
        now_us_epoch(), std::memory_order_relaxed);
}

void FeedManager::push_tick(Tick t) noexcept {
    int si = static_cast<int>(t.source);

    // Track consecutive low-depth ticks per source.
    // 30 consecutive → escalate to at least DEGRADED to widen maker spread.
    // Coinbase ticks always have low_depth=false so they never trigger this path.
    if (t.low_depth) {
        if (++consecutive_low_depth_[si] == 30) {
            FeedFaultLevel cur = fault_level_[si].load(std::memory_order_relaxed);
            if (cur == FeedFaultLevel::NOMINAL) {
                fault_level_[si].store(FeedFaultLevel::DEGRADED,
                                       std::memory_order_release);
                std::fprintf(stderr,
                    "feed_manager: source %d → DEGRADED (30 consecutive low-depth ticks)\n",
                    si);
            }
        }
    } else {
        consecutive_low_depth_[si] = 0;
    }

    if (!ring_.push(t)) {
        // Ring buffer full — consumer (Thread 2) is lagging.  Log and drop.
        std::fprintf(stderr, "feed_manager: ring buffer full — tick dropped\n");
    }
}

// ---------------------------------------------------------------------------
// Message dispatch
// ---------------------------------------------------------------------------

void FeedManager::dispatch_binance_msg(const std::string& raw, int64_t local_us) {
    json j = json::parse(raw, nullptr, /*exceptions=*/false);
    if (j.is_discarded() || !j.contains("stream") || !j.contains("data"))
        return;

    const std::string& stream = j["stream"];
    const json&        data   = j["data"];

    // Extract exchange event time (field "E" in milliseconds)
    int64_t exchange_us = 0;
    if (data.contains("E"))
        exchange_us = data["E"].get<int64_t>() * 1000LL;

    clock_sync_.update(Source::BINANCE, local_us, exchange_us);
    int64_t offset = clock_sync_.offset_us(Source::BINANCE);

    Tick tick{};
    if (stream.find("depth") != std::string::npos) {
        if (parse_binance_depth(data, exchange_us, offset, tick)) {
            record_received(Source::BINANCE);
            push_tick(tick);
        }
    } else if (stream.find("aggTrade") != std::string::npos) {
        if (parse_binance_trade(data, exchange_us, offset, tick)) {
            record_received(Source::BINANCE);
            push_tick(tick);
        }
    } else if (stream.find("markPrice") != std::string::npos) {
        parse_binance_funding(data, last_funding_rate_, last_mark_price_);
        record_received(Source::BINANCE);
        // Funding updates don't produce a ring-buffer Tick — read directly via accessor
    }
}

void FeedManager::dispatch_coinbase_msg(const std::string& raw, int64_t local_us) {
    json j = json::parse(raw, nullptr, false);
    if (j.is_discarded()) return;

    if (!j.contains("channel") || j["channel"] != "ticker") return;

    // Extract envelope timestamp (ISO 8601)
    int64_t envelope_us = 0;
    if (j.contains("timestamp"))
        envelope_us = iso8601_to_us(j["timestamp"].get<std::string>());
    if (envelope_us == 0) envelope_us = local_us;

    clock_sync_.update(Source::COINBASE, local_us, envelope_us);
    int64_t offset = clock_sync_.offset_us(Source::COINBASE);

    Tick tick{};
    if (parse_coinbase_ticker(j, envelope_us, offset, tick)) {
        record_received(Source::COINBASE);
        push_tick(tick);
    }
}

void FeedManager::dispatch_polymarket_msg(const std::string& raw, int64_t local_us) {
    json j = json::parse(raw, nullptr, false);
    if (j.is_discarded() || !j.contains("event_type")) return;

    const std::string& etype = j["event_type"];

    // Extract exchange timestamp (ms-epoch string)
    int64_t exchange_us = 0;
    if (j.contains("timestamp")) {
        try { exchange_us = std::stoll(j["timestamp"].get<std::string>()) * 1000LL; }
        catch (...) {}
    }
    if (exchange_us == 0) exchange_us = local_us;

    clock_sync_.update(Source::POLYMARKET, local_us, exchange_us);
    int64_t offset = clock_sync_.offset_us(Source::POLYMARKET);

    Tick tick{};
    bool ok = false;
    if (etype == "book")
        ok = parse_polymarket_book(j, offset, tick);
    else if (etype == "price_change")
        ok = parse_polymarket_price_change(j, offset, tick);

    if (ok) {
        record_received(Source::POLYMARKET);
        push_tick(tick);
    }
}

// ---------------------------------------------------------------------------
// run() — blocks the calling thread; launches all coroutines on the ioc
// ---------------------------------------------------------------------------

void FeedManager::run(
    const std::vector<std::string>& poly_token_ids,
    const std::string& binance_ws_host,
    const std::string& binance_ws_port,
    const std::string& coinbase_ws_host,
    const std::string& coinbase_ws_port,
    const std::string& polymarket_ws_host,
    const std::string& polymarket_ws_port)
{
    poly_token_ids_ = poly_token_ids;
    running_.store(true, std::memory_order_release);

    asio::io_context ioc;
    ssl::context     ssl_ctx{ssl::context::tls_client};
    ssl_ctx.set_verify_mode(ssl::verify_peer);
    ssl_ctx.set_default_verify_paths();

    // Exponential backoff helper: 100ms → 200ms → … → 30s (per-feed, not global)
    auto next_delay = [](int attempt) {
        return std::chrono::milliseconds(
            std::min(100 * (1 << std::min(attempt, 8)), 30000));
    };

    // ------------------------------------------------------------------
    // Binance combined stream
    // ------------------------------------------------------------------
    // BTCUSDC USDC-margined perpetual futures on fstream.binance.com (CONTEXT_ADDENDUM A6)
    const std::string binance_path =
        "/stream?streams=btcusdc@depth20@100ms/btcusdc@aggTrade/btcusdc@markPrice@1s";

    asio::spawn(ioc, [&](asio::yield_context yield) {
        for (int attempt = 0; running_.load(); ++attempt) {
            try {
                auto ws = connect_wss(ioc, ssl_ctx,
                                      binance_ws_host, binance_ws_port,
                                      binance_path, yield);
                attempt = 0;  // reset backoff on successful connect

                beast::flat_buffer buf;
                while (running_.load()) {
                    ws.async_read(buf, yield);
                    int64_t local = now_us();
                    dispatch_binance_msg(beast::buffers_to_string(buf.data()), local);
                    buf.clear();
                }
            } catch (const std::exception& e) {
                std::fprintf(stderr, "feed_manager[binance]: %s — reconnecting\n",
                             e.what());
            }
            asio::steady_timer t{ioc};
            t.expires_after(next_delay(attempt));
            t.async_wait(yield);
        }
    });

    // ------------------------------------------------------------------
    // Coinbase Advanced Trade ticker
    // ------------------------------------------------------------------
    const std::string cb_subscribe =
        R"({"type":"subscribe","product_ids":["BTC-USD"],"channel":"ticker"})";

    asio::spawn(ioc, [&](asio::yield_context yield) {
        for (int attempt = 0; running_.load(); ++attempt) {
            try {
                auto ws = connect_wss(ioc, ssl_ctx,
                                      coinbase_ws_host, coinbase_ws_port,
                                      "/", yield);
                ws.async_write(asio::buffer(cb_subscribe), yield);
                attempt = 0;

                beast::flat_buffer buf;
                while (running_.load()) {
                    ws.async_read(buf, yield);
                    int64_t local = now_us();
                    dispatch_coinbase_msg(beast::buffers_to_string(buf.data()), local);
                    buf.clear();
                }
            } catch (const std::exception& e) {
                std::fprintf(stderr, "feed_manager[coinbase]: %s — reconnecting\n",
                             e.what());
            }
            asio::steady_timer t{ioc};
            t.expires_after(next_delay(attempt));
            t.async_wait(yield);
        }
    });

    // ------------------------------------------------------------------
    // Polymarket CLOB market channel
    // ------------------------------------------------------------------
    asio::spawn(ioc, [&](asio::yield_context yield) {
        // Build subscription JSON with all token IDs
        json sub;
        sub["type"]       = "market";
        sub["assets_ids"] = poly_token_ids;
        const std::string poly_sub = sub.dump();

        for (int attempt = 0; running_.load(); ++attempt) {
            try {
                auto ws = connect_wss(ioc, ssl_ctx,
                                      polymarket_ws_host, polymarket_ws_port,
                                      "/ws/market", yield);
                ws.async_write(asio::buffer(poly_sub), yield);
                attempt = 0;

                beast::flat_buffer buf;
                while (running_.load()) {
                    ws.async_read(buf, yield);
                    int64_t local = now_us();
                    dispatch_polymarket_msg(
                        beast::buffers_to_string(buf.data()), local);
                    buf.clear();
                }
            } catch (const std::exception& e) {
                std::fprintf(stderr, "feed_manager[polymarket]: %s — reconnecting\n",
                             e.what());
            }
            asio::steady_timer t{ioc};
            t.expires_after(next_delay(attempt));
            t.async_wait(yield);
        }
    });

    // ------------------------------------------------------------------
    // Fault monitor — checks gaps every 500ms and updates fault_level_[]
    // ------------------------------------------------------------------
    asio::spawn(ioc, [&](asio::yield_context yield) {
        asio::steady_timer timer{ioc};
        while (running_.load()) {
            timer.expires_after(std::chrono::milliseconds(500));
            timer.async_wait(yield);

            int64_t now = now_us();
            for (int i = 0; i < SOURCE_COUNT; ++i) {
                int64_t last = last_received_us_[i].load(std::memory_order_relaxed);
                if (last == 0) continue;  // never received — don't fault yet

                double gap_s = static_cast<double>(now - last) / 1e6;
                FeedFaultLevel level;
                if (gap_s <= constants::FEED_DEGRADED_THRESHOLD_S)
                    level = FeedFaultLevel::NOMINAL;
                else if (gap_s <= constants::FEED_FALLBACK_THRESHOLD_S)
                    level = FeedFaultLevel::DEGRADED;
                else if (gap_s <= constants::FEED_FLATTEN_SLICE_COUNT *
                                  constants::FEED_FLATTEN_SLICE_INTERVAL_S)
                    level = FeedFaultLevel::REST_FALLBACK;
                else
                    level = FeedFaultLevel::EMERGENCY_FLATTEN;

                FeedFaultLevel prev = fault_level_[i].load(std::memory_order_relaxed);
                if (level != prev) {
                    fault_level_[i].store(level, std::memory_order_release);
                    std::fprintf(stderr,
                        "feed_manager: source %d fault level → %d (gap %.1fs)\n",
                        i, static_cast<int>(level), gap_s);
                }
            }
        }
    });

    ioc.run();
}

} // namespace data
