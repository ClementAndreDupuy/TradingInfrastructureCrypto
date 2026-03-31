#pragma once

#include "../common/logging.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/mman.h>
#include <unistd.h>

namespace trading {
    struct AlphaSignal {
        double signal_bps;
        double risk_score;
        double size_fraction;
        int64_t horizon_ticks;
        int64_t ts_ns;
    };

    class AlphaSignalReader {
    public:
        static constexpr size_t FILE_SIZE = 48;
        static constexpr int64_t STALE_NS = 2'000'000'000LL;
        static constexpr double DEFAULT_RISK = 0.5;
        static constexpr int k_max_retries = 16;
        static constexpr const char *k_default_path = "/tmp/trt_ipc/trt_alpha.bin";

        explicit AlphaSignalReader(const std::string &path,
                                   double signal_min_bps, double risk_max,
                                   int64_t warn_throttle_ns = 10'000'000'000LL)
            : path_(path), signal_min_bps_(signal_min_bps), risk_max_(risk_max),
              warn_throttle_ns_(warn_throttle_ns) {
        }

        ~AlphaSignalReader() { close(); }

        AlphaSignalReader(const AlphaSignalReader &) = delete;

        AlphaSignalReader &operator=(const AlphaSignalReader &) = delete;

        [[nodiscard]] bool open() {
            fd_ = ::open(path_.c_str(), O_RDONLY);
            if (fd_ < 0)
                return false;

            ptr_ = static_cast<const char *>(::mmap(nullptr, FILE_SIZE, PROT_READ, MAP_SHARED, fd_, 0));
            if (ptr_ == MAP_FAILED) {
                ::close(fd_);
                fd_ = -1;
                ptr_ = nullptr;
                return false;
            }
            return true;
        }

        void close() {
            if (ptr_) {
                ::munmap(const_cast<char *>(ptr_), FILE_SIZE);
                ptr_ = nullptr;
            }
            if (fd_ >= 0) {
                ::close(fd_);
                fd_ = -1;
            }
        }

        bool is_open() const noexcept { return ptr_ != nullptr; }

        AlphaSignal read() const noexcept {
            if (!ptr_)
                return {};

            for (int i = 0; i < k_max_retries; ++i) {
                uint64_t seq1;
                std::memcpy(&seq1, ptr_, sizeof(seq1));
                if (seq1 & 1u) {
                    continue;
                }
                std::atomic_thread_fence(std::memory_order_acquire);

                AlphaSignal s;
                std::memcpy(&s.signal_bps, ptr_ + 8, sizeof(s.signal_bps));
                std::memcpy(&s.risk_score, ptr_ + 16, sizeof(s.risk_score));
                std::memcpy(&s.size_fraction, ptr_ + 24, sizeof(s.size_fraction));
                std::memcpy(&s.horizon_ticks, ptr_ + 32, sizeof(s.horizon_ticks));
                std::memcpy(&s.ts_ns, ptr_ + 40, sizeof(s.ts_ns));

                std::atomic_thread_fence(std::memory_order_acquire);
                uint64_t seq2;
                std::memcpy(&seq2, ptr_, sizeof(seq2));

                if (seq1 == seq2) {
                    return s;
                }
            }
            return {};
        }

        bool allows_long() const noexcept {
            if (!ptr_) {
                // Fail-open: allow trading when IPC is unavailable so that
                // infrastructure failures do not halt order flow. The operator
                // is warned via a throttled log message that signal gating is
                // inactive for this reader.
                maybe_warn_ipc_unavailable();
                return true;
            }
            AlphaSignal s = read();
            if (is_stale(s))
                return true;
            return s.signal_bps >= signal_min_bps_ && s.risk_score < risk_max_;
        }

        bool allows_short() const noexcept {
            if (!ptr_) {
                // Fail-open: allow trading when IPC is unavailable so that
                // infrastructure failures do not halt order flow. The operator
                // is warned via a throttled log message that signal gating is
                // inactive for this reader.
                maybe_warn_ipc_unavailable();
                return true;
            }
            AlphaSignal s = read();
            if (is_stale(s))
                return true;
            return s.signal_bps <= -signal_min_bps_ && s.risk_score < risk_max_;
        }

        uint64_t ipc_warn_count() const noexcept { return ipc_warn_count_; }

        bool allows_mm() const noexcept {
            if (!ptr_)
                return true;
            AlphaSignal s = read();
            if (is_stale(s))
                return true;
            return s.risk_score < risk_max_;
        }

    private:
        static int64_t now_ns() noexcept {
            using namespace std::chrono;
            return duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count();
        }

        bool is_stale(const AlphaSignal &s) const noexcept {
            return s.ts_ns == 0 || (now_ns() - s.ts_ns) > STALE_NS;
        }

        void maybe_warn_ipc_unavailable() const noexcept {
            const int64_t now = now_ns();
            if (now - last_warn_ns_ >= warn_throttle_ns_) {
                last_warn_ns_ = now;
                ++ipc_warn_count_;
                LOG_WARN("alpha_signal_reader ipc_unavailable: signal gating inactive (fail-open)", "path", path_.c_str());
            }
        }

        std::string path_;
        double signal_min_bps_;
        double risk_max_;
        int64_t warn_throttle_ns_;
        mutable int64_t last_warn_ns_ = 0;
        mutable uint64_t ipc_warn_count_ = 0;
        int fd_ = -1;
        const char *ptr_ = nullptr;
    };
}
