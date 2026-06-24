#pragma once
#include <atomic>
#include <cstdint>
#include <string>

namespace wallet {

// Manages the CLOB-level order nonce for the account.
//
// The `nonce` field in every signed Order struct must be monotonically
// increasing and unique — the exchange uses it to prevent order replay.
// It is NOT an on-chain Polygon transaction count (CLOB orders don't
// create Polygon transactions), but it is seeded from
// eth_getTransactionCount("pending") via the Polygon RPC at startup.
//
// Lifecycle:
//   1. sync() — called once at startup; queries the Polygon RPC and sets
//               the internal counter.
//   2. next() — atomically fetch-and-increment; returns the nonce to embed
//               in the next Order struct.
//   3. force_resync() — called by the order state machine when the exchange
//                       rejects an order with a nonce error; re-queries the RPC.
//
// The counter is lock-free on all mainstream architectures.

class NonceManager {
public:
    NonceManager() = default;

    // Seed from eth_getTransactionCount(wallet_address, "pending").
    // rpc_url: e.g. "https://polygon-rpc.com"
    // wallet_hex: "0x..." 40-char lowercase hex address
    // Throws std::runtime_error on network failure or unexpected response.
    void sync(const std::string& rpc_url, const std::string& wallet_hex);

    // Atomically return current nonce and post-increment.
    // Returns the uint256_t representation (as uint64_t — nonces never reach 2^64).
    uint64_t next() noexcept;

    // Re-query the RPC.  Call this when an order comes back with a nonce error.
    // Thread-safe: updates the atomic with a strong CAS to avoid going backwards.
    void force_resync(const std::string& rpc_url, const std::string& wallet_hex);

    // Peek at the current counter without incrementing (for diagnostics only).
    uint64_t current() const noexcept { return nonce_.load(std::memory_order_relaxed); }

private:
    std::atomic<uint64_t> nonce_{0};
};

} // namespace wallet
