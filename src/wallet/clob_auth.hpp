#pragma once
#include "key_manager.hpp"
#include <map>
#include <optional>
#include <string>

namespace wallet {

// Polymarket CLOB session authentication.
//
// Two-phase flow derived from py-clob-client:
//
//   Phase 1 — Level 1 headers (signed with L1 wallet key, EIP-712):
//     Sign a ClobAuth struct { address, timestamp, nonce, message } under
//     domain "ClobAuthDomain" / version "1" / chainId 137.
//     POST to /auth/api-key → receive { apiKey, secret, passphrase }.
//
//   Phase 2 — Level 2 headers (HMAC-SHA256 with the retrieved secret):
//     Every subsequent CLOB request sends:
//       POLY_ADDRESS, POLY_SIGNATURE, POLY_TIMESTAMP, POLY_API_KEY, POLY_PASSPHRASE
//     where POLY_SIGNATURE = base64url(HMAC-SHA256(b64url_decode(secret),
//                                                   timestamp + METHOD + path + body))
//     Body: replace single-quotes with double-quotes before HMAC (py-clob-client spec).
//
// Usage:
//   ClobAuth auth("https://clob.polymarket.com");
//   auth.authenticate(key_manager);
//   auto hdrs = auth.level2_headers("POST", "/order", json_body);

class ClobAuth {
public:
    struct ApiCreds {
        std::string api_key;
        std::string secret;      // base64url-encoded HMAC key
        std::string passphrase;
    };

    explicit ClobAuth(std::string clob_host);

    // Level 1: sign ClobAuth EIP-712 struct and POST /auth/api-key.
    // nonce: session nonce for the auth struct — typically 0, not the order nonce.
    // Throws std::runtime_error on HTTP failure, non-200 response, or bad JSON.
    void authenticate(const KeyManager& km, uint64_t nonce = 0);

    // Level 2 headers for a subsequent authenticated CLOB request.
    // method: uppercase HTTP verb ("GET", "POST", …)
    // path:   e.g. "/order" or "/orders?market=…"
    // body:   JSON string (empty string for GET requests)
    std::map<std::string, std::string> level2_headers(
        const std::string& method,
        const std::string& path,
        const std::string& body = "") const;

    // Level 1 headers (normally only called internally by authenticate(),
    // exposed for testing).
    std::map<std::string, std::string> level1_headers(
        const KeyManager& km,
        uint64_t nonce = 0) const;

    bool               authenticated() const { return creds_.has_value(); }
    const ApiCreds&    creds()         const { return *creds_; }
    const std::string& wallet_hex()    const { return wallet_hex_; }

private:
    // Returns hex-string EIP-712 signature over the ClobAuth struct.
    std::string sign_clob_auth(const KeyManager& km,
                               const std::string& timestamp,
                               uint64_t nonce) const;

    std::string             host_;
    std::string             wallet_hex_;  // "0x..." derived from KeyManager
    std::optional<ApiCreds> creds_;
};

} // namespace wallet
