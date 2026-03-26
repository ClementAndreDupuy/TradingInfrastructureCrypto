#pragma once

#include "../common/logging.hpp"
#include "../feeds/common/book_manager.hpp"
#include "../ipc/lob_publisher.hpp"

#include <string>

namespace trading::engine {
    constexpr double k_default_fallback_tick_size = 1.0;

    template<typename FeedHandler>
    double refresh_tick_size_for_book_init(FeedHandler &feed, bool enabled, const char *venue_name,
                                           const std::string &symbol,
                                           double fallback_tick_size =
                                                   k_default_fallback_tick_size) {
        if (!enabled) {
            return fallback_tick_size;
        }

        if (feed.refresh_tick_size() != Result::SUCCESS) {
            LOG_WARN("Feed tick size fetch failed before book init", "venue", venue_name, "symbol",
                     symbol.c_str());
        }

        const double tick_size = feed.tick_size();
        return tick_size > 0.0 ? tick_size : fallback_tick_size;
    }

    template<typename FeedHandler>
    void wire_book_bridge_and_callbacks(FeedHandler &feed, BookManager &book,
                                        LobPublisher *publisher) {
        book.set_publisher(publisher);
        feed.set_snapshot_callback(book.snapshot_handler());
        feed.set_delta_callback(book.delta_handler());
        feed.set_trade_callback(book.trade_handler());
    }

    template<typename FeedHandler>
    Result start_feed_after_wiring(FeedHandler &feed, bool enabled, const char *venue_name,
                                   const std::string &symbol) {
        if (!enabled) {
            return Result::SUCCESS;
        }

        const Result start_result = feed.start();
        if (start_result != Result::SUCCESS) {
            LOG_WARN("Feed failed to start", "venue", venue_name, "symbol", symbol.c_str());
        }
        return start_result;
    }
} 
