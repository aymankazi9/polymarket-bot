#pragma once
#include "../../constants.hpp"
#include <array>
#include <string>

// Two likelihood models as specified in CONTEXT.md §3.2 and §3.3.
//
// Model A — Student-t with dynamic vol scaling  (primary)
// Model B — 2-D calibration lookup table        (secondary; comparison in backtest)
//
// Both models return P(BTC > strike | data), i.e. the probability that the
// "BTC above strike" prediction resolves YES given current market conditions.
// For "below" markets, the caller inverts: 1 - result.
//
// No Boost. student_t_cdf uses a custom regularised incomplete beta function.

namespace signals {

// ---------------------------------------------------------------------------
// student_t_cdf: P(T ≤ t) for a t-distribution with `nu` degrees of freedom.
// Uses the regularised incomplete beta function via Lentz continued fraction.
// Accurate to ~1e-6 for all practical inputs.
// ---------------------------------------------------------------------------
double student_t_cdf(double t, double nu) noexcept;

// ---------------------------------------------------------------------------
// compute_likelihood: P(data | side) using the Student-t model.
//
// Parameters exactly as CONTEXT.md §3.2:
//   btc_price           — latest BTC mid price
//   strike              — market's BTC threshold price
//   time_remaining_s    — seconds until market resolution
//   realized_vol_60s    — 60-second rolling realized vol (decimal fraction)
//   ob_imbalance        — top-5 OB imbalance from Binance, in [-1, +1]
//   is_yes_side         — true → return P(data | YES); false → P(data | NO)
// ---------------------------------------------------------------------------
double compute_likelihood(
    double btc_price,
    double strike,
    double time_remaining_s,
    double realized_vol_60s,
    double ob_imbalance,
    bool   is_yes_side) noexcept;

// ---------------------------------------------------------------------------
// LookupTable: 2-D calibration table loaded from a CSV file.
//
// Axes (see CONTEXT.md §3.3):
//   Row    : distance_bps = round((btc - strike)/strike * 10000) ∈ [-100, +100]
//            → bin index = distance_bps + 100 ∈ [0, 199]
//   Column : time_bin = floor(time_remaining_s / 5) ∈ [0, 59]
//            → bin 0 = [0,5)s remaining; bin 59 = [295,300)s
//
// CSV layout: 200 rows × 60 columns of floating-point P(YES) values.
// ---------------------------------------------------------------------------
class LookupTable {
public:
    // Load calibration CSV. Returns false and leaves table unchanged on error.
    bool load_csv(const std::string& path);

    // Look up P(YES) for given inputs. Falls back to 0.5 if not loaded.
    double lookup(double btc_price, double strike,
                  double time_remaining_s) const noexcept;

    bool loaded() const noexcept { return loaded_; }

private:
    double table_[constants::STRIKE_DISTANCE_BINS][constants::TIME_REMAINING_BINS]{};
    bool   loaded_ = false;
};

} // namespace signals
