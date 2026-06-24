#include "nonce_manager.hpp"
#include "../infra/rpc_client.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>

// Delegates RPC calls to infra::RpcClient, which handles Alchemy primary /
// polygon-rpc.com fallback routing with 3s timeout (CONTEXT_ADDENDUM A1.2).
//
// The rpc_url parameter on sync() / force_resync() is accepted for API
// compatibility but ignored — routing is handled internally by RpcClient.
// Stage 5 will drop the parameter from the public interface.

namespace wallet {

namespace {

uint64_t query(const std::string& wallet_hex) {
    infra::RpcClient rpc;
    return rpc.get_transaction_count(wallet_hex);
}

} // anonymous namespace

void NonceManager::sync(const std::string& /* rpc_url */,
                        const std::string& wallet_hex) {
    uint64_t v = query(wallet_hex);
    nonce_.store(v, std::memory_order_relaxed);
}

uint64_t NonceManager::next() noexcept {
    return nonce_.fetch_add(1, std::memory_order_relaxed);
}

void NonceManager::force_resync(const std::string& /* rpc_url */,
                                 const std::string& wallet_hex) {
    uint64_t fresh   = query(wallet_hex);
    uint64_t current = nonce_.load(std::memory_order_relaxed);
    // Only advance — never go backwards.
    while (fresh > current) {
        if (nonce_.compare_exchange_weak(
                current, fresh,
                std::memory_order_release,
                std::memory_order_relaxed))
            break;
    }
}

} // namespace wallet
