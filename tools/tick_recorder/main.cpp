// tick-recorder: standalone market data recording tool.
//
// Connects to the same Binance / Coinbase / Polymarket WebSocket feeds as the
// live bot and writes every normalised Tick to a binary file.  Runs completely
// independently — shares no state with a running polymarket-bot process.
//
// Usage:
//   tick-recorder [--out-dir <dir>] [--token-id <id>]...
//
// Options:
//   --out-dir  <path>    Directory to write tick files (default: ./tick_data/)
//   --token-id <id>      Polymarket YES/NO token ID to subscribe to (repeatable)
//                        Omit entirely to record only Binance + Coinbase.
//
// Behaviour:
//   - Creates <out_dir>/ticks_<UTC_ISO8601>.bin on startup (new file each run).
//   - Writes one 64-byte record per Tick; see format table below.
//   - Logs progress as NDJSON to stderr every 60 s.
//   - Flushes and closes the file cleanly on SIGINT / SIGTERM.
//   - Suitable for continuous operation under systemd (Restart=on-failure).
//     Each restart creates a new file; no existing data is overwritten.
//   - FeedManager logs "ring buffer full — tick dropped" to stderr if the
//     drain thread falls behind.  Watch for those lines when monitoring.
//
// ─────────────────────────────────────────────────────────────────────────────
// BINARY FILE FORMAT  (spec for bt-runner loader)
// ─────────────────────────────────────────────────────────────────────────────
//
// Raw array of fixed-size records; no file header, no framing between records.
//
//   record_count = file_size_bytes / 64
//
// Each record is exactly sizeof(data::Tick) = 64 bytes (verified at compile
// time via static_assert below).  Layout on ARM64 / x86-64 with default ABI:
//
//   Offset  Size  Type     Field            Notes
//   ------  ----  -------  ---------------  -----------------------------------
//        0     8  int64_t  timestamp_us     µs since Unix epoch (local clock,
//                                           offset-corrected via ClockSync EMA)
//        8     1  uint8_t  source           0 = BINANCE
//                                           1 = COINBASE
//                                           2 = POLYMARKET
//        9     7  ---      [padding]        implicit compiler padding; ignore
//       16     8  double   best_bid         IEEE 754 binary64, little-endian
//       24     8  double   best_ask
//       32     8  double   mid
//       40     8  double   ob_imbalance     (bid_qty−ask_qty)/(bid_qty+ask_qty),
//                                           top-5 OB levels; 0 when low_depth
//       48     8  double   last_trade       last matched price; 0 = not a trade
//       56     1  bool     low_depth        1 = OB has <2 bid or <2 ask levels
//       57     7  ---      [padding]        implicit compiler padding; ignore
//   ──────
//       64     total bytes per record
//
// Endianness: little-endian (native ARM64 Graviton3, native x86-64).
//
// Minimal bt-runner loader:
//   FILE* f = fopen(path, "rb");
//   data::Tick t;
//   while (fread(&t, sizeof(t), 1, f) == 1) { /* replay t */ }
//   fclose(f);

#include "src/data/feed_manager.hpp"
#include "src/data/tick.hpp"
#include "src/data/ring_buffer.hpp"

#include <atomic>
#include <chrono>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <thread>
#include <vector>

#include <sys/stat.h>
#include <sys/types.h>

// Compile-time guard: if Tick layout ever changes, this binary format spec
// becomes stale — catch it at build time, not at bt-runner load time.
static_assert(sizeof(data::Tick) == 64,
    "Tick struct is not 64 bytes — update the format table in this file "
    "and the bt-runner loader before rebuilding tick-recorder");

// ---------------------------------------------------------------------------
// Shutdown coordination
// ---------------------------------------------------------------------------
static std::atomic<bool> g_shutdown{false};
static std::atomic<bool> g_feed_done{false};

static void sig_handler(int) {
    g_shutdown.store(true, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// NDJSON logger (no dependency on src/infra/logger)
// ---------------------------------------------------------------------------
static std::string utc_now_iso8601() {
    using namespace std::chrono;
    auto now = system_clock::now();
    auto ms  = duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000;
    std::time_t t = system_clock::to_time_t(now);
    std::tm tm_buf{};
    gmtime_r(&t, &tm_buf);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03lldZ",
        tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
        tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec,
        (long long)ms);
    return buf;
}

static void log_ndjson(const char* level, const char* component, const char* msg) {
    std::fprintf(stderr,
        "{\"ts\":\"%s\",\"level\":\"%s\",\"component\":\"%s\",\"msg\":\"%s\"}\n",
        utc_now_iso8601().c_str(), level, component, msg);
    std::fflush(stderr);
}

// ---------------------------------------------------------------------------
// Path helpers
// ---------------------------------------------------------------------------
static std::string make_output_path(const std::string& out_dir) {
    std::time_t t = std::time(nullptr);
    std::tm tm_buf{};
    gmtime_r(&t, &tm_buf);
    // Colons replaced with hyphens so the filename is portable across filesystems.
    char name[64];
    std::snprintf(name, sizeof(name),
        "ticks_%04d-%02d-%02dT%02d-%02d-%02dZ.bin",
        tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
        tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);
    std::string path = out_dir;
    if (!path.empty() && path.back() != '/') path += '/';
    path += name;
    return path;
}

static bool ensure_dir(const std::string& dir) {
    if (dir.empty() || dir == ".") return true;
    if (::mkdir(dir.c_str(), 0755) == 0 || errno == EEXIST) return true;
    return false;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    std::string out_dir = "./tick_data";
    std::vector<std::string> token_ids;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--out-dir") == 0 && i + 1 < argc) {
            out_dir = argv[++i];
        } else if (std::strcmp(argv[i], "--token-id") == 0 && i + 1 < argc) {
            token_ids.push_back(argv[++i]);
        } else {
            std::fprintf(stderr,
                "Usage: tick-recorder [--out-dir <dir>] [--token-id <id>]...\n");
            return 1;
        }
    }

    std::signal(SIGINT,  sig_handler);
    std::signal(SIGTERM, sig_handler);

    if (!ensure_dir(out_dir)) {
        std::fprintf(stderr,
            "tick-recorder: cannot create output directory '%s': %s\n",
            out_dir.c_str(), std::strerror(errno));
        return 1;
    }

    const std::string out_path = make_output_path(out_dir);
    FILE* out_file = std::fopen(out_path.c_str(), "wb");
    if (!out_file) {
        std::fprintf(stderr,
            "tick-recorder: cannot open '%s': %s\n",
            out_path.c_str(), std::strerror(errno));
        return 1;
    }

    log_ndjson("INFO", "STARTUP",
        ("writing ticks to " + out_path).c_str());
    if (token_ids.empty())
        log_ndjson("INFO", "STARTUP",
            "no --token-id args; Polymarket feed disabled (Binance+Coinbase only)");

    // ---- Shared ring buffer -------------------------------------------------
    data::TickBuffer ring;
    data::FeedManager feed(ring);

    // ---- Per-source counters (updated by drain thread, read by main) --------
    std::atomic<uint64_t> cnt_binance{0};
    std::atomic<uint64_t> cnt_coinbase{0};
    std::atomic<uint64_t> cnt_polymarket{0};
    std::atomic<uint64_t> bytes_written{0};

    // ---- Drain thread -------------------------------------------------------
    // Single consumer of the SPSC ring.  Runs until FeedManager has stopped
    // (g_feed_done) AND the ring is fully empty, guaranteeing no tick is lost
    // to a race between stop() and the final push_tick() calls.
    std::thread drain_thread([&] {
        data::Tick t;
        while (!g_feed_done.load(std::memory_order_acquire) || !ring.empty()) {
            if (ring.pop(t)) {
                std::fwrite(&t, sizeof(t), 1, out_file);
                bytes_written.fetch_add(sizeof(t), std::memory_order_relaxed);
                switch (t.source) {
                    case data::Source::BINANCE:
                        cnt_binance.fetch_add(1, std::memory_order_relaxed);    break;
                    case data::Source::COINBASE:
                        cnt_coinbase.fetch_add(1, std::memory_order_relaxed);   break;
                    case data::Source::POLYMARKET:
                        cnt_polymarket.fetch_add(1, std::memory_order_relaxed); break;
                }
            } else {
                // Ring empty — yield briefly rather than spinning a full core.
                // 50 µs is negligible vs. the slowest feed (1 s mark-price tick).
                std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
        }
        std::fflush(out_file);
        std::fclose(out_file);
    });

    // ---- FeedManager thread -------------------------------------------------
    int feed_exit_code = 0;
    std::thread feed_thread([&] {
        try {
            feed.run(token_ids);
        } catch (const std::exception& e) {
            log_ndjson("ERROR", "FEED", e.what());
            feed_exit_code = 1;
            g_shutdown.store(true, std::memory_order_relaxed);
        }
        // Signal drain thread: no more ticks will be pushed after this point.
        g_feed_done.store(true, std::memory_order_release);
    });

    // ---- Main loop: periodic stats + shutdown watch -------------------------
    using clock = std::chrono::steady_clock;
    const auto start_time  = clock::now();
    auto       last_stats  = clock::now();

    while (!g_shutdown.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::seconds(1));

        auto now       = clock::now();
        double elapsed = std::chrono::duration<double>(now - last_stats).count();
        double uptime  = std::chrono::duration<double>(now - start_time).count();

        if (elapsed >= 60.0) {
            last_stats = now;
            char msg[320];
            std::snprintf(msg, sizeof(msg),
                "{\"ts\":\"%s\",\"level\":\"INFO\",\"component\":\"STATS\","
                "\"ticks_binance\":%llu,"
                "\"ticks_coinbase\":%llu,"
                "\"ticks_polymarket\":%llu,"
                "\"bytes_written\":%llu,"
                "\"uptime_s\":%.0f}",
                utc_now_iso8601().c_str(),
                (unsigned long long)cnt_binance.load(std::memory_order_relaxed),
                (unsigned long long)cnt_coinbase.load(std::memory_order_relaxed),
                (unsigned long long)cnt_polymarket.load(std::memory_order_relaxed),
                (unsigned long long)bytes_written.load(std::memory_order_relaxed),
                uptime);
            std::fprintf(stderr, "%s\n", msg);
            std::fflush(stderr);
        }
    }

    // ---- Graceful shutdown --------------------------------------------------
    log_ndjson("INFO", "SHUTDOWN", "stopping feed");
    feed.stop();
    feed_thread.join();
    // g_feed_done is now set; drain thread will empty the ring and close the file.
    drain_thread.join();

    uint64_t total_ticks = cnt_binance.load()
                         + cnt_coinbase.load()
                         + cnt_polymarket.load();
    char final_msg[256];
    std::snprintf(final_msg, sizeof(final_msg),
        "wrote %llu ticks (%llu bytes) to %s",
        (unsigned long long)total_ticks,
        (unsigned long long)bytes_written.load(),
        out_path.c_str());
    log_ndjson("INFO", "SHUTDOWN", final_msg);

    return feed_exit_code;
}
