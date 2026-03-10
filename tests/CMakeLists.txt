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

add_test(NAME KrakenFeedTest COMMAND kraken_feed_test)

set_tests_properties(KrakenFeedTest PROPERTIES
    TIMEOUT 30
    LABELS "unit;feeds"
)
