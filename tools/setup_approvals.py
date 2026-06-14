#!/usr/bin/env python3
"""
One-time on-chain setup for Polymarket BTC arb bot.

Fires two Polygon transactions (CONTEXT.md §9.1):
  1. USDC.approve(CTF_Exchange, 2**256 - 1)
  2. CTF.setApprovalForAll(neg_risk_adapter, true)

Usage:
  python3 setup_approvals.py --rpc https://polygon-rpc.com

The private key is read from stdin (same security model as the bot).
Verify both transactions on Polygonscan before starting the bot.

Requirements: pip install web3
"""

import argparse
import getpass
import sys
from web3 import Web3

# ---------------------------------------------------------------------------
# Contract addresses (Polygon mainnet) — verified against CONTEXT.md §9.1
# ---------------------------------------------------------------------------
CTF_EXCHANGE     = Web3.to_checksum_address("0x4bFb41d5B3570DeFd03C39a9A4D8dE6Bd8B8982E")
NEG_RISK_ADAPTER = Web3.to_checksum_address("0xd91E80cF2E7be2e162c6513ceD06f1dD0dA35296")
USDC_POLYGON     = Web3.to_checksum_address("0x2791Bca1f2de4661ED88A30C99A7a9449Aa84174")

UINT256_MAX = 2 ** 256 - 1

# Minimal ABIs — only the functions we need
USDC_ABI = [
    {
        "name": "approve",
        "type": "function",
        "inputs": [
            {"name": "spender", "type": "address"},
            {"name": "amount",  "type": "uint256"},
        ],
        "outputs": [{"name": "", "type": "bool"}],
        "stateMutability": "nonpayable",
    },
    {
        "name": "allowance",
        "type": "function",
        "inputs": [
            {"name": "owner",   "type": "address"},
            {"name": "spender", "type": "address"},
        ],
        "outputs": [{"name": "", "type": "uint256"}],
        "stateMutability": "view",
    },
]

CTF_ABI = [
    {
        "name": "setApprovalForAll",
        "type": "function",
        "inputs": [
            {"name": "operator", "type": "address"},
            {"name": "approved", "type": "bool"},
        ],
        "outputs": [],
        "stateMutability": "nonpayable",
    },
    {
        "name": "isApprovedForAll",
        "type": "function",
        "inputs": [
            {"name": "account",  "type": "address"},
            {"name": "operator", "type": "address"},
        ],
        "outputs": [{"name": "", "type": "bool"}],
        "stateMutability": "view",
    },
]


def send_tx(w3, account, contract_fn, description: str) -> str:
    """Build, sign, and broadcast a transaction. Returns tx hash."""
    nonce = w3.eth.get_transaction_count(account.address)
    gas_price = w3.eth.gas_price

    tx = contract_fn.build_transaction({
        "from":     account.address,
        "nonce":    nonce,
        "gasPrice": int(gas_price * 1.1),  # 10% tip to avoid stuck txns
    })
    tx["gas"] = w3.eth.estimate_gas(tx)

    signed = account.sign_transaction(tx)
    tx_hash = w3.eth.send_raw_transaction(signed.rawTransaction)
    print(f"  {description}")
    print(f"  tx hash : {tx_hash.hex()}")

    receipt = w3.eth.wait_for_transaction_receipt(tx_hash, timeout=120)
    if receipt["status"] != 1:
        raise RuntimeError(f"transaction reverted: {tx_hash.hex()}")
    print(f"  confirmed in block {receipt['blockNumber']}")
    return tx_hash.hex()


def main():
    parser = argparse.ArgumentParser(description="Polymarket one-time on-chain approval setup")
    parser.add_argument("--rpc", default="https://polygon-rpc.com",
                        help="Polygon JSON-RPC URL (default: https://polygon-rpc.com)")
    parser.add_argument("--dry-run", action="store_true",
                        help="Check current approvals without submitting transactions")
    args = parser.parse_args()

    w3 = Web3(Web3.HTTPProvider(args.rpc))
    if not w3.is_connected():
        print(f"ERROR: cannot connect to RPC: {args.rpc}", file=sys.stderr)
        sys.exit(1)

    chain_id = w3.eth.chain_id
    print(f"Connected to chain ID {chain_id} (expected 137 for Polygon mainnet)")
    if chain_id != 137:
        print("WARNING: not Polygon mainnet — proceed with caution", file=sys.stderr)

    # Read private key from stdin (never echo it)
    print("Enter private key (hex, no 0x prefix):", end=" ", flush=True)
    raw_key = getpass.getpass(prompt="")
    if raw_key.startswith("0x"):
        raw_key = raw_key[2:]

    account = w3.eth.account.from_key(bytes.fromhex(raw_key))
    del raw_key  # wipe from local variable scope as soon as possible
    print(f"Wallet address: {account.address}")

    usdc = w3.eth.contract(address=USDC_POLYGON,     abi=USDC_ABI)
    ctf  = w3.eth.contract(address=CTF_EXCHANGE,     abi=CTF_ABI)

    # ---- Check current state ----
    allowance = usdc.functions.allowance(account.address, CTF_EXCHANGE).call()
    approved  = ctf.functions.isApprovedForAll(account.address, NEG_RISK_ADAPTER).call()

    print(f"\nCurrent USDC allowance : {allowance}")
    print(f"CTF approved for all   : {approved}")

    if args.dry_run:
        print("\n[dry-run] No transactions submitted.")
        return

    # ---- Transaction 1: USDC approve ----
    if allowance >= UINT256_MAX // 2:
        print("\n[1/2] USDC allowance already at max — skipping")
    else:
        print("\n[1/2] Approving USDC max allowance to CTF Exchange...")
        send_tx(w3, account,
                usdc.functions.approve(CTF_EXCHANGE, UINT256_MAX),
                "USDC.approve(CTF_Exchange, 2**256-1)")

    # ---- Transaction 2: CTF setApprovalForAll ----
    if approved:
        print("[2/2] CTF already approved for neg-risk adapter — skipping")
    else:
        print("[2/2] Setting CTF approval for neg-risk adapter...")
        send_tx(w3, account,
                ctf.functions.setApprovalForAll(NEG_RISK_ADAPTER, True),
                "CTF.setApprovalForAll(neg_risk_adapter, true)")

    print("\nSetup complete. Verify both transactions on Polygonscan before starting the bot.")


if __name__ == "__main__":
    main()
