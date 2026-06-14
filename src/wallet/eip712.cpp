#include "eip712.hpp"
#include "../crypto/keccak256.hpp"
#include "../../constants.hpp"

#include <cstring>
#include <cstdio>
#include <stdexcept>
#include <array>
#include <cstdint>

// EIP-712 structured-data signing for Polymarket CLOB orders.
//
// Reference: https://eips.ethereum.org/EIPS/eip-712
//
// Type string (must match CTFExchange.sol exactly):
//   Order(uint256 salt,address maker,address signer,address taker,
//         uint256 tokenId,uint256 makerAmount,uint256 takerAmount,
//         uint256 expiration,uint256 nonce,uint256 feeRateBps,
//         uint8 side,uint8 signatureType)
//
// Domain (Polygon mainnet):
//   name               = "Polymarket CTF Exchange"
//   version            = "1"
//   chainId            = 137
//   verifyingContract  = 0x4bFb41d5B3570DeFd03C39a9A4D8dE6Bd8B8982E
//
// Note: neg-risk markets use the same CTFExchange for order signing; the
// neg-risk adapter (0xd91E80cF2E7be2e162c6513ceD06f1dD0dA35296) is only
// relevant for the separate neg-risk merge/split operations, not for CLOB
// order signing.

namespace wallet {

namespace {

// ---------------------------------------------------------------------------
// Pre-computed constants (computed once at static-init time)
// ---------------------------------------------------------------------------

// keccak256("EIP712Domain(string name,string version,uint256 chainId,address verifyingContract)")
constexpr char DOMAIN_TYPE_STRING[] =
    "EIP712Domain(string name,string version,uint256 chainId,address verifyingContract)";

// keccak256("Order(uint256 salt,address maker,address signer,address taker,uint256 tokenId,"
//           "uint256 makerAmount,uint256 takerAmount,uint256 expiration,uint256 nonce,"
//           "uint256 feeRateBps,uint8 side,uint8 signatureType)")
constexpr char ORDER_TYPE_STRING[] =
    "Order(uint256 salt,address maker,address signer,address taker,"
    "uint256 tokenId,uint256 makerAmount,uint256 takerAmount,"
    "uint256 expiration,uint256 nonce,uint256 feeRateBps,"
    "uint8 side,uint8 signatureType)";

// CTF Exchange address on Polygon mainnet (bytes, not hex string)
constexpr uint8_t CTF_EXCHANGE_BYTES[20] = {
    0x4b, 0xFb, 0x41, 0xd5, 0xB3, 0x57, 0x0D, 0xeF, 0xd0, 0x3C,
    0x39, 0xa9, 0xA4, 0xD8, 0xdE, 0x6B, 0xd8, 0xB8, 0x98, 0x2E,
};

// ---------------------------------------------------------------------------
// ABI encoding helpers
// ---------------------------------------------------------------------------

// Encode uint256_t (already big-endian 32 bytes) → 32-byte word. Trivial copy.
inline void encode_uint256(uint8_t* out, const uint256_t& v) {
    std::memcpy(out, v.data(), 32);
}

// Encode address (20 bytes) as a 32-byte ABI word: 12 zero bytes + 20 address bytes.
inline void encode_address(uint8_t* out, const address_t& addr) {
    std::memset(out, 0, 12);
    std::memcpy(out + 12, addr.data(), 20);
}

// Encode uint8_t as a 32-byte ABI word: 31 zero bytes + 1 value byte.
inline void encode_uint8(uint8_t* out, uint8_t v) {
    std::memset(out, 0, 31);
    out[31] = v;
}

// ---------------------------------------------------------------------------
// Hash helpers
// ---------------------------------------------------------------------------

// keccak256 of a string literal (the string bytes, not null-terminated length).
template<size_t N>
std::array<uint8_t, 32> hash_str(const char (&s)[N]) {
    // N includes the null terminator; hash N-1 bytes.
    return crypto::keccak256(reinterpret_cast<const uint8_t*>(s), N - 1);
}

// ---------------------------------------------------------------------------
// Singleton domain separator (computed at program startup)
// ---------------------------------------------------------------------------

struct DomainCache {
    std::array<uint8_t, 32> domain_type_hash;
    std::array<uint8_t, 32> order_type_hash;
    std::array<uint8_t, 32> domain_separator;

    DomainCache() {
        domain_type_hash = hash_str(DOMAIN_TYPE_STRING);
        order_type_hash  = hash_str(ORDER_TYPE_STRING);

        // domainSeparator = keccak256(abi.encode(
        //     domainTypeHash,
        //     keccak256(bytes("Polymarket CTF Exchange")),  // name
        //     keccak256(bytes("1")),                        // version
        //     uint256(137),                                 // chainId
        //     address(CTF_EXCHANGE)                         // verifyingContract
        // ))
        //
        // abi.encode packs each field to 32 bytes, concatenated.
        constexpr char NAME[]    = "Polymarket CTF Exchange";
        constexpr char VERSION[] = "1";

        auto name_hash    = crypto::keccak256(
            reinterpret_cast<const uint8_t*>(NAME),    sizeof(NAME)    - 1);
        auto version_hash = crypto::keccak256(
            reinterpret_cast<const uint8_t*>(VERSION), sizeof(VERSION) - 1);

        // 5 fields × 32 bytes = 160 bytes
        uint8_t enc[5 * 32] = {};
        std::memcpy(enc + 0 * 32, domain_type_hash.data(), 32);
        std::memcpy(enc + 1 * 32, name_hash.data(),        32);
        std::memcpy(enc + 2 * 32, version_hash.data(),     32);

        // chainId = 137 → big-endian 32 bytes
        enc[3 * 32 + 30] = 0x00;
        enc[3 * 32 + 31] = static_cast<uint8_t>(constants::POLYGON_CHAIN_ID); // 137 = 0x89

        // verifyingContract = CTF_EXCHANGE (left-padded address in 32 bytes)
        std::memset(enc + 4 * 32, 0, 12);
        std::memcpy(enc + 4 * 32 + 12, CTF_EXCHANGE_BYTES, 20);

        domain_separator = crypto::keccak256(enc, sizeof(enc));
    }
};

const DomainCache& get_domain() {
    static DomainCache cache;
    return cache;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

std::array<uint8_t, 32> hash_order(const Order& order) {
    const DomainCache& dom = get_domain();

    // hashStruct(order) = keccak256(ORDER_TYPE_HASH || enc(field0) || ... || enc(field11))
    // 13 words × 32 bytes = 416 bytes
    constexpr size_t FIELD_COUNT = 13;  // typeHash + 12 order fields
    uint8_t struct_enc[FIELD_COUNT * 32] = {};

    size_t off = 0;
    std::memcpy(struct_enc + off, dom.order_type_hash.data(), 32); off += 32;

    encode_uint256(struct_enc + off, order.salt);         off += 32;
    encode_address(struct_enc + off, order.maker);        off += 32;
    encode_address(struct_enc + off, order.signer);       off += 32;
    encode_address(struct_enc + off, order.taker);        off += 32;
    encode_uint256(struct_enc + off, order.tokenId);      off += 32;
    encode_uint256(struct_enc + off, order.makerAmount);  off += 32;
    encode_uint256(struct_enc + off, order.takerAmount);  off += 32;
    encode_uint256(struct_enc + off, order.expiration);   off += 32;
    encode_uint256(struct_enc + off, order.nonce);        off += 32;
    encode_uint256(struct_enc + off, order.feeRateBps);   off += 32;
    encode_uint8  (struct_enc + off, order.side);         off += 32;
    encode_uint8  (struct_enc + off, order.signatureType);

    auto struct_hash = crypto::keccak256(struct_enc, sizeof(struct_enc));

    // Final digest: keccak256(0x1901 || domainSeparator || structHash)
    uint8_t digest_input[2 + 32 + 32];
    digest_input[0] = 0x19;
    digest_input[1] = 0x01;
    std::memcpy(digest_input + 2,      dom.domain_separator.data(), 32);
    std::memcpy(digest_input + 2 + 32, struct_hash.data(),          32);

    return crypto::keccak256(digest_input, sizeof(digest_input));
}

// ---------------------------------------------------------------------------
// Utility implementations
// ---------------------------------------------------------------------------

uint256_t u64_to_uint256(uint64_t v) {
    uint256_t out{};
    // Big-endian: most significant byte at index 0
    out[24] = static_cast<uint8_t>(v >> 56);
    out[25] = static_cast<uint8_t>(v >> 48);
    out[26] = static_cast<uint8_t>(v >> 40);
    out[27] = static_cast<uint8_t>(v >> 32);
    out[28] = static_cast<uint8_t>(v >> 24);
    out[29] = static_cast<uint8_t>(v >> 16);
    out[30] = static_cast<uint8_t>(v >>  8);
    out[31] = static_cast<uint8_t>(v);
    return out;
}

std::array<uint8_t, 32> address_to_word(const address_t& addr) {
    std::array<uint8_t, 32> word{};
    std::memcpy(word.data() + 12, addr.data(), 20);
    return word;
}

uint256_t random_salt() {
    uint256_t salt{};
    FILE* urandom = std::fopen("/dev/urandom", "rb");
    if (!urandom)
        throw std::runtime_error("eip712: cannot open /dev/urandom");
    if (std::fread(salt.data(), 1, 32, urandom) != 32) {
        std::fclose(urandom);
        throw std::runtime_error("eip712: /dev/urandom read failed");
    }
    std::fclose(urandom);
    return salt;
}

} // namespace wallet
