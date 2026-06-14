#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <span>

// Keccak-256 as used by Ethereum.
// NOT SHA3-256: the padding byte is 0x01, not 0x06.
// Assumes little-endian host (x86_64 / Linux target).

namespace crypto {

namespace detail {

inline void keccak_f1600(uint64_t* A) noexcept {
    static constexpr uint64_t RC[24] = {
        0x0000000000000001ULL, 0x0000000000008082ULL,
        0x800000000000808AULL, 0x8000000080008000ULL,
        0x000000000000808BULL, 0x0000000080000001ULL,
        0x8000000080008081ULL, 0x8000000000008009ULL,
        0x000000000000008AULL, 0x0000000000000088ULL,
        0x0000000080008009ULL, 0x000000008000000AULL,
        0x000000008000808BULL, 0x800000000000008BULL,
        0x8000000000008089ULL, 0x8000000000008003ULL,
        0x8000000000008002ULL, 0x8000000000000080ULL,
        0x000000000000800AULL, 0x800000008000000AULL,
        0x8000000080008081ULL, 0x8000000000008080ULL,
        0x0000000080000001ULL, 0x8000000080008008ULL,
    };
    // Destination lane index for each step of the rho-pi cycle starting at lane 1
    static constexpr int PI[24] = {
        10, 7, 11, 17, 18, 3, 5, 16, 8, 21, 24, 4,
        15, 23, 19, 13, 12, 2, 20, 14, 22, 9, 6, 1,
    };
    // Rotation amount for each step of the same cycle
    static constexpr int RHO[24] = {
        1, 3, 6, 10, 15, 21, 28, 36, 45, 55, 2, 14,
        27, 41, 56, 8, 25, 43, 62, 18, 39, 61, 20, 44,
    };

    uint64_t C[5], D[5], B[5], T;

    for (int round = 0; round < 24; ++round) {
        // θ
        for (int x = 0; x < 5; ++x)
            C[x] = A[x] ^ A[x+5] ^ A[x+10] ^ A[x+15] ^ A[x+20];
        for (int x = 0; x < 5; ++x) {
            D[x] = C[(x+4)%5] ^ ((C[(x+1)%5] << 1) | (C[(x+1)%5] >> 63));
            for (int y = 0; y < 5; ++y) A[x + 5*y] ^= D[x];
        }
        // ρ + π  (combined in-place using the cycle trick)
        T = A[1];
        for (int i = 0; i < 24; ++i) {
            int j = PI[i];
            uint64_t T2 = A[j];
            A[j] = (T << RHO[i]) | (T >> (64 - RHO[i]));
            T = T2;
        }
        // χ
        for (int y = 0; y < 5; ++y) {
            for (int x = 0; x < 5; ++x) B[x] = A[x + 5*y];
            for (int x = 0; x < 5; ++x)
                A[x + 5*y] = B[x] ^ ((~B[(x+1)%5]) & B[(x+2)%5]);
        }
        // ι
        A[0] ^= RC[round];
    }
}

} // namespace detail

// Compute keccak256 of arbitrary-length input.
inline std::array<uint8_t, 32> keccak256(const uint8_t* data, size_t len) noexcept {
    constexpr size_t RATE = 136;  // (1600 - 2*256) / 8

    uint64_t A[25] = {};

    // Absorb full blocks
    size_t offset = 0;
    while (offset + RATE <= len) {
        for (size_t i = 0; i < RATE / 8; ++i) {
            uint64_t lane;
            std::memcpy(&lane, data + offset + i * 8, 8);
            A[i] ^= lane;
        }
        detail::keccak_f1600(A);
        offset += RATE;
    }

    // Final block with Keccak padding (0x01 ... 0x80)
    uint8_t buf[RATE] = {};
    std::memcpy(buf, data + offset, len - offset);
    buf[len - offset]  = 0x01;
    buf[RATE - 1]     ^= 0x80;

    for (size_t i = 0; i < RATE / 8; ++i) {
        uint64_t lane;
        std::memcpy(&lane, buf + i * 8, 8);
        A[i] ^= lane;
    }
    detail::keccak_f1600(A);

    // Squeeze first 32 bytes
    std::array<uint8_t, 32> out;
    std::memcpy(out.data(), A, 32);
    return out;
}

inline std::array<uint8_t, 32> keccak256(std::span<const uint8_t> data) noexcept {
    return keccak256(data.data(), data.size());
}

// Convenience: hash a string literal (e.g. for type-hash computation)
inline std::array<uint8_t, 32> keccak256(const char* str) noexcept {
    return keccak256(reinterpret_cast<const uint8_t*>(str), std::strlen(str));
}

} // namespace crypto
