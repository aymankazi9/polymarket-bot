#!/usr/bin/env python3
"""
One-time Polymarket wallet setup.
Fires two on-chain approvals required before the bot can trade.
Run this once on a machine with the private key accessible.
Verify both transactions on Polygonscan before starting the bot.

Dependencies: pip install web3 eth-account
"""

# CONTEXT_ADDENDUM A9 — full specification.
# Uses EIP-1559 transaction format (maxFeePerGas / maxPriorityFeePerGas).

from web3 import Web3
from eth_account import Account
import getpass
import sys

# ---------------------------------------------------------------------------
# Configuration — replace <YOUR_KEY> with your Alchemy project key
# ---------------------------------------------------------------------------
ALCHEMY_URL      = "https://polygon-mainnet.g.alchemy.com/v2/<YOUR_KEY>"
CTF_EXCHANGE     = "0x4bFb41d5B3570DeFd03C39a9A4D8dE6Bd8B8982E"
NEG_RISK_ADAPTER = "0xd91E80cF2E7be2e162c6513ceD06f1dD0dA35296"
USDC_POLYGON     = "0x2791Bca1f2de4661ED88A30C99A7a9449Aa84174"
MAX_UINT256      = 2**256 - 1

# Minimal ABIs — only the functions we need
USDC_ABI = [{"name": "approve", "type": "function", "inputs": [
    {"name": "spender", "type": "address"},
    {"name": "amount",  "type": "uint256"}],
    "outputs": [{"name": "", "type": "bool"}], "stateMutability": "nonpayable"}]

CTF_ABI = [{"name": "setApprovalForAll", "type": "function", "inputs": [
    {"name": "operator", "type": "address"},
    {"name": "approved", "type": "bool"}],
    "outputs": [], "stateMutability": "nonpayable"}]


def main():
    w3 = Web3(Web3.HTTPProvider(ALCHEMY_URL))
    assert w3.is_connected(), "Cannot connect to Polygon RPC"

    key = getpass.getpass("Enter private key (hex, with 0x): ")
    account = Account.from_key(key)
    del key  # wipe from local scope immediately
    print(f"Wallet address: {account.address}")
    print(f"MATIC balance:  {w3.from_wei(w3.eth.get_balance(account.address), 'ether')}")

    usdc = w3.eth.contract(address=Web3.to_checksum_address(USDC_POLYGON), abi=USDC_ABI)
    ctf  = w3.eth.contract(address=Web3.to_checksum_address(CTF_EXCHANGE),  abi=CTF_ABI)

    nonce = w3.eth.get_transaction_count(account.address, "pending")

    # ---- TX 1: USDC.approve(CTF_EXCHANGE, MAX_UINT256) ----
    print("\n[1/2] Approving USDC spending by CTF Exchange...")
    tx1 = usdc.functions.approve(
        Web3.to_checksum_address(CTF_EXCHANGE), MAX_UINT256
    ).build_transaction({
        "from":                 account.address,
        "nonce":                nonce,
        "maxFeePerGas":         w3.to_wei("100", "gwei"),
        "maxPriorityFeePerGas": w3.to_wei("30",  "gwei"),
        "chainId":              137,
    })
    signed1 = account.sign_transaction(tx1)
    hash1   = w3.eth.send_raw_transaction(signed1.raw_transaction)
    print(f"TX 1 hash: {hash1.hex()}")
    receipt1 = w3.eth.wait_for_transaction_receipt(hash1, timeout=120)
    assert receipt1.status == 1, "TX 1 FAILED — check Polygonscan"
    print("TX 1 confirmed.")

    # ---- TX 2: CTF.setApprovalForAll(NEG_RISK_ADAPTER, true) ----
    print("\n[2/2] Setting approval for neg-risk adapter...")
    tx2 = ctf.functions.setApprovalForAll(
        Web3.to_checksum_address(NEG_RISK_ADAPTER), True
    ).build_transaction({
        "from":                 account.address,
        "nonce":                nonce + 1,
        "maxFeePerGas":         w3.to_wei("100", "gwei"),
        "maxPriorityFeePerGas": w3.to_wei("30",  "gwei"),
        "chainId":              137,
    })
    signed2 = account.sign_transaction(tx2)
    hash2   = w3.eth.send_raw_transaction(signed2.raw_transaction)
    print(f"TX 2 hash: {hash2.hex()}")
    receipt2 = w3.eth.wait_for_transaction_receipt(hash2, timeout=120)
    assert receipt2.status == 1, "TX 2 FAILED — check Polygonscan"
    print("TX 2 confirmed.")

    print(f"\nSetup complete. Verify on Polygonscan:")
    print(f"  https://polygonscan.com/tx/{hash1.hex()}")
    print(f"  https://polygonscan.com/tx/{hash2.hex()}")
    print("Do not start the bot until both show Status: Success.")


if __name__ == "__main__":
    main()
