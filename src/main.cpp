#include "../constants.hpp"

#include "wallet/key_manager.hpp"
#include "wallet/clob_auth.hpp"
#include "wallet/nonce_manager.hpp"

#include "data/tick.hpp"
#include "data/ring_buffer.hpp"
#include "data/feed_manager.hpp"
#include "data/market_scanner.hpp"

#include "signal/shared_state.hpp"
#include "signal/bayesian_engine.hpp"

#include "execution/order_state_machine.hpp"

#include "risk/watchdog.hpp"

#include "infra/logger.hpp"
#include "infra/metrics.hpp"
#include "infra/heartbeat.hpp"

#include <curl/curl.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// ---------------------------------------------------------------------------
// Global shutdown flag — set by SIGTERM / SIGINT
// ---------------------------------------------------------------------------
static std::atomic<bool> g_shutdown{false};

static void signal_handler(int) {
    g_shutdown.store(true, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// Hex-encode wallet address for RPC
// ---------------------------------------------------------------------------
static std::string to_hex_address(const std::vector<uint8_t>& addr) {
    static const char HEX[] = "0123456789abcdef";
    std::string out = "0x";
    out.reserve(2 + addr.size() * 2);
    for (uint8_t b : addr) {
        out += HEX[b >> 4];
        out += HEX[b & 0xf];
    }
    return out;
}

// ---------------------------------------------------------------------------
// OSM map types (condition_id → OSM / thread)
// ---------------------------------------------------------------------------
using OsmMap    = std::unordered_map<std::string,
                      std::unique_ptr<execution::OrderStateMachine>>;
using ThreadMap = std::unordered_map<std::string, std::thread>;

// ---------------------------------------------------------------------------
// Add new OSMs for markets not yet tracked; clean up resolved ones.
// Call from main loop with osm_map_mu NOT held — this function acquires it.
// Lock order: scanner.mu (shared, via with_markets) → osm_map_mu.
// ---------------------------------------------------------------------------
static void sync_osms_with_scanner(
    data::MarketScanner& scanner,
    signal::SharedState& ss,
    wallet::KeyManager&  km,
    wallet::ClobAuth&    clob_auth,
    wallet::NonceManager& nonce_mgr,
    std::mutex& osm_map_mu,
    OsmMap&     osm_map,
    ThreadMap&  osm_threads)
{
    auto& log = infra::Logger::instance();

    // Snapshot active markets (shared lock; released before we acquire osm_map_mu)
    std::unordered_map<std::string, data::ActiveMarket> snapshot;
    scanner.with_markets([&](const auto& markets) { snapshot = markets; });

    std::lock_guard<std::mutex> lock(osm_map_mu);

    // Add OSMs for newly discovered markets
    for (const auto& [cid, am] : snapshot) {
        if (osm_map.count(cid)) continue;

        execution::OSMConfig cfg;
        cfg.condition_id  = am.condition_id;
        cfg.token_id      = am.token_id_yes;
        cfg.strike_price  = am.strike_price;
        cfg.resolution_us = am.resolution_us;
        cfg.is_above      = am.is_above;

        auto osm = std::make_unique<execution::OrderStateMachine>(
            ss, km, clob_auth, nonce_mgr, cfg);
        auto* raw = osm.get();
        osm_map.emplace(cid, std::move(osm));
        osm_threads.emplace(cid, std::thread([raw]{ raw->run(); }));

        log.info("MAIN", "started OSM for market " + cid);
    }

    // Stop and join OSMs for markets no longer active
    std::vector<std::string> to_clean;
    for (auto& [cid, osm] : osm_map) {
        if (!snapshot.count(cid)) {
            osm->stop();
            to_clean.push_back(cid);
        }
    }
    for (const auto& cid : to_clean) {
        auto it = osm_threads.find(cid);
        if (it != osm_threads.end()) {
            if (it->second.joinable()) it->second.join();
            osm_threads.erase(it);
        }
        osm_map.erase(cid);
        log.info("MAIN", "removed OSM for resolved market " + cid);
    }
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::fprintf(stderr,
            "Usage: polymarket-bot <key_file>\n"
            "  key_file : AES-256-GCM encrypted secrets file\n"
            "  Passphrase is read from stdin at startup.\n");
        return 1;
    }

    const std::string key_file = argv[1];

    std::signal(SIGTERM, signal_handler);
    std::signal(SIGINT,  signal_handler);

    curl_global_init(CURL_GLOBAL_ALL);

    // ---- Infrastructure: metrics + heartbeat ----
    auto& metrics = infra::Metrics::instance();
    metrics.start("0.0.0.0:" + std::to_string(constants::METRICS_PORT));
    auto& log = infra::Logger::instance();
    log.info("STARTUP", "metrics server started on port "
             + std::to_string(constants::METRICS_PORT));

    infra::Heartbeat heartbeat;
    std::thread hb_thread([&]{ heartbeat.run(); });

    // ---- Load encrypted key (prompts stdin for passphrase) ----
    wallet::KeyManager km;
    try {
        km.load(key_file);
    } catch (const std::exception& e) {
        log.error("STARTUP", std::string("key load failed: ") + e.what());
        return 1;
    }
    log.info("STARTUP", "key loaded");

    // ---- CLOB authentication ----
    wallet::ClobAuth clob_auth("https://clob.polymarket.com");
    try {
        clob_auth.authenticate(km);
    } catch (const std::exception& e) {
        log.error("STARTUP", std::string("CLOB auth failed: ") + e.what());
        return 1;
    }
    log.info("STARTUP", "CLOB authenticated");

    // ---- Nonce sync ----
    wallet::NonceManager nonce_mgr;
    try {
        std::string wallet_hex = to_hex_address(km.credentials().wallet_address);
        nonce_mgr.sync("", wallet_hex);
    } catch (const std::exception& e) {
        log.warn("STARTUP", std::string("nonce sync failed (starting at 0): ") + e.what());
    }

    // ---- Shared state + ring buffer ----
    signal::SharedState ss;
    ss.bankroll_usdc.store(
        constants::ramp::INITIAL_MAX_TOTAL_EXPOSURE_USDC.to_double() * 5.0,
        std::memory_order_relaxed);

    data::TickBuffer ring;

    // ---- Market scanner: initial synchronous scan ----
    data::MarketScanner scanner;
    log.info("STARTUP", "scanning for active BTC 5m markets...");
    scanner.scan_now();
    log.info("STARTUP", std::to_string(scanner.active_count()) + " market(s) found");

    // ---- OSM map (keyed by condition_id) ----
    std::mutex  osm_map_mu;
    OsmMap      osm_map;
    ThreadMap   osm_threads;

    // ---- Lifecycle callbacks: scanner → OSMs (condition_id dispatch) ----
    // Callbacks fire OUTSIDE scanner's write lock (see check_lifecycle).
    // They only acquire osm_map_mu, so lock order: scanner.mu → osm_map_mu is respected.
    {
        data::LifecycleCallbacks cbs;

        cbs.on_stop_entries = [&](const std::string& cid) {
            std::lock_guard<std::mutex> g(osm_map_mu);
            auto it = osm_map.find(cid);
            if (it != osm_map.end()) it->second->notify_stop_entries();
        };
        cbs.on_cancel_quotes = [&](const std::string& cid) {
            std::lock_guard<std::mutex> g(osm_map_mu);
            auto it = osm_map.find(cid);
            if (it != osm_map.end()) it->second->notify_cancel_quotes();
        };
        cbs.on_close_positions = [&](const std::string& cid) {
            std::lock_guard<std::mutex> g(osm_map_mu);
            auto it = osm_map.find(cid);
            if (it != osm_map.end()) it->second->notify_close_positions();
        };
        cbs.on_force_close = [&](const std::string& cid) {
            std::lock_guard<std::mutex> g(osm_map_mu);
            auto it = osm_map.find(cid);
            if (it != osm_map.end()) it->second->notify_force_close();
        };

        scanner.set_lifecycle_callbacks(std::move(cbs));
    }

    // ---- Build initial OSMs from scanner snapshot ----
    {
        std::lock_guard<std::mutex> lock(osm_map_mu);
        scanner.with_markets([&](const auto& markets) {
            for (const auto& [cid, am] : markets) {
                execution::OSMConfig cfg;
                cfg.condition_id  = am.condition_id;
                cfg.token_id      = am.token_id_yes;
                cfg.strike_price  = am.strike_price;
                cfg.resolution_us = am.resolution_us;
                cfg.is_above      = am.is_above;

                auto osm = std::make_unique<execution::OrderStateMachine>(
                    ss, km, clob_auth, nonce_mgr, cfg);
                auto* raw = osm.get();
                osm_map.emplace(cid, std::move(osm));
                osm_threads.emplace(cid, std::thread([raw]{ raw->run(); }));
                log.info("STARTUP", "started OSM for market " + cid);
            }
        });
    }

    // ---- Thread 1: FeedManager (subscribes to all current token IDs) ----
    std::vector<std::string> initial_token_ids = scanner.all_token_ids();
    data::FeedManager feed_manager(ring);
    std::thread t1([&]{ feed_manager.run(initial_token_ids); });

    // ---- Thread 2: BayesianEngine (uses first discovered market for config) ----
    // Limitation: one engine per ring buffer. Multi-market requires fan-out.
    signal::MarketConfig engine_cfg{};
    scanner.with_markets([&](const auto& markets) {
        if (!markets.empty()) {
            const auto& am = markets.begin()->second;
            engine_cfg.token_id      = am.token_id_yes;
            engine_cfg.strike_price  = am.strike_price;
            engine_cfg.resolution_us = am.resolution_us;
            engine_cfg.is_above      = am.is_above;
        }
    });
    signal::BayesianEngine engine(ring, ss, feed_manager, engine_cfg);
    std::thread t2([&]{ engine.run(); });

    // ---- Thread 4: RiskWatchdog (single callback flattens all OSMs) ----
    std::vector<risk::RiskWatchdog::FlattenCallback> flatten_cbs;
    flatten_cbs.push_back([&osm_map_mu, &osm_map] {
        std::lock_guard<std::mutex> g(osm_map_mu);
        for (auto& [_, osm] : osm_map)
            osm->emergency_flatten();
    });
    risk::RiskWatchdog watchdog(
        ss, flatten_cbs,
        ss.bankroll_usdc.load(std::memory_order_relaxed));
    std::thread t4([&]{ watchdog.run(); });

    // ---- Scanner background thread (polls every 30s for new markets) ----
    scanner.start();

    log.info("STARTUP", "all threads launched");

    // ---- Main loop ----
    using clock = std::chrono::steady_clock;
    auto last_sync = clock::now();

    while (!g_shutdown.load(std::memory_order_relaxed)) {
        if (ss.kill_switch.load(std::memory_order_relaxed)) {
            log.error("MAIN", "kill switch tripped — halting (manual reset required)");
            infra::Metrics::instance().inc_circuit_breaker_trips();
            break;
        }

        // Sync OSM map with scanner every ~30s (picks up newly discovered markets)
        auto now = clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - last_sync).count() >= 30) {
            sync_osms_with_scanner(scanner, ss, km, clob_auth, nonce_mgr,
                                   osm_map_mu, osm_map, osm_threads);
            last_sync = now;
        }

        metrics.set_open_positions(ss.open_position_count.load(std::memory_order_relaxed));
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    // ---- Graceful shutdown ----
    log.info("SHUTDOWN", "stopping all threads");

    scanner.stop();
    feed_manager.stop();
    engine.stop();
    watchdog.stop();
    heartbeat.stop();

    {
        std::lock_guard<std::mutex> lock(osm_map_mu);
        for (auto& [_, osm] : osm_map) osm->stop();
    }

    t1.join();
    t2.join();
    t4.join();
    hb_thread.join();

    {
        std::lock_guard<std::mutex> lock(osm_map_mu);
        for (auto& [_, t] : osm_threads)
            if (t.joinable()) t.join();
    }

    curl_global_cleanup();
    log.info("SHUTDOWN", "clean exit");
    return 0;
}
