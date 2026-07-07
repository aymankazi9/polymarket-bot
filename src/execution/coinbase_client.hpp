#pragma once
#include <string>

// REST client for the Coinbase Advanced Trade API — hedge leg (BTC perpetual futures).
//
// Endpoint: https://api.coinbase.com
// Auth: JWT bearer token, fresh per request (EdDSA / Ed25519 via CDP API key).
//   Header: {"alg":"EdDSA","typ":"JWT","kid":<key_name>,"nonce":<hex>}
//   Payload: {"sub":<key_name>,"iss":"cdp","nbf":<now>,"exp":<now+120>,"uri":"METHOD api.coinbase.com/path"}
//
// Order placement: POST /api/v3/brokerage/orders
//   Entry hedge  : order_configuration.sor_limit_ioc  (limit IOC, book-sweeping)
//   Unwind hedge : order_configuration.market_market_ioc (price <= 0.0 sentinel)
//
// Fill data is NOT in the create response; poll_fill() issues one GET after a
// brief delay to retrieve filled_size and average_filled_price.
//
// Interface is intentionally identical to BinanceClient so callers need no changes
// beyond the class name and credential fields.
//
// Not thread-safe — one instance per OSM (Thread 3).

namespace execution {

class CoinbaseClient {
public:
    struct OrderResult {
        bool        success      = false;
        std::string order_id;
        double      executed_qty = 0.0;
        double      avg_price    = 0.0;
        std::string error;
    };

    // key_name:       "organizations/{org_id}/apiKeys/{key_id}"
    // key_secret_pem: Ed25519 private key in PKCS8 PEM format
    //                 (-----BEGIN PRIVATE KEY-----  from CDP key generator)
    CoinbaseClient(std::string key_name,
                   std::string key_secret_pem,
                   std::string base_url = "https://api.coinbase.com");

    // Submit a limit IOC order on the configured hedge product.
    // side:  "BUY" or "SELL"
    // price: USDC per BTC — pass <= 0.0 to use a market IOC (for unwinds only).
    OrderResult submit_order(const std::string& side,
                              double quantity,
                              double price);

    // Cancel an open futures order.  Returns true on success.
    bool cancel_order(const std::string& order_id);

private:
    // Build a fresh JWT for one request (EdDSA, 120s expiry).
    std::string build_jwt(const std::string& method,
                           const std::string& path) const;

    // Issue an HTTP request with a freshly-built JWT.
    std::string http_request(const std::string& method,
                              const std::string& path,
                              const std::string& body) const;

    // Poll GET /orders/{id} once (after a brief IOC settle delay) for fill data.
    OrderResult poll_fill(const std::string& order_id) const;

    std::string key_name_;
    std::string key_secret_pem_;
    std::string base_url_;
};

} // namespace execution
