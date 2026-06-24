#pragma once
#include <atomic>
#include <chrono>
#include <functional>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// MarketScanner — polls the Polymarket CLOB REST API every 30s to discover
// and manage active BTC 5-minute prediction markets.  CONTEXT_ADDENDUM A2.
//
// Threading:
//   Background scanner thread holds an exclusive lock only while writing.
//   All consumers (OSM, FeedManager token refresh) take a shared lock via
//   with_markets().  The scanner thread is separate from Thread 1 (FeedManager).
//
// Lifecycle callbacks are invoked from the scanner's background thread.
// They must be fast and non-blocking — offload any heavy work to Thread 3.

namespace data {

struct ActiveMarket {
    std::string condition_id;
    std::string token_id_yes;
    std::string token_id_no;
    int64_t     resolution_us;         // epoch microseconds (from endDateIso)
    double      min_incentive_size;    // from getClobMarketInfo; quote size floor
    double      max_incentive_spread;  // from getClobMarketInfo; max spread for rewards
    // BTC-updown-5m markets: YES = BTC goes up; strike is relative (no absolute level needed)
    bool        is_above       = true;  // YES wins when BTC moves up
    double      strike_price   = 0.0;   // absolute strike; 0.0 for relative-direction markets
    // Lifecycle state — only advanced forward, never reset
    bool        warn_logged      = false;
    bool        entries_stopped  = false;
    bool        quotes_cancelled = false;
    bool        positions_closed = false;
    bool        force_closed     = false;
    bool        redemption_checked = false;
};

// Lifecycle callbacks (CONTEXT_ADDENDUM A2.2 / A5.1).
// Each is invoked once per market, in order, as the expiry deadlines pass.
// Register before calling start().  Unset callbacks are silently skipped.
struct LifecycleCallbacks {
    std::function<void(const std::string& condition_id)> on_stop_entries;    // T-120s
    std::function<void(const std::string& condition_id)> on_cancel_quotes;   // T-45s
    std::function<void(const std::string& condition_id)> on_close_positions; // T-30s
    std::function<void(const std::string& condition_id)> on_force_close;     // T-10s
};

class MarketScanner {
public:
    explicit MarketScanner(std::string clob_base_url = "https://clob.polymarket.com");
    ~MarketScanner();

    // Register lifecycle callbacks.  Must be called before start().
    void set_lifecycle_callbacks(LifecycleCallbacks cbs);

    // Perform one discovery + lifecycle check synchronously in the calling thread.
    // Call this before start() to populate the market map at startup.
    void scan_now() noexcept;

    // Start background polling thread (polls every 30s; does NOT do an initial scan).
    void start();
    void stop() noexcept;

    // Invoke fn under a shared read lock.
    // fn receives: const std::unordered_map<std::string, ActiveMarket>&
    template<typename Fn>
    void with_markets(Fn&& fn) const {
        std::shared_lock lock(mu_);
        fn(markets_);
    }

    // Snapshot of all token IDs (YES + NO) across active markets.
    // Used to (re-)subscribe the FeedManager to new markets.
    std::vector<std::string> all_token_ids() const;

    int active_count() const noexcept;

private:
    void run_loop() noexcept;
    void scan_once() noexcept;
    void fetch_market_info(ActiveMarket& m) noexcept;
    void check_lifecycle() noexcept;
    void check_midnight_rebate() noexcept;

    // Returns response body or empty string on failure.
    std::string http_get(const std::string& path) const noexcept;

    std::string          clob_base_url_;
    LifecycleCallbacks   callbacks_;

    mutable std::shared_mutex                         mu_;
    std::unordered_map<std::string, ActiveMarket>     markets_;  // key = condition_id

    std::thread       thread_;
    std::atomic<bool> running_{false};

    // Track last midnight-UTC we checked for rebates (epoch seconds)
    std::atomic<int64_t> last_rebate_check_epoch_s_{0};
};

} // namespace data
