# Core library components

# libwebsockets (required for real WebSocket connections)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBWEBSOCKETS REQUIRED libwebsockets)

# libcurl: prefer system package, fall back to runtime-only link.
# Install: sudo apt install libcurl4-openssl-dev
find_package(CURL QUIET)
if(NOT CURL_FOUND)
    message(STATUS "libcurl dev headers not found; using runtime-only link via curl_abi.hpp")
    add_library(CURL::libcurl INTERFACE IMPORTED)
    target_link_libraries(CURL::libcurl INTERFACE curl)
endif()

# nlohmann/json: prefer system package, fall back to vendored core/common/json.hpp.
# Install: sudo apt install nlohmann-json3-dev
find_package(nlohmann_json QUIET)
if(NOT nlohmann_json_FOUND)
    message(STATUS "nlohmann/json not found; using vendored core/common/json.hpp")
    add_library(nlohmann_json::nlohmann_json INTERFACE IMPORTED)
endif()

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
    ${LIBWEBSOCKETS_LIBRARIES}
    CURL::libcurl
    nlohmann_json::nlohmann_json
)

target_include_directories(binance_feed PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${LIBWEBSOCKETS_INCLUDE_DIRS}
)

target_compile_options(binance_feed PRIVATE ${LIBWEBSOCKETS_CFLAGS_OTHER})

# Kraken feed handler library
add_library(kraken_feed
    feeds/kraken/kraken_feed_handler.cpp
)

target_link_libraries(kraken_feed
    trading_common
    ${LIBWEBSOCKETS_LIBRARIES}
    CURL::libcurl
    nlohmann_json::nlohmann_json
)

target_include_directories(kraken_feed PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${LIBWEBSOCKETS_INCLUDE_DIRS}
)

target_compile_options(kraken_feed PRIVATE ${LIBWEBSOCKETS_CFLAGS_OTHER})

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
