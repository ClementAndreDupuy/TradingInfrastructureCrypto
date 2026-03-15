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

# Enable HTTP mock transport in connector code paths for test builds.
# execution target owns the connector translation units that include rest_client.hpp.
target_compile_definitions(execution PUBLIC TRT_ENABLE_HTTP_MOCK_TRANSPORT=1)

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
target_include_directories(orderbook_test PRIVATE
    ${CMAKE_SOURCE_DIR}
)

add_test(NAME OrderBookTest COMMAND orderbook_test)

set_tests_properties(OrderBookTest PROPERTIES
    TIMEOUT 10
    LABELS "unit;orderbook"
)



# Smart order router unit tests
add_executable(smart_order_router_test
    unit/smart_order_router_test.cpp
)

target_link_libraries(smart_order_router_test
    execution
    trading_common
    GTest::gtest
    GTest::gtest_main
)

target_include_directories(smart_order_router_test PRIVATE
    ${CMAKE_SOURCE_DIR}
)

add_test(NAME SmartOrderRouterTest COMMAND smart_order_router_test)
set_tests_properties(SmartOrderRouterTest PROPERTIES
    TIMEOUT 10
    LABELS "unit;execution"
)

# Live connectors contract tests (mock transport)
add_executable(live_connectors_test
    unit/live_connectors_test.cpp
)

target_link_libraries(live_connectors_test
    execution
    trading_common
    GTest::gtest
    GTest::gtest_main
)

target_include_directories(live_connectors_test PRIVATE
    ${CMAKE_SOURCE_DIR}
)

add_test(NAME LiveConnectorsTest COMMAND live_connectors_test)
set_tests_properties(LiveConnectorsTest PROPERTIES
    TIMEOUT 10
    LABELS "unit;execution"
)


# Deterministic connector contract tests (state-machine invariants)
add_executable(connector_contract_test
    unit/connector_contract_test.cpp
)

target_link_libraries(connector_contract_test
    execution
    trading_common
    GTest::gtest
    GTest::gtest_main
)

target_include_directories(connector_contract_test PRIVATE
    ${CMAKE_SOURCE_DIR}
)

add_test(NAME ConnectorContractTest COMMAND connector_contract_test)
set_tests_properties(ConnectorContractTest PROPERTIES
    TIMEOUT 10
    LABELS "unit;execution"
)

# Market maker + risk gate unit tests
add_executable(market_maker_risk_test
    unit/market_maker_risk_test.cpp
)

target_link_libraries(market_maker_risk_test
    execution
    trading_common
    GTest::gtest
    GTest::gtest_main
)

target_include_directories(market_maker_risk_test PRIVATE
    ${CMAKE_SOURCE_DIR}
)

add_test(NAME MarketMakerRiskTest COMMAND market_maker_risk_test)
set_tests_properties(MarketMakerRiskTest PROPERTIES
    TIMEOUT 10
    LABELS "unit;execution;risk"
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


# Performance latency budget gates
add_executable(latency_budget_test
    perf/latency_budget_test.cpp
)

target_link_libraries(latency_budget_test
    binance_feed
    trading_common
    GTest::gtest
    GTest::gtest_main
)

target_include_directories(latency_budget_test PRIVATE
    ${CMAKE_SOURCE_DIR}
)

add_test(NAME LatencyBudgetTest COMMAND latency_budget_test)
set_tests_properties(LatencyBudgetTest PROPERTIES
    TIMEOUT 60
    LABELS "perf"
)


# Replay-based disconnect/reconnect chaos tests per exchange
add_executable(feed_disconnect_chaos_test
    replay/feed_disconnect_chaos_test.cpp
)

target_link_libraries(feed_disconnect_chaos_test
    binance_feed
    kraken_feed
    coinbase_feed
    okx_feed
    trading_common
    GTest::gtest
    GTest::gtest_main
)

target_include_directories(feed_disconnect_chaos_test PRIVATE
    ${CMAKE_SOURCE_DIR}
)

add_test(NAME FeedDisconnectChaosTest COMMAND feed_disconnect_chaos_test)
set_tests_properties(FeedDisconnectChaosTest PROPERTIES
    TIMEOUT 30
    LABELS "replay;feeds;chaos"
)
