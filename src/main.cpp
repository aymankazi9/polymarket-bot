#include "../constants.hpp"

#include "wallet/key_manager.hpp"
#include "wallet/clob_auth.hpp"
#include "wallet/nonce_manager.hpp"

#include "data/tick.hpp"
#include "data/ring_buffer.hpp"
#include "data/feed_manager.hpp"

#include "signal/shared_state.hpp"
#include "signal/bayesian_engine.hpp"

#include "execution/order_state_machine.hpp"

#include "risk/watchdog.hpp"

#include "infra/logger.hpp"
#include "infra/metrics.hpp"
#include "infra/heartbeat.hpp"

#include <nlohmann/json.hpp>
#include <curl/curl.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

// ---------------------------------------------------------------------------
// Global shutdown flag — set by SIGTERM / SIGINT
// ---------------------------------------------------------------------------
static std::atomic<bool> g_shutdown{false};

static void signal_handler(int) {
    g_shutdown.store(true, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// Config loader
// ---------------------------------------------------------------------------
struct MarketDef {
    std::string token_id;      // YES token decimal string
    double      strike_price;
    int64_t     resolution_us; // Unix microseconds
    bool        is_above;
};

static std::vector<MarketDef> load_markets(const std::string& path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("cannot open markets config: " + path);
    auto j = nlohmann::json::parse(f);
    std::vector<MarketDef> markets;
    for (const auto& m : j["markets"]) {
        MarketDef d;
        d.token_id      = m.at("token_id").get<std::string>();
        d.strike_price  = m.at("strike_price").get<double>();
        d.resolution_us = m.at("resolution_us").get<int64_t>();
        d.is_above      = m.at("is_above").get<bool>();
        markets.push_back(d);
    }
    return markets;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::fprintf(stderr,
            "Usage: polymarket-bot <key_file> <markets_config>\n"
            "  key_file       : AES-256-GCM encrypted secrets file\n"
            "  markets_config : JSON file listing active markets\n");
        return 1;
    }

    const std::string key_file     = argv[1];
    const std::string markets_file = argv[2];

    // ---- Signal handlers ----
    std::signal(SIGTERM, signal_handler);
    std::signal(SIGINT,  signal_handler);

    // ---- curl global init ----
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

    // ---- Load market config ----
    std::vector<MarketDef> markets;
    try {
        markets = load_markets(markets_file);
    } catch (const std::exception& e) {
        log.error("STARTUP", std::string("market config load failed: ") + e.what());
        return 1;
    }
    if (markets.empty()) {
        log.error("STARTUP", "no markets configured");
        return 1;
    }
    log.info("STARTUP", std::to_string(markets.size()) + " market(s) loaded");

    // ---- CLOB authentication (Level 1 → get API credentials) ----
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
        // Derive hex address for RPC query
        const auto& addr = km.credentials().wallet_address;
        std::string wallet_hex = "0x";
        static const char HEX[] = "0123456789abcdef";
        for (uint8_t b : addr) {
            wallet_hex += HEX[b >> 4];
            wallet_hex += HEX[b & 0xf];
        }
        nonce_mgr.sync("https://polygon-rpc.com", wallet_hex);
    } catch (const std::exception& e) {
        log.warn("STARTUP", std::string("nonce sync failed (starting at 0): ") + e.what());
    }

    // ---- Shared state + ring buffer ----
    signal::SharedState ss;
    ss.bankroll_usdc.store(constants::ramp::INITIAL_MAX_TOTAL_EXPOSURE_USDC * 5.0,
                            std::memory_order_relaxed);  // placeholder; replaced by REST query at runtime

    data::TickBuffer ring;

    // ---- Build token ID list for FeedManager ----
    std::vector<std::string> token_ids;
    for (const auto& m : markets) token_ids.push_back(m.token_id);

    // ---- Thread 1: FeedManager ----
    data::FeedManager feed_manager(ring);
    std::thread t1([&]{ feed_manager.run(token_ids); });

    // ---- Thread 2: BayesianEngine (one per market; share the ring buffer) ----
    // For multiple markets, a fan-out ring buffer would be needed.
    // For now: one BayesianEngine and one OSM per market, sharing the single ring.
    // The BayesianEngine updates the shared SharedState with the first market's signals.
    const auto& m0 = markets[0];
    signal::BayesianEngine engine(
        ring, ss, feed_manager,
        signal::MarketConfig{
            m0.token_id, m0.strike_price, m0.resolution_us, m0.is_above});
    std::thread t2([&]{ engine.run(); });

    // ---- Thread 3: OrderStateMachines (one per market) ----
    std::vector<std::unique_ptr<execution::OrderStateMachine>> osms;
    for (const auto& m : markets) {
        execution::OSMConfig osm_cfg;
        osm_cfg.token_id      = m.token_id;
        osm_cfg.strike_price  = m.strike_price;
        osm_cfg.resolution_us = m.resolution_us;
        osm_cfg.is_above      = m.is_above;
        osms.emplace_back(std::make_unique<execution::OrderStateMachine>(
            ss, km, clob_auth, nonce_mgr, osm_cfg));
    }
    std::vector<std::thread> osm_threads;
    for (auto& osm : osms)
        osm_threads.emplace_back([&osm]{ osm->run(); });

    // ---- Thread 4: RiskWatchdog ----
    std::vector<risk::RiskWatchdog::FlattenCallback> flatten_cbs;
    for (auto& osm : osms)
        flatten_cbs.push_back([&osm]{ osm->emergency_flatten(); });
    risk::RiskWatchdog watchdog(
        ss, flatten_cbs,
        ss.bankroll_usdc.load(std::memory_order_relaxed));
    std::thread t4([&]{ watchdog.run(); });

    log.info("STARTUP", "all threads launched");

    // ---- Main loop: wait for shutdown or kill switch ----
    while (!g_shutdown.load(std::memory_order_relaxed)) {
        if (ss.kill_switch.load(std::memory_order_relaxed)) {
            log.error("MAIN", "kill switch tripped — halting (manual reset required)");
            infra::Metrics::instance().inc_circuit_breaker_trips();
            break;
        }
        // Update Prometheus metrics from shared state
        metrics.set_open_positions(ss.open_position_count.load(std::memory_order_relaxed));
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    // ---- Graceful shutdown ----
    log.info("SHUTDOWN", "stopping all threads");

    feed_manager.stop();
    engine.stop();
    for (auto& osm : osms) osm->stop();
    watchdog.stop();
    heartbeat.stop();

    t1.join();
    t2.join();
    for (auto& t : osm_threads) t.join();
    t4.join();
    hb_thread.join();

    curl_global_cleanup();
    log.info("SHUTDOWN", "clean exit");
    return 0;
}
