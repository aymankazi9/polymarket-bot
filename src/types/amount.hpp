#pragma once
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>

// Strong monetary amount type.  CONTEXT_ADDENDUM §A3.
//
// All USDC balances, PnL, fee values, order amounts, and edge thresholds are
// expressed as Amount — never as raw double.  This prevents rounding errors
// from accumulating through double arithmetic on 6-decimal USDC values.
//
// Internal representation: integer micro-units with 6 decimal places.
//   1 USDC  = 1,000,000 units
//   0.01¢   = 100 units
//
// Construction rules:
//   from_poly_units()    — parse integer strings from Polymarket API responses
//   from_binance_units() — parse integer strings from Binance (8-decimal) responses
//   from_double()        — ONLY for compile-time config constants; never for API values
//
// The only place raw double is used for money is inside the Bayesian engine's
// probability calculations (probabilities are not monetary values).

enum class Denomination { POLY_USDC, BINANCE_USDC };

class Amount {
public:
    static constexpr int64_t POLY_SCALE    = 1'000'000;    // 6 decimal places
    static constexpr int64_t BINANCE_SCALE = 100'000'000;  // 8 decimal places

    // ---- Construction -------------------------------------------------------

    // From raw 6-decimal integer (Polymarket API returns amounts as decimal strings).
    // Usage: Amount::from_poly_units(std::stoll(json_str))
    static constexpr Amount from_poly_units(int64_t units) noexcept {
        return Amount(units);
    }

    // From Binance 8-decimal integer — normalises to 6 by dividing by 100.
    // Truncation of sub-micro-USDC amounts is intentional and negligible.
    static constexpr Amount from_binance_units(int64_t units) noexcept {
        return Amount(units / 100);
    }

    // From human-readable double: 1.50 → 1,500,000 units.
    // ONLY use for compile-time config constants (e.g. constants.hpp).
    // Uses round() to avoid floating-point truncation of exact decimal values.
    static Amount from_double(double d) noexcept {
        return Amount(static_cast<int64_t>(std::round(d * POLY_SCALE)));
    }

    // Zero amount.
    static constexpr Amount zero() noexcept { return Amount(0); }

    // ---- Accessors ----------------------------------------------------------

    constexpr int64_t poly_units()    const noexcept { return units_; }
    constexpr int64_t binance_units() const noexcept { return units_ * 100; }

    // Convert to double for display and probability calculations ONLY.
    // Do not feed the result back into Amount arithmetic.
    double to_double() const noexcept {
        return static_cast<double>(units_) / POLY_SCALE;
    }

    // ---- Predicates ---------------------------------------------------------

    constexpr bool is_zero()     const noexcept { return units_ == 0; }
    constexpr bool is_positive() const noexcept { return units_ > 0; }
    constexpr bool is_negative() const noexcept { return units_ < 0; }

    // ---- Arithmetic ---------------------------------------------------------

    constexpr Amount operator+(const Amount& o) const noexcept { return Amount(units_ + o.units_); }
    constexpr Amount operator-(const Amount& o) const noexcept { return Amount(units_ - o.units_); }
    constexpr Amount operator-()                const noexcept { return Amount(-units_); }

    // Scale by an integer factor (e.g. multiply shares × price).
    constexpr Amount operator*(int64_t factor) const noexcept { return Amount(units_ * factor); }
    friend constexpr Amount operator*(int64_t factor, const Amount& a) noexcept {
        return Amount(factor * a.units_);
    }

    // Integer division (truncates sub-unit remainder).
    constexpr Amount operator/(int64_t divisor) const {
        if (divisor == 0) throw std::domain_error("Amount: divide by zero");
        return Amount(units_ / divisor);
    }

    Amount& operator+=(const Amount& o) noexcept { units_ += o.units_; return *this; }
    Amount& operator-=(const Amount& o) noexcept { units_ -= o.units_; return *this; }

    // ---- Comparison ---------------------------------------------------------

    constexpr bool operator==(const Amount& o) const noexcept { return units_ == o.units_; }
    constexpr bool operator!=(const Amount& o) const noexcept { return units_ != o.units_; }
    constexpr bool operator< (const Amount& o) const noexcept { return units_ <  o.units_; }
    constexpr bool operator<=(const Amount& o) const noexcept { return units_ <= o.units_; }
    constexpr bool operator> (const Amount& o) const noexcept { return units_ >  o.units_; }
    constexpr bool operator>=(const Amount& o) const noexcept { return units_ >= o.units_; }

    // ---- Utility ------------------------------------------------------------

    // Absolute value.
    constexpr Amount abs() const noexcept {
        return Amount(units_ < 0 ? -units_ : units_);
    }

    // Decimal string for logging: e.g. 1500000 → "1.500000"
    std::string to_string() const {
        int64_t whole = units_ / POLY_SCALE;
        int64_t frac  = units_ % POLY_SCALE;
        if (frac < 0) frac = -frac;
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%lld.%06lld",
                      (long long)whole, (long long)frac);
        return buf;
    }

private:
    constexpr explicit Amount(int64_t units) noexcept : units_(units) {}
    int64_t units_;
};
