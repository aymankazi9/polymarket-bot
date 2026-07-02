#include "logger.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace infra {

Logger& Logger::instance() {
    static Logger inst;
    return inst;
}

std::string Logger::now_iso8601() {
    using namespace std::chrono;
    auto now  = system_clock::now();
    auto tt   = system_clock::to_time_t(now);
    auto ms   = duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000;
    std::tm utc{};
    gmtime_r(&tt, &utc);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03lldZ",
        utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
        utc.tm_hour, utc.tm_min, utc.tm_sec, (long long)ms);
    return buf;
}

void Logger::write(const std::string& level, const std::string& event,
                    const std::string& data_json)
{
    std::lock_guard<std::mutex> lk(mu_);
    std::fprintf(stdout,
        "{\"ts\":\"%s\",\"level\":\"%s\",\"event\":\"%s\",\"data\":%s}\n",
        now_iso8601().c_str(), level.c_str(), event.c_str(), data_json.c_str());
    std::fflush(stdout);
}

void Logger::order_fired(const std::string& order_id, const std::string& side,
                          double price, double size_usdc, const std::string& arm)
{
    char buf[256];
    std::snprintf(buf, sizeof(buf),
        "{\"order_id\":\"%s\",\"side\":\"%s\",\"price\":%.4f,\"size_usdc\":%.2f,\"arm\":\"%s\"}",
        order_id.c_str(), side.c_str(), price, size_usdc, arm.c_str());
    write("INFO", "ORDER_FIRED", buf);
}

void Logger::fill_received(const std::string& order_id, int64_t fire_to_fill_us) {
    char buf[128];
    std::snprintf(buf, sizeof(buf),
        "{\"order_id\":\"%s\",\"latency_us\":%lld}",
        order_id.c_str(), (long long)fire_to_fill_us);
    write("INFO", "FILL_RECEIVED", buf);
}

void Logger::position_opened(const std::string& order_id, double entry_price,
                               double size_usdc, const std::string& token_id)
{
    char buf[256];
    std::snprintf(buf, sizeof(buf),
        "{\"order_id\":\"%s\",\"entry_price\":%.4f,\"size_usdc\":%.2f,\"token_id\":\"%s\"}",
        order_id.c_str(), entry_price, size_usdc, token_id.c_str());
    write("INFO", "POSITION_OPENED", buf);
}

void Logger::position_closed(double entry_price, double exit_price,
                               double pnl_usdc, const std::string& reason)
{
    char buf[256];
    std::snprintf(buf, sizeof(buf),
        "{\"entry_price\":%.4f,\"exit_price\":%.4f,\"pnl_usdc\":%.4f,\"reason\":\"%s\"}",
        entry_price, exit_price, pnl_usdc, reason.c_str());
    write("INFO", "POSITION_CLOSED", buf);
}

void Logger::circuit_breaker(const std::string& reason,
                               const signals::SharedState& ss)
{
    char buf[512];
    std::snprintf(buf, sizeof(buf),
        "{\"reason\":\"%s\",\"p_true\":%.4f,\"p_market\":%.4f,"
        "\"btc_mid\":%.2f,\"bankroll\":%.2f,\"exposure\":%.2f,\"open_positions\":%d}",
        reason.c_str(),
        ss.p_true.load(std::memory_order_relaxed),
        ss.p_market.load(std::memory_order_relaxed),
        ss.btc_mid.load(std::memory_order_relaxed),
        ss.bankroll_usdc.load(std::memory_order_relaxed),
        ss.total_exposure_usdc.load(std::memory_order_relaxed),
        ss.open_position_count.load(std::memory_order_relaxed));
    write("ERROR", "CIRCUIT_BREAKER", buf);
}

void Logger::feed_fault(const std::string& source, int tier) {
    char buf[128];
    std::snprintf(buf, sizeof(buf),
        "{\"source\":\"%s\",\"tier\":%d}", source.c_str(), tier);
    write("WARN", "FEED_FAULT", buf);
}

void Logger::info(const std::string& event, const std::string& msg) {
    char buf[512];
    std::snprintf(buf, sizeof(buf), "{\"msg\":\"%s\"}", msg.c_str());
    write("INFO", event, buf);
}

void Logger::warn(const std::string& event, const std::string& msg) {
    char buf[512];
    std::snprintf(buf, sizeof(buf), "{\"msg\":\"%s\"}", msg.c_str());
    write("WARN", event, buf);
}

void Logger::error(const std::string& event, const std::string& msg) {
    char buf[512];
    std::snprintf(buf, sizeof(buf), "{\"msg\":\"%s\"}", msg.c_str());
    write("ERROR", event, buf);
}

} // namespace infra
