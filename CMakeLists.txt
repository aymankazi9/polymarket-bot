cmake_minimum_required(VERSION 3.18)
project(polymarket-bot LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# ---------------------------------------------------------------------------
# Compiler flags
# ---------------------------------------------------------------------------
add_compile_options(-Wall -Wextra -Wpedantic)

if(CMAKE_BUILD_TYPE STREQUAL "Release")
    add_compile_options(-O2 -march=native)
elseif(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_options(-O0 -g -fsanitize=address,undefined)
    add_link_options(-fsanitize=address,undefined)
endif()

# ---------------------------------------------------------------------------
# Optional: io_uring backend for Boost.Asio (Linux 5.1+, requires liburing)
# ---------------------------------------------------------------------------
option(USE_IO_URING "Enable Boost.Asio io_uring backend" OFF)
if(USE_IO_URING)
    add_compile_definitions(BOOST_ASIO_HAS_IO_URING BOOST_ASIO_DISABLE_EPOLL)
    find_library(URING_LIB uring REQUIRED)
    message(STATUS "io_uring enabled")
endif()

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------

# OpenSSL (AES-GCM key decryption, PBKDF2, HMAC for CLOB auth and Binance signing)
find_package(OpenSSL REQUIRED)

# Boost — header-only Asio + Beast; link against Boost::system for error_code
find_package(Boost REQUIRED COMPONENTS system)

# libcurl (CLOB REST + Binance REST)
find_package(CURL REQUIRED)

# nlohmann/json
find_package(nlohmann_json 3.9 REQUIRED)

# libsecp256k1 with recovery module
find_library(SECP256K1_LIB secp256k1 REQUIRED
    HINTS /usr/local/lib /usr/lib)
find_path(SECP256K1_INCLUDE_DIR secp256k1_recovery.h
    HINTS /usr/local/include /usr/include
    PATH_SUFFIXES secp256k1)
if(NOT SECP256K1_INCLUDE_DIR)
    message(FATAL_ERROR "secp256k1 headers not found. Install libsecp256k1-dev.")
endif()

# prometheus-cpp (pull model for /metrics endpoint)
find_package(prometheus-cpp CONFIG REQUIRED)

# ---------------------------------------------------------------------------
# Source files
# ---------------------------------------------------------------------------
set(BOT_SOURCES
    src/main.cpp

    # wallet
    src/wallet/key_manager.cpp
    src/wallet/eip712.cpp
    src/wallet/clob_auth.cpp
    src/wallet/nonce_manager.cpp

    # data pipeline
    src/data/clock_sync.cpp
    src/data/feed_manager.cpp

    # signal
    src/signal/likelihood.cpp
    src/signal/regime.cpp
    src/signal/bayesian_engine.cpp

    # execution
    src/execution/clob_client.cpp
    src/execution/binance_client.cpp
    src/execution/position_manager.cpp
    src/execution/taker_arm.cpp
    src/execution/maker_arm.cpp
    src/execution/order_state_machine.cpp

    # risk
    src/risk/watchdog.cpp

    # infra
    src/infra/logger.cpp
    src/infra/metrics.cpp
    src/infra/heartbeat.cpp
)

# ---------------------------------------------------------------------------
# Main bot executable
# ---------------------------------------------------------------------------
add_executable(polymarket-bot ${BOT_SOURCES})

target_include_directories(polymarket-bot PRIVATE
    ${CMAKE_SOURCE_DIR}       # for #include "constants.hpp"
    ${SECP256K1_INCLUDE_DIR}
)

target_link_libraries(polymarket-bot PRIVATE
    OpenSSL::SSL
    OpenSSL::Crypto
    Boost::system
    CURL::libcurl
    nlohmann_json::nlohmann_json
    ${SECP256K1_LIB}
    prometheus-cpp::core
    prometheus-cpp::pull
    pthread
    rt
    $<$<BOOL:${USE_IO_URING}>:${URING_LIB}>
)

# ---------------------------------------------------------------------------
# Backtest runner (links against the same signal/risk logic, no execution)
# ---------------------------------------------------------------------------
add_executable(bt-runner
    tools/bt_runner/main.cpp
    src/signal/likelihood.cpp
    src/signal/regime.cpp
    src/signal/bayesian_engine.cpp
    src/data/clock_sync.cpp
)

target_include_directories(bt-runner PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(bt-runner PRIVATE
    OpenSSL::SSL OpenSSL::Crypto pthread)

# ---------------------------------------------------------------------------
# Install targets
# ---------------------------------------------------------------------------
install(TARGETS polymarket-bot DESTINATION bin)

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
message(STATUS "Build type     : ${CMAKE_BUILD_TYPE}")
message(STATUS "OpenSSL        : ${OPENSSL_VERSION}")
message(STATUS "Boost          : ${Boost_VERSION}")
message(STATUS "secp256k1 lib  : ${SECP256K1_LIB}")
message(STATUS "io_uring       : ${USE_IO_URING}")
