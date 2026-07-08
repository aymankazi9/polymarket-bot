// bt-runner: backtest replay engine for polymarket-bot.  CONTEXT.md §11.
//
// Usage:
//   bt-runner --tick-dir <dir> --bankroll <usdc>
//             [--out-csv <file>]        (default: trades.csv)
//             [--sample-frac <0-1>]     (in-sample split, default: 0.70)
//
// Reads all .bin tick files in <dir> (produced by tick-recorder), merges and
// sorts them, detects 5-minute BTC prediction-market windows, and replays
// them through the production BayesianEngine + PositionManager.  Taker fills
// are simulated at the recorded NBBO with a FILL_LATENCY_US timestamp offset.
//
// Limitations vs live bot:
//   - Taker fills only; maker arm requires order-book replay (not in tick files).
//   - Funding rate = 0.0 throughout; auxiliary signal contribution is zero.
//   - PositionManager Coinbase PnL uses (entry - exit) * qty for both directions,
//     matching production code exactly (correct for short-BTC / above markets).
//   - POLYMARKET ticks are assumed to be for the ABOVE market's YES token.
//     Below-market replays derive the below price as 1 - above, and flip the
//     tick before feeding the below-configured BayesianEngine.

#include "src/signal/bayesian_engine.hpp"
#include "src/execution/position_manager.hpp"
#include "src/risk/sizing.hpp"
#include "src/data/tick.hpp"
#include "src/data/ring_buffer.hpp"
#include "src/data/feed_manager.hpp"
#include "constants.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <limits>
#include <map>
#include <string>
#include <sys/stat.h>
#include <vector>

static_assert(sizeof(data::Tick) == 64,
    "Tick struct size changed — update format table in tick-recorder and rebuild");

static constexpr int64_t FILL_LATENCY_US    = 5'000;               // 5 ms simulated latency
static constexpr int64_t MARKET_DURATION_US = 300LL * 1'000'000LL; // 5-minute markets
static constexpr int64_t WARMUP_US          = 120LL * 1'000'000LL; // warm-up before market open
static constexpr double  ANNUALISE          = 15.87451;             // sqrt(252)

// ─────────────────────────────────────────────────────────────────────────────
// Trade record
// ─────────────────────────────────────────────────────────────────────────────

struct Trade {
    int64_t     entry_us = 0, exit_us = 0;
    std::string market_id;
    bool        is_above        = true;
    double      entry_price     = 0.0;  // Poly fill price (probability)
    double      exit_price      = 0.0;
    double      shares          = 0.0;
    double      hedge_entry_btc = 0.0;
    double      hedge_exit_btc  = 0.0;
    double      hedge_qty_btc   = 0.0;
    double      pnl_usdc        = 0.0;
    double      predicted_edge  = 0.0;
    bool        expired         = false; // true = forced close at resolution
};

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

static std::string ts_to_iso(int64_t us) {
    std::time_t t = static_cast<std::time_t>(us / 1'000'000LL);
    std::tm     m{};
    gmtime_r(&t, &m);
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ",
        m.tm_year + 1900, m.tm_mon + 1, m.tm_mday,
        m.tm_hour, m.tm_min, m.tm_sec);
    return buf;
}

// Replicate OSM::compute_e_min_taker using current SharedState spread.
static double compute_e_min(const signals::SharedState& ss) noexcept {
    double b = ss.poly_best_bid.load(std::memory_order_relaxed);
    double a = ss.poly_best_ask.load(std::memory_order_relaxed);
    double h = (a > b && a - b < 1.0) ? (a - b) / 2.0
                                       : constants::HALF_SPREAD_DEFAULT_CENTS / 100.0;
    return (constants::FEE_TAKER_CENTS + h * 100.0
          + constants::FRICTION_SETTLEMENT_CENTS
          + constants::ALPHA_BUFFER_CENTS) / 100.0;
}

// ─────────────────────────────────────────────────────────────────────────────
// Tick loading
// ─────────────────────────────────────────────────────────────────────────────

static std::vector<data::Tick> load_tick_dir(const std::string& dir) {
    std::vector<data::Tick> all;
    DIR* d = opendir(dir.c_str());
    if (!d) {
        std::fprintf(stderr, "bt-runner: cannot open tick dir '%s': %s\n",
                     dir.c_str(), std::strerror(errno));
        return all;
    }
    struct dirent* ent;
    while ((ent = readdir(d)) != nullptr) {
        const char* nm = ent->d_name;
        size_t      ln = std::strlen(nm);
        if (ln < 5 || std::strcmp(nm + ln - 4, ".bin") != 0) continue;
        std::string path = dir + (dir.back() == '/' ? "" : "/") + nm;
        FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) continue;
        struct stat st{};
        if (::stat(path.c_str(), &st) == 0) {
            size_t n    = static_cast<size_t>(st.st_size) / sizeof(data::Tick);
            size_t prev = all.size();
            all.resize(prev + n);
            size_t got  = std::fread(all.data() + prev, sizeof(data::Tick), n, f);
            all.resize(prev + got);
        }
        std::fclose(f);
    }
    closedir(d);

    std::sort(all.begin(), all.end(),
        [](const data::Tick& a, const data::Tick& b){
            return a.timestamp_us < b.timestamp_us;
        });

    std::fprintf(stderr, "bt-runner: loaded %zu ticks from %s\n", all.size(), dir.c_str());
    return all;
}

// ─────────────────────────────────────────────────────────────────────────────
// Market detection — one above + one below market per 5-minute BTC boundary
// ─────────────────────────────────────────────────────────────────────────────

struct Market {
    int64_t     open_us, resolution_us;
    double      strike;
    bool        is_above;
    std::string id;
};

static std::vector<Market> detect_markets(const std::vector<data::Tick>& ticks) {
    std::vector<Market> out;
    int64_t t_first = 0, t_last = 0;
    for (const auto& t : ticks) {
        if (t.source == data::Source::BINANCE && t.mid > 0.0) {
            if (!t_first) t_first = t.timestamp_us;
            t_last = t.timestamp_us;
        }
    }
    if (!t_first) return out;

    int64_t boundary = ((t_first / MARKET_DURATION_US) + 1) * MARKET_DURATION_US;
    double  last_btc = 0.0;
    size_t  cur      = 0;

    for (; boundary + MARKET_DURATION_US <= t_last; boundary += MARKET_DURATION_US) {
        while (cur < ticks.size() && ticks[cur].timestamp_us <= boundary) {
            if (ticks[cur].source == data::Source::BINANCE && ticks[cur].mid > 0.0)
                last_btc = ticks[cur].mid;
            ++cur;
        }
        if (last_btc <= 0.0) continue;

        char idbuf[128];
        std::snprintf(idbuf, sizeof(idbuf), "BTC-ABOVE-%.2f@%s",
            last_btc, ts_to_iso(boundary + MARKET_DURATION_US).c_str());
        out.push_back({boundary, boundary + MARKET_DURATION_US, last_btc, true,  idbuf});

        std::snprintf(idbuf, sizeof(idbuf), "BTC-BELOW-%.2f@%s",
            last_btc, ts_to_iso(boundary + MARKET_DURATION_US).c_str());
        out.push_back({boundary, boundary + MARKET_DURATION_US, last_btc, false, idbuf});
    }
    std::fprintf(stderr, "bt-runner: %zu markets (%zu windows)\n",
                 out.size(), out.size() / 2);
    return out;
}

// ─────────────────────────────────────────────────────────────────────────────
// Per-market replay
// ─────────────────────────────────────────────────────────────────────────────

static void replay_market(const Market&               mkt,
                           const std::vector<data::Tick>& ticks,
                           size_t t_start, size_t t_end,
                           double& bankroll, double& exposure,
                           std::vector<Trade>& out)
{
    // Fresh state per market — no leakage across windows.
    data::TickBuffer     dummy_ring;
    data::FeedManager    dummy_feed(dummy_ring); // never run(); funding_rate = 0.0
    signals::SharedState ss;

    signals::BayesianEngine engine(dummy_ring, ss, dummy_feed, {
        "",             // token_id: unused in process_tick
        mkt.strike,
        mkt.resolution_us,
        mkt.is_above
    });

    execution::PositionManager pos;
    bool    entries_stopped = false;
    int64_t last_entry_us   = 0;

    // Entry state (valid while pos.is_open())
    double  e_price = 0.0, e_hedge_btc = 0.0, e_hedge_qty = 0.0;
    double  e_shares = 0.0, e_edge = 0.0;
    int64_t e_us = 0;

    // Unified close helper: records the trade and updates bookkeeping.
    auto do_close = [&](double exit_bid, double btc_exit,
                        int64_t exit_us_val, bool expired_flag) {
        Trade tr;
        tr.entry_us        = e_us;
        tr.exit_us         = exit_us_val;
        tr.market_id       = mkt.id;
        tr.is_above        = mkt.is_above;
        tr.entry_price     = e_price;
        tr.exit_price      = exit_bid;
        tr.shares          = e_shares;
        tr.hedge_entry_btc = e_hedge_btc;
        tr.hedge_exit_btc  = btc_exit;
        tr.hedge_qty_btc   = e_hedge_qty;
        // Matches PositionManager::combined_pnl_usdc formula for both legs.
        tr.pnl_usdc        = (exit_bid - e_price) * e_shares
                           + (e_hedge_btc - btc_exit) * e_hedge_qty;
        tr.predicted_edge  = e_edge;
        tr.expired         = expired_flag;
        out.push_back(tr);
        exposure -= e_shares * e_price;
        if (exposure < 0.0) exposure = 0.0;
        bankroll += tr.pnl_usdc;
        pos.close();
    };

    for (size_t i = t_start; i < t_end; ++i) {
        data::Tick tick = ticks[i]; // copy — may be flipped for below markets

        // For below markets, flip POLYMARKET ticks so the engine sees the
        // below-YES price (= 1 - above-YES price) and resets its prior correctly.
        if (!mkt.is_above && tick.source == data::Source::POLYMARKET
            && tick.mid > 0.0 && tick.mid < 1.0)
        {
            double orig_bid  = tick.best_bid;
            double orig_ask  = tick.best_ask;
            tick.mid      = 1.0 - tick.mid;
            tick.best_bid = 1.0 - orig_ask;
            tick.best_ask = 1.0 - orig_bid;
        }

        engine.feed_tick(tick); // updates ss atomics

        double t_rem = (static_cast<double>(mkt.resolution_us)
                      - static_cast<double>(ticks[i].timestamp_us)) / 1e6;

        // ── Open position management ──────────────────────────────────────
        if (pos.is_open()) {
            if (t_rem <= 0.0) {
                // Past resolution: outcome determined by final BTC price.
                double btc_fin = ss.btc_mid.load(std::memory_order_relaxed);
                if (btc_fin <= 0.0) btc_fin = e_hedge_btc;
                bool wins = mkt.is_above ? (btc_fin > mkt.strike)
                                         : (btc_fin <= mkt.strike);
                do_close(wins ? 1.0 : 0.0, btc_fin, mkt.resolution_us, true);
                break;
            }

            // T-30s forced close (mirrors HEDGE_CLOSE_BEFORE_EXPIRY_S gate).
            if (t_rem <= constants::HEDGE_CLOSE_BEFORE_EXPIRY_S) {
                double bid = ss.poly_best_bid.load(std::memory_order_relaxed);
                double btc = ss.btc_mid.load(std::memory_order_relaxed);
                do_close(bid, btc, ticks[i].timestamp_us + FILL_LATENCY_US, false);
                continue;
            }

            // PositionManager: hard stop / trailing stop / early exit.
            auto reason = pos.evaluate(ss);
            if (reason != execution::PositionManager::ExitReason::NONE) {
                double bid = ss.poly_best_bid.load(std::memory_order_relaxed);
                double btc = ss.btc_mid.load(std::memory_order_relaxed);
                do_close(bid, btc, ticks[i].timestamp_us + FILL_LATENCY_US, false);
            }
            continue;
        }

        // ── Entry logic (BINANCE ticks only; after market open) ───────────
        if (ticks[i].source != data::Source::BINANCE) continue;
        if (ticks[i].timestamp_us < mkt.open_us)       continue;
        if (entries_stopped)                            continue;
        if (t_rem <= 120.0) { entries_stopped = true;  continue; }

        // Skip extreme-vol regime.
        if (ss.regime.load(std::memory_order_relaxed) == signals::Regime::SPIKE) continue;

        // Taker cooldown between entries on the same market.
        if (last_entry_us > 0 &&
            (ticks[i].timestamp_us - last_entry_us) / 1e6
                < constants::MIN_TAKER_INTERVAL_S)
            continue;

        double p  = ss.p_true.load(std::memory_order_acquire);   // P(this_dir | data)
        double pm = ss.p_market.load(std::memory_order_acquire);  // P(this_dir | market)
        double bt = ss.btc_mid.load(std::memory_order_relaxed);
        if (p <= 0.0 || pm <= 0.0 || bt <= 0.0) continue;

        // Edge: p and pm are already in this market's direction because:
        //   - above engine: p = P(above|data), pm = P(above|market)  [tick unchanged]
        //   - below engine: p = P(below|data), pm = P(below|market)  [tick flipped]
        double edge = p - pm;
        if (edge < compute_e_min(ss)) continue;

        Amount bk  = Amount::from_double(bankroll);
        Amount exp = Amount::from_double(exposure);
        Amount sz  = risk::kelly_size_usdc(p, pm, bk, exp);
        if (sz < Amount::from_double(1.0)) continue;

        double hqty = risk::hedge_btc_qty(sz, bt, bk);
        if (hqty < 0.001) continue;

        double ask = ss.poly_best_ask.load(std::memory_order_relaxed);
        if (ask <= 0.0 || ask >= 1.0) continue;

        double shr = sz.to_double() / ask;

        execution::PositionManager::EntryData ed;
        ed.entry_price_poly   = ask;
        ed.shares             = shr;
        ed.hedge_entry_btc    = bt;
        ed.hedge_qty_btc      = hqty;
        ed.initial_edge_value = Amount::from_double(edge * sz.to_double());
        ed.is_yes_long        = true;  // mirrors taker_arm.cpp:146

        pos.open(ed);
        e_price     = ask;
        e_hedge_btc = bt;
        e_hedge_qty = hqty;
        e_shares    = shr;
        e_edge      = edge;
        e_us        = ticks[i].timestamp_us + FILL_LATENCY_US;
        last_entry_us = ticks[i].timestamp_us;
        exposure += sz.to_double();
    }

    // Loop ended with open position (resolution not crossed in tick data).
    if (pos.is_open()) {
        double btc_fin = ss.btc_mid.load(std::memory_order_relaxed);
        if (btc_fin <= 0.0) btc_fin = e_hedge_btc;
        bool wins = mkt.is_above ? (btc_fin > mkt.strike) : (btc_fin <= mkt.strike);
        do_close(wins ? 1.0 : 0.0, btc_fin, mkt.resolution_us, true);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Sharpe ratio (daily P&L aggregation, annualised)
// ─────────────────────────────────────────────────────────────────────────────

static double daily_sharpe(const std::vector<Trade>& trades,
                             int64_t from_us, int64_t to_us) {
    std::map<int, double> d;
    for (const auto& t : trades) {
        if (t.exit_us < from_us || t.exit_us >= to_us) continue;
        d[static_cast<int>(t.exit_us / (86400LL * 1'000'000LL))] += t.pnl_usdc;
    }
    if (d.size() < 2) return std::numeric_limits<double>::quiet_NaN();

    std::vector<double> v;
    v.reserve(d.size());
    for (auto& kv : d) v.push_back(kv.second);

    double mu = 0.0;
    for (double x : v) mu += x;
    mu /= static_cast<double>(v.size());

    double s2 = 0.0;
    for (double x : v) s2 += (x - mu) * (x - mu);
    s2 /= static_cast<double>(v.size() - 1);

    double sd = std::sqrt(s2);
    return (sd < 1e-12) ? std::numeric_limits<double>::quiet_NaN()
                        : (mu / sd) * ANNUALISE;
}

// ─────────────────────────────────────────────────────────────────────────────
// CSV output
// ─────────────────────────────────────────────────────────────────────────────

static void write_csv(const std::string& path, const std::vector<Trade>& trades) {
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) {
        std::fprintf(stderr, "bt-runner: cannot write '%s': %s\n",
                     path.c_str(), std::strerror(errno));
        return;
    }
    std::fprintf(f,
        "entry_time,exit_time,market,direction,"
        "entry_price,exit_price,shares,"
        "hedge_entry_btc,hedge_exit_btc,hedge_qty_btc,"
        "pnl_usdc,predicted_edge,expired\n");
    for (const auto& t : trades)
        std::fprintf(f,
            "%s,%s,%s,%s,"
            "%.6f,%.6f,%.6f,"
            "%.2f,%.2f,%.6f,"
            "%.6f,%.6f,%d\n",
            ts_to_iso(t.entry_us).c_str(),
            ts_to_iso(t.exit_us).c_str(),
            t.market_id.c_str(),
            t.is_above ? "ABOVE" : "BELOW",
            t.entry_price, t.exit_price, t.shares,
            t.hedge_entry_btc, t.hedge_exit_btc, t.hedge_qty_btc,
            t.pnl_usdc, t.predicted_edge, static_cast<int>(t.expired));
    std::fclose(f);
    std::fprintf(stderr, "bt-runner: wrote %zu trades to %s\n",
                 trades.size(), path.c_str());
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    std::string tick_dir;
    std::string out_csv    = "trades.csv";
    double      bankroll   = 1000.0;
    double      samp_frac  = 0.70;

    for (int i = 1; i < argc; ++i) {
        if      (!std::strcmp(argv[i], "--tick-dir")    && i+1 < argc) tick_dir  = argv[++i];
        else if (!std::strcmp(argv[i], "--bankroll")    && i+1 < argc) bankroll  = std::atof(argv[++i]);
        else if (!std::strcmp(argv[i], "--out-csv")     && i+1 < argc) out_csv   = argv[++i];
        else if (!std::strcmp(argv[i], "--sample-frac") && i+1 < argc) samp_frac = std::atof(argv[++i]);
        else {
            std::fprintf(stderr,
                "Usage: bt-runner --tick-dir <dir> --bankroll <usdc>"
                " [--out-csv <file>] [--sample-frac <0-1>]\n");
            return 1;
        }
    }
    if (tick_dir.empty()) {
        std::fprintf(stderr, "bt-runner: --tick-dir is required\n");
        return 1;
    }

    auto ticks = load_tick_dir(tick_dir);
    if (ticks.empty()) {
        std::fprintf(stderr, "bt-runner: no ticks loaded — check --tick-dir\n");
        return 1;
    }

    auto markets = detect_markets(ticks);
    if (markets.empty()) {
        std::fprintf(stderr, "bt-runner: no 5-minute markets detected in tick data\n");
        return 1;
    }

    // Split at the IS boundary (by number of 5-min windows, not wall time).
    size_t  n_windows = markets.size() / 2;
    size_t  split_wnd = static_cast<size_t>(samp_frac * static_cast<double>(n_windows));
    int64_t split_us  = (split_wnd * 2 < markets.size())
                      ? markets[split_wnd * 2].open_us
                      : markets.back().resolution_us;

    std::vector<Trade> trades;
    double exposure = 0.0;

    for (const auto& mkt : markets) {
        int64_t ws = mkt.open_us - WARMUP_US;

        // Binary search for warmup start
        size_t lo = 0, hi = ticks.size();
        while (lo < hi) {
            size_t mid = (lo + hi) / 2;
            if (ticks[mid].timestamp_us < ws) lo = mid + 1; else hi = mid;
        }
        size_t t_start = lo;

        // Binary search for resolution end (first tick past resolution)
        lo = t_start; hi = ticks.size();
        while (lo < hi) {
            size_t mid = (lo + hi) / 2;
            if (ticks[mid].timestamp_us <= mkt.resolution_us) lo = mid + 1; else hi = mid;
        }
        size_t t_end = lo;

        if (t_start < t_end)
            replay_market(mkt, ticks, t_start, t_end, bankroll, exposure, trades);
    }

    write_csv(out_csv, trades);

    int64_t t0 = ticks.front().timestamp_us;
    int64_t t1 = ticks.back().timestamp_us;

    double s_is  = daily_sharpe(trades, t0, split_us);
    double s_oos = daily_sharpe(trades, split_us, t1);
    double s_all = daily_sharpe(trades, t0, t1);

    auto fmt = [](double s) -> std::string {
        return std::isnan(s) ? "n/a (<2 trading days)" : std::to_string(s);
    };

    double total_pnl = 0.0;
    int    n_above = 0, n_below = 0, n_win = 0, n_expired = 0;
    for (const auto& t : trades) {
        total_pnl += t.pnl_usdc;
        if (t.is_above) ++n_above; else ++n_below;
        if (t.pnl_usdc > 0.0) ++n_win;
        if (t.expired)  ++n_expired;
    }

    std::fprintf(stdout,
        "═══════════════════════════════════════════════\n"
        "bt-runner results\n"
        "═══════════════════════════════════════════════\n"
        "Tick range   : %s → %s\n"
        "Markets      : %zu windows, %zu total\n"
        "Trades       : %zu  (above=%d below=%d expired=%d)\n"
        "Win rate     : %.1f%%\n"
        "Total P&L    : %+.2f USDC\n"
        "Final bankroll: %.2f USDC\n"
        "───────────────────────────────────────────────\n"
        "Sharpe (daily, annualised):\n"
        "  In-sample  (%.0f%%) : %s\n"
        "  Out-of-sample      : %s\n"
        "  Full period        : %s\n"
        "IS cutoff    : %s\n"
        "═══════════════════════════════════════════════\n",
        ts_to_iso(t0).c_str(), ts_to_iso(t1).c_str(),
        n_windows, markets.size(),
        trades.size(), n_above, n_below, n_expired,
        trades.empty() ? 0.0 : 100.0 * n_win / static_cast<double>(trades.size()),
        total_pnl, bankroll,
        samp_frac * 100.0, fmt(s_is).c_str(),
        fmt(s_oos).c_str(),
        fmt(s_all).c_str(),
        ts_to_iso(split_us).c_str());

    return 0;
}
