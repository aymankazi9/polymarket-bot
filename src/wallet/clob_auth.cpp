#include "clob_auth.hpp"
#include "../crypto/keccak256.hpp"
#include "../../constants.hpp"

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// EIP-712 domain for CLOB auth (separate from the Order domain):
//   name    = "ClobAuthDomain"
//   version = "1"
//   chainId = 137
//   (no verifyingContract)
//
// ClobAuth struct type:
//   ClobAuth(address address, string timestamp, uint256 nonce, string message)
//
// Fixed attestation message (from py-clob-client):
//   "This message attests that I control the given wallet"
// ---------------------------------------------------------------------------

namespace wallet {

namespace {

// ---------------------------------------------------------------------------
// Encoding helpers (EIP-712 ABI encoding)
// ---------------------------------------------------------------------------

// In EIP-712, `string` fields are encoded as keccak256(UTF-8 bytes of the value).
// `address` is left-padded to 32 bytes.
// `uint256` is big-endian 32 bytes.

inline std::array<uint8_t, 32> hash_utf8(const std::string& s) {
    return crypto::keccak256(
        reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

inline std::array<uint8_t, 32> hash_cstr(const char* s) {
    return crypto::keccak256(s);  // keccak256.hpp overload hashes strlen bytes
}

inline void encode_address_word(uint8_t* out, const std::array<uint8_t, 20>& addr) {
    std::memset(out, 0, 12);
    std::memcpy(out + 12, addr.data(), 20);
}

inline void encode_u64_word(uint8_t* out, uint64_t v) {
    std::memset(out, 0, 32);
    for (int i = 7; i >= 0; --i) {
        out[24 + (7 - i)] = static_cast<uint8_t>(v >> (i * 8));
    }
}

// ---------------------------------------------------------------------------
// ClobAuth domain separator — computed once via Meyer's singleton
// ---------------------------------------------------------------------------

const std::array<uint8_t, 32>& clob_auth_domain_separator() {
    static const std::array<uint8_t, 32> DS = [] {
        constexpr char DOMAIN_TYPE[] =
            "EIP712Domain(string name,string version,uint256 chainId)";
        constexpr char NAME[]    = "ClobAuthDomain";
        constexpr char VERSION[] = "1";

        auto type_hash    = hash_cstr(DOMAIN_TYPE);
        auto name_hash    = hash_cstr(NAME);
        auto version_hash = hash_cstr(VERSION);

        // abi.encode(typeHash, nameHash, versionHash, chainId) → 4 × 32 bytes
        uint8_t enc[4 * 32] = {};
        std::memcpy(enc + 0 * 32, type_hash.data(),    32);
        std::memcpy(enc + 1 * 32, name_hash.data(),    32);
        std::memcpy(enc + 2 * 32, version_hash.data(), 32);
        enc[3 * 32 + 31] = static_cast<uint8_t>(constants::POLYGON_CHAIN_ID); // 137 = 0x89

        return crypto::keccak256(enc, sizeof(enc));
    }();
    return DS;
}

// Build the 32-byte EIP-712 digest for a ClobAuth struct
std::array<uint8_t, 32> clob_auth_digest(
    const std::array<uint8_t, 20>& wallet_addr,
    const std::string& timestamp,
    uint64_t nonce)
{
    constexpr char AUTH_TYPE[] =
        "ClobAuth(address address,string timestamp,uint256 nonce,string message)";
    constexpr char FIXED_MSG[] =
        "This message attests that I control the given wallet";

    static const auto type_hash = hash_cstr(AUTH_TYPE);
    static const auto msg_hash  = hash_cstr(FIXED_MSG);

    auto ts_hash = hash_utf8(timestamp);

    // hashStruct: 5 words × 32 bytes = 160 bytes
    uint8_t struct_enc[5 * 32] = {};
    std::memcpy(struct_enc + 0 * 32, type_hash.data(), 32);
    encode_address_word(struct_enc + 1 * 32, wallet_addr);
    std::memcpy(struct_enc + 2 * 32, ts_hash.data(), 32);
    encode_u64_word(struct_enc + 3 * 32, nonce);
    std::memcpy(struct_enc + 4 * 32, msg_hash.data(), 32);

    auto struct_hash = crypto::keccak256(struct_enc, sizeof(struct_enc));
    const auto& ds   = clob_auth_domain_separator();

    // Final digest: keccak256(0x1901 || domainSeparator || structHash)
    uint8_t input[2 + 32 + 32];
    input[0] = 0x19;
    input[1] = 0x01;
    std::memcpy(input + 2,      ds.data(),          32);
    std::memcpy(input + 2 + 32, struct_hash.data(), 32);

    return crypto::keccak256(input, sizeof(input));
}

// ---------------------------------------------------------------------------
// Address formatting
// ---------------------------------------------------------------------------

std::string address_to_hex(const std::array<uint8_t, 20>& addr) {
    static constexpr char HEX[] = "0123456789abcdef";
    std::string s;
    s.reserve(42);
    s += "0x";
    for (uint8_t b : addr) {
        s += HEX[b >> 4];
        s += HEX[b & 0xf];
    }
    return s;
}

// ---------------------------------------------------------------------------
// Base64url encode / decode  (for HMAC Level-2 signatures)
// ---------------------------------------------------------------------------

std::string b64url_encode(const uint8_t* data, size_t len) {
    // Allocate enough space for standard base64 output
    std::vector<uint8_t> buf((len + 2) / 3 * 4 + 1);
    int out_len = EVP_EncodeBlock(buf.data(), data, static_cast<int>(len));
    std::string s(reinterpret_cast<char*>(buf.data()),
                  static_cast<size_t>(out_len));
    // URL-safe substitutions and strip padding
    for (char& c : s) {
        if (c == '+') c = '-';
        if (c == '/') c = '_';
    }
    while (!s.empty() && s.back() == '=')
        s.pop_back();
    return s;
}

std::vector<uint8_t> b64url_decode(const std::string& s) {
    // Undo URL-safe substitutions and restore padding
    std::string padded;
    padded.reserve(s.size() + 3);
    for (char c : s) {
        if (c == '-') padded += '+';
        else if (c == '_') padded += '/';
        else padded += c;
    }
    while (padded.size() % 4 != 0) padded += '=';

    std::vector<uint8_t> out(padded.size() * 3 / 4 + 1);
    int len = EVP_DecodeBlock(
        out.data(),
        reinterpret_cast<const uint8_t*>(padded.data()),
        static_cast<int>(padded.size()));
    if (len < 0)
        throw std::runtime_error("clob_auth: base64url decode failed");

    // EVP_DecodeBlock may include padding bytes — trim them
    // The true decoded length ignores trailing '=' padding chars
    size_t padding = 0;
    for (int i = static_cast<int>(padded.size()) - 1;
         i >= 0 && padded[i] == '='; --i)
        ++padding;
    out.resize(static_cast<size_t>(len) - padding);
    return out;
}

// ---------------------------------------------------------------------------
// HMAC-SHA256
// ---------------------------------------------------------------------------

std::array<uint8_t, 32> hmac_sha256(const std::vector<uint8_t>& key,
                                     const std::string& msg) {
    std::array<uint8_t, 32> digest;
    unsigned int digest_len = 32;
    if (!HMAC(EVP_sha256(),
              key.data(), static_cast<int>(key.size()),
              reinterpret_cast<const uint8_t*>(msg.data()), msg.size(),
              digest.data(), &digest_len))
        throw std::runtime_error("clob_auth: HMAC-SHA256 failed");
    return digest;
}

// ---------------------------------------------------------------------------
// Current Unix timestamp as decimal string
// ---------------------------------------------------------------------------

std::string now_timestamp_str() {
    using namespace std::chrono;
    auto secs = duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
    return std::to_string(static_cast<long long>(secs));
}

// ---------------------------------------------------------------------------
// libcurl HTTP POST — startup only, not on hot path
// ---------------------------------------------------------------------------

struct HttpResult {
    long        status = 0;
    std::string body;
};

static size_t curl_write(void* ptr, size_t sz, size_t nmemb, std::string* s) {
    s->append(static_cast<const char*>(ptr), sz * nmemb);
    return sz * nmemb;
}

HttpResult http_post(const std::string& url,
                     const std::map<std::string, std::string>& headers,
                     const std::string& body) {
    CURL* c = curl_easy_init();
    if (!c) throw std::runtime_error("clob_auth: curl_easy_init failed");
    struct G { CURL* c; ~G() { curl_easy_cleanup(c); } } g{c};

    HttpResult result;
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_POST, 1L);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_write);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &result.body);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);

    struct curl_slist* hlist = nullptr;
    hlist = curl_slist_append(hlist, "Content-Type: application/json");
    for (const auto& [k, v] : headers) {
        std::string h = k + ": " + v;
        hlist = curl_slist_append(hlist, h.c_str());
    }
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hlist);

    CURLcode rc = curl_easy_perform(c);
    curl_slist_free_all(hlist);

    if (rc != CURLE_OK)
        throw std::runtime_error(
            std::string("clob_auth: HTTP POST failed: ") + curl_easy_strerror(rc));

    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &result.status);
    return result;
}

HttpResult http_get(const std::string& url,
                    const std::map<std::string, std::string>& headers) {
    CURL* c = curl_easy_init();
    if (!c) throw std::runtime_error("clob_auth: curl_easy_init failed");
    struct G { CURL* c; ~G() { curl_easy_cleanup(c); } } g{c};

    HttpResult result;
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_write);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &result.body);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);

    struct curl_slist* hlist = nullptr;
    for (const auto& [k, v] : headers) {
        std::string h = k + ": " + v;
        hlist = curl_slist_append(hlist, h.c_str());
    }
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hlist);

    CURLcode rc = curl_easy_perform(c);
    curl_slist_free_all(hlist);

    if (rc != CURLE_OK)
        throw std::runtime_error(
            std::string("clob_auth: HTTP GET failed: ") + curl_easy_strerror(rc));

    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &result.status);
    return result;
}

ClobAuth::ApiCreds parse_api_creds(const std::string& json_body) {
    auto j = nlohmann::json::parse(json_body);
    ClobAuth::ApiCreds c;
    // Polymarket returns either camelCase (apiKey) or snake_case (api_key)
    c.api_key    = j.contains("apiKey") ? j["apiKey"].get<std::string>()
                                        : j.at("api_key").get<std::string>();
    c.secret     = j.at("secret").get<std::string>();
    c.passphrase = j.at("passphrase").get<std::string>();
    return c;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// ClobAuth implementation
// ---------------------------------------------------------------------------

ClobAuth::ClobAuth(std::string clob_host)
    : host_(std::move(clob_host)) {}

void ClobAuth::authenticate(const KeyManager& km, uint64_t nonce) {
    if (!km.loaded())
        throw std::logic_error("clob_auth: KeyManager not loaded");

    wallet_hex_ = address_to_hex(km.credentials().wallet_address);

    // Build L1 headers once; the same signed payload is reused for both
    // create and (if needed) derive so the timestamp/nonce stay consistent.
    auto hdrs = level1_headers(km, nonce);

    // Attempt 1: create a new API key.
    auto create = http_post(host_ + "/auth/api-key", hdrs, "");

    if (create.status == 200) {
        creds_ = parse_api_creds(create.body);
        return;
    }

    // Attempt 2: if the server returned 400, the key already exists for this
    // address+nonce.  Retrieve the existing credentials via the derive endpoint.
    //
    // We do NOT fall back to derive for other status codes (401 means the
    // signature is wrong, 5xx means a server fault) — derive would fail for
    // the same reason and would just waste another round-trip.
    //
    // We intentionally do not string-match the error body here: Polymarket has
    // not documented a stable error code for "key already exists", so 400 is
    // the reliable signal.  The raw body is preserved in the fatal error
    // message below for post-mortem diagnosis if both attempts fail.
    if (create.status != 400) {
        throw std::runtime_error(
            "clob_auth: /auth/api-key returned HTTP " +
            std::to_string(create.status) + ": " + create.body);
    }

    auto derive = http_get(host_ + "/auth/derive-api-key", hdrs);

    if (derive.status == 200) {
        creds_ = parse_api_creds(derive.body);
        return;
    }

    throw std::runtime_error(
        "clob_auth: failed to create or derive API key — "
        "create HTTP " + std::to_string(create.status) +
        " body=(" + create.body + "); "
        "derive HTTP " + std::to_string(derive.status) +
        " body=(" + derive.body + ")");
}

std::map<std::string, std::string> ClobAuth::level1_headers(
    const KeyManager& km, uint64_t nonce) const
{
    std::string ts  = now_timestamp_str();
    std::string sig = sign_clob_auth(km, ts, nonce);
    std::string addr = address_to_hex(km.credentials().wallet_address);

    return {
        {"POLY_ADDRESS",   addr},
        {"POLY_SIGNATURE", sig},
        {"POLY_TIMESTAMP", ts},
        {"POLY_NONCE",     std::to_string(nonce)},
    };
}

std::map<std::string, std::string> ClobAuth::level2_headers(
    const std::string& method,
    const std::string& path,
    const std::string& body) const
{
    if (!creds_.has_value())
        throw std::logic_error("clob_auth: not authenticated — call authenticate() first");

    std::string ts = now_timestamp_str();

    // Normalise body: py-clob-client replaces single quotes with double quotes
    // before computing HMAC so the signature matches Go/TypeScript implementations.
    std::string normalised_body = body;
    for (char& c : normalised_body)
        if (c == '\'') c = '"';

    // HMAC message: timestamp + METHOD + path + body
    std::string hmac_msg = ts + method + path + normalised_body;

    auto key     = b64url_decode(creds_->secret);
    auto digest  = hmac_sha256(key, hmac_msg);
    auto sig     = b64url_encode(digest.data(), digest.size());

    return {
        {"POLY_ADDRESS",    wallet_hex_},
        {"POLY_SIGNATURE",  sig},
        {"POLY_TIMESTAMP",  ts},
        {"POLY_API_KEY",    creds_->api_key},
        {"POLY_PASSPHRASE", creds_->passphrase},
    };
}

std::string ClobAuth::sign_clob_auth(
    const KeyManager& km,
    const std::string& timestamp,
    uint64_t nonce) const
{
    auto digest = clob_auth_digest(
        km.credentials().wallet_address, timestamp, nonce);

    auto sig65 = km.sign(digest);  // r (32) || s (32) || v (1)

    // Return as "0x" + 130-char hex
    static constexpr char HEX[] = "0123456789abcdef";
    std::string hex;
    hex.reserve(132);
    hex += "0x";
    for (uint8_t b : sig65) {
        hex += HEX[b >> 4];
        hex += HEX[b & 0xf];
    }
    return hex;
}

} // namespace wallet
