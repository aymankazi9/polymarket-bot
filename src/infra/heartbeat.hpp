#pragma once
#include <atomic>
#include <string>

// Writes a heartbeat file every HEARTBEAT_INTERVAL_MS by touching its mtime.
// A separate watchdog process (supervised by systemd) checks this file;
// if the mtime is >2s stale it sends SIGTERM → SIGKILL.
//
// CONTEXT.md §10.1: file path = /tmp/bot.heartbeat, interval = 500ms.

namespace infra {

class Heartbeat {
public:
    explicit Heartbeat(std::string path = "/tmp/bot.heartbeat");

    // Blocks until stop() is called.  Run on a dedicated low-priority thread.
    void run()  noexcept;
    void stop() noexcept;

private:
    std::string       path_;
    std::atomic<bool> stop_flag_{false};
};

} // namespace infra
