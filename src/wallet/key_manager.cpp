#include "key_manager.hpp"
#include "../crypto/keccak256.hpp"

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <secp256k1.h>
#include <secp256k1_recovery.h>

#include <nlohmann/json.hpp>

#include <sys/mman.h>   // mlock / munlock
#include <unistd.h>
#include <termios.h>    // tcgetattr / tcsetattr

#include <cerrno>
#include <cstdio>
#include <cstdlib>      // aligned_alloc / free
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace wallet {

namespace {

// Read passphrase from stdin without terminal echo.
std::string read_passphrase_stdin() {
    struct termios old_term{};
    bool is_tty = (tcgetattr(STDIN_FILENO, &old_term) == 0);
    if (is_tty) {
        struct termios no_echo = old_term;
        no_echo.c_lflag &= ~(tcflag_t)ECHO;
        tcsetattr(STDIN_FILENO, TCSANOW, &no_echo);
    }

    std::fprintf(stderr, "Enter key passphrase: ");
    std::fflush(stderr);

    std::string passphrase;
    std::getline(std::cin, passphrase);

    if (is_tty) {
        tcsetattr(STDIN_FILENO, TCSANOW, &old_term);
        std::fprintf(stderr, "\n");
    }
    return passphrase;
}

int hex_nibble(char c) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::vector<uint8_t> hex_decode(const std::string& s) {
    const char* p   = s.data();
    size_t      len = s.size();
    if (len >= 2 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2; len -= 2;
    }
    if (len % 2 != 0)
        throw std::runtime_error("key_manager: odd-length hex string");

    std::vector<uint8_t> out(len / 2);
    for (size_t i = 0; i < len; i += 2) {
        int hi = hex_nibble(p[i]);
        int lo = hex_nibble(p[i + 1]);
        if (hi < 0 || lo < 0)
            throw std::runtime_error("key_manager: invalid hex character in key");
        out[i / 2] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return out;
}

// PBKDF2-HMAC-SHA256 → 32-byte AES key.
void pbkdf2_sha256(const std::string& passphrase,
                   const uint8_t* salt, size_t salt_len,
                   int iterations,
                   uint8_t* out_key, size_t key_len) {
    if (PKCS5_PBKDF2_HMAC(passphrase.data(), static_cast<int>(passphrase.size()),
                           salt, static_cast<int>(salt_len),
                           iterations, EVP_sha256(),
                           static_cast<int>(key_len), out_key) != 1)
        throw std::runtime_error("key_manager: PBKDF2 failed");
}

// AES-256-GCM decrypt. Throws if authentication tag is wrong.
std::vector<uint8_t> aes256gcm_decrypt(
    const uint8_t* key,   // 32 bytes
    const uint8_t* nonce, // 12 bytes
    const uint8_t* tag,   // 16 bytes
    const uint8_t* ct, size_t ct_len)
{
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx)
        throw std::runtime_error("key_manager: EVP_CIPHER_CTX_new failed");

    struct Guard { EVP_CIPHER_CTX* c; ~Guard() { EVP_CIPHER_CTX_free(c); } } g{ctx};

    if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1 ||
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 12, nullptr) != 1 ||
        EVP_DecryptInit_ex(ctx, nullptr, nullptr, key, nonce) != 1)
        throw std::runtime_error("key_manager: AES-GCM init failed");

    std::vector<uint8_t> plaintext(ct_len);
    int out_len = 0;

    if (EVP_DecryptUpdate(ctx, plaintext.data(), &out_len,
                          ct, static_cast<int>(ct_len)) != 1)
        throw std::runtime_error("key_manager: DecryptUpdate failed");

    // Set expected authentication tag before finalising
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16,
                             const_cast<uint8_t*>(tag)) != 1)
        throw std::runtime_error("key_manager: set GCM tag failed");

    int final_len = 0;
    if (EVP_DecryptFinal_ex(ctx, plaintext.data() + out_len, &final_len) != 1)
        throw std::runtime_error(
            "key_manager: authentication failed — wrong passphrase or corrupted file");

    plaintext.resize(static_cast<size_t>(out_len + final_len));
    return plaintext;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// KeyManager
// ---------------------------------------------------------------------------

KeyManager::KeyManager() = default;

KeyManager::~KeyManager() { wipe(); }

void KeyManager::load(std::string_view key_file_path) {
    // Read encrypted blob from disk
    std::ifstream f(std::string(key_file_path), std::ios::binary | std::ios::ate);
    if (!f)
        throw std::runtime_error("key_manager: cannot open: " + std::string(key_file_path));

    auto fsize = static_cast<size_t>(f.tellg());
    constexpr size_t HEADER_SIZE = 16 + 12 + 16;  // salt + nonce + tag
    if (fsize < HEADER_SIZE)
        throw std::runtime_error("key_manager: key file too small");

    f.seekg(0);
    std::vector<uint8_t> blob(fsize);
    f.read(reinterpret_cast<char*>(blob.data()), static_cast<std::streamsize>(fsize));
    if (!f)
        throw std::runtime_error("key_manager: key file read error");

    const uint8_t* salt  = blob.data();
    const uint8_t* iv    = salt + 16;
    const uint8_t* tag   = iv   + 12;
    const uint8_t* ct    = tag  + 16;
    size_t         ct_len = fsize - HEADER_SIZE;

    // Prompt for passphrase, derive AES key, then immediately wipe both
    std::string passphrase = read_passphrase_stdin();

    uint8_t aes_key[32];
    pbkdf2_sha256(passphrase, salt, 16, 100'000, aes_key, 32);
    OPENSSL_cleanse(passphrase.data(), passphrase.size());
    passphrase.clear();

    std::vector<uint8_t> plaintext = aes256gcm_decrypt(aes_key, iv, tag, ct, ct_len);
    OPENSSL_cleanse(aes_key, sizeof(aes_key));

    // Parse JSON and extract secrets
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(plaintext.begin(), plaintext.end());
    } catch (const nlohmann::json::exception& e) {
        OPENSSL_cleanse(plaintext.data(), plaintext.size());
        throw std::runtime_error(std::string("key_manager: JSON parse error: ") + e.what());
    }

    credentials_.binance_api_key = j.at("binance_api_key").get<std::string>();
    credentials_.binance_secret  = j.at("binance_secret").get<std::string>();

    std::string privkey_hex = j.at("private_key").get<std::string>();

    // Wipe plaintext before we do anything that can throw
    OPENSSL_cleanse(plaintext.data(), plaintext.size());

    parse_private_key(privkey_hex);
    OPENSSL_cleanse(privkey_hex.data(), privkey_hex.size());

    // Init secp256k1 — SIGN + VERIFY so we can derive the pubkey for the address
    ctx_ = secp256k1_context_create(SECP256K1_CONTEXT_SIGN | SECP256K1_CONTEXT_VERIFY);
    if (!ctx_)
        throw std::runtime_error("key_manager: secp256k1_context_create failed");

    // Randomise context to harden against side-channel attacks
    uint8_t rand_seed[32];
    if (RAND_bytes(rand_seed, 32) != 1)
        throw std::runtime_error("key_manager: RAND_bytes for context seed failed");
    secp256k1_context_randomize(ctx_, rand_seed);
    OPENSSL_cleanse(rand_seed, sizeof(rand_seed));

    if (!secp256k1_ec_seckey_verify(ctx_, privkey_))
        throw std::runtime_error("key_manager: invalid private key");

    derive_address();
    loaded_ = true;
}

std::array<uint8_t, 65> KeyManager::sign(const std::array<uint8_t, 32>& digest) const {
    if (!loaded_)
        throw std::logic_error("key_manager: sign() called before load()");

    secp256k1_ecdsa_recoverable_signature rsig;
    if (!secp256k1_ecdsa_sign_recoverable(
            ctx_, &rsig, digest.data(), privkey_,
            secp256k1_nonce_function_rfc6979, nullptr))
        throw std::runtime_error("key_manager: secp256k1_ecdsa_sign_recoverable failed");

    std::array<uint8_t, 65> out;
    int recid = 0;
    secp256k1_ecdsa_recoverable_signature_serialize_compact(
        ctx_, out.data(), &recid, &rsig);

    // Ethereum EOA: v = recid + 27
    out[64] = static_cast<uint8_t>(recid + 27);
    return out;
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

void KeyManager::parse_private_key(const std::string& hex) {
    auto bytes = hex_decode(hex);
    if (bytes.size() != 32)
        throw std::runtime_error("key_manager: private key must be exactly 32 bytes");

    // Allocate one full page so mlock() covers an aligned region with no false sharing.
    privkey_ = static_cast<uint8_t*>(::aligned_alloc(4096, 4096));
    if (!privkey_)
        throw std::runtime_error("key_manager: aligned_alloc for privkey failed");

    std::memset(privkey_, 0, 4096);
    std::memcpy(privkey_, bytes.data(), 32);
    OPENSSL_cleanse(bytes.data(), bytes.size());

    if (::mlock(privkey_, 4096) == 0) {
        mlocked_ = true;
    } else {
        std::fprintf(stderr,
            "key_manager: WARNING — mlock() failed (errno %d); "
            "key material may be swapped to disk. "
            "Check /proc/self/limits or grant CAP_IPC_LOCK.\n", errno);
    }
}

void KeyManager::derive_address() {
    // Ethereum address = last 20 bytes of keccak256(uncompressed_pubkey[1..64])
    secp256k1_pubkey pubkey;
    if (!secp256k1_ec_pubkey_create(ctx_, &pubkey, privkey_))
        throw std::runtime_error("key_manager: pubkey derivation failed");

    uint8_t pub65[65];
    size_t  pub_len = 65;
    secp256k1_ec_pubkey_serialize(ctx_, pub65, &pub_len, &pubkey,
                                  SECP256K1_EC_UNCOMPRESSED);

    // Hash the 64-byte XY portion (skip the 0x04 prefix byte)
    auto hash = crypto::keccak256(pub65 + 1, 64);

    // Address is the rightmost 20 bytes of the 32-byte hash
    std::copy(hash.begin() + 12, hash.end(),
              credentials_.wallet_address.begin());
}

void KeyManager::wipe() noexcept {
    if (privkey_) {
        OPENSSL_cleanse(privkey_, 4096);
        if (mlocked_) ::munlock(privkey_, 4096);
        ::free(privkey_);
        privkey_ = nullptr;
        mlocked_ = false;
    }
    if (ctx_) {
        secp256k1_context_destroy(ctx_);
        ctx_ = nullptr;
    }
    if (!credentials_.binance_api_key.empty())
        OPENSSL_cleanse(credentials_.binance_api_key.data(),
                        credentials_.binance_api_key.size());
    if (!credentials_.binance_secret.empty())
        OPENSSL_cleanse(credentials_.binance_secret.data(),
                        credentials_.binance_secret.size());
    loaded_ = false;
}

} // namespace wallet
