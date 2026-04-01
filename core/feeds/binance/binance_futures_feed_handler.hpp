#pragma once

#include "../../common/logging.hpp"
#include "../../common/rest_client.hpp"
#include "../../common/symbol_mapper.hpp"
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace trading {
    class BinanceFuturesFeedHandler {
    public:
        using MarkPriceCallback = std::function<void(double)>;

        explicit BinanceFuturesFeedHandler(const std::string &symbol, const std::string &ws_url);

        ~BinanceFuturesFeedHandler();

        BinanceFuturesFeedHandler(const BinanceFuturesFeedHandler &) = delete;
        BinanceFuturesFeedHandler &operator=(const BinanceFuturesFeedHandler &) = delete;

        void set_mark_price_callback(MarkPriceCallback cb) { mark_price_callback_ = std::move(cb); }

        void start();
        void stop();

        void dispatch_mark_price(double price) {
            if (mark_price_callback_) {
                mark_price_callback_(price);
            }
        }

        bool is_running() const { return running_.load(std::memory_order_acquire); }

    private:
        void ws_event_loop();

        std::string symbol_;
        std::string ws_url_;
        std::string futures_symbol_;

        std::atomic<bool> running_{false};
        std::atomic<void *> lws_ctx_{nullptr};

        std::thread ws_thread_;
        std::mutex ws_mutex_;
        std::condition_variable ws_cv_;

        MarkPriceCallback mark_price_callback_;
    };
}
