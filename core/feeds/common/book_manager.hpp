#pragma once

#include "../../common/types.hpp"
#include "../../ipc/lob_publisher.hpp"
#include "../../orderbook/orderbook.hpp"
#include <atomic>
#include <chrono>
#include <functional>
#include <cstdint>
#include <vector>

namespace trading {
    class BookManager {
    public:
        using TradeCallback = std::function<void(const TradeFlow &)>;

        BookManager(const std::string &symbol, Exchange exchange, double tick_size = 1.0,
                    size_t max_levels = 20000)
            : book_(symbol, exchange, tick_size, max_levels), last_update_steady_ns_(0),
              publisher_(nullptr), last_publish_local_ts_ns_(0), last_publish_exchange_ts_ns_(0),
              timestamp_issue_count_(0), trade_window_start_ns_(0), trade_window_volume_(0.0) {
            pub_bids_.reserve(10);
            pub_asks_.reserve(10);
        }

        void set_publisher(LobPublisher *publisher) noexcept { publisher_ = publisher; }

        std::function<void(const Snapshot &)> snapshot_handler() {
            return [this](const Snapshot &s) {
                auto result = book_.apply_snapshot(s);
                if (result != Result::SUCCESS) {
                    LOG_ERROR("BookManager: snapshot apply failed", "symbol", book_.symbol().c_str(),
                              "exchange", exchange_to_string(book_.exchange()));
                } else {
                    const int64_t wall_ts_ns =
                            (s.timestamp_local_ns > 0) ? s.timestamp_local_ns : wall_now_ns();
                    inspect_timestamp("snapshot", wall_ts_ns, s.timestamp_exchange_ns);
                    last_update_steady_ns_.store(steady_now_ns(), std::memory_order_release);
                    publish_lob(wall_ts_ns);
                }
            };
        }

        std::function<void(const Delta &)> delta_handler() {
            return [this](const Delta &d) {
                auto result = book_.apply_delta(d);
                if (result == Result::ERROR_INVALID_PRICE) {
                    LOG_WARN("BookManager: delta out of grid range — re-snapshot needed", "symbol",
                             book_.symbol().c_str(), "price", d.price);
                } else if (result == Result::SUCCESS) {
                    const int64_t wall_ts_ns =
                            (d.timestamp_local_ns > 0) ? d.timestamp_local_ns : wall_now_ns();
                    inspect_timestamp("delta", wall_ts_ns, d.timestamp_exchange_ns);
                    last_update_steady_ns_.store(steady_now_ns(), std::memory_order_release);
                    publish_lob(wall_ts_ns);
                }
            };
        }

        TradeCallback trade_handler() {
            return [this](const TradeFlow &trade) {
                const int64_t now_ns = wall_now_ns();
                if (trade_window_start_ns_ == 0 || now_ns - trade_window_start_ns_ >= 1'000'000'000LL) {
                    trade_window_start_ns_ = now_ns;
                    trade_window_volume_ = 0.0;
                }
                trade_window_volume_ += trade.last_trade_size;
                last_trade_flow_ = trade;
                last_trade_flow_.recent_traded_volume = trade_window_volume_;
            };
        }

        double best_bid() const { return book_.get_best_bid(); }
        double best_ask() const { return book_.get_best_ask(); }
        double mid_price() const { return book_.get_mid_price(); }
        double spread() const { return book_.get_spread(); }
        bool is_ready() const { return book_.is_initialized(); }

        int64_t age_ms() const noexcept {
            int64_t ts = last_update_steady_ns_.load(std::memory_order_acquire);
            if (ts == 0)
                return INT64_MAX;
            return (steady_now_ns() - ts) / 1'000'000LL;
        }

        void get_top_levels(size_t n, std::vector<PriceLevel> &bids,
                            std::vector<PriceLevel> &asks) const {
            book_.get_top_levels(n, bids, asks);
        }

        const OrderBook &book() const { return book_; }

    private:
        OrderBook book_;
        std::atomic<int64_t> last_update_steady_ns_;
        LobPublisher *publisher_;
        std::atomic<int64_t> last_publish_local_ts_ns_;
        std::atomic<int64_t> last_publish_exchange_ts_ns_;
        std::atomic<uint64_t> timestamp_issue_count_;
        TradeFlow last_trade_flow_;
        int64_t trade_window_start_ns_;
        double trade_window_volume_;
        std::vector<PriceLevel> pub_bids_;
        std::vector<PriceLevel> pub_asks_;

        void publish_lob(int64_t timestamp_ns) {
            if (publisher_ == nullptr || !publisher_->is_open()) {
                return;
            }

            pub_bids_.clear();
            pub_asks_.clear();
            book_.get_top_levels(10, pub_bids_, pub_asks_);
            publisher_->publish(book_.exchange(), book_.symbol(), timestamp_ns, book_.get_mid_price(),
                                pub_bids_, pub_asks_, last_trade_flow_);
        }

        static int64_t wall_now_ns() noexcept {
            using namespace std::chrono;
            return duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count();
        }

        static int64_t steady_now_ns() noexcept {
            using namespace std::chrono;
            return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
        }

        void inspect_timestamp(const char *source, int64_t local_ts_ns, int64_t exchange_ts_ns) noexcept {
            constexpr int64_t k_max_local_gap_ns = 60LL * 1000LL * 1000LL * 1000LL;
            const int64_t prev_local = last_publish_local_ts_ns_.load(std::memory_order_acquire);
            const int64_t prev_exchange = last_publish_exchange_ts_ns_.load(std::memory_order_acquire);
            bool issue = false;
            const char *issue_kind = "none";
            int64_t reference_ns = 0;

            if (local_ts_ns <= 0) {
                issue = true;
                issue_kind = "local_missing";
            } else if (prev_local > 0 && local_ts_ns < prev_local) {
                issue = true;
                issue_kind = "local_non_monotonic";
                reference_ns = prev_local;
            } else if (prev_local > 0 && (local_ts_ns - prev_local) > k_max_local_gap_ns) {
                issue = true;
                issue_kind = "local_gap";
                reference_ns = local_ts_ns - prev_local;
            } else if (exchange_ts_ns > 0 && prev_exchange > 0 && exchange_ts_ns < prev_exchange) {
                issue = true;
                issue_kind = "exchange_non_monotonic";
                reference_ns = prev_exchange;
            }

            if (issue) {
                const uint64_t issue_count =
                        timestamp_issue_count_.fetch_add(1, std::memory_order_acq_rel) + 1;
                LOG_WARN("BookManager timestamp anomaly", "symbol", book_.symbol().c_str(), "exchange",
                         exchange_to_string(book_.exchange()), "source", source, "issue", issue_kind,
                         "local_ts_ns", local_ts_ns, "exchange_ts_ns", exchange_ts_ns, "reference_ns",
                         reference_ns, "issue_count", issue_count);
            }

            if (local_ts_ns > 0) {
                last_publish_local_ts_ns_.store(local_ts_ns, std::memory_order_release);
            }
            if (exchange_ts_ns > 0) {
                last_publish_exchange_ts_ns_.store(exchange_ts_ns, std::memory_order_release);
            }
        }
    };
} 
