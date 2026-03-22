#pragma once

#include "../common/logging.hpp"
#include <atomic>
#include <chrono>
#include <cstdint>

namespace trading {
    enum class KillReason : uint8_t {
        MANUAL = 0,
        DRAWDOWN = 1,
        CIRCUIT_BREAKER = 2,
        HEARTBEAT_MISSED = 3,
        BOOK_CORRUPTED = 4,
    };

    class KillSwitch {
    public:
        static constexpr int64_t DEFAULT_HEARTBEAT_TIMEOUT_NS = 5'000'000'000LL; // 5s

        explicit KillSwitch(int64_t heartbeat_timeout_ns = DEFAULT_HEARTBEAT_TIMEOUT_NS)
            : active_(false), reason_(KillReason::MANUAL), last_heartbeat_ns_(now_ns()),
              heartbeat_timeout_ns_(heartbeat_timeout_ns) {
        }

        bool is_active() const noexcept { return active_.load(std::memory_order_acquire); }

        void trigger(KillReason reason) noexcept {
            reason_.store(reason, std::memory_order_relaxed);
            active_.store(true, std::memory_order_release);
            LOG_ERROR("[KillSwitch] Kill switch triggered", "reason", reason_to_string(reason));
        }

        void reset() noexcept {
            active_.store(false, std::memory_order_release);
            last_heartbeat_ns_.store(now_ns(), std::memory_order_release);
            LOG_INFO("[KillSwitch] Kill switch reset", "active", false);
        }

        void heartbeat() noexcept { last_heartbeat_ns_.store(now_ns(), std::memory_order_release); }

        bool check_heartbeat() noexcept {
            int64_t elapsed = now_ns() - last_heartbeat_ns_.load(std::memory_order_acquire);
            if (elapsed > heartbeat_timeout_ns_) {
                trigger(KillReason::HEARTBEAT_MISSED);
                return false;
            }
            return true;
        }

        KillReason get_reason() const noexcept { return reason_.load(std::memory_order_acquire); }

        static const char *reason_to_string(KillReason r) noexcept {
            switch (r) {
                case KillReason::MANUAL:
                    return "MANUAL";
                case KillReason::DRAWDOWN:
                    return "DRAWDOWN";
                case KillReason::CIRCUIT_BREAKER:
                    return "CIRCUIT_BREAKER";
                case KillReason::HEARTBEAT_MISSED:
                    return "HEARTBEAT_MISSED";
                case KillReason::BOOK_CORRUPTED:
                    return "BOOK_CORRUPTED";
                default:
                    return "UNKNOWN";
            }
        }

    private:
        std::atomic<bool> active_;
        std::atomic<KillReason> reason_;
        std::atomic<int64_t> last_heartbeat_ns_;
        int64_t heartbeat_timeout_ns_;

        static int64_t now_ns() noexcept {
            return std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                    .count();
        }
    };
} // namespace trading
