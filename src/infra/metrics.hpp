#pragma once
#include <memory>
#include <string>
#include <vector>

// Prometheus metrics endpoint — localhost:METRICS_PORT (9090).
//
// Uses prometheus-cpp pull model:
//   Exposer binds the HTTP port; Registry holds all metric families.
//
// Singleton — call Metrics::instance() from any thread.
// All update methods are thread-safe (prometheus-cpp guarantees this).
//
// Metrics exposed:
//   bot_pnl_usdc                      gauge
//   bot_open_positions                 gauge
//   bot_order_latency_us               histogram (p50/p99 computable in Grafana)
//   bot_feed_lag_us{source}            gauge per source
//   bot_circuit_breaker_trips_total    counter

namespace prometheus {
class Registry;
class Exposer;
template<typename> class Family;
class Gauge;
class Histogram;
class Counter;
}

namespace infra {

class Metrics {
public:
    static Metrics& instance();

    // Starts the HTTP server.  Must be called once, typically at startup.
    // port: e.g. "0.0.0.0:9090"
    void start(const std::string& bind_addr = "0.0.0.0:9090");

    void set_pnl(double pnl_usdc);
    void set_open_positions(int n);
    void observe_order_latency_us(double latency_us);
    void set_feed_lag_us(const std::string& source, double lag_us);
    void inc_circuit_breaker_trips();

private:
    Metrics() = default;
    void init();

    std::shared_ptr<prometheus::Registry> registry_;
    std::unique_ptr<prometheus::Exposer>  exposer_;

    prometheus::Gauge*     pnl_gauge_         = nullptr;
    prometheus::Gauge*     open_pos_gauge_    = nullptr;
    prometheus::Histogram* latency_hist_      = nullptr;
    prometheus::Counter*   cb_trips_counter_  = nullptr;

    // feed_lag gauges keyed by source name — created on first use
    struct FeedLagEntry {
        std::string             source;
        prometheus::Gauge*      gauge;
    };
    std::vector<FeedLagEntry> feed_lag_gauges_;
};

} // namespace infra
