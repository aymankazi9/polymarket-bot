#!/usr/bin/env python3
"""Encrypt bot secrets into the binary format expected by KeyManager.

Usage:
    python3 tools/encrypt_secrets.py <output.key>

Prompts for:
    1. Ethereum private key (0x-prefixed hex, 64 chars after 0x)
    2. Coinbase CDP API key ID  — flat UUID shown on the key detail page
    3. Coinbase CDP API key secret — base64-encoded raw 64-byte Ed25519 key
    4. Encryption passphrase (hidden, confirmed twice)

Output binary layout (matches KeyManager::load):
    [16 bytes]  PBKDF2-SHA256 salt
    [12 bytes]  AES-256-GCM nonce (IV)
    [16 bytes]  AES-256-GCM authentication tag
    [N  bytes]  AES-256-GCM ciphertext

PBKDF2 parameters: HMAC-SHA256, 100 000 iterations, 32-byte output.

Dependencies:
    pip install cryptography
"""
import base64
import getpass
import json
import os
import sys

try:
    from cryptography.hazmat.primitives.kdf.pbkdf2 import PBKDF2HMAC
    from cryptography.hazmat.primitives import hashes
    from cryptography.hazmat.primitives.ciphers.aead import AESGCM
except ImportError:
    print("ERROR: 'cryptography' package required — run: pip install cryptography",
          file=sys.stderr)
    sys.exit(1)


def main() -> None:
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} <output.key>", file=sys.stderr)
        sys.exit(1)

    output_path = sys.argv[1]

    # ── Ethereum private key ────────────────────────────────────────────────
    priv_key = input("Ethereum private key (0x + 64 hex chars): ").strip()
    if not priv_key.startswith("0x") or len(priv_key) != 66:
        print("ERROR: Must be 0x-prefixed 32-byte hex (total 66 chars).", file=sys.stderr)
        sys.exit(1)
    try:
        bytes.fromhex(priv_key[2:])
    except ValueError:
        print("ERROR: Not valid hex after '0x'.", file=sys.stderr)
        sys.exit(1)

    # ── Coinbase CDP key ID ─────────────────────────────────────────────────
    print()
    print("Coinbase CDP API key ID: the flat identifier shown on the key detail page")
    print("(not an org-scoped path — just the UUID or short identifier).")
    cb_key_id = input("Coinbase CDP API key ID: ").strip()
    if not cb_key_id:
        print("ERROR: Key ID cannot be empty.", file=sys.stderr)
        sys.exit(1)

    # ── Coinbase CDP key secret (raw base64 Ed25519) ────────────────────────
    print()
    print("Coinbase CDP API key secret: the base64-encoded 64-byte Ed25519 key")
    print("shown on the key detail page (seed || pubkey, standard base64, ~88 chars).")
    cb_key_secret = input("Coinbase CDP API key secret: ").strip()
    if not cb_key_secret:
        print("ERROR: Key secret cannot be empty.", file=sys.stderr)
        sys.exit(1)

    # Validate: must decode to at least 32 bytes (seed portion)
    try:
        raw = base64.b64decode(cb_key_secret, validate=True)
    except Exception:
        print("ERROR: Key secret is not valid base64.", file=sys.stderr)
        sys.exit(1)
    if len(raw) not in (32, 64):
        print(f"ERROR: Key secret decoded to {len(raw)} bytes; expected 32 or 64 "
              "(Coinbase Ed25519 keys are 64 bytes: seed + public key).", file=sys.stderr)
        sys.exit(1)
    del raw  # wipe decoded bytes from local scope

    # ── Passphrase ──────────────────────────────────────────────────────────
    print()
    passphrase = getpass.getpass("Encryption passphrase: ")
    if not passphrase:
        print("ERROR: Passphrase cannot be empty.", file=sys.stderr)
        sys.exit(1)
    passphrase2 = getpass.getpass("Confirm passphrase: ")
    if passphrase != passphrase2:
        print("ERROR: Passphrases do not match.", file=sys.stderr)
        sys.exit(1)

    # ── Build JSON plaintext ────────────────────────────────────────────────
    plaintext = json.dumps({
        "private_key":         priv_key,
        "coinbase_key_id":     cb_key_id,
        "coinbase_key_secret": cb_key_secret,
    }, indent=None, separators=(",", ":")).encode("utf-8")

    # ── Derive AES-256 key via PBKDF2-HMAC-SHA256 ───────────────────────────
    salt = os.urandom(16)
    kdf = PBKDF2HMAC(
        algorithm=hashes.SHA256(),
        length=32,
        salt=salt,
        iterations=100_000,
    )
    aes_key = kdf.derive(passphrase.encode("utf-8"))
    del passphrase, passphrase2  # wipe from local scope as soon as possible

    # ── AES-256-GCM encrypt ─────────────────────────────────────────────────
    nonce = os.urandom(12)
    aesgcm = AESGCM(aes_key)
    # AESGCM.encrypt appends 16-byte GCM authentication tag to ciphertext.
    ct_with_tag = aesgcm.encrypt(nonce, plaintext, associated_data=None)
    ciphertext  = ct_with_tag[:-16]
    tag         = ct_with_tag[-16:]

    # ── Write output: salt + nonce + tag + ciphertext ───────────────────────
    with open(output_path, "wb") as f:
        f.write(salt)       # 16 bytes
        f.write(nonce)      # 12 bytes
        f.write(tag)        # 16 bytes
        f.write(ciphertext) # N bytes

    total = 16 + 12 + 16 + len(ciphertext)
    print(f"\nSecrets encrypted to: {output_path}  ({total} bytes)")
    print("Keep this file and your passphrase secure.")
    print("Run the bot with:  polymarket-bot <path/to/output.key>")


if __name__ == "__main__":
    main()
