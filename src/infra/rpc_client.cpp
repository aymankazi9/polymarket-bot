#include "rpc_client.hpp"
#include "../../constants.hpp"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <cstdio>
#include <stdexcept>
#include <string>

namespace infra {

namespace {

static size_t write_cb(char* ptr, size_t size, size_t nmemb, void* ud) {
    static_cast<std::string*>(ud)->append(ptr, size * nmemb);
    return size * nmemb;
}

// Synchronous HTTPS POST.  Returns empty string on curl failure; does not throw.
static std::string https_post(const std::string& url,
                               const std::string& body,
                               int timeout_ms) noexcept {
    CURL* curl = curl_easy_init();
    if (!curl) return {};

    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body.size());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)timeout_ms);

    curl_slist* hdrs = curl_slist_append(nullptr, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);

    CURLcode rc = curl_easy_perform(curl);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) return {};
    return response;
}

// Returns true if the JSON-RPC response contains a "result" key (not an error object).
static bool has_result(const std::string& resp) noexcept {
    if (resp.empty()) return false;
    try {
        auto j = nlohmann::json::parse(resp);
        return j.contains("result");
    } catch (...) { return false; }
}

} // anonymous namespace

std::string RpcClient::rpc_call(const std::string& method,
                                 const std::string& params_json) {
    const std::string body =
        R"({"jsonrpc":"2.0","method":")" + method +
        R"(","params":)" + params_json +
        R"(,"id":1})";

    // Primary attempt
    std::string resp = https_post(constants::RPC_PRIMARY, body, constants::RPC_TIMEOUT_MS);
    if (has_result(resp)) return resp;

    // Fallback attempt
    std::fprintf(stderr,
        "rpc_client: primary failed for %s — retrying on fallback\n", method.c_str());
    resp = https_post(constants::RPC_FALLBACK, body, constants::RPC_TIMEOUT_MS);
    if (has_result(resp)) return resp;

    throw std::runtime_error(
        "rpc_client: both primary and fallback failed for " + method);
}

uint64_t RpcClient::get_transaction_count(const std::string& hex_address) {
    const std::string params = R"([")" + hex_address + R"(","pending"])";
    const std::string resp   = rpc_call("eth_getTransactionCount", params);

    auto j = nlohmann::json::parse(resp);
    std::string hex = j["result"].get<std::string>();

    // Strip "0x" prefix before parsing as hex
    if (hex.size() > 2 && hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X'))
        hex = hex.substr(2);
    return std::stoull(hex, nullptr, 16);
}

std::string RpcClient::send_raw_transaction(const std::string& signed_hex) {
    const std::string params = R"([")" + signed_hex + R"("])";
    const std::string resp   = rpc_call("eth_sendRawTransaction", params);

    auto j = nlohmann::json::parse(resp);
    if (j.contains("error")) {
        throw std::runtime_error(
            "eth_sendRawTransaction error: "
            + j["error"].value("message", "unknown"));
    }
    return j["result"].get<std::string>();  // tx hash
}

} // namespace infra
