#include "metrics.hpp"

#include <prometheus/counter.h>
#include <prometheus/exposer.h>
#include <prometheus/gauge.h>
#include <prometheus/histogram.h>
#include <prometheus/registry.h>

namespace infra {

Metrics& Metrics::instance() {
    static Metrics inst;
    return inst;
}

void Metrics::start(const std::string& bind_addr) {
    registry_ = std::make_shared<prometheus::Registry>();
    exposer_  = std::make_unique<prometheus::Exposer>(bind_addr);
    exposer_->RegisterCollectable(registry_);
    init();
}

void Metrics::init() {
    pnl_gauge_ = &prometheus::BuildGauge()
        .Name("bot_pnl_usdc")
        .Help("Current realised PnL in USDC")
        .Register(*registry_)
        .Add({});

    open_pos_gauge_ = &prometheus::BuildGauge()
        .Name("bot_open_positions")
        .Help("Number of open Polymarket positions")
        .Register(*registry_)
        .Add({});

    latency_hist_ = &prometheus::BuildHistogram()
        .Name("bot_order_latency_us")
        .Help("Order fire-to-fill latency in microseconds")
        .Register(*registry_)
        .Add({}, prometheus::Histogram::BucketBoundaries{
            1000, 5000, 10000, 25000, 50000, 100000, 250000, 500000, 1000000});

    cb_trips_counter_ = &prometheus::BuildCounter()
        .Name("bot_circuit_breaker_trips_total")
        .Help("Total circuit breaker trips since startup")
        .Register(*registry_)
        .Add({});
}

void Metrics::set_pnl(double pnl_usdc) {
    if (pnl_gauge_) pnl_gauge_->Set(pnl_usdc);
}

void Metrics::set_open_positions(int n) {
    if (open_pos_gauge_) open_pos_gauge_->Set(static_cast<double>(n));
}

void Metrics::observe_order_latency_us(double latency_us) {
    if (latency_hist_) latency_hist_->Observe(latency_us);
}

void Metrics::set_feed_lag_us(const std::string& source, double lag_us) {
    if (!registry_) return;

    // Find or create a per-source gauge
    prometheus::Gauge* g = nullptr;
    for (auto& entry : feed_lag_gauges_)
        if (entry.source == source) { g = entry.gauge; break; }

    if (!g) {
        g = &prometheus::BuildGauge()
            .Name("bot_feed_lag_us")
            .Help("Feed lag in microseconds per source")
            .Register(*registry_)
            .Add({{"source", source}});
        feed_lag_gauges_.push_back({source, g});
    }
    g->Set(lag_us);
}

void Metrics::inc_circuit_breaker_trips() {
    if (cb_trips_counter_) cb_trips_counter_->Increment();
}

} // namespace infra
