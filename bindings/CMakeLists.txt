# CMake build target for the trading_core pybind11 extension.
#
# Invoked automatically from the root CMakeLists when pybind11 is found.
# Requires: trading_common interface target + LIBWEBSOCKETS / CURL / nlohmann
# targets already defined by add_subdirectory(core).

pybind11_add_module(trading_core MODULE
    bindings.cpp
    ${CMAKE_SOURCE_DIR}/core/feeds/binance/binance_feed_handler.cpp
    ${CMAKE_SOURCE_DIR}/core/feeds/kraken/kraken_feed_handler.cpp
)

target_include_directories(trading_core PRIVATE
    ${CMAKE_SOURCE_DIR}/core
    ${LIBWEBSOCKETS_INCLUDE_DIRS}
)

target_link_libraries(trading_core PRIVATE
    trading_common
    ${LIBWEBSOCKETS_LIBRARIES}
    CURL::libcurl
    nlohmann_json::nlohmann_json
)

target_compile_options(trading_core PRIVATE ${LIBWEBSOCKETS_CFLAGS_OTHER})

# Install into the wheel root so `pip install` places the .so on sys.path.
install(TARGETS trading_core DESTINATION .)
