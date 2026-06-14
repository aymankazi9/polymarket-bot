#pragma once
#include <array>
#include <cstdint>
#include <string>

namespace wallet {

// 256-bit and 20-byte types used in EIP-712 ABI encoding.
// uint256_t is stored big-endian (most-significant byte first), matching Ethereum ABI.
using uint256_t = std::array<uint8_t, 32>;
using address_t = std::array<uint8_t, 20>;

// Polymarket CLOB order — must match the Solidity struct in CTFExchange exactly.
struct Order {
    uint256_t salt;           // random per order (from /dev/urandom)
    address_t maker;          // bot wallet address
    address_t signer;         // same as maker for EOA
    address_t taker;          // zero address = open order
    uint256_t tokenId;        // YES or NO conditional token ID for this market
    uint256_t makerAmount;    // USDC amount, 6 decimal places
    uint256_t takerAmount;    // share amount
    uint256_t expiration;     // unix timestamp; IOC: now + 10s
    uint256_t nonce;          // CLOB-level account nonce (not Polygon tx nonce)
    uint256_t feeRateBps;     // 0 for maker, 200 for taker
    uint8_t   side;           // 0 = BUY, 1 = SELL
    uint8_t   signatureType;  // 0 = EOA
};

// Compute the 32-byte EIP-712 digest that must be signed for this order:
//   keccak256(0x1901 || domainSeparator || hashStruct(order))
//
// Uses the Polygon mainnet CTF Exchange domain (chain ID 137,
// verifyingContract = 0x4bFb41d5B3570DeFd03C39a9A4D8dE6Bd8B8982E).
std::array<uint8_t, 32> hash_order(const Order& order);

// ---------------------------------------------------------------------------
// Utility: build a uint256_t from common value types
// ---------------------------------------------------------------------------

// Zero-extend a 64-bit value to 32 bytes, big-endian.
uint256_t u64_to_uint256(uint64_t v);

// Zero-extend an address to a 32-byte ABI word (left-pad with 12 zero bytes).
std::array<uint8_t, 32> address_to_word(const address_t& addr);

// Random salt for a new order (reads 32 bytes from /dev/urandom).
uint256_t random_salt();

} // namespace wallet
