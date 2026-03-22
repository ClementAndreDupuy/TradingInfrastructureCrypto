#pragma once

#include <atomic>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/mman.h>
#include <unistd.h>

namespace trading {

struct AlphaSignal {
    double signal_bps = 0.0;
    double risk_score = 0.5;
    double size_fraction = 0.0;
    int64_t horizon_ticks = 0;
    int64_t ts_ns = 0;
};

class AlphaSignalReader {
  public:

    static constexpr size_t FILE_SIZE = 48;
    static constexpr int64_t STALE_NS = 2'000'000'000LL;
    static constexpr double DEFAULT_RISK = 0.5;
    static constexpr int k_max_retries = 16;

    explicit AlphaSignalReader(const std::string& path = "/tmp/trt_ipc/neural_alpha_signal.bin",
                               double signal_min_bps = 3.0, double risk_max = 0.65)
        : path_(path), signal_min_bps_(signal_min_bps), risk_max_(risk_max) {}

    ~AlphaSignalReader() { close(); }

    AlphaSignalReader(const AlphaSignalReader&) = delete;
    AlphaSignalReader& operator=(const AlphaSignalReader&) = delete;

    [[nodiscard]] bool open() {
        fd_ = ::open(path_.c_str(), O_RDONLY);
        if (fd_ < 0)
            return false;

        ptr_ = static_cast<const char*>(::mmap(nullptr, FILE_SIZE, PROT_READ, MAP_SHARED, fd_, 0));
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
            ::munmap(const_cast<char*>(ptr_), FILE_SIZE);
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
        if (!ptr_)
            return true; // fail-open
        AlphaSignal s = read();
        if (is_stale(s))
            return true;
        return s.signal_bps >= signal_min_bps_ && s.risk_score < risk_max_;
    }
    bool allows_short() const noexcept {
        if (!ptr_)
            return true;
        AlphaSignal s = read();
        if (is_stale(s))
            return true;
        return s.signal_bps <= -signal_min_bps_ && s.risk_score < risk_max_;
    }
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

    bool is_stale(const AlphaSignal& s) const noexcept {
        return s.ts_ns == 0 || (now_ns() - s.ts_ns) > STALE_NS;
    }

    std::string path_;
    double signal_min_bps_;
    double risk_max_;
    int fd_ = -1;
    const char* ptr_ = nullptr;
};

} // namespace trading
