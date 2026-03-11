#pragma once

// Alpha signal bridge: Python neural model → C++ strategy via mmap file.
//
// Layout of /tmp/neural_alpha_signal.bin (24 bytes, written atomically):
//   offset  0 : float64  signal_bps   — mid-horizon return prediction (bps)
//   offset  8 : float64  risk_score   — adverse-selection probability [0, 1]
//   offset 16 : int64    ts_ns        — nanosecond timestamp of last update
//
// Python writes via struct.pack("dql", ...) every 500 ms.
// C++ reads via mmap on every book update (< 100 ns).
//
// Gate logic:
//   LONG  trade allowed only when signal_bps > signal_min_bps AND risk_score < risk_max
//   SHORT trade allowed only when signal_bps < -signal_min_bps AND risk_score < risk_max
//   NEUTRAL (|signal| < min_bps) → allow market-maker only (no directional taker arb)
//   STALE  (age > stale_ns)      → allow taker arb (fail-open to preserve uptime)

#include <atomic>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/mman.h>
#include <unistd.h>

namespace trading {

struct AlphaSignal {
    double  signal_bps  = 0.0;
    double  risk_score  = 0.5;
    int64_t ts_ns       = 0;
};

class AlphaSignalReader {
public:
    static constexpr size_t  FILE_SIZE     = 24;
    static constexpr int64_t STALE_NS      = 2'000'000'000LL;  // 2 s
    static constexpr double  DEFAULT_RISK  = 0.5;

    explicit AlphaSignalReader(
        const std::string& path       = "/tmp/neural_alpha_signal.bin",
        double signal_min_bps         = 3.0,
        double risk_max               = 0.65
    ) : path_(path), signal_min_bps_(signal_min_bps), risk_max_(risk_max) {}

    ~AlphaSignalReader() { close(); }

    AlphaSignalReader(const AlphaSignalReader&) = delete;
    AlphaSignalReader& operator=(const AlphaSignalReader&) = delete;

    // Open the mmap file. Returns false if file does not exist yet.
    // The strategy falls back to unconditional trading when not open.
    bool open() {
        fd_ = ::open(path_.c_str(), O_RDONLY);
        if (fd_ < 0) return false;

        ptr_ = static_cast<const char*>(
            ::mmap(nullptr, FILE_SIZE, PROT_READ, MAP_SHARED, fd_, 0));
        if (ptr_ == MAP_FAILED) {
            ::close(fd_);
            fd_  = -1;
            ptr_ = nullptr;
            return false;
        }
        return true;
    }

    void close() {
        if (ptr_) { ::munmap(const_cast<char*>(ptr_), FILE_SIZE); ptr_ = nullptr; }
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    }

    bool is_open() const noexcept { return ptr_ != nullptr; }

    // Read the latest signal. Thread-safe (reads are atomic at 8-byte alignment).
    AlphaSignal read() const noexcept {
        if (!ptr_) return {};
        AlphaSignal s;
        std::memcpy(&s.signal_bps, ptr_ + 0,  8);
        std::memcpy(&s.risk_score, ptr_ + 8,  8);
        std::memcpy(&s.ts_ns,      ptr_ + 16, 8);
        return s;
    }

    // Returns true if a LONG taker arb is allowed by the model.
    bool allows_long() const noexcept {
        if (!ptr_) return true;  // fail-open
        AlphaSignal s = read();
        if (is_stale(s)) return true;
        return s.signal_bps >= signal_min_bps_ && s.risk_score < risk_max_;
    }

    // Returns true if a SHORT taker arb is allowed by the model.
    bool allows_short() const noexcept {
        if (!ptr_) return true;
        AlphaSignal s = read();
        if (is_stale(s)) return true;
        return s.signal_bps <= -signal_min_bps_ && s.risk_score < risk_max_;
    }

    // Returns true if market-making is allowed (low adverse-selection risk).
    bool allows_mm() const noexcept {
        if (!ptr_) return true;
        AlphaSignal s = read();
        if (is_stale(s)) return true;
        return s.risk_score < risk_max_;
    }

private:
    static int64_t now_ns() noexcept {
        using namespace std::chrono;
        return duration_cast<nanoseconds>(
            high_resolution_clock::now().time_since_epoch()).count();
    }

    bool is_stale(const AlphaSignal& s) const noexcept {
        return s.ts_ns == 0 || (now_ns() - s.ts_ns) > STALE_NS;
    }

    std::string  path_;
    double       signal_min_bps_;
    double       risk_max_;
    int          fd_  = -1;
    const char*  ptr_ = nullptr;
};

}  // namespace trading
