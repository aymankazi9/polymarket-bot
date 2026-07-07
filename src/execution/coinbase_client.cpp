#include "coinbase_client.hpp"
#include "../../constants.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

namespace execution {

namespace {

static size_t write_cb(char* ptr, size_t size, size_t nmemb, void* ud) {
    auto* s = static_cast<std::string*>(ud);
    s->append(ptr, size * nmemb);
    return size * nmemb;
}

// RFC 4648 §5 base64url — no padding, + → -, / → _
static std::string base64url_encode(const uint8_t* data, size_t len) {
    static const char ENC[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t b = static_cast<uint32_t>(data[i]) << 16;
        if (i + 1 < len) b |= static_cast<uint32_t>(data[i + 1]) << 8;
        if (i + 2 < len) b |= static_cast<uint32_t>(data[i + 2]);
        out += ENC[(b >> 18) & 0x3F];
        out += ENC[(b >> 12) & 0x3F];
        if (i + 1 < len) out += ENC[(b >> 6) & 0x3F];
        if (i + 2 < len) out += ENC[b & 0x3F];
    }
    for (char& c : out) {
        if      (c == '+') c = '-';
        else if (c == '/') c = '_';
    }
    return out;
}

static std::string base64url_encode(const std::string& s) {
    return base64url_encode(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

static std::string hex_nonce_16() {
    static const char HEX[] = "0123456789abcdef";
    uint8_t buf[16];
    RAND_bytes(buf, 16);
    std::string out;
    out.reserve(32);
    for (uint8_t b : buf) { out += HEX[b >> 4]; out += HEX[b & 0xf]; }
    return out;
}

// Floor quantity to 3 decimal places (Coinbase base_size in BTC).
static std::string fmt_qty(double q) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.3f", std::floor(q * 1000.0) / 1000.0);
    return buf;
}

// Round price to 1 decimal place (USDC per BTC).
static std::string fmt_price(double p) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f", std::round(p * 10.0) / 10.0);
    return buf;
}

// Generate a 32-hex-char client order ID from random bytes.
static std::string random_client_oid() {
    static const char HEX[] = "0123456789abcdef";
    uint8_t buf[16];
    RAND_bytes(buf, 16);
    std::string out;
    out.reserve(32);
    for (uint8_t b : buf) { out += HEX[b >> 4]; out += HEX[b & 0xf]; }
    return out;
}

} // anonymous namespace

CoinbaseClient::CoinbaseClient(std::string key_name,
                                std::string key_secret_pem,
                                std::string base_url)
    : key_name_(std::move(key_name))
    , key_secret_pem_(std::move(key_secret_pem))
    , base_url_(std::move(base_url))
{}

// Build a fresh JWT for one request.
// Coinbase requires a distinct token per request (the "uri" claim binds it).
// Ed25519 key signs the base64url(header).base64url(payload) string.
std::string CoinbaseClient::build_jwt(const std::string& method,
                                       const std::string& path) const
{
    using namespace std::chrono;
    int64_t now = duration_cast<seconds>(
        system_clock::now().time_since_epoch()).count();

    // Load private key from PKCS8 PEM (-----BEGIN PRIVATE KEY-----)
    BIO* bio = BIO_new_mem_buf(key_secret_pem_.data(),
                                static_cast<int>(key_secret_pem_.size()));
    if (!bio)
        throw std::runtime_error("coinbase_client: BIO_new_mem_buf failed");
    EVP_PKEY* pkey = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (!pkey)
        throw std::runtime_error("coinbase_client: failed to load Ed25519 PEM key — "
                                  "key must be PKCS8 PEM (-----BEGIN PRIVATE KEY-----)");

    // Only Ed25519 is supported; reject EC keys (ES256 needs DER→raw-rs conversion).
    int key_type = EVP_PKEY_base_id(pkey);
    if (key_type != EVP_PKEY_ED25519) {
        EVP_PKEY_free(pkey);
        throw std::runtime_error(
            "coinbase_client: unsupported key type — only Ed25519 keys are "
            "supported. Generate a new key at https://cloud.coinbase.com/access/api");
    }

    // URI claim: "METHOD api.coinbase.com/path" (no scheme, no port)
    std::string uri = method + " api.coinbase.com" + path;

    nlohmann::json header_j = {
        {"alg",   "EdDSA"},
        {"typ",   "JWT"},
        {"kid",   key_name_},
        {"nonce", hex_nonce_16()}
    };
    nlohmann::json payload_j = {
        {"sub", key_name_},
        {"iss", "cdp"},
        {"nbf", now},
        {"exp", now + 120},
        {"uri", uri}
    };

    std::string signing_input = base64url_encode(header_j.dump())
                              + "."
                              + base64url_encode(payload_j.dump());

    // Sign with EVP_PKEY_sign — correct API for Ed25519 (no separate digest step).
    EVP_PKEY_CTX* pctx = EVP_PKEY_CTX_new(pkey, nullptr);
    EVP_PKEY_free(pkey);
    if (!pctx)
        throw std::runtime_error("coinbase_client: EVP_PKEY_CTX_new failed");

    if (EVP_PKEY_sign_init(pctx) <= 0) {
        EVP_PKEY_CTX_free(pctx);
        throw std::runtime_error("coinbase_client: EVP_PKEY_sign_init failed");
    }

    const uint8_t* msg_data =
        reinterpret_cast<const uint8_t*>(signing_input.data());
    size_t msg_len = signing_input.size();

    size_t sig_len = 64;  // Ed25519 always produces 64 bytes
    uint8_t sig[64];
    if (EVP_PKEY_sign(pctx, sig, &sig_len, msg_data, msg_len) <= 0) {
        EVP_PKEY_CTX_free(pctx);
        throw std::runtime_error("coinbase_client: EVP_PKEY_sign failed");
    }
    EVP_PKEY_CTX_free(pctx);

    return signing_input + "." + base64url_encode(sig, sig_len);
}

std::string CoinbaseClient::http_request(const std::string& method,
                                          const std::string& path,
                                          const std::string& body) const
{
    CURL* curl = curl_easy_init();
    if (!curl) return {};

    std::string response;
    std::string url = base_url_ + path;

    std::string jwt;
    try { jwt = build_jwt(method, path); }
    catch (const std::exception& e) {
        std::fprintf(stderr, "coinbase: JWT build failed: %s\n", e.what());
        curl_easy_cleanup(curl);
        return {};
    }

    curl_slist* hdrs = nullptr;
    std::string auth_hdr = "Authorization: Bearer " + jwt;
    hdrs = curl_slist_append(hdrs, auth_hdr.c_str());
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL,           url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,     &response);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,    hdrs);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,       10L);

    if (method == "POST") {
        curl_easy_setopt(curl, CURLOPT_POST,          1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS,    body.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    }
    // GET is the default CURLOPT behaviour; no extra option needed.

    curl_easy_perform(curl);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    return response;
}

// IOC orders settle within milliseconds; poll once for fill data.
// Retries up to 3 times if the order is still OPEN/PENDING (shouldn't happen
// for IOC, but guards against exchange processing delays).
CoinbaseClient::OrderResult CoinbaseClient::poll_fill(
    const std::string& order_id) const
{
    OrderResult r;
    r.order_id = order_id;

    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    for (int attempt = 0; attempt < 3; ++attempt) {
        std::string resp = http_request(
            "GET", "/api/v3/brokerage/orders/" + order_id, "");
        if (resp.empty()) { r.error = "no response from get_order"; return r; }

        try {
            auto j = nlohmann::json::parse(resp);
            if (!j.contains("order")) { r.error = resp; return r; }

            auto& o = j["order"];
            std::string status = o.value("status", "");

            // Still settling — brief wait and retry.
            if (status == "OPEN" || status == "PENDING") {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }

            std::string filled_s = o.value("filled_size",           "0");
            std::string price_s  = o.value("average_filled_price",  "0");
            double filled = filled_s.empty() ? 0.0 : std::stod(filled_s);
            double price  = price_s.empty()  ? 0.0 : std::stod(price_s);

            if (filled > 0.0) {
                r.success      = true;
                r.executed_qty = filled;
                r.avg_price    = price;
            } else {
                r.error = "order not filled (status=" + status + ")";
            }
            return r;
        } catch (const std::exception& e) {
            r.error = e.what();
            return r;
        }
    }
    r.error = "order still pending after retries";
    return r;
}

CoinbaseClient::OrderResult CoinbaseClient::submit_order(
    const std::string& side, double quantity, double price)
{
    OrderResult r;

    nlohmann::json body;
    body["client_order_id"] = random_client_oid();
    // TODO: verify exact product_id for US CFTC nano BTC perpetual via
    //       GET /api/v3/brokerage/cfm/products once credentials are available.
    body["product_id"]      = constants::COINBASE_HEDGE_SYMBOL;
    body["side"]            = side;

    if (price <= 0.0) {
        // Market IOC: used for hedge unwinds where any fill price is acceptable.
        body["order_configuration"] = {
            {"market_market_ioc", {{"base_size", fmt_qty(quantity)}}}
        };
    } else {
        // Limit IOC (sor_limit_ioc): sweeps the book up to limit_price.
        // Used for all normal hedge entries.
        body["order_configuration"] = {
            {"sor_limit_ioc", {
                {"base_size",   fmt_qty(quantity)},
                {"limit_price", fmt_price(price)}
            }}
        };
    }

    std::string resp = http_request("POST", "/api/v3/brokerage/orders", body.dump());
    if (resp.empty()) { r.error = "no response"; return r; }

    try {
        auto j = nlohmann::json::parse(resp);
        bool ok = j.value("success", false);

        if (!ok) {
            if (j.contains("error_response")) {
                auto& er = j["error_response"];
                r.error = er.value("message", er.value("error", "order rejected"));
            } else {
                r.error = j.value("failure_reason", "order rejected");
            }
            return r;
        }

        // Extract server-assigned order_id (may be at top level or in success_response).
        std::string oid = j.value("order_id", "");
        if (oid.empty() && j.contains("success_response"))
            oid = j["success_response"].value("order_id", "");
        if (oid.empty()) { r.error = "no order_id in response"; return r; }

        // Poll for fill data (not included in create response).
        r = poll_fill(oid);
        if (!r.success)
            r.order_id = oid;  // preserve id for cancel even when unfilled
    } catch (const std::exception& e) {
        r.error = e.what();
    }
    return r;
}

bool CoinbaseClient::cancel_order(const std::string& order_id) {
    nlohmann::json body;
    body["order_ids"] = nlohmann::json::array({order_id});

    std::string resp = http_request(
        "POST", "/api/v3/brokerage/orders/batch_cancel", body.dump());
    if (resp.empty()) return false;

    try {
        auto j = nlohmann::json::parse(resp);
        if (j.contains("results") && !j["results"].empty())
            return j["results"][0].value("success", false);
    } catch (...) {}
    return false;
}

} // namespace execution
