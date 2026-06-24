#include "market_scanner.hpp"
#include "../infra/logger.hpp"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <stdexcept>
#include <string>
#include <thread>

namespace data {

using namespace std::chrono;

namespace {

static size_t write_cb(char* ptr, size_t size, size_t nmemb, void* ud) {
    static_cast<std::string*>(ud)->append(ptr, size * nmemb);
    return size * nmemb;
}

// Parse ISO 8601 timestamp to epoch microseconds.
// Handles "2024-01-15T12:00:00Z" and "2024-01-15T12:00:00.000Z".
int64_t iso8601_to_us(const std::string& s) noexcept {
    if (s.empty()) return 0;
    std::tm tm{};
    const char* p = s.c_str();
    if (std::sscanf(p, "%4d-%2d-%2dT%2d:%2d:%2d",
                    &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
                    &tm.tm_hour, &tm.tm_min, &tm.tm_sec) != 6)
        return 0;
    tm.tm_year -= 1900;
    tm.tm_mon  -= 1;
    tm.tm_isdst = 0;

    double frac = 0.0;
    const char* dot = std::strchr(p, '.');
    if (dot) std::sscanf(dot, "%lf", &frac);

    time_t epoch = timegm(&tm);
    if (epoch == static_cast<time_t>(-1)) return 0;
    return static_cast<int64_t>(epoch) * 1'000'000LL
         + static_cast<int64_t>(frac * 1e6);
}

int64_t now_us() noexcept {
    return duration_cast<microseconds>(
        system_clock::now().time_since_epoch()).count();
}

// Epoch seconds at midnight UTC today.
int64_t today_midnight_epoch_s() noexcept {
    auto now_s = duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
    return (now_s / 86400) * 86400;
}

// True if slug or question matches the btc-updown-5m pattern.
bool is_btc_5m_market(const std::string& slug, const std::string& question) noexcept {
    auto contains = [](const std::string& haystack, const std::string& needle) {
        return haystack.find(needle) != std::string::npos;
    };
    return contains(slug, "btc-updown-5m") || contains(question, "btc-updown-5m");
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// MarketScanner
// ---------------------------------------------------------------------------

MarketScanner::MarketScanner(std::string clob_base_url)
    : clob_base_url_(std::move(clob_base_url))
{}

MarketScanner::~MarketScanner() { stop(); }

void MarketScanner::set_lifecycle_callbacks(LifecycleCallbacks cbs) {
    callbacks_ = std::move(cbs);
}

void MarketScanner::start() {
    running_.store(true, std::memory_order_release);
    thread_ = std::thread([this]{ run_loop(); });
}

void MarketScanner::stop() noexcept {
    running_.store(false, std::memory_order_release);
    if (thread_.joinable()) thread_.join();
}

int MarketScanner::active_count() const noexcept {
    std::shared_lock lock(mu_);
    return static_cast<int>(markets_.size());
}

std::vector<std::string> MarketScanner::all_token_ids() const {
    std::shared_lock lock(mu_);
    std::vector<std::string> ids;
    ids.reserve(markets_.size() * 2);
    for (const auto& [_, m] : markets_) {
        ids.push_back(m.token_id_yes);
        ids.push_back(m.token_id_no);
    }
    return ids;
}

// ---------------------------------------------------------------------------
// HTTP helper
// ---------------------------------------------------------------------------

std::string MarketScanner::http_get(const std::string& path) const noexcept {
    std::string url = clob_base_url_ + path;
    CURL* curl = curl_easy_init();
    if (!curl) return {};

    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

    curl_slist* hdrs = curl_slist_append(nullptr, "Accept: application/json");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);

    CURLcode rc = curl_easy_perform(curl);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        std::fprintf(stderr, "market_scanner: GET %s failed: %s\n",
                     path.c_str(), curl_easy_strerror(rc));
        return {};
    }
    return response;
}

// ---------------------------------------------------------------------------
// Background loop
// ---------------------------------------------------------------------------

void MarketScanner::scan_now() noexcept {
    scan_once();
    check_lifecycle();
}

void MarketScanner::run_loop() noexcept {
    // Caller is expected to call scan_now() before start() for the initial population.
    // Background thread only handles subsequent polls.
    while (running_.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(seconds(30));
        if (!running_.load(std::memory_order_acquire)) break;
        scan_once();
        check_lifecycle();
        check_midnight_rebate();
    }
}

// ---------------------------------------------------------------------------
// Discovery
// ---------------------------------------------------------------------------

void MarketScanner::scan_once() noexcept {
    // Paginate through all active bitcoin-tagged markets.
    std::string cursor;
    int new_count = 0;

    do {
        std::string path = "/markets?tag=bitcoin&active=true&limit=100";
        if (!cursor.empty()) path += "&next_cursor=" + cursor;

        std::string resp = http_get(path);
        if (resp.empty()) return;

        nlohmann::json j;
        try { j = nlohmann::json::parse(resp); }
        catch (...) { return; }

        // Advance cursor for next page (empty or null → last page)
        cursor = {};
        if (j.contains("next_cursor") && !j["next_cursor"].is_null())
            cursor = j["next_cursor"].get<std::string>();
        if (cursor == "LTE=") cursor = {};  // Polymarket sentinel for "no more pages"

        if (!j.contains("data")) break;

        for (const auto& m : j["data"]) {
            // Skip closed, inactive, or fee-ineligible markets
            if (!m.value("active", false))       continue;
            if ( m.value("closed", true))        continue;
            if (!m.value("feesEnabled", false))  continue;

            std::string slug     = m.value("slug", "");
            std::string question = m.value("question", "");
            if (!is_btc_5m_market(slug, question)) continue;

            std::string cid = m.value("conditionId", "");
            if (cid.empty()) continue;

            {
                std::shared_lock rl(mu_);
                if (markets_.count(cid)) continue;  // already known
            }

            // Extract token IDs (Polymarket: tokens[0]=YES, tokens[1]=NO)
            std::string token_yes, token_no;
            if (m.contains("tokens") && m["tokens"].size() >= 2) {
                const auto& t = m["tokens"];
                for (const auto& tok : t) {
                    std::string outcome = tok.value("outcome", "");
                    std::string tid     = tok.value("token_id", "");
                    if (outcome == "Yes")  token_yes = tid;
                    else                   token_no  = tid;
                }
            }
            if (token_yes.empty() || token_no.empty()) continue;

            std::string end_date = m.value("endDateIso", "");
            int64_t res_us = iso8601_to_us(end_date);
            if (res_us == 0) continue;

            // Skip already-expired markets
            if (res_us < now_us()) continue;

            ActiveMarket am;
            am.condition_id    = cid;
            am.token_id_yes    = token_yes;
            am.token_id_no     = token_no;
            am.resolution_us   = res_us;
            am.min_incentive_size    = 0.0;
            am.max_incentive_spread  = 0.0;

            // Fetch per-market CLOB parameters
            fetch_market_info(am);

            {
                std::unique_lock wl(mu_);
                markets_.emplace(cid, std::move(am));
                ++new_count;
            }

            auto& log = infra::Logger::instance();
            log.info("SCANNER", "discovered market " + cid + " (" + slug + ")");
        }
    } while (!cursor.empty());

    if (new_count > 0) {
        auto& log = infra::Logger::instance();
        log.info("SCANNER", std::to_string(new_count) + " new market(s) discovered");
    }
}

// ---------------------------------------------------------------------------
// Per-market CLOB info (incentive parameters)
// ---------------------------------------------------------------------------

void MarketScanner::fetch_market_info(ActiveMarket& m) noexcept {
    std::string resp = http_get("/clob-markets/" + m.condition_id);
    if (resp.empty()) return;

    try {
        auto j = nlohmann::json::parse(resp);
        // min_incentive_size and max_incentive_spread may be nested or top-level
        if (j.contains("min_incentive_size"))
            m.min_incentive_size = std::stod(j["min_incentive_size"].get<std::string>());
        if (j.contains("max_incentive_spread"))
            m.max_incentive_spread = std::stod(j["max_incentive_spread"].get<std::string>());
    } catch (...) {}
}

// ---------------------------------------------------------------------------
// Lifecycle management (CONTEXT_ADDENDUM A2.2 / A5.1)
// ---------------------------------------------------------------------------

void MarketScanner::check_lifecycle() noexcept {
    int64_t now = now_us();

    // Collect pending callbacks under the write lock (flag updates only).
    // Fire callbacks AFTER releasing the lock to avoid holding scanner.mu_
    // while callers may acquire other mutexes (e.g. osm_map_mu in main.cpp).
    enum class CB { StopEntries, CancelQuotes, ClosePositions, ForceClose, Remove };
    std::vector<std::pair<std::string, CB>> pending;

    {
        std::unique_lock wl(mu_);
        for (auto& [cid, m] : markets_) {
            int64_t rem_us = m.resolution_us - now;

            if (!m.warn_logged && rem_us <= 120'000'000LL) {
                m.warn_logged = m.entries_stopped = true;
                pending.push_back({cid, CB::StopEntries});
            }
            if (!m.quotes_cancelled && rem_us <= 45'000'000LL) {
                m.quotes_cancelled = true;
                pending.push_back({cid, CB::CancelQuotes});
            }
            if (!m.positions_closed && rem_us <= 30'000'000LL) {
                m.positions_closed = true;
                pending.push_back({cid, CB::ClosePositions});
            }
            if (!m.force_closed && rem_us <= 10'000'000LL) {
                m.force_closed = true;
                pending.push_back({cid, CB::ForceClose});
            }
            if (!m.redemption_checked && rem_us <= -60'000'000LL) {
                m.redemption_checked = true;
                pending.push_back({cid, CB::Remove});
            }
        }
        // Erase expired markets under the same lock
        for (const auto& [cid, type] : pending)
            if (type == CB::Remove) markets_.erase(cid);
    }

    // Fire callbacks and log outside the lock
    auto& log = infra::Logger::instance();
    for (const auto& [cid, type] : pending) {
        switch (type) {
            case CB::StopEntries:
                log.warn("SCANNER", "market " + cid + " T-120s — taker entries stopped");
                if (callbacks_.on_stop_entries) callbacks_.on_stop_entries(cid);
                break;
            case CB::CancelQuotes:
                log.info("SCANNER", "market " + cid + " T-45s — cancelling maker quotes");
                if (callbacks_.on_cancel_quotes) callbacks_.on_cancel_quotes(cid);
                break;
            case CB::ClosePositions:
                log.info("SCANNER", "market " + cid + " T-30s — closing taker positions");
                if (callbacks_.on_close_positions) callbacks_.on_close_positions(cid);
                break;
            case CB::ForceClose:
                log.warn("SCANNER", "market " + cid + " T-10s — forced expiry close");
                if (callbacks_.on_force_close) callbacks_.on_force_close(cid);
                break;
            case CB::Remove:
                log.info("SCANNER", "market " + cid + " resolved — removed from active set");
                break;
        }
    }
}

// ---------------------------------------------------------------------------
// Midnight rebate reconciliation (CONTEXT_ADDENDUM A4.2)
// Stage 6 will add the actual Alchemy query here.
// For now: log the reconciliation window so Stage 6 can hook in.
// ---------------------------------------------------------------------------

void MarketScanner::check_midnight_rebate() noexcept {
    int64_t midnight_s = today_midnight_epoch_s();
    int64_t last       = last_rebate_check_epoch_s_.load(std::memory_order_relaxed);

    // Fire once per day, 5 minutes after midnight UTC
    int64_t trigger_s = midnight_s + 300;
    int64_t now_s     = duration_cast<seconds>(
        system_clock::now().time_since_epoch()).count();

    if (now_s >= trigger_s && last < midnight_s) {
        last_rebate_check_epoch_s_.store(midnight_s, std::memory_order_relaxed);
        infra::Logger::instance().info("SCANNER",
            "midnight rebate reconciliation window (Stage 6 hook)");
        // Stage 6: query Alchemy for USDC transfers from rebate distributor,
        // update bot_daily_rebate_usdc Prometheus gauge.
    }
}

} // namespace data
