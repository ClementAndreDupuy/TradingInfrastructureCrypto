# Core library components

# Common types library (header-only)
add_library(trading_common INTERFACE)
target_include_directories(trading_common INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# Binance feed handler library
add_library(binance_feed
    feeds/binance/binance_feed_handler.cpp
)

target_link_libraries(binance_feed
    trading_common
)

target_include_directories(binance_feed PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# Example executable to test feed handler
add_executable(binance_feed_example
    feeds/binance/example_main.cpp
)

target_link_libraries(binance_feed_example
    binance_feed
    trading_common
)

# Kraken feed handler library
add_library(kraken_feed
    feeds/kraken/kraken_feed_handler.cpp
)

target_link_libraries(kraken_feed
    trading_common
)

target_include_directories(kraken_feed PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

add_executable(kraken_feed_example
    feeds/kraken/example_main.cpp
)

target_link_libraries(kraken_feed_example
    kraken_feed
    trading_common
)

# Combined Binance + Kraken example with live order books
add_executable(combined_example
    feeds/combined_example.cpp
)

target_link_libraries(combined_example
    binance_feed
    kraken_feed
    trading_common
)

target_include_directories(combined_example PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# Execution layer — order manager + exchange connectors (Binance + Kraken)
add_library(execution
    execution/binance_connector.cpp
    execution/kraken_connector.cpp
)

target_link_libraries(execution
    trading_common
    OpenSSL::Crypto
)

target_include_directories(execution PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# Perp futures arbitrage executable (shadow + live modes)
add_executable(perp_arb
    strategy/perp_arb_main.cpp
    execution/binance_connector.cpp
    execution/kraken_connector.cpp
)

target_link_libraries(perp_arb
    binance_feed
    kraken_feed
    execution
    trading_common
    OpenSSL::Crypto
)

target_include_directories(perp_arb PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# Install targets
install(TARGETS binance_feed kraken_feed execution perp_arb
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)

install(DIRECTORY common/
    DESTINATION include/trading/common
    FILES_MATCHING PATTERN "*.hpp"
)

install(DIRECTORY feeds/binance/
    DESTINATION include/trading/feeds/binance
    FILES_MATCHING PATTERN "*.hpp"
)

install(DIRECTORY feeds/kraken/
    DESTINATION include/trading/feeds/kraken
    FILES_MATCHING PATTERN "*.hpp"
)

install(DIRECTORY execution/
    DESTINATION include/trading/execution
    FILES_MATCHING PATTERN "*.hpp"
)
