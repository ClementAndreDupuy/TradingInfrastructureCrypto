#pragma once

#include "../common/logging.hpp"
#include "../feeds/common/book_manager.hpp"
#include "kill_switch.hpp"

#include <atomic>
#include <cmath>
#include <cstdint>

namespace trading {
    struct CircuitBreakerConfig {
        int32_t consecutive_loss_count = 5;
        int32_t max_orders_per_second = 10;
        int32_t max_orders_per_minute = 200;
        int32_t max_messages_per_second = 1000;
        int64_t max_book_age_ms = 500;

        double max_drawdown_usd = -5000.0;
        double max_price_deviation_bps = 300.0;
        double price_ema_alpha = 0.01;
        double per_leg_loss_usd = -50.0;
    };

    enum class CircuitCheckResult : uint8_t {
        OK = 0,
        RATE_LIMIT_SEC = 1,
        RATE_LIMIT_MIN = 2,
        MSG_RATE_LIMIT = 3,
        DRAWDOWN = 4,
        STALE_BOOK = 5,
        FLASH_CRASH = 6,
        CONSEC_LOSSES = 7,
    };

    class CircuitBreaker {
    public:
        explicit CircuitBreaker(const CircuitBreakerConfig &cfg, KillSwitch &ks)
            : cfg_(cfg), kill_switch_(ks), order_count_sec_(0), sec_window_start_ns_(0),
              order_count_min_(0), min_window_start_ns_(0), msg_count_sec_(0), msg_window_start_ns_(0),
              consec_losses_(0), realized_pnl_(0.0), ref_price_(0.0) {
        }

        CircuitCheckResult check_order_rate() noexcept {
            int64_t now = now_ns();

            int64_t sec_start = sec_window_start_ns_.load(std::memory_order_acquire);
            if (now - sec_start >= 1'000'000'000LL) {
                if (sec_window_start_ns_.compare_exchange_strong(sec_start, now,
                                                                 std::memory_order_acq_rel)) {
                    order_count_sec_.store(0, std::memory_order_release);
                }
            }
            int32_t sec_count = order_count_sec_.fetch_add(1, std::memory_order_acq_rel);
            if (sec_count >= cfg_.max_orders_per_second) {
                LOG_WARN("[Circuit breaker] per-second order rate exceeded", "count", sec_count + 1,
                         "limit", cfg_.max_orders_per_second);
                return CircuitCheckResult::RATE_LIMIT_SEC;
            }

            int64_t min_start = min_window_start_ns_.load(std::memory_order_acquire);
            if (now - min_start >= 60'000'000'000LL) {
                if (min_window_start_ns_.compare_exchange_strong(min_start, now,
                                                                 std::memory_order_acq_rel)) {
                    order_count_min_.store(0, std::memory_order_release);
                }
            }
            int32_t min_count = order_count_min_.fetch_add(1, std::memory_order_acq_rel);
            if (min_count >= cfg_.max_orders_per_minute) {
                LOG_WARN("[Circuit breaker] per-minute order rate exceeded", "count", min_count + 1,
                         "limit", cfg_.max_orders_per_minute);
                return CircuitCheckResult::RATE_LIMIT_MIN;
            }

            return CircuitCheckResult::OK;
        }

        CircuitCheckResult check_message_rate() noexcept {
            int64_t now = now_ns();
            int64_t msg_start = msg_window_start_ns_.load(std::memory_order_acquire);
            if (now - msg_start >= 1'000'000'000LL) {
                if (msg_window_start_ns_.compare_exchange_strong(msg_start, now,
                                                                 std::memory_order_acq_rel)) {
                    msg_count_sec_.store(0, std::memory_order_release);
                }
            }
            int32_t count = msg_count_sec_.fetch_add(1, std::memory_order_acq_rel);
            if (count >= cfg_.max_messages_per_second) {
                LOG_WARN("[Circuit breaker] feed message rate exceeded", "count", count + 1, "limit",
                         cfg_.max_messages_per_second);
                return CircuitCheckResult::MSG_RATE_LIMIT;
            }
            return CircuitCheckResult::OK;
        }

        CircuitCheckResult check_drawdown(double realized_pnl) noexcept {
            realized_pnl_.store(realized_pnl, std::memory_order_release);
            if (realized_pnl <= cfg_.max_drawdown_usd) {
                LOG_ERROR("[Circuit breaker] drawdown limit breached", "pnl", realized_pnl, "limit",
                          cfg_.max_drawdown_usd);
                kill_switch_.trigger(KillReason::DRAWDOWN);
                return CircuitCheckResult::DRAWDOWN;
            }
            return CircuitCheckResult::OK;
        }

        CircuitCheckResult check_book_age(const BookManager &book) noexcept {
            int64_t age = book.age_ms();
            if (age > cfg_.max_book_age_ms) {
                LOG_ERROR("[Circuit breaker] stale book", "age_ms", age, "limit_ms",
                          cfg_.max_book_age_ms, "symbol", book.book().symbol().c_str());
                kill_switch_.trigger(KillReason::BOOK_CORRUPTED);
                return CircuitCheckResult::STALE_BOOK;
            }
            return CircuitCheckResult::OK;
        }

        CircuitCheckResult check_price_deviation(double price) noexcept {
            if (price <= 0.0)
                return CircuitCheckResult::OK;

            double ref = ref_price_.load(std::memory_order_relaxed);
            if (ref == 0.0) {
                ref_price_.store(price, std::memory_order_release);
                return CircuitCheckResult::OK;
            }

            double alpha = cfg_.price_ema_alpha;
            double new_ref = ref + alpha * (price - ref);
            ref_price_.store(new_ref, std::memory_order_release);

            double deviation_bps = std::abs(price - ref) / ref * 10000.0;
            if (deviation_bps > cfg_.max_price_deviation_bps) {
                LOG_ERROR("[Circuit breaker] flash crash detected", "price", price, "ref", ref,
                          "dev_bps", deviation_bps, "limit_bps", cfg_.max_price_deviation_bps);
                kill_switch_.trigger(KillReason::CIRCUIT_BREAKER);
                return CircuitCheckResult::FLASH_CRASH;
            }
            return CircuitCheckResult::OK;
        }

        CircuitCheckResult record_leg_result(double leg_pnl) noexcept {
            if (leg_pnl <= cfg_.per_leg_loss_usd) {
                int32_t n = consec_losses_.fetch_add(1, std::memory_order_acq_rel) + 1;
                if (n >= cfg_.consecutive_loss_count) {
                    LOG_ERROR("[Circuit breaker] consecutive loss limit hit", "count", n, "limit",
                              cfg_.consecutive_loss_count, "last_pnl", leg_pnl);
                    kill_switch_.trigger(KillReason::CIRCUIT_BREAKER);
                    return CircuitCheckResult::CONSEC_LOSSES;
                }
                LOG_WARN("[Circuit breaker] consecutive loss incremented", "count", n, "limit",
                         cfg_.consecutive_loss_count);
            } else {
                consec_losses_.store(0, std::memory_order_release);
            }
            return CircuitCheckResult::OK;
        }

        CircuitCheckResult check_consecutive_losses() const noexcept {
            if (consec_losses_.load(std::memory_order_acquire) >= cfg_.consecutive_loss_count) {
                return CircuitCheckResult::CONSEC_LOSSES;
            }
            return CircuitCheckResult::OK;
        }

        static const char *result_to_string(CircuitCheckResult r) noexcept {
            switch (r) {
                case CircuitCheckResult::OK:
                    return "OK";
                case CircuitCheckResult::RATE_LIMIT_SEC:
                    return "RATE_LIMIT_SEC";
                case CircuitCheckResult::RATE_LIMIT_MIN:
                    return "RATE_LIMIT_MIN";
                case CircuitCheckResult::MSG_RATE_LIMIT:
                    return "MSG_RATE_LIMIT";
                case CircuitCheckResult::DRAWDOWN:
                    return "DRAWDOWN";
                case CircuitCheckResult::STALE_BOOK:
                    return "STALE_BOOK";
                case CircuitCheckResult::FLASH_CRASH:
                    return "FLASH_CRASH";
                case CircuitCheckResult::CONSEC_LOSSES:
                    return "CONSEC_LOSSES";
                default:
                    return "UNKNOWN";
            }
        }

        double realized_pnl() const noexcept { return realized_pnl_.load(std::memory_order_acquire); }
        double ref_price() const noexcept { return ref_price_.load(std::memory_order_acquire); }

        int32_t consec_losses() const noexcept {
            return consec_losses_.load(std::memory_order_acquire);
        }

        void reset_daily() noexcept {
            realized_pnl_.store(0.0, std::memory_order_release);
            consec_losses_.store(0, std::memory_order_release);
            LOG_INFO("[CircuitBreaker] daily counters reset", "component", "circuit_breaker");
        }

    private:
        CircuitBreakerConfig cfg_;
        KillSwitch &kill_switch_;

        std::atomic<int32_t> order_count_sec_;
        std::atomic<int64_t> sec_window_start_ns_;
        std::atomic<int32_t> order_count_min_;
        std::atomic<int64_t> min_window_start_ns_;
        std::atomic<int32_t> msg_count_sec_;
        std::atomic<int64_t> msg_window_start_ns_;

        std::atomic<int32_t> consec_losses_;

        std::atomic<double> realized_pnl_;
        std::atomic<double> ref_price_;

        static int64_t now_ns() noexcept {
            using namespace std::chrono;
            return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
        }
    };
}
