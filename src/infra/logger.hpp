#pragma once
#include "../signal/shared_state.hpp"
#include <cstdint>
#include <mutex>
#include <string>

// Structured JSON logger — writes NDJSON to stdout (captured by systemd journal).
//
// Format: {"ts":"<ISO8601>","level":"<level>","event":"<event>","data":{...}}
//
// Thread-safe: internal mutex serialises all writes.
// NOT on the hot path — called from Thread 3 and Thread 4 on order/position events.
// Do NOT log any field that contains key material (private key, api secret, passphrase).

namespace infra {

class Logger {
public:
    static Logger& instance();

    // Order lifecycle
    void order_fired(const std::string& order_id, const std::string& side,
                     double price, double size_usdc, const std::string& arm);
    void fill_received(const std::string& order_id, int64_t fire_to_fill_us);

    // Position lifecycle
    void position_opened(const std::string& order_id, double entry_price,
                          double size_usdc, const std::string& token_id);
    void position_closed(double entry_price, double exit_price,
                          double pnl_usdc, const std::string& reason);

    // Circuit breaker / fault
    void circuit_breaker(const std::string& reason, const signal::SharedState& ss);
    void feed_fault(const std::string& source, int tier);

    // Generic key/value log
    void info(const std::string& event, const std::string& msg);
    void warn(const std::string& event, const std::string& msg);
    void error(const std::string& event, const std::string& msg);

private:
    Logger() = default;
    void write(const std::string& level, const std::string& event,
               const std::string& data_json);
    static std::string now_iso8601();
    std::mutex mu_;
};

} // namespace infra
