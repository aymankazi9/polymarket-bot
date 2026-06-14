#include "heartbeat.hpp"
#include "../../constants.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

#include <fcntl.h>
#include <sys/stat.h>
#include <utime.h>

namespace infra {

Heartbeat::Heartbeat(std::string path) : path_(std::move(path)) {}

void Heartbeat::run() noexcept {
    using namespace std::chrono;
    // Create the file if it doesn't exist
    int fd = ::open(path_.c_str(), O_CREAT | O_WRONLY, 0644);
    if (fd >= 0) ::close(fd);

    while (!stop_flag_.load(std::memory_order_relaxed)) {
        // Touch mtime
        if (::utime(path_.c_str(), nullptr) != 0)
            std::fprintf(stderr, "heartbeat: utime(%s) failed\n", path_.c_str());

        std::this_thread::sleep_for(
            milliseconds(static_cast<int>(constants::HEARTBEAT_INTERVAL_MS)));
    }
}

void Heartbeat::stop() noexcept {
    stop_flag_.store(true, std::memory_order_relaxed);
}

} // namespace infra
