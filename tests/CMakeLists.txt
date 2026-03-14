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

add_test(NAME BinanceFeedTest COMMAND binance_feed_test)

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


# Coinbase feed unit tests
add_executable(coinbase_feed_test
    unit/coinbase_feed_test.cpp
)

target_link_libraries(coinbase_feed_test
    coinbase_feed
    trading_common
    GTest::gtest
    GTest::gtest_main
)

target_include_directories(coinbase_feed_test PRIVATE
    ${CMAKE_SOURCE_DIR}
)

add_test(NAME CoinbaseFeedTest COMMAND coinbase_feed_test)

set_tests_properties(CoinbaseFeedTest PROPERTIES
    TIMEOUT 30
    LABELS "unit;feeds"
)

# OKX feed unit tests
add_executable(okx_feed_test
    unit/okx_feed_test.cpp
)

target_link_libraries(okx_feed_test
    okx_feed
    trading_common
    GTest::gtest
    GTest::gtest_main
)

target_include_directories(okx_feed_test PRIVATE
    ${CMAKE_SOURCE_DIR}
)

add_test(NAME OkxFeedTest COMMAND okx_feed_test)

set_tests_properties(OkxFeedTest PROPERTIES
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


# Replay tests (deterministic book-state regression)
add_executable(feed_replay_test
    replay/feed_replay_test.cpp
)

target_link_libraries(feed_replay_test
    trading_common
    GTest::gtest
    GTest::gtest_main
)

target_include_directories(feed_replay_test PRIVATE
    ${CMAKE_SOURCE_DIR}
)

add_test(NAME FeedReplayTest COMMAND feed_replay_test)
set_tests_properties(FeedReplayTest PROPERTIES
    TIMEOUT 30
    LABELS "replay;feeds"
)

# Integration replay test: feed -> book -> market maker -> shadow
add_executable(full_pipeline_replay_test
    integration/full_pipeline_replay_test.cpp
)

target_link_libraries(full_pipeline_replay_test
    trading_common
    GTest::gtest
    GTest::gtest_main
)

target_include_directories(full_pipeline_replay_test PRIVATE
    ${CMAKE_SOURCE_DIR}
)

add_test(NAME FullPipelineReplayTest COMMAND full_pipeline_replay_test)
set_tests_properties(FullPipelineReplayTest PROPERTIES
    TIMEOUT 30
    LABELS "integration;replay"
)
