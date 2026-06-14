#include "likelihood.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace signal {

namespace {

// ---------------------------------------------------------------------------
// Regularised incomplete beta function  I(x; a, b)
// Using Lentz's continued-fraction algorithm (Numerical Recipes §6.4).
//
// Accurate to ~3e-7 for most inputs.  Switches between two formulations
// (direct CF and symmetry relation x→1-x) for convergence stability.
// ---------------------------------------------------------------------------

static constexpr int    CF_MAX_ITER = 200;
static constexpr double CF_EPS      = 3.0e-7;
static constexpr double CF_FPMIN    = 1.0e-30;

// betacf: evaluates the continued fraction for the incomplete beta function.
double betacf(double a, double b, double x) noexcept {
    double qab = a + b;
    double qap = a + 1.0;
    double qam = a - 1.0;
    double c = 1.0;
    double d = 1.0 - qab * x / qap;
    if (std::fabs(d) < CF_FPMIN) d = CF_FPMIN;
    d = 1.0 / d;
    double h = d;

    for (int m = 1; m <= CF_MAX_ITER; ++m) {
        double m2 = 2.0 * m;

        // Even step
        double aa = m * (b - m) * x / ((qam + m2) * (a + m2));
        d = 1.0 + aa * d;
        if (std::fabs(d) < CF_FPMIN) d = CF_FPMIN;
        c = 1.0 + aa / c;
        if (std::fabs(c) < CF_FPMIN) c = CF_FPMIN;
        d = 1.0 / d;
        h *= d * c;

        // Odd step
        aa = -(a + m) * (qab + m) * x / ((a + m2) * (qap + m2));
        d = 1.0 + aa * d;
        if (std::fabs(d) < CF_FPMIN) d = CF_FPMIN;
        c = 1.0 + aa / c;
        if (std::fabs(c) < CF_FPMIN) c = CF_FPMIN;
        d = 1.0 / d;
        double delta = d * c;
        h *= delta;
        if (std::fabs(delta - 1.0) < CF_EPS) break;
    }
    return h;
}

// betai: regularised incomplete beta function I(x; a, b) ∈ [0, 1].
double betai(double a, double b, double x) noexcept {
    if (x <= 0.0) return 0.0;
    if (x >= 1.0) return 1.0;

    // log B(a, b) = lgamma(a) + lgamma(b) - lgamma(a+b)
    double lbeta = std::lgamma(a) + std::lgamma(b) - std::lgamma(a + b);
    double bt    = std::exp(lbeta + a * std::log(x) + b * std::log(1.0 - x));

    // Use direct CF when x < (a+1)/(a+b+2) for better convergence
    if (x < (a + 1.0) / (a + b + 2.0))
        return bt * betacf(a, b, x) / a;
    else
        return 1.0 - bt * betacf(b, a, 1.0 - x) / b;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// student_t_cdf  —  P(T ≤ t) for t-distribution with `nu` d.f.
//
// Derivation: if T ~ t(ν), then T²/(T²+ν) ~ Beta(1/2, ν/2).
// The CDF is:
//   F(t; ν) = I(ν/(t²+ν); ν/2, 1/2) / 2         for t < 0
//   F(t; ν) = 1 - I(ν/(t²+ν); ν/2, 1/2) / 2     for t ≥ 0
// ---------------------------------------------------------------------------
double student_t_cdf(double t, double nu) noexcept {
    double t2   = t * t;
    double x    = nu / (t2 + nu);
    double ibeta = betai(nu * 0.5, 0.5, x);

    if (t >= 0.0)
        return 1.0 - 0.5 * ibeta;
    else
        return 0.5 * ibeta;
}

// ---------------------------------------------------------------------------
// compute_likelihood  —  CONTEXT.md §3.2
// ---------------------------------------------------------------------------
double compute_likelihood(
    double btc_price,
    double strike,
    double time_remaining_s,
    double realized_vol_60s,
    double ob_imbalance,
    bool   is_yes_side) noexcept
{
    if (strike <= 0.0 || time_remaining_s <= 0.0 || realized_vol_60s <= 0.0)
        return 0.5;

    // Dynamic vol scaling: base vol + OB imbalance multiplier + time scaling
    double scaled_vol = realized_vol_60s
                      * (1.0 + constants::OB_IMBALANCE_MULTIPLIER * std::fabs(ob_imbalance))
                      * std::sqrt(time_remaining_s / constants::TIME_REMAINING_MAX_S);

    if (scaled_vol <= 0.0) return 0.5;

    // Standardised distance from strike
    double d = (btc_price - strike) / (strike * scaled_vol);

    // P(BTC > strike) via Student-t CDF
    double p_above = student_t_cdf(d, constants::T_DEGREES_OF_FREEDOM);

    return is_yes_side ? p_above : (1.0 - p_above);
}

// ---------------------------------------------------------------------------
// LookupTable
// ---------------------------------------------------------------------------

bool LookupTable::load_csv(const std::string& path) {
    std::ifstream f(path);
    if (!f) {
        std::fprintf(stderr, "likelihood: cannot open calibration CSV: %s\n",
                     path.c_str());
        return false;
    }

    int row = 0;
    std::string line;
    while (std::getline(f, line) && row < constants::STRIKE_DISTANCE_BINS) {
        std::istringstream ss(line);
        std::string token;
        int col = 0;
        while (std::getline(ss, token, ',') && col < constants::TIME_REMAINING_BINS) {
            try {
                table_[row][col] = std::stod(token);
            } catch (...) {
                std::fprintf(stderr, "likelihood: bad value at row %d col %d\n",
                             row, col);
                table_[row][col] = 0.5;
            }
            ++col;
        }
        if (col != constants::TIME_REMAINING_BINS) {
            std::fprintf(stderr, "likelihood: row %d has %d columns (expected %d)\n",
                         row, col, constants::TIME_REMAINING_BINS);
        }
        ++row;
    }

    if (row != constants::STRIKE_DISTANCE_BINS) {
        std::fprintf(stderr,
            "likelihood: calibration CSV has %d rows (expected %d)\n",
            row, constants::STRIKE_DISTANCE_BINS);
        return false;
    }

    loaded_ = true;
    return true;
}

double LookupTable::lookup(double btc_price, double strike,
                            double time_remaining_s) const noexcept {
    if (!loaded_) return 0.5;

    // Axis 0: distance in bps, rounded, clamped to [-100, +100]
    int distance_bps = static_cast<int>(
        std::round((btc_price - strike) / strike * 10000.0));
    int row = std::clamp(distance_bps + 100,
                         0, constants::STRIKE_DISTANCE_BINS - 1);

    // Axis 1: time bin = floor(time_remaining / 5), clamped to [0, 59]
    int col = std::clamp(static_cast<int>(time_remaining_s / 5.0),
                         0, constants::TIME_REMAINING_BINS - 1);

    return table_[row][col];
}

} // namespace signal
