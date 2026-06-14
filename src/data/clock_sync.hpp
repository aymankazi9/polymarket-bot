#pragma once
#include "tick.hpp"
#include <atomic>
#include <cstdint>

// Per-source clock offset estimation.
//
// Each exchange timestamps its messages with its own server clock.  We measure
// the difference between the exchange timestamp and our local clock on every
// incoming message, then apply a slow EMA to track drift without reacting to
// individual noisy samples.
//
// The raw measurement is:
//   raw_offset_us = local_us_when_received - exchange_event_us
//
// This is positive when the exchange clock is behind our clock (or when there
// is significant network delay making us receive the message later than the
// exchange sent it).  Subtracting this offset from the exchange timestamp
// converts it to a local-clock equivalent.
//
// tick.timestamp_us = exchange_event_us + clock_offset_us(source)
//    ≈ exchange_event_us + (local_now - exchange_now)
//    ≈ local_now at the time the event occurred  (ignoring propagation delay)

namespace data {

class ClockSync {
public:
    // alpha: EMA smoothing factor per sample (0 < alpha ≤ 1).
    // Smaller alpha = slower adaptation = more noise rejection.
    explicit ClockSync(double alpha = 0.05);

    // Record a single clock measurement for `source`.
    // local_us:    local monotonic time when the message arrived (us since epoch)
    // exchange_us: exchange-reported event time (us since epoch)
    void update(Source source, int64_t local_us, int64_t exchange_us) noexcept;

    // Current best estimate of (local - exchange) offset for `source`, in us.
    // Returns 0 if no samples have been recorded yet.
    int64_t offset_us(Source source) const noexcept;

    // Reset all offset estimates (e.g. after a reconnect).
    void reset(Source source) noexcept;

private:
    double alpha_;

    // EMA offset as a double (us), protected by atomic load/store.
    // Written by Thread 1, read by Thread 1 (no cross-thread reads needed —
    // the converted timestamp goes into the ring buffer).
    // Use double rather than int64_t so the EMA doesn't quantise.
    alignas(64) std::atomic<double> offset_ema_[SOURCE_COUNT];
    alignas(64) std::atomic<bool>   initialised_[SOURCE_COUNT];
};

} // namespace data
