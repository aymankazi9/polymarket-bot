#include "watchdog.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

#include <pthread.h>
#include <sched.h>

namespace risk {

using namespace std::chrono;

static int64_t now_us_monotonic() {
    return duration_cast<microseconds>(
        steady_clock::now().time_since_epoch()).count();
}

static int64_t now_us_wall() {
    return duration_cast<microseconds>(
        system_clock::now().time_since_epoch()).count();
}

RiskWatchdog::RiskWatchdog(signals::SharedState&         ss,
                             std::vector<FlattenCallback> flatten_all,
                             double                       initial_bankroll_usdc)
    : ss_(ss)
    , flatten_cbs_(std::move(flatten_all))
    , initial_bankroll_(initial_bankroll_usdc)
{}

void RiskWatchdog::run() noexcept {
    // Attempt SCHED_FIFO — warns on EPERM (normal if not root/CAP_SYS_NICE)
    sched_param sp{};
    sp.sched_priority = 99;
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0)
        std::fprintf(stderr, "watchdog: SCHED_FIFO unavailable (EPERM) — running SCHED_OTHER\n");

    nav_window_start_us_   = now_us_monotonic();
    nav_window_start_usdc_ = ss_.bankroll_usdc.load(std::memory_order_relaxed);

    while (!stop_flag_.load(std::memory_order_relaxed)) {
        if (!tripped_) evaluate();
        std::this_thread::sleep_for(milliseconds(500));
    }
}

void RiskWatchdog::stop() noexcept {
    stop_flag_.store(true, std::memory_order_relaxed);
}

void RiskWatchdog::evaluate() noexcept {
    double btc = ss_.btc_mid.load(std::memory_order_relaxed);
    if (btc > 0.0) record_btc(btc, now_us_wall());

    check_consecutive_losses();
    check_btc_spike();
    check_nav_drawdown();

    // Soft guardrail: set kill_switch if bankroll collapses below floor
    double bankroll = ss_.bankroll_usdc.load(std::memory_order_relaxed);
    if (bankroll > 0.0 &&
        bankroll < initial_bankroll_ * constants::CEX_MARGIN_FLOOR_FRACTION) {
        trip("bankroll below margin floor");
    }
}

void RiskWatchdog::check_consecutive_losses() noexcept {
    int losses = ss_.consecutive_losses.load(std::memory_order_relaxed);
    if (losses < constants::CB_MAX_CONSECUTIVE_LOSSES) return;

    int64_t streak_start = ss_.first_loss_streak_us.load(std::memory_order_relaxed);
    if (streak_start == 0) return;

    double elapsed_s = (now_us_wall() - streak_start) / 1e6;
    if (elapsed_s <= constants::CB_LOSS_WINDOW_S)
        trip("consecutive_losses");
}

void RiskWatchdog::check_btc_spike() noexcept {
    if (btc_count_ < 2) return;

    int64_t cutoff = now_us_wall() -
        static_cast<int64_t>(constants::CB_BTC_SPIKE_WINDOW_S * 1e6);

    // Advance read pointer to evict stale entries
    while (btc_count_ > 0 && btc_buf_[btc_read_ & (BTC_BUF_N-1)].ts_us < cutoff) {
        btc_read_ = (btc_read_ + 1) & (BTC_BUF_N - 1);
        --btc_count_;
    }
    if (btc_count_ < 2) return;

    double lo = btc_buf_[btc_read_ & (BTC_BUF_N-1)].price;
    double hi = lo;
    for (int i = 0; i < btc_count_; ++i) {
        double p = btc_buf_[(btc_read_ + i) & (BTC_BUF_N-1)].price;
        if (p < lo) lo = p;
        if (p > hi) hi = p;
    }
    if (lo > 0.0 && (hi - lo) / lo > constants::CB_BTC_SPIKE_THRESHOLD)
        trip("btc_spike");
}

void RiskWatchdog::check_nav_drawdown() noexcept {
    int64_t now = now_us_monotonic();
    double  bankroll = ss_.bankroll_usdc.load(std::memory_order_relaxed);

    // Roll the 1-hour window
    double window_s = (now - nav_window_start_us_) / 1e6;
    if (window_s >= constants::CB_NAV_DRAWDOWN_WINDOW_S) {
        nav_window_start_us_   = now;
        nav_window_start_usdc_ = bankroll;
        return;
    }

    if (nav_window_start_usdc_ <= 0.0) return;
    double drawdown = (nav_window_start_usdc_ - bankroll) / nav_window_start_usdc_;
    if (drawdown > constants::CB_NAV_DRAWDOWN_THRESHOLD)
        trip("nav_drawdown");
}

void RiskWatchdog::record_btc(double price, int64_t ts_us) noexcept {
    int slot = btc_write_ & (BTC_BUF_N - 1);
    btc_buf_[slot] = {ts_us, price};
    btc_write_ = (btc_write_ + 1) & (BTC_BUF_N - 1);
    if (btc_count_ < BTC_BUF_N)
        ++btc_count_;
    else
        btc_read_ = (btc_read_ + 1) & (BTC_BUF_N - 1);  // overwrite oldest
}

void RiskWatchdog::trip(const char* reason) noexcept {
    if (tripped_) return;
    tripped_ = true;
    ss_.kill_switch.store(true, std::memory_order_release);
    std::fprintf(stderr,
        "CIRCUIT BREAKER: %s — p_true=%.4f p_market=%.4f btc=%.2f "
        "bankroll=%.2f exposure=%.2f open=%d\n",
        reason,
        ss_.p_true.load(std::memory_order_relaxed),
        ss_.p_market.load(std::memory_order_relaxed),
        ss_.btc_mid.load(std::memory_order_relaxed),
        ss_.bankroll_usdc.load(std::memory_order_relaxed),
        ss_.total_exposure_usdc.load(std::memory_order_relaxed),
        ss_.open_position_count.load(std::memory_order_relaxed));
    for (auto& cb : flatten_cbs_) cb();
}

} // namespace risk
