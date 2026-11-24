cmake_minimum_required(VERSION 3.20)
project(peewee VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

option(PEEWEE_BUILD_TESTS    "Build tests"    ON)
option(PEEWEE_BUILD_EXAMPLES "Build examples" ON)
option(PEEWEE_WITH_NETWORK   "Link Boost.Beast, OpenSSL and simdjson (required for real venues)" ON)

add_library(peewee_warnings INTERFACE)
target_compile_options(peewee_warnings INTERFACE -Wall -Wextra -Wpedantic -Wshadow)

# ---------------------------------------------------------------------------
# Each module is a separate library. Dependencies flow in one direction:
#
#   engine -> algo -> { book, oms } -> core (+ venue interface)
#   venues/* -> core (+ transport, private)
#
# All targets share the include/ directory, so CMake alone cannot stop, say,
# algo from including a Bybit header. The check_layers target below does:
# it runs on every build and fails it if a module breaks these rules
# (rules and details in cmake/check_layers.cmake).
# ---------------------------------------------------------------------------

add_custom_target(check_layers ALL
    COMMAND ${CMAKE_COMMAND} -DROOT=${CMAKE_CURRENT_SOURCE_DIR} -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_layers.cmake
    COMMENT "Checking module dependency rules"
    VERBATIM)

# core + venue interface: header only
add_library(peewee_core INTERFACE)
target_include_directories(peewee_core INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(peewee_core INTERFACE peewee_warnings)

add_library(peewee_book src/book/order_book.cpp)
target_link_libraries(peewee_book PUBLIC peewee_core)

add_library(peewee_oms src/oms/order_manager.cpp)
target_link_libraries(peewee_oms PUBLIC peewee_core)

add_library(peewee_algo src/algo/chase_order.cpp)
target_link_libraries(peewee_algo PUBLIC peewee_book peewee_oms)

add_library(peewee_engine src/engine/engine.cpp)
target_link_libraries(peewee_engine PUBLIC peewee_algo)

# transport: private, visible only to adapters
add_library(peewee_transport src/transport/ws_client.cpp src/transport/http_client.cpp)
target_include_directories(peewee_transport PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(peewee_transport PUBLIC peewee_core)

# venues
add_library(peewee_sim src/venues/sim/sim_venue.cpp)
target_link_libraries(peewee_sim PUBLIC peewee_book)

add_library(peewee_bybit
    src/venues/bybit/bybit_venue.cpp
    src/venues/bybit/bybit_codec.cpp
    src/venues/bybit/bybit_auth.cpp)
target_link_libraries(peewee_bybit PUBLIC peewee_core PRIVATE peewee_transport)

add_library(peewee_hyperliquid
    src/venues/hyperliquid/hl_venue.cpp
    src/venues/hyperliquid/hl_codec.cpp
    src/venues/hyperliquid/hl_signer.cpp)
target_link_libraries(peewee_hyperliquid PUBLIC peewee_core PRIVATE peewee_transport)

# Network dependencies (the Bybit adapter does not build without them):
#   sudo apt install libboost-dev libssl-dev libsimdjson-dev
# (Hyperliquid's keccak256 and msgpack are written in src/venues/hyperliquid,
# ECDSA uses OpenSSL: no extra packages.)
if(PEEWEE_WITH_NETWORK)
    find_package(Boost 1.81 REQUIRED)
    find_package(OpenSSL REQUIRED)
    find_package(simdjson REQUIRED)
    find_package(Threads REQUIRED)
    target_link_libraries(peewee_transport PRIVATE Boost::boost OpenSSL::SSL OpenSSL::Crypto)
    target_link_libraries(peewee_bybit PRIVATE Boost::boost simdjson::simdjson OpenSSL::Crypto Threads::Threads)
    target_link_libraries(peewee_hyperliquid PRIVATE Boost::boost simdjson::simdjson OpenSSL::Crypto Threads::Threads)
    target_compile_definitions(peewee_core INTERFACE PEEWEE_WITH_NETWORK=1)
endif()

# Simple interface on top of everything: Exchange, OrderHandle.
find_package(Threads REQUIRED)
add_library(peewee_client src/client/exchange.cpp)
target_link_libraries(peewee_client PUBLIC peewee_engine peewee_sim peewee_bybit peewee_hyperliquid
                                     PRIVATE Threads::Threads)

# Combined target for library consumers
add_library(peewee INTERFACE)
target_link_libraries(peewee INTERFACE peewee_client peewee_engine peewee_sim peewee_bybit peewee_hyperliquid)
add_library(peewee::peewee ALIAS peewee)

if(PEEWEE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(PEEWEE_BUILD_EXAMPLES)
    add_executable(chase_sim examples/chase_sim.cpp)
    target_link_libraries(chase_sim PRIVATE peewee::peewee)
    add_executable(chase_live examples/chase_live.cpp)
    target_link_libraries(chase_live PRIVATE peewee::peewee)
    add_executable(bybit_book examples/bybit_book.cpp)
    target_link_libraries(bybit_book PRIVATE peewee::peewee)
    add_executable(bybit_order_types_test examples/bybit_order_types_test.cpp)
    target_link_libraries(bybit_order_types_test PRIVATE peewee::peewee)
    add_executable(hl_book examples/hl_book.cpp)
    target_link_libraries(hl_book PRIVATE peewee::peewee)
    add_executable(hl_order_test examples/hl_order_test.cpp)
    target_link_libraries(hl_order_test PRIVATE peewee::peewee)
    add_executable(hl_trigger_test examples/hl_trigger_test.cpp)
    target_link_libraries(hl_trigger_test PRIVATE peewee::peewee)
    add_executable(hl_stop_fill_test examples/hl_stop_fill_test.cpp)
    target_link_libraries(hl_stop_fill_test PRIVATE peewee::peewee)
endif()
