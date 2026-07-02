// Backtest harness entry point.  CONTEXT.md §11.
//
// Reads recorded tick files (binary), replays them into the Bayesian engine
// at recorded timestamps, simulates fills with a 5ms latency assumption,
// and outputs a CSV of all simulated trades.
//
// Build: the bt-runner CMake target in CMakeLists.txt links against the same
// signal/ sources as the live bot so the model is never duplicated.
//
// TODO: implement replay loop, fill simulation, PnL tracking.
//       See CONTEXT.md §11 for the full specification.

#include <cstdio>

int main() {
    std::fprintf(stderr, "bt-runner: not yet implemented\n");
    return 1;
}
