#include "nonce_manager.hpp"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>

// JSON-RPC call: eth_getTransactionCount(wallet, "pending")
// Response: {"jsonrpc":"2.0","id":1,"result":"0x<hex>"}
//
// We use this as the CLOB order nonce seed (per CONTEXT.md §9.5).
// The CLOB exchange does not create Polygon txs, but seeds the counter here
// so the nonce starts beyond any on-chain activity from this wallet and is
// therefore guaranteed to be fresh on a new session.

namespace wallet {

namespace {

static size_t curl_write(void* ptr, size_t sz, size_t nmemb, std::string* s) {
    s->append(static_cast<const char*>(ptr), sz * nmemb);
    return sz * nmemb;
}

std::string http_post_json(const std::string& url, const std::string& body) {
    CURL* c = curl_easy_init();
    if (!c) throw std::runtime_error("nonce_manager: curl_easy_init failed");
    struct G { CURL* c; ~G() { curl_easy_cleanup(c); } } g{c};

    std::string response;
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_POST, 1L);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_write);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 15L);

    struct curl_slist* hlist = nullptr;
    hlist = curl_slist_append(hlist, "Content-Type: application/json");
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hlist);

    CURLcode rc = curl_easy_perform(c);
    curl_slist_free_all(hlist);

    if (rc != CURLE_OK)
        throw std::runtime_error(
            std::string("nonce_manager: RPC request failed: ") +
            curl_easy_strerror(rc));

    long status = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    if (status != 200)
        throw std::runtime_error(
            "nonce_manager: RPC returned HTTP " + std::to_string(status));

    return response;
}

// Parse a "0x<hex>" string returned by eth_getTransactionCount
uint64_t parse_hex_uint64(const std::string& hex) {
    const char* p = hex.c_str();
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
    uint64_t v = 0;
    while (*p) {
        uint64_t nibble;
        if (*p >= '0' && *p <= '9')      nibble = *p - '0';
        else if (*p >= 'a' && *p <= 'f') nibble = *p - 'a' + 10;
        else if (*p >= 'A' && *p <= 'F') nibble = *p - 'A' + 10;
        else throw std::runtime_error("nonce_manager: bad hex char in RPC result");
        v = (v << 4) | nibble;
        ++p;
    }
    return v;
}

} // anonymous namespace

// ---------------------------------------------------------------------------

uint64_t NonceManager::query_rpc(const std::string& rpc_url,
                                  const std::string& wallet_hex) const {
    // JSON-RPC 2.0 request body
    std::string body =
        R"({"jsonrpc":"2.0","method":"eth_getTransactionCount",)"
        R"("params":[")" + wallet_hex + R"(","pending"],"id":1})";

    std::string raw = http_post_json(rpc_url, body);

    nlohmann::json j = nlohmann::json::parse(raw);

    if (j.contains("error"))
        throw std::runtime_error(
            "nonce_manager: RPC error: " + j["error"].dump());

    std::string result = j.at("result").get<std::string>();
    return parse_hex_uint64(result);
}

void NonceManager::sync(const std::string& rpc_url,
                        const std::string& wallet_hex) {
    uint64_t v = query_rpc(rpc_url, wallet_hex);
    nonce_.store(v, std::memory_order_relaxed);
}

uint64_t NonceManager::next() noexcept {
    // post-increment: returns the current value, then increments
    return nonce_.fetch_add(1, std::memory_order_relaxed);
}

void NonceManager::force_resync(const std::string& rpc_url,
                                 const std::string& wallet_hex) {
    uint64_t fresh = query_rpc(rpc_url, wallet_hex);
    uint64_t current = nonce_.load(std::memory_order_relaxed);

    // Only advance the counter — never go backwards — to avoid replaying
    // a nonce that was already submitted and might still be in-flight.
    while (fresh > current) {
        if (nonce_.compare_exchange_weak(
                current, fresh,
                std::memory_order_release,
                std::memory_order_relaxed))
            break;
        // CAS failed: reload current and retry
    }
}

} // namespace wallet
