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

# Install targets
install(TARGETS binance_feed kraken_feed
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
