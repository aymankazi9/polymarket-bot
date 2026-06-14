#include "binance_client.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <openssl/hmac.h>

namespace execution {

namespace {

static size_t write_cb(char* ptr, size_t size, size_t nmemb, void* ud) {
    auto* s = static_cast<std::string*>(ud);
    s->append(ptr, size * nmemb);
    return size * nmemb;
}

static uint64_t ts_ms() {
    using namespace std::chrono;
    return (uint64_t)duration_cast<milliseconds>(
        system_clock::now().time_since_epoch()).count();
}

static std::string fmt_price(double p) {
    char buf[32];
    double rounded = std::round(p * 10.0) / 10.0;
    std::snprintf(buf, sizeof(buf), "%.1f", rounded);
    return buf;
}

static std::string fmt_qty(double q) {
    char buf[32];
    double floored = std::floor(q * 1000.0) / 1000.0;
    std::snprintf(buf, sizeof(buf), "%.3f", floored);
    return buf;
}

static std::string url_encode(const std::string& s) {
    std::string r;
    r.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
            r += (char)c;
        else {
            char buf[4];
            std::snprintf(buf, sizeof(buf), "%%%02X", c);
            r += buf;
        }
    }
    return r;
}

} // anonymous namespace

BinanceClient::BinanceClient(std::string api_key, std::string secret,
                               std::string base_url)
    : api_key_(std::move(api_key))
    , secret_(std::move(secret))
    , base_url_(std::move(base_url))
{}

std::string BinanceClient::hmac_hex(const std::string& data) const {
    static const char HEX[] = "0123456789abcdef";
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int  dlen = 0;
    HMAC(EVP_sha256(),
         secret_.data(), (int)secret_.size(),
         reinterpret_cast<const unsigned char*>(data.data()), data.size(),
         digest, &dlen);
    std::string result;
    result.reserve(dlen * 2);
    for (unsigned i = 0; i < dlen; ++i) {
        result += HEX[digest[i] >> 4];
        result += HEX[digest[i] & 0xf];
    }
    return result;
}

BinanceClient::OrderResult BinanceClient::submit_order(
    const std::string& side, double quantity, double price)
{
    std::string params = "symbol=BTCUSDT"
        "&side=" + side +
        "&type=LIMIT"
        "&timeInForce=IOC"
        "&quantity=" + fmt_qty(quantity) +
        "&price=" + fmt_price(price) +
        "&recvWindow=5000"
        "&timestamp=" + std::to_string(ts_ms());

    params += "&signature=" + hmac_hex(params);

    std::string resp = sign_and_post("/fapi/v1/order", params);
    OrderResult r;
    if (resp.empty()) { r.error = "no response"; return r; }
    try {
        auto j = nlohmann::json::parse(resp);
        if (j.contains("code") && j["code"].get<int>() != 200 && j.contains("msg")) {
            r.error = j["msg"].get<std::string>();
            return r;
        }
        r.success      = true;
        r.order_id     = std::to_string(j.value("orderId", 0LL));
        r.executed_qty = std::stod(j.value("executedQty", "0"));
        r.avg_price    = std::stod(j.value("avgPrice",    "0"));
    } catch (const std::exception& e) {
        r.error = e.what();
    }
    return r;
}

bool BinanceClient::cancel_order(const std::string& order_id) {
    std::string params = "symbol=BTCUSDT"
        "&orderId=" + order_id +
        "&recvWindow=5000"
        "&timestamp=" + std::to_string(ts_ms());
    params += "&signature=" + hmac_hex(params);
    std::string resp = sign_and_delete("/fapi/v1/order", params);
    if (resp.empty()) return false;
    try {
        auto j = nlohmann::json::parse(resp);
        std::string status = j.value("status", "");
        return status == "CANCELED";
    } catch (...) { return false; }
}

std::string BinanceClient::sign_and_post(const std::string& path,
                                           const std::string& params)
{
    CURL* curl = curl_easy_init();
    if (!curl) return {};
    std::string response;
    std::string url = base_url_ + path;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, params.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)params.size());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    std::string apikey_hdr = "X-MBX-APIKEY: " + api_key_;
    curl_slist* hdrs = curl_slist_append(nullptr, apikey_hdr.c_str());
    hdrs = curl_slist_append(hdrs, "Content-Type: application/x-www-form-urlencoded");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_perform(curl);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    return response;
}

std::string BinanceClient::sign_and_delete(const std::string& path,
                                             const std::string& params)
{
    CURL* curl = curl_easy_init();
    if (!curl) return {};
    std::string response;
    std::string url = base_url_ + path + "?" + params;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    std::string apikey_hdr = "X-MBX-APIKEY: " + api_key_;
    curl_slist* hdrs = curl_slist_append(nullptr, apikey_hdr.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_perform(curl);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    return response;
}

} // namespace execution
