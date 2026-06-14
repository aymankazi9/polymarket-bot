#pragma once
#include <string>

// REST client for the Binance USD-M Futures API.
//
// Endpoint: https://fapi.binance.com
// All signed requests use HMAC-SHA256 over the query string, with the
// binance_secret from KeyManager::Credentials as the key.
//
// Price and quantity precision for BTCUSDT futures:
//   price step:    $0.10  (2 decimal places, rounded to nearest $0.10)
//   quantity step: 0.001  (3 decimal places, floored to nearest 0.001 BTC)
//
// Not thread-safe — Thread 3 owns one instance.

namespace execution {

class BinanceClient {
public:
    struct OrderResult {
        bool        success      = false;
        std::string order_id;
        double      executed_qty = 0.0;
        double      avg_price    = 0.0;
        std::string error;
    };

    BinanceClient(std::string api_key, std::string secret,
                  std::string base_url = "https://fapi.binance.com");

    // Submit a LIMIT IOC order on BTCUSDT perpetual futures.
    // side:  "BUY" or "SELL"
    // Returns immediately; IOC fills or cancels at the exchange.
    OrderResult submit_order(const std::string& side,
                              double quantity,
                              double price);

    // Cancel an open futures order.  Returns true on success.
    bool cancel_order(const std::string& order_id);

private:
    std::string sign_and_post(const std::string& path,
                               const std::string& params);
    std::string sign_and_delete(const std::string& path,
                                 const std::string& params);
    std::string hmac_hex(const std::string& data) const;

    std::string api_key_;
    std::string secret_;
    std::string base_url_;
};

} // namespace execution
