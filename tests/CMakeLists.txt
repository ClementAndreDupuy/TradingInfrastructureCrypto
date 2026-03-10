# Tests

# Find GTest (or download if not found)
find_package(GTest)

if(NOT GTest_FOUND)
    message(STATUS "GTest not found, downloading...")
    include(FetchContent)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG release-1.12.1
    )
    FetchContent_MakeAvailable(googletest)
endif()

# Unit tests
add_executable(binance_feed_test
    unit/binance_feed_test.cpp
)

target_link_libraries(binance_feed_test
    binance_feed
    trading_common
    GTest::gtest
    GTest::gtest_main
)

target_include_directories(binance_feed_test PRIVATE
    ${CMAKE_SOURCE_DIR}
)

# Add test to CTest
add_test(NAME BinanceFeedTest COMMAND binance_feed_test)

# Set test properties
set_tests_properties(BinanceFeedTest PROPERTIES
    TIMEOUT 30
    LABELS "unit;feeds"
)

# Kraken feed unit tests
add_executable(kraken_feed_test
    unit/kraken_feed_test.cpp
)

target_link_libraries(kraken_feed_test
    kraken_feed
    trading_common
    GTest::gtest
    GTest::gtest_main
)

target_include_directories(kraken_feed_test PRIVATE
    ${CMAKE_SOURCE_DIR}
)

add_test(NAME KrakenFeedTest COMMAND kraken_feed_test)

set_tests_properties(KrakenFeedTest PROPERTIES
    TIMEOUT 30
    LABELS "unit;feeds"
)

# OrderBook unit tests (pure logic, no network)
add_executable(orderbook_test
    unit/orderbook_test.cpp
)

target_link_libraries(orderbook_test
    trading_common
    GTest::gtest
    GTest::gtest_main
)

add_test(NAME OrderBookTest COMMAND orderbook_test)

set_tests_properties(OrderBookTest PROPERTIES
    TIMEOUT 10
    LABELS "unit;orderbook"
)

# Arbitrage risk manager unit tests (header-only, no extra library needed)
add_executable(arb_risk_manager_test
    unit/arb_risk_manager_test.cpp
)

target_link_libraries(arb_risk_manager_test
    trading_common
    GTest::gtest
    GTest::gtest_main
)

target_include_directories(arb_risk_manager_test PRIVATE
    ${CMAKE_SOURCE_DIR}
)

add_test(NAME ArbRiskManagerTest COMMAND arb_risk_manager_test)

set_tests_properties(ArbRiskManagerTest PROPERTIES
    TIMEOUT 30
    LABELS "unit;risk"
)

# Execution layer unit tests (OrderManager + connector WS parsing, no network)
add_executable(execution_test
    unit/execution_test.cpp
)

target_link_libraries(execution_test
    execution
    trading_common
    GTest::gtest
    GTest::gtest_main
)

target_include_directories(execution_test PRIVATE
    ${CMAKE_SOURCE_DIR}
)

add_test(NAME ExecutionTest COMMAND execution_test)

set_tests_properties(ExecutionTest PROPERTIES
    TIMEOUT 30
    LABELS "unit;execution"
)
