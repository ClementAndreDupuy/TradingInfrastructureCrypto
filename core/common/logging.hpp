#pragma once

// Async ring-buffer logger.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <thread>

#ifdef __x86_64__
#include <x86intrin.h>
#define TRADING_RDTSC() __rdtsc()
#else
#define TRADING_RDTSC()                                                                            \
    static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count())
#endif

namespace trading {
    enum class LogLevel : uint8_t { DEBUG = 0, INFO = 1, WARN = 2, ERROR = 3 };

    struct TscCalibration {
        uint64_t tsc_base;
        int64_t ns_base;
        double ns_per_cycle;

        static TscCalibration calibrate() noexcept {
            TscCalibration c;
            using WallClock = std::chrono::system_clock;
            using SteadyClock = std::chrono::steady_clock;

            auto t0_wall = WallClock::now();
            auto t0_steady = SteadyClock::now();
            uint64_t t0_tsc = TRADING_RDTSC();

            while (
                std::chrono::duration_cast<std::chrono::milliseconds>(SteadyClock::now() - t0_steady)
                .count() <
                50) {
            }

            uint64_t t1_tsc = TRADING_RDTSC();
            auto t1_steady = SteadyClock::now();

            int64_t elapsed_ns =
                    std::chrono::duration_cast<std::chrono::nanoseconds>(t1_steady - t0_steady).count();
            uint64_t elapsed_tsc = t1_tsc - t0_tsc;

            c.tsc_base = t0_tsc;
            c.ns_base = std::chrono::duration_cast<std::chrono::nanoseconds>(t0_wall.time_since_epoch())
                    .count();
            c.ns_per_cycle = (elapsed_tsc > 0)
                                 ? static_cast<double>(elapsed_ns) / static_cast<double>(elapsed_tsc)
                                 : 1.0;
            return c;
        }

        int64_t tsc_to_ns(uint64_t tsc) const noexcept {
            return ns_base + static_cast<int64_t>(static_cast<double>(tsc - tsc_base) * ns_per_cycle);
        }
    };

    static constexpr size_t LOG_MSG_SIZE = 496;

    struct alignas(512) LogEntry {
        uint64_t tsc;
        LogLevel level;
        char msg[LOG_MSG_SIZE];
        char _pad[512 - sizeof(uint64_t) - sizeof(LogLevel) - LOG_MSG_SIZE];
    };

    static_assert(sizeof(LogEntry) == 512, "LogEntry must be 512 bytes");

    static constexpr size_t LOG_RING_SIZE = 2048;

    struct LogRing {
        alignas(64) std::atomic<uint64_t> head{0};
        alignas(64) std::atomic<uint64_t> tail{0}; // writer advances tail
        alignas(256) LogEntry entries[LOG_RING_SIZE];
    };

    class AsyncLogger {
    public:
        static AsyncLogger &instance() noexcept {
            static AsyncLogger inst;
            return inst;
        }

        void set_level(LogLevel lvl) noexcept { min_level_.store(lvl, std::memory_order_relaxed); }
        LogLevel min_level() const noexcept { return min_level_.load(std::memory_order_relaxed); }

        void push(LogLevel level, const char *msg, size_t msg_len) noexcept {
            uint64_t tail = ring_.tail.load(std::memory_order_relaxed);
            uint64_t head = ring_.head.load(std::memory_order_acquire);

            if (tail - head >= LOG_RING_SIZE) {
                dropped_.fetch_add(1, std::memory_order_relaxed);
                return;
            }

            LogEntry &e = ring_.entries[tail & (LOG_RING_SIZE - 1)];
            e.tsc = TRADING_RDTSC();
            e.level = level;

            size_t copy_len = (msg_len < LOG_MSG_SIZE - 1) ? msg_len : (LOG_MSG_SIZE - 1);
            std::memcpy(e.msg, msg, copy_len);
            e.msg[copy_len] = '\0';

            ring_.tail.store(tail + 1, std::memory_order_release);
        }

        uint64_t dropped() const noexcept { return dropped_.load(std::memory_order_relaxed); }

        void stop() noexcept {
            if (!running_.exchange(false, std::memory_order_acq_rel))
                return;
            if (writer_thread_.joinable())
                writer_thread_.join();
            drain(/*force=*/true);
        }

        ~AsyncLogger() { stop(); }

    private:
        AsyncLogger()
            : min_level_(LogLevel::INFO), running_(true), calib_(TscCalibration::calibrate()) {
            writer_thread_ = std::thread(&AsyncLogger::writer_loop, this);
        }

        void writer_loop() noexcept {
            while (running_.load(std::memory_order_acquire)) {
                drain(/*force=*/false);
                std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
        }

        void drain(bool force) noexcept {
            for (;;) {
                uint64_t head = ring_.head.load(std::memory_order_relaxed);
                uint64_t tail = ring_.tail.load(std::memory_order_acquire);
                if (head == tail)
                    break;

                const LogEntry &e = ring_.entries[head & (LOG_RING_SIZE - 1)];
                write_entry(e);

                ring_.head.store(head + 1, std::memory_order_release);
                (void) force;
            }
        }

        void write_entry(const LogEntry &e) noexcept {
            int64_t ns = calib_.tsc_to_ns(e.tsc);

            time_t sec = static_cast<time_t>(ns / 1'000'000'000LL);
            int nsub = static_cast<int>(ns % 1'000'000'000LL);
            if (nsub < 0) {
                sec--;
                nsub += 1'000'000'000;
            }

            struct tm tm_buf;
            gmtime_r(&sec, &tm_buf);

            char ts[64] = {};
            size_t written = std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm_buf);
            if (written > 0 && written < sizeof(ts)) {
                std::snprintf(ts + written, sizeof(ts) - written, ".%09d", nsub);
            }

            const char *lvl = level_to_string(e.level);
            std::fprintf(stderr, "%s [%s] %s\n", ts, lvl, e.msg);
        }

        static const char *level_to_string(LogLevel l) noexcept {
            switch (l) {
                case LogLevel::DEBUG:
                    return "DEBUG";
                case LogLevel::INFO:
                    return "INFO ";
                case LogLevel::WARN:
                    return "WARN ";
                case LogLevel::ERROR:
                    return "ERROR";
                default:
                    return "?????";
            }
        }

        std::atomic<LogLevel> min_level_;
        std::atomic<bool> running_;
        std::atomic<uint64_t> dropped_{0};
        LogRing ring_;
        TscCalibration calib_;
        std::thread writer_thread_;
    };

    namespace detail {
        inline bool can_append(size_t pos, size_t cap) noexcept { return cap > 0 && pos < (cap - 1); }

        inline void fmt_append(char *buf, size_t &pos, size_t cap, const char *s) noexcept {
            if (!s)
                return;
            while (can_append(pos, cap) && *s)
                buf[pos++] = *s++;
        }

        inline void fmt_append_int(char *buf, size_t &pos, size_t cap, long long v) noexcept {
            char tmp[32];
            int n = std::snprintf(tmp, sizeof(tmp), "%lld", v);
            for (int i = 0; i < n && can_append(pos, cap); ++i)
                buf[pos++] = tmp[i];
        }

        inline void fmt_append_uint(char *buf, size_t &pos, size_t cap, unsigned long long v) noexcept {
            char tmp[32];
            int n = std::snprintf(tmp, sizeof(tmp), "%llu", v);
            for (int i = 0; i < n && can_append(pos, cap); ++i)
                buf[pos++] = tmp[i];
        }

        inline void fmt_append_double(char *buf, size_t &pos, size_t cap, double v) noexcept {
            char tmp[32];
            int n = std::snprintf(tmp, sizeof(tmp), "%.6g", v);
            for (int i = 0; i < n && can_append(pos, cap); ++i)
                buf[pos++] = tmp[i];
        }

        // Terminal case
        inline void fmt_fields(char *, size_t &, size_t) noexcept {
        }

        template<typename V, typename... Rest>
        void fmt_fields(char *buf, size_t &pos, size_t cap, const char *key, V &&val,
                        Rest &&... rest) noexcept;

        // Value dispatch
        inline void fmt_value(char *buf, size_t &pos, size_t cap, const char *v) noexcept {
            fmt_append(buf, pos, cap, v);
        }

        inline void fmt_value(char *buf, size_t &pos, size_t cap, char *v) noexcept {
            fmt_append(buf, pos, cap, v);
        }

        inline void fmt_value(char *buf, size_t &pos, size_t cap, bool v) noexcept {
            fmt_append(buf, pos, cap, v ? "true" : "false");
        }

        inline void fmt_value(char *buf, size_t &pos, size_t cap, int v) noexcept {
            fmt_append_int(buf, pos, cap, v);
        }

        inline void fmt_value(char *buf, size_t &pos, size_t cap, long v) noexcept {
            fmt_append_int(buf, pos, cap, v);
        }

        inline void fmt_value(char *buf, size_t &pos, size_t cap, long long v) noexcept {
            fmt_append_int(buf, pos, cap, v);
        }

        inline void fmt_value(char *buf, size_t &pos, size_t cap, unsigned v) noexcept {
            fmt_append_uint(buf, pos, cap, v);
        }

        inline void fmt_value(char *buf, size_t &pos, size_t cap, unsigned long v) noexcept {
            fmt_append_uint(buf, pos, cap, v);
        }

        inline void fmt_value(char *buf, size_t &pos, size_t cap, unsigned long long v) noexcept {
            fmt_append_uint(buf, pos, cap, v);
        }

        inline void fmt_value(char *buf, size_t &pos, size_t cap, double v) noexcept {
            fmt_append_double(buf, pos, cap, v);
        }

        inline void fmt_value(char *buf, size_t &pos, size_t cap, float v) noexcept {
            fmt_append_double(buf, pos, cap, static_cast<double>(v));
        }

        template<typename V, typename... Rest>
        void fmt_fields(char *buf, size_t &pos, size_t cap, const char *key, V &&val,
                        Rest &&... rest) noexcept {
            if (can_append(pos, cap))
                buf[pos++] = ' ';
            fmt_append(buf, pos, cap, key);
            if (can_append(pos, cap))
                buf[pos++] = '=';
            fmt_value(buf, pos, cap, std::forward<V>(val));
            fmt_fields(buf, pos, cap, std::forward<Rest>(rest)...);
        }
    } // namespace detail

    template<typename... Args>
    inline void log(LogLevel level, const char *msg, Args &&... args) noexcept {
        AsyncLogger &logger = AsyncLogger::instance();
        if (level < logger.min_level())
            return;

        char buf[LOG_MSG_SIZE];
        size_t pos = 0;

        detail::fmt_append(buf, pos, sizeof(buf), msg);
        detail::fmt_fields(buf, pos, sizeof(buf), std::forward<Args>(args)...);
        buf[(pos < sizeof(buf)) ? pos : (sizeof(buf) - 1)] = '\0';

        logger.push(level, buf, pos);
    }

    inline void set_log_level(LogLevel lvl) noexcept { AsyncLogger::instance().set_level(lvl); }
} // namespace trading

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wgnu-zero-variadic-macro-arguments"
#define LOG_DEBUG(msg, ...) ::trading::log(::trading::LogLevel::DEBUG, msg, ##__VA_ARGS__)
#define LOG_INFO(msg, ...) ::trading::log(::trading::LogLevel::INFO, msg, ##__VA_ARGS__)
#define LOG_WARN(msg, ...) ::trading::log(::trading::LogLevel::WARN, msg, ##__VA_ARGS__)
#define LOG_ERROR(msg, ...) ::trading::log(::trading::LogLevel::ERROR, msg, ##__VA_ARGS__)
#pragma clang diagnostic pop
