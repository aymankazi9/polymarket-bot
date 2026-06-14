#include "clob_client.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

namespace execution {

namespace {

// ---------------------------------------------------------------------------
// curl helpers
// ---------------------------------------------------------------------------

static size_t write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* buf = static_cast<std::string*>(userdata);
    buf->append(ptr, size * nmemb);
    return size * nmemb;
}

struct CurlHandle {
    CURL* h;
    explicit CurlHandle() : h(curl_easy_init()) {}
    ~CurlHandle() { if (h) curl_easy_cleanup(h); }
    CurlHandle(const CurlHandle&) = delete;
};

static std::string curl_do(const std::string& url, const std::string& method,
                             const std::string& body,
                             const std::map<std::string, std::string>& hdrs)
{
    CurlHandle ch;
    if (!ch.h) return {};

    std::string response;
    curl_easy_setopt(ch.h, CURLOPT_URL, url.c_str());
    curl_easy_setopt(ch.h, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(ch.h, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(ch.h, CURLOPT_TIMEOUT, 10L);

    // Method
    if (method == "POST") {
        curl_easy_setopt(ch.h, CURLOPT_POST, 1L);
        curl_easy_setopt(ch.h, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(ch.h, CURLOPT_POSTFIELDSIZE, (long)body.size());
    } else if (method == "DELETE") {
        curl_easy_setopt(ch.h, CURLOPT_CUSTOMREQUEST, "DELETE");
        if (!body.empty()) {
            curl_easy_setopt(ch.h, CURLOPT_POSTFIELDS, body.c_str());
            curl_easy_setopt(ch.h, CURLOPT_POSTFIELDSIZE, (long)body.size());
        }
    }
    // GET is default

    // Headers
    curl_slist* list = nullptr;
    for (const auto& [k, v] : hdrs) {
        std::string hdr = k + ": " + v;
        list = curl_slist_append(list, hdr.c_str());
    }
    if (!body.empty() && method != "GET")
        list = curl_slist_append(list, "Content-Type: application/json");

    if (list) curl_easy_setopt(ch.h, CURLOPT_HTTPHEADER, list);

    curl_easy_perform(ch.h);
    if (list) curl_slist_free_all(list);
    return response;
}

// ---------------------------------------------------------------------------
// Serialisation helpers
// ---------------------------------------------------------------------------

static const char HEX_CHARS[] = "0123456789abcdef";

static std::string address_to_hex(const wallet::address_t& a) {
    std::string s = "0x";
    s.reserve(42);
    for (uint8_t b : a) { s += HEX_CHARS[b >> 4]; s += HEX_CHARS[b & 0xf]; }
    return s;
}

static std::string bytes_to_hex(const uint8_t* data, size_t n) {
    std::string s = "0x";
    s.reserve(2 + 2 * n);
    for (size_t i = 0; i < n; ++i) { s += HEX_CHARS[data[i] >> 4]; s += HEX_CHARS[data[i] & 0xf]; }
    return s;
}

static std::string uint256_to_decimal(const wallet::uint256_t& v) {
    uint64_t L[4];
    for (int i = 0; i < 4; ++i) {
        L[i] = 0;
        for (int j = 0; j < 8; ++j) L[i] = (L[i] << 8) | v[i * 8 + j];
    }
    if (!L[0] && !L[1] && !L[2] && !L[3]) return "0";
    std::string digits;
    digits.reserve(78);
    while (L[0] || L[1] || L[2] || L[3]) {
        uint64_t rem = 0;
        for (int i = 0; i < 4; ++i) {
            __uint128_t acc = ((__uint128_t)rem << 64) | L[i];
            L[i] = (uint64_t)(acc / 10);
            rem  = (uint64_t)(acc % 10);
        }
        digits += (char)('0' + rem);
    }
    std::reverse(digits.begin(), digits.end());
    return digits;
}

static std::string build_order_json(const wallet::Order& order,
                                     const std::array<uint8_t,65>& sig,
                                     const std::string& owner_hex,
                                     std::string_view order_type)
{
    nlohmann::json j;
    auto& o = j["order"];
    o["salt"]          = uint256_to_decimal(order.salt);
    o["maker"]         = address_to_hex(order.maker);
    o["signer"]        = address_to_hex(order.signer);
    o["taker"]         = address_to_hex(order.taker);
    o["tokenId"]       = uint256_to_decimal(order.tokenId);
    o["makerAmount"]   = uint256_to_decimal(order.makerAmount);
    o["takerAmount"]   = uint256_to_decimal(order.takerAmount);
    o["expiration"]    = uint256_to_decimal(order.expiration);
    o["nonce"]         = uint256_to_decimal(order.nonce);
    o["feeRateBps"]    = uint256_to_decimal(order.feeRateBps);
    o["side"]          = (order.side == 0) ? "BUY" : "SELL";
    o["signatureType"] = static_cast<int>(order.signatureType);
    j["signature"]     = bytes_to_hex(sig.data(), sig.size());
    j["owner"]         = owner_hex;
    j["orderType"]     = std::string(order_type);
    return j.dump();
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// ClobClient
// ---------------------------------------------------------------------------

ClobClient::ClobClient(std::string base_url)
    : base_url_(std::move(base_url))
{}

ClobClient::SubmitResult ClobClient::submit_order(
    const wallet::Order&           order,
    const std::array<uint8_t,65>&  sig,
    const wallet::ClobAuth&        auth,
    std::string_view               order_type)
{
    std::string body = build_order_json(order, sig, auth.wallet_hex(), order_type);
    auto hdrs = auth.level2_headers("POST", "/order", body);
    std::string resp = post("/order", body, hdrs);

    SubmitResult result;
    if (resp.empty()) { result.error = "no response"; return result; }
    try {
        auto j = nlohmann::json::parse(resp);
        result.success  = j.value("success", false);
        result.order_id = j.value("orderID", "");
        if (!result.success)
            result.error = j.value("errorMsg", "unknown error");
    } catch (const std::exception& e) {
        result.error = e.what();
    }
    return result;
}

bool ClobClient::cancel_order(const std::string& order_id,
                               const wallet::ClobAuth& auth)
{
    std::string path = "/order/" + order_id;
    auto hdrs = auth.level2_headers("DELETE", path, "");
    std::string resp = del(path, hdrs);
    if (resp.empty()) return false;
    try {
        auto j = nlohmann::json::parse(resp);
        return j.value("success", false);
    } catch (...) { return false; }
}

ClobClient::OrderStatus ClobClient::get_status(const std::string& order_id)
{
    std::string resp = get("/order/" + order_id);
    OrderStatus status;
    if (resp.empty()) return status;
    try {
        auto j = nlohmann::json::parse(resp);
        std::string s = j.value("status", "UNKNOWN");
        if      (s == "LIVE")      status.state = OrderState::OPEN;
        else if (s == "MATCHED")   status.state = OrderState::MATCHED;
        else if (s == "FILLED")    status.state = OrderState::FILLED;
        else if (s == "CANCELLED") status.state = OrderState::CANCELLED;
        status.size_matched = std::stod(j.value("sizeMatched", "0"));
        status.avg_price    = std::stod(j.value("avgPrice",    "0"));
    } catch (...) {}
    return status;
}

std::string ClobClient::post(const std::string& path, const std::string& body,
                              const std::map<std::string, std::string>& headers)
{
    return curl_do(base_url_ + path, "POST", body, headers);
}

std::string ClobClient::get(const std::string& path)
{
    return curl_do(base_url_ + path, "GET", "", {});
}

std::string ClobClient::del(const std::string& path,
                             const std::map<std::string, std::string>& headers)
{
    return curl_do(base_url_ + path, "DELETE", "", headers);
}

// ---------------------------------------------------------------------------
// parse_token_id: decimal string → big-endian uint256_t
// ---------------------------------------------------------------------------
wallet::uint256_t parse_token_id(const std::string& s)
{
    if (s.empty()) throw std::invalid_argument("empty token ID string");
    wallet::uint256_t result{};
    // result is a 256-bit big-endian integer.  Build it with repeated multiply-and-add.
    // We accumulate in 4 limbs (uint64_t each), big-endian ordering.
    uint64_t L[4]{};
    for (char c : s) {
        if (c < '0' || c > '9')
            throw std::invalid_argument("non-decimal character in token ID");
        uint64_t carry = c - '0';
        // Multiply all limbs by 10 and add carry, right-to-left
        for (int i = 3; i >= 0; --i) {
            __uint128_t acc = (__uint128_t)L[i] * 10 + carry;
            L[i]  = (uint64_t)(acc & 0xFFFFFFFFFFFFFFFFULL);
            carry = (uint64_t)(acc >> 64);
        }
        if (carry) throw std::invalid_argument("token ID exceeds uint256");
    }
    // Pack limbs into big-endian bytes
    for (int i = 0; i < 4; ++i)
        for (int j = 7; j >= 0; --j) {
            result[i * 8 + (7 - j)] = (uint8_t)(L[i] >> (j * 8));
        }
    return result;
}

} // namespace execution
