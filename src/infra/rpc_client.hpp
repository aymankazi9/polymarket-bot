#pragma once
#include <cstdint>
#include <string>

// JSON-RPC 2.0 client for Polygon.
//
// Routing: tries RPC_PRIMARY (Alchemy) first with RPC_TIMEOUT_MS timeout.
// If the primary returns no response or an error object, retries once on
// RPC_FALLBACK (polygon-rpc.com).  Throws std::runtime_error if both fail.
//
// RPC usage by this bot is intentionally rare (startup nonce sync only).
// No connection pooling — one synchronous HTTPS call per operation.
// Never log or surface the Alchemy key (it lives in the URL parameter).

namespace infra {

class RpcClient {
public:
    RpcClient() = default;

    // eth_getTransactionCount(address, "pending")
    // Returns the account's pending nonce.  Used by NonceManager at startup.
    uint64_t get_transaction_count(const std::string& hex_address);

    // eth_sendRawTransaction(signed_hex)
    // Broadcasts a pre-signed raw transaction. Returns the tx hash string.
    // Called only from one-time tooling, not from the trading hot path.
    std::string send_raw_transaction(const std::string& signed_hex);

private:
    // Fire the RPC body against primary; retry on fallback if primary fails.
    std::string rpc_call(const std::string& method, const std::string& params_json);
};

} // namespace infra
