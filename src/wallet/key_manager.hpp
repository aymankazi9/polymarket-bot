#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <string_view>

// Forward-declare the opaque secp256k1 context to avoid pulling in the header everywhere.
struct secp256k1_context_struct;

namespace wallet {

// KeyManager owns the bot's private key for the entire process lifetime.
//
// Lifecycle:
//   1. load() reads an AES-256-GCM-encrypted secrets file and prompts for the passphrase
//      on stdin (never from env / never logged).
//   2. The raw 32-byte private key is kept in a mlock()'d buffer.
//   3. sign() produces recoverable secp256k1 ECDSA signatures (r||s||v, 65 bytes).
//   4. The destructor wipes the key with explicit_bzero before releasing memory.
//
// Encrypted file format (all fields concatenated, no framing):
//   [16 bytes]  PBKDF2-SHA256 salt
//   [12 bytes]  AES-256-GCM nonce
//   [16 bytes]  AES-256-GCM authentication tag
//   [N  bytes]  ciphertext
//
// Plaintext (JSON):
//   {
//     "private_key":         "0x<64 hex chars>",
//     "coinbase_key_name":   "organizations/{org_id}/apiKeys/{key_id}",
//     "coinbase_key_secret": "-----BEGIN PRIVATE KEY-----\n...\n-----END PRIVATE KEY-----\n"
//   }
// coinbase_key_secret is the Ed25519 private key in PKCS8 PEM format,
// exactly as exported by the Coinbase CDP key generator.
//
// PBKDF2 parameters: HMAC-SHA256, 100 000 iterations, 32-byte output.

class KeyManager {
public:
    struct Credentials {
        std::string coinbase_key_name;    // "organizations/{org_id}/apiKeys/{key_id}"
        std::string coinbase_key_secret;  // Ed25519 PKCS8 PEM
        std::array<uint8_t, 20> wallet_address{};  // Ethereum address derived from the pubkey
    };

    KeyManager();
    ~KeyManager();

    // Not copyable: owns sensitive mlock'd memory.
    KeyManager(const KeyManager&)            = delete;
    KeyManager& operator=(const KeyManager&) = delete;

    // Load and decrypt secrets file; prompts for passphrase on stdin.
    // Throws std::runtime_error on decryption failure or bad JSON.
    void load(std::string_view key_file_path);

    // Sign a 32-byte digest with the private key.
    // Returns r (32 bytes) || s (32 bytes) || v (1 byte), where v = recid + 27.
    std::array<uint8_t, 65> sign(const std::array<uint8_t, 32>& digest) const;

    const Credentials& credentials() const { return credentials_; }
    bool               loaded()      const { return loaded_; }

private:
    void parse_private_key(const std::string& hex);
    void derive_address();
    void wipe() noexcept;

    bool loaded_ = false;

    // 32-byte private key — sits in a mlock'd allocation.
    // Allocated separately so mlock covers exactly this region.
    uint8_t* privkey_   = nullptr;   // heap-allocated, mlock'd, 32 bytes
    bool     mlocked_   = false;

    secp256k1_context_struct* ctx_ = nullptr;
    Credentials               credentials_;
};

} // namespace wallet
