# Polymarket BTC Arbitrage Bot

A low-latency, statistically-driven arbitrage bot for Polymarket BTC above/below 5-minute prediction markets, written in C++17. The bot maintains a live Bayesian probability model and exploits divergences between its model price and the Polymarket CLOB price, with optional directional hedging via Binance USDC-margined perpetual futures.

---

## Table of Contents

- [Project Overview](#project-overview)
- [Architecture & How It Works](#architecture--how-it-works)
- [Core Modules & File Structure](#core-modules--file-structure)
- [Prerequisites & Tech Stack](#prerequisites--tech-stack)
- [Configuration](#configuration)
- [Deployment & Setup](#deployment--setup)
- [Risk & Safety Disclaimer](#risk--safety-disclaimer)

---

## Project Overview

### What it trades

Polymarket's `btc-updown-5m` markets are binary prediction markets that resolve YES or NO based on whether BTC's price is above or below a threshold at the end of a 5-minute window. They trade on Polymarket's Central Limit Order Book (CLOB) — a signed, off-chain order book settled on-chain via Polygon smart contracts. Shares are priced from $0.00 to $1.00 (USDC), with a YES share paying $1.00 if the outcome resolves true.

### Strategy

The bot runs two arms that are mutually exclusive per market:

**Taker arm** — when the bot's model probability diverges from the CLOB mid by more than a minimum edge threshold (`E_min` ≈ 1.37¢/share by default), it crosses the spread immediately with an IOC limit order and simultaneously fires a BTC perpetual futures order on Binance to hedge the directional BTC exposure.

**Maker arm** — when BTC realized volatility is low (SIDEWAYS regime), the bot posts passive bids and asks around its fair-value estimate. Maker orders are not hedged on Binance; profit comes from spread capture and Polymarket USDC maker rebates.

Markets are discovered automatically by polling the Polymarket REST API every 30 seconds — no static market configuration file is required for live operation.

---

## Architecture & How It Works

### Thread model

The process runs four threads with fixed responsibilities:

```
polymarket-bot (process)
│
├── Thread 1 — FeedManager (market data ingestor)
│     Connections: Binance WS (depth20 + aggTrade + markPrice)
│                  Coinbase Advanced Trade WS (BTC-USD ticker)
│                  Polymarket CLOB WS (YES/NO book for each active market)
│     Output:      SPSC lock-free ring buffer of normalised Tick structs
│
├── Thread 2 — BayesianEngine (signal computation, hot path)
│     Input:   ring buffer
│     Computes: P_true via Student's t likelihood update
│               Realized vol → market regime (SIDEWAYS / TRENDING / VOLATILE / SPIKE)
│     Output:  atomic<double> p_true, p_market, btc_mid, realized_vol_60s
│              atomic<MarketRegime> regime
│
├── Thread 3 — OrderStateMachine(s) (one per active market)
│     Input:   SharedState atomics from Thread 2
│     Runs:    per-market state machine: IDLE → TAKER_EVAL → POSITION_OPEN → CLOSING
│              OR:                       IDLE → MAKER_QUOTED → POSITION_OPEN → CLOSING
│     Fires:   EIP-712 signed IOC orders to Polymarket CLOB
│              HMAC-signed orders to Binance USDC perpetual futures
│
└── Thread 4 — RiskWatchdog (SCHED_FIFO priority)
      Monitors: consecutive losses, BTC vol spike, hourly NAV drawdown
      Action:   sets kill_switch atomic; calls emergency_flatten on all OSMs
```

No heap allocation on the hot path. All shared data passes through `std::atomic` fields; the ring buffer is SPSC with no mutex.

### Data flow

```
Binance WS ──┐
Coinbase WS ──┼──► FeedManager ──► SPSCRingBuffer<Tick, 4096> ──► BayesianEngine
Polymarket WS┘         │                                                  │
                        │                                          SharedState atomics
                        └── fault_level[] ──────────────────────►        │
                                                                          ▼
                                                                 OrderStateMachine
                                                                  ├── ClobClient (REST)
                                                                  ├── BinanceClient (REST)
                                                                  ├── TakerArm / MakerArm
                                                                  └── PositionManager
```

### Bayesian signal engine

On each tick, the engine computes:

```
P_posterior = P_prior × L(data | YES) / Z
```

Where `L(data | YES)` is the likelihood of observing the current BTC price and order book given that YES resolves true. The likelihood is a Student's t CDF with `ν = 4.0` degrees of freedom (fat-tail model) and dynamic volatility scaling:

```
scaled_vol = realized_vol_60s × (1 + 0.3 × |ob_imbalance|) × √(t_remaining / 300s)
d          = (btc_price - strike) / (strike × scaled_vol)
L(YES)     = t_CDF(d, ν)
```

The prior resets to the live Polymarket CLOB mid every 30 seconds to prevent drift. Auxiliary log-likelihood adjustments (capped at ±10% per signal) are applied from Binance funding rate, open-interest delta, order book imbalance, and Coinbase cross-venue delta.

### Market auto-discovery and lifecycle

A background `MarketScanner` thread polls `GET /markets?tag=bitcoin&active=true` every 30 seconds and auto-discovers `btc-updown-5m` markets. As markets approach resolution, sequenced lifecycle callbacks fire to the relevant OSMs:

| Deadline | Action |
|---|---|
| T−120 s | Stop all new taker entries |
| T−45 s  | Cancel open maker quotes |
| T−30 s  | Close taker positions with limit IOC |
| T−10 s  | Forced market-order close |
| T+60 s  | Remove market from active set |

Internal time-checks inside each OSM replicate these deadlines as a fallback, so expiry handling is correct even between scanner poll intervals.

### Position sizing

Quarter-Kelly with hard caps:

```
f* = (edge / odds) × 0.25
```

Hard caps (override Kelly if Kelly would exceed them):

| Parameter | Value |
|---|---|
| `MAX_TRADE_USDC` | $200 per trade |
| `MAX_TOTAL_EXPOSURE_USDC` | $400 across all open positions |
| `MAX_MAKER_QUOTE_USDC` | $100 per maker quote side |
| `MAX_HEDGE_FRACTION` | 5% of bankroll per Binance hedge |
| `MAX_OPEN_POSITIONS` | 3 simultaneously |

### Circuit breakers

Thread 4 independently monitors three conditions. Any single trip sets `kill_switch`, triggers emergency flattening of all positions, and halts all new order submission. **Manual operator reset is required** — the bot does not auto-restart after a circuit breaker trip.

| Condition | Threshold | Window |
|---|---|---|
| Consecutive losses | 4 losses | 15 minutes |
| BTC volatility spike | >1.5% move | 60 seconds |
| NAV drawdown | >8% of bankroll | 1 hour |

### Feed fault tiers

| Gap since last tick | Action |
|---|---|
| 0–2 s | DEGRADED — widen maker spread 2×, stop new taker entries |
| 2–5 s | REST FALLBACK — poll REST API every 500 ms, no new entries |
| >5 s  | EMERGENCY FLATTEN — TWAP exit over 30 s (5 slices × 6 s) |
| Both CEX feeds dead >5 s | Halt all markets, page operator — no blind flattening |

---

## Core Modules & File Structure

```
polymarket-bot/
├── CMakeLists.txt                  # Build system; produces polymarket-bot and bt-runner
├── constants.hpp                   # Every trading threshold as constexpr — single source of truth
│
├── src/
│   ├── main.cpp                    # Startup: key load, thread launch, OSM sync loop, shutdown
│   │
│   ├── types/
│   │   └── amount.hpp              # Strong monetary type (int64 micro-units, 6 dp); bans raw double for money
│   │
│   ├── data/
│   │   ├── tick.hpp                # Normalised Tick struct (timestamp, source, bid/ask, ob_imbalance, low_depth)
│   │   ├── ring_buffer.hpp         # Lock-free SPSC ring buffer (SPSCRingBuffer<T, N>)
│   │   ├── clock_sync.{hpp,cpp}    # Measures offset between local clock and each exchange's server timestamp
│   │   ├── feed_manager.{hpp,cpp}  # Manages Binance + Coinbase + Polymarket WS connections; writes ring buffer
│   │   └── market_scanner.{hpp,cpp}# Polls /markets every 30s; fires lifecycle callbacks (T-120s...T-10s)
│   │
│   ├── signal/
│   │   ├── shared_state.hpp        # All cross-thread atomics: p_true, regime, bankroll, kill_switch, etc.
│   │   ├── likelihood.{hpp,cpp}    # Student's t likelihood + 2D calibration lookup table
│   │   ├── regime.{hpp,cpp}        # Rolling 60s realized vol → MarketRegime enum
│   │   └── bayesian_engine.{hpp,cpp} # Thread 2 main loop: consumes ring, updates SharedState
│   │
│   ├── execution/
│   │   ├── order_state_machine.{hpp,cpp} # Per-market FSM; drives taker/maker arms; lifecycle flags
│   │   ├── taker_arm.{hpp,cpp}     # Fires simultaneous IOC orders on CLOB + Binance; Kelly sizing
│   │   ├── maker_arm.{hpp,cpp}     # Posts/reprices/cancels passive maker quotes; 1500ms max age
│   │   ├── position_manager.{hpp,cpp} # Hard stop, trailing stop, early exit evaluation per tick
│   │   ├── clob_client.{hpp,cpp}   # Polymarket CLOB REST: submit_order, get_status, cancel
│   │   └── binance_client.{hpp,cpp}# Binance USDC perp REST (fapi.binance.com): submit_order, cancel
│   │
│   ├── wallet/
│   │   ├── key_manager.{hpp,cpp}   # AES-256-GCM decrypt, mlock, secp256k1 ctx init, explicit_bzero
│   │   ├── eip712.{hpp,cpp}        # EIP-712 domain separator + Order struct hashing (keccak256)
│   │   ├── clob_auth.{hpp,cpp}     # Derives L2 key, signs CLOB auth message, manages session headers
│   │   └── nonce_manager.{hpp,cpp} # Atomic order nonce; seeded from eth_getTransactionCount at startup
│   │
│   ├── risk/
│   │   ├── watchdog.{hpp,cpp}      # Thread 4: consecutive-loss / BTC-spike / NAV-drawdown circuit breakers
│   │   └── sizing.hpp              # kelly_size_usdc() + hedge_btc_qty() — pure functions, no state
│   │
│   ├── crypto/
│   │   └── keccak256.hpp           # Inline keccak256 implementation (no external dependency)
│   │
│   └── infra/
│       ├── logger.{hpp,cpp}        # Structured NDJSON logs to stdout; never logs key material
│       ├── metrics.{hpp,cpp}       # Prometheus pull endpoint on :9090 via prometheus-cpp
│       ├── heartbeat.{hpp,cpp}     # Touches /tmp/bot.heartbeat every 500 ms for systemd watchdog
│       └── rpc_client.{hpp,cpp}    # JSON-RPC 2.0 client; Alchemy primary → polygon-rpc.com fallback
│
├── tools/
│   ├── setup_approvals.py          # One-time Python script: USDC.approve + CTF.setApprovalForAll
│   └── bt_runner/
│       └── main.cpp                # Backtest harness binary; replays tick files through signal engine
│
└── config/
    └── markets.json                # Reference example of market fields (superseded by auto-discovery)
```

### Key design decisions

- **`Amount` type** — all USDC values are stored as `int64_t` micro-units (1 USDC = 1,000,000 units). Raw `double` is banned for monetary values to prevent rounding error accumulation.
- **`SharedState`** — all inter-thread data is `std::atomic`; no mutex on the hot path.
- **Compile-time constants** — every risk threshold and trading parameter lives in `constants.hpp` as `constexpr`. There are no runtime overrides and no environment variable fallbacks.
- **`libsecp256k1`** — used exclusively for EIP-712 order signing. OpenSSL is used only for AES-GCM key decryption and HMAC (Binance authentication).

---

## Prerequisites & Tech Stack

### Runtime environment

| Requirement | Version | Notes |
|---|---|---|
| OS | Ubuntu 22.04+ (Linux) | Must have `mlock()` available |
| Architecture | ARM64 (AWS t4g.medium, Graviton3) | Release build uses `-march=armv8.2-a+crypto` |
| Compiler | GCC 12+ or Clang 15+ | C++17 required |
| CMake | 3.18+ | |
| Python | 3.9+ | For `setup_approvals.py` only |

### C++ library dependencies

| Library | Purpose | Install |
|---|---|---|
| OpenSSL >= 3.0 | AES-256-GCM key decryption, PBKDF2, HMAC (Binance) | `apt install libssl-dev` |
| Boost >= 1.78 | Asio (WebSockets), Beast, system | `apt install libboost-all-dev` |
| libcurl | CLOB REST + Binance REST + market scanner | `apt install libcurl4-openssl-dev` |
| nlohmann/json >= 3.9 | JSON serialisation for all API payloads | `apt install nlohmann-json3-dev` |
| libsecp256k1 (with recovery) | EIP-712 order signing | See build notes below |
| prometheus-cpp | Prometheus `/metrics` pull endpoint | See build notes below |

**libsecp256k1** must be built with the recovery module enabled:

```bash
git clone https://github.com/bitcoin-core/secp256k1.git
cd secp256k1
./autogen.sh && ./configure --enable-module-recovery
make && sudo make install
```

**prometheus-cpp**:

```bash
git clone https://github.com/jupp0r/prometheus-cpp.git
cd prometheus-cpp && mkdir build && cd build
cmake .. -DENABLE_PUSH=OFF -DENABLE_COMPRESSION=OFF
make -j$(nproc) && sudo make install
```

### Blockchain infrastructure

| Component | Details |
|---|---|
| Network | Polygon mainnet (chain ID 137) |
| RPC (primary) | Alchemy — `https://polygon-mainnet.g.alchemy.com/v2/<YOUR_KEY>` |
| RPC (fallback) | Public `https://polygon-rpc.com` (rate-limited; backup only) |
| Wallet | Ethereum-compatible EOA with MATIC for gas |
| USDC | Polygon-native USDC — `0x2791Bca1f2de4661ED88A30C99A7a9449Aa84174` |
| CEX hedge | Binance USDC-margined perpetual futures (`BTCUSDC`). **Not** BTCUSDT. |

### Connectivity requirements

The bot must have low-latency access to:

- `fstream.binance.com:443` — Binance futures WebSocket
- `advanced-trade-ws.coinbase.com:443` — Coinbase WebSocket
- `ws-subscriptions-clob.polymarket.com:443` — Polymarket CLOB WebSocket
- `clob.polymarket.com` — Polymarket CLOB REST
- `fapi.binance.com` — Binance futures REST

---

## Configuration

### Secrets file (encrypted)

All secrets are stored in a single AES-256-GCM encrypted file. The bot prompts for the decryption passphrase on stdin at startup. **Secrets are never read from environment variables or config files at runtime.**

The plaintext (JSON before encryption) must contain:

```json
{
  "private_key":     "0x<64 hex chars>",
  "binance_api_key": "<your Binance API key>",
  "binance_secret":  "<your Binance API secret>"
}
```

Encrypted file binary layout (no framing):

```
[16 bytes]  PBKDF2-SHA256 salt
[12 bytes]  AES-256-GCM nonce
[16 bytes]  AES-256-GCM authentication tag
[N  bytes]  ciphertext (JSON above)
```

PBKDF2 parameters: HMAC-SHA256, 100,000 iterations, 32-byte output.

To create the encrypted key file, use a trusted offline script (not provided here — implement with OpenSSL or `age`). Store the passphrase in a password manager; do not write it to disk.

### Compile-time constants (`constants.hpp`)

All trading parameters are `constexpr` in [`constants.hpp`](constants.hpp). Key values to review before deployment:

| Constant | Default | Description |
|---|---|---|
| `RPC_PRIMARY` | `...g.alchemy.com/v2/<YOUR_KEY>` | **Replace `<YOUR_KEY>` before building** |
| `KELLY_FRACTION` | `0.25` | Quarter-Kelly; only raise after 200+ live trades |
| `MAX_TRADE_USDC` | `$200` | Hard cap per taker trade |
| `MAX_TOTAL_EXPOSURE_USDC` | `$400` | Total open exposure across all markets |
| `MAX_MAKER_QUOTE_USDC` | `$100` | Per-side maker quote size |
| `ALPHA_BUFFER_CENTS` | `0.75¢` | Target net profit per share; tune in backtest (0.5¢–1.5¢) |
| `CB_MAX_CONSECUTIVE_LOSSES` | `4` | Circuit breaker: losing streak length |
| `CB_NAV_DRAWDOWN_THRESHOLD` | `8%` | Circuit breaker: hourly NAV drawdown |
| `ramp::INITIAL_MAX_TRADE_USDC` | `$20` | Go-live ramp: starting trade cap |
| `ramp::INITIAL_MAX_TOTAL_EXPOSURE_USDC` | `$40` | Go-live ramp: starting exposure cap |

> **Go-live ramp**: Deploy with the `ramp::` values. Review every trade against the model's predicted edge for 48 hours, then double the caps. Repeat until reaching full caps.

### Alchemy API key

Replace `<YOUR_KEY>` in `constants.hpp` with your Alchemy Polygon project key before building:

```cpp
// constants.hpp
inline constexpr char RPC_PRIMARY[] =
    "https://polygon-mainnet.g.alchemy.com/v2/YOUR_ACTUAL_KEY_HERE";
```

### Prometheus metrics

The bot exposes a Prometheus pull endpoint on `localhost:9090`. Recommended Grafana panels:

| Metric | Type | Description |
|---|---|---|
| `bot_pnl_usdc` | gauge | Cumulative PnL |
| `bot_open_positions` | gauge | Number of open positions |
| `bot_order_latency_us` | histogram | Order fire-to-fill latency (p50, p99) |
| `bot_feed_lag_us{source}` | gauge | Per-feed lag (BINANCE / COINBASE / POLYMARKET) |
| `bot_circuit_breaker_trips_total` | counter | Circuit breaker event count |

---

## Deployment & Setup

### 1. One-time on-chain approvals

Before the bot can submit orders, two Polygon transactions must be confirmed. Run the setup script once from any machine with the wallet's private key accessible:

```bash
pip install web3 eth-account
python3 tools/setup_approvals.py
```

The script will:
1. Prompt for the private key (input is hidden)
2. Submit `USDC.approve(CTF_Exchange, 2^256 - 1)`
3. Submit `CTF.setApprovalForAll(NegRiskAdapter, true)`

**Verify both transactions on Polygonscan before proceeding.**

```
CTF Exchange:     0x4bFb41d5B3570DeFd03C39a9A4D8dE6Bd8B8982E
Neg-Risk Adapter: 0xd91E80cF2E7be2e162c6513ceD06f1dD0dA35296
USDC (Polygon):   0x2791Bca1f2de4661ED88A30C99A7a9449Aa84174
```

### 2. Install system dependencies

```bash
sudo apt update
sudo apt install -y \
    build-essential cmake git \
    libssl-dev libboost-all-dev libcurl4-openssl-dev \
    nlohmann-json3-dev

# libsecp256k1 (with recovery module)
git clone https://github.com/bitcoin-core/secp256k1.git /tmp/secp256k1
cd /tmp/secp256k1
./autogen.sh && ./configure --enable-module-recovery
make -j$(nproc) && sudo make install

# prometheus-cpp
git clone https://github.com/jupp0r/prometheus-cpp.git /tmp/prometheus-cpp
cd /tmp/prometheus-cpp && mkdir build && cd build
cmake .. -DENABLE_PUSH=OFF -DENABLE_COMPRESSION=OFF -DCMAKE_BUILD_TYPE=Release
make -j$(nproc) && sudo make install
```

### 3. Build

```bash
git clone <repo-url> polymarket-bot
cd polymarket-bot

# Replace YOUR_ACTUAL_KEY_HERE in constants.hpp with your Alchemy key
sed -i 's/<YOUR_KEY>/YOUR_ACTUAL_KEY_HERE/' constants.hpp

mkdir build && cd build

# Release build targeting ARM64 Graviton3
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)

# Debug build with AddressSanitizer + UBSan (development only)
cmake .. -DCMAKE_BUILD_TYPE=Debug
make -j$(nproc)

# Optional: io_uring backend for lower-latency WS I/O (Linux kernel 5.1+)
cmake .. -DCMAKE_BUILD_TYPE=Release -DUSE_IO_URING=ON
make -j$(nproc)
```

Produced binaries:
- `build/polymarket-bot` — the live trading bot
- `build/bt-runner` — the backtest harness

### 4. Create the encrypted key file

Produce an AES-256-GCM encrypted secrets file from the JSON plaintext using OpenSSL or a trusted offline tool. Store it at a restricted path:

```bash
chmod 600 /etc/polymarket-bot/secrets.enc
chown botuser:botuser /etc/polymarket-bot/secrets.enc
```

The file is never read after the bot starts; the passphrase is prompted once at startup and immediately discarded after the secp256k1 signing context is initialised.

### 5. Run locally (manual)

```bash
./build/polymarket-bot /path/to/secrets.enc
# Prompt: "Passphrase: " — enter interactively, input is not echoed
```

On startup the bot will:
1. Decrypt the key file, derive the wallet address, and wipe the raw key bytes
2. Authenticate with the Polymarket CLOB (L2 key derivation + EIP-712 session auth)
3. Sync the order nonce from `eth_getTransactionCount("pending")`
4. Run an initial synchronous market discovery scan
5. Launch all threads and begin evaluating ticks

Logs are NDJSON to stdout:

```bash
./build/polymarket-bot /path/to/secrets.enc 2>&1 | jq .
```

### 6. Backtest before go-live

> **Do not go live without completing this step.**

Record at least 2 weeks of tick data from Binance and Polymarket, then replay through the signal engine:

```bash
./build/bt-runner --ticks /path/to/tick_data/ --output trades.csv
```

Go-live requirements:
- Backtest Sharpe ratio > 1.5 on **out-of-sample** data
- 5 days of paper trading (signals computed live, orders logged but not submitted)
- Fill rate and edge capture consistent with backtest expectations

### 7. Deploy as a systemd service

Create `/etc/systemd/system/polymarket-bot.service`:

```ini
[Unit]
Description=Polymarket BTC Arbitrage Bot
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=botuser
ExecStart=/usr/local/bin/polymarket-bot /etc/polymarket-bot/secrets.enc
StandardInput=null
StandardOutput=journal
StandardError=journal
Restart=on-failure
RestartSec=5s
LimitNOFILE=65536
LimitMEMLOCK=infinity

[Install]
WantedBy=multi-user.target
```

> **Passphrase at startup**: The bot reads the passphrase from stdin. For unattended operation, use `systemd-creds` or a secrets manager to inject it — do not hardcode it in the unit file or in any environment variable.

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now polymarket-bot
```

#### Heartbeat watchdog (recommended)

The bot writes `/tmp/bot.heartbeat` every 500 ms. A companion watchdog process (also under systemd with `Restart=always`) checks the mtime and sends `SIGTERM` → `SIGKILL` if it goes stale beyond 2 seconds. Systemd then restarts the bot. The watchdog itself does not restart the bot after a **circuit breaker trip** — that requires manual operator review.

### 8. Monitor

```bash
# Live structured log stream
journalctl -fu polymarket-bot | jq .

# Circuit breaker state
curl -s http://localhost:9090/metrics | grep bot_circuit_breaker

# Current PnL
curl -s http://localhost:9090/metrics | grep bot_pnl_usdc

# Feed lag per source
curl -s http://localhost:9090/metrics | grep bot_feed_lag_us
```

#### After a circuit breaker trip

The bot halts all trading and waits indefinitely. After reviewing the triggering condition in logs:

```bash
# Review the trip reason
journalctl -u polymarket-bot | grep circuit_breaker | jq .

# Restart once satisfied
sudo systemctl restart polymarket-bot
```

---

## Risk & Safety Disclaimer

**This software is provided for educational and research purposes. Trading prediction markets involves significant financial risk.**

- **Capital at risk**: All funds deposited are at risk of partial or total loss. Binary markets can move to zero in seconds.
- **Model risk**: The Bayesian signal model relies on historical assumptions about BTC price distributions. It may not generalise to future market conditions, regime changes, or liquidity crises.
- **Execution risk**: Slippage, order rejection, network latency, and API outages can cause the bot to deviate materially from its intended strategy. Legged positions (one side filled, the other rejected) are handled but not guaranteed to unwind at break-even.
- **Smart contract risk**: The bot interacts with Polymarket's CTF Exchange and Neg-Risk Adapter contracts on Polygon mainnet. Smart contract bugs, Polygon network congestion, or sequencer downtime could result in stuck or lost funds.
- **No financial advice**: Nothing in this repository constitutes financial or investment advice. Use entirely at your own risk.
- **Regulatory compliance**: It is your sole responsibility to ensure that operating an automated trading bot on prediction markets complies with all applicable laws and regulations in your jurisdiction.
- **Operational risk**: A misconfigured key file, incorrect on-chain approvals, a missed circuit breaker reset, or a server outage during an open position can cause unexpected financial exposure.

**Pre-flight checklist before any live deployment:**

- [ ] Both on-chain approvals confirmed on Polygonscan
- [ ] `<YOUR_KEY>` replaced in `constants.hpp` with a valid Alchemy project key
- [ ] Encrypted key file created, ownership/permissions restricted (`chmod 600`)
- [ ] Backtest Sharpe > 1.5 on out-of-sample data
- [ ] 5 days of paper trading completed without unexpected behaviour
- [ ] Prometheus dashboard live with alerting on `bot_circuit_breaker_trips_total`
- [ ] Wallet funded with **only the amount you are prepared to lose entirely**
- [ ] Starting with ramp limits (`$20` / `$40`) — not full caps
