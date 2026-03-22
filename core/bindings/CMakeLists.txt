# CMake build target for the trading_core pybind11 extension.
#
# Invoked automatically from the root CMakeLists when pybind11 is found.
# Requires: trading_common interface target + LIBWEBSOCKETS / CURL / nlohmann
# targets already defined by add_subdirectory(core).

find_package(PkgConfig REQUIRED)
pkg_check_modules(BINDINGS_LIBWEBSOCKETS REQUIRED libwebsockets)
find_package(CURL QUIET)
find_package(OpenSSL QUIET)

pybind11_add_module(trading_core MODULE
    bindings.cpp
    ${CMAKE_SOURCE_DIR}/core/feeds/binance/binance_feed_handler.cpp
    ${CMAKE_SOURCE_DIR}/core/feeds/coinbase/coinbase_feed_handler.cpp
    ${CMAKE_SOURCE_DIR}/core/feeds/kraken/kraken_feed_handler.cpp
    ${CMAKE_SOURCE_DIR}/core/feeds/okx/okx_feed_handler.cpp
)

target_include_directories(trading_core PRIVATE
    ${CMAKE_SOURCE_DIR}/core
    ${BINDINGS_LIBWEBSOCKETS_INCLUDE_DIRS}
    ${CURL_INCLUDE_DIRS}
)
target_link_directories(trading_core PRIVATE
    ${BINDINGS_LIBWEBSOCKETS_LIBRARY_DIRS}
)

target_link_libraries(trading_core PRIVATE
    trading_common
    ${BINDINGS_LIBWEBSOCKETS_LIBRARIES}
)
if(CURL_FOUND)
    target_link_libraries(trading_core PRIVATE ${CURL_LIBRARIES})
else()
    target_link_libraries(trading_core PRIVATE curl)
endif()
if(OpenSSL_FOUND)
    target_link_libraries(trading_core PRIVATE OpenSSL::SSL OpenSSL::Crypto)
endif()
target_compile_options(trading_core PRIVATE ${BINDINGS_LIBWEBSOCKETS_CFLAGS_OTHER})

# Install into the wheel root so `pip install` places the .so on sys.path.
install(TARGETS trading_core DESTINATION .)
