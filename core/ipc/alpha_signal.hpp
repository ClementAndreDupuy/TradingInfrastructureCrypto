#pragma once

// Alpha signal bridge: Python neural model → C++ strategy via mmap file.
//
// Layout of /tmp/neural_alpha_signal.bin (32 bytes):
//   offset  0 : uint64  seq         — seqlock counter (even = stable, odd = writer active)
//   offset  8 : float64 signal_bps  — mid-horizon return prediction (bps)
//   offset 16 : float64 risk_score  — adverse-selection probability [0, 1]
//   offset 24 : int64   ts_ns       — nanosecond timestamp of last update
//
// Python writes via the seqlock protocol (shadow_session._SignalPublisher):
//   1. increment seq to odd  (signals write-in-progress to readers)
//   2. write signal_bps, risk_score, ts_ns
//   3. increment seq to even (signals write-complete)
//
// C++ reads via seqlock: load seq1 (must be even) → load fields →
//   load seq2 (must equal seq1); retry on mismatch or odd seq.
// Each read is < 100 ns; called on every book update.
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
    double signal_bps = 0.0;
    double risk_score = 0.5;
    int64_t ts_ns = 0;
};

class AlphaSignalReader {
  public:
    // File layout: [uint64 seq][float64 signal_bps][float64 risk_score][int64 ts_ns]
    static constexpr size_t FILE_SIZE = 32;
    static constexpr int64_t STALE_NS = 2'000'000'000LL; // 2 s
    static constexpr double DEFAULT_RISK = 0.5;
    static constexpr int k_max_retries = 16; // seqlock spin limit before fail-open

    explicit AlphaSignalReader(const std::string& path = "/tmp/neural_alpha_signal.bin",
                               double signal_min_bps = 3.0, double risk_max = 0.65)
        : path_(path), signal_min_bps_(signal_min_bps), risk_max_(risk_max) {}

    ~AlphaSignalReader() { close(); }

    AlphaSignalReader(const AlphaSignalReader&) = delete;
    AlphaSignalReader& operator=(const AlphaSignalReader&) = delete;

    // Open the mmap file. Returns false if file does not exist yet.
    // The strategy falls back to unconditional trading when not open.
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

    // Read the latest signal using a seqlock for consistency.
    //
    // The seqlock counter at offset 0 is incremented to an odd value by the Python
    // writer before touching the data fields, and back to even after. We spin until
    // seq is even and stable across the read. Retries are bounded; on exhaustion we
    // return a neutral signal (ts_ns == 0) which triggers fail-open behaviour.
    //
    // Memory ordering: acquire fences prevent the compiler (and hardware on non-TSO
    // architectures) from reordering the seq loads around the data loads.
    AlphaSignal read() const noexcept {
        if (!ptr_)
            return {};

        for (int i = 0; i < k_max_retries; ++i) {
            uint64_t seq1;
            std::memcpy(&seq1, ptr_, sizeof(seq1));
            if (seq1 & 1u) {
                // Writer is active; spin briefly without pausing the caller long.
                continue;
            }
            // Acquire fence: all subsequent loads see stores that completed before
            // the writer incremented seq to even.
            std::atomic_thread_fence(std::memory_order_acquire);

            AlphaSignal s;
            std::memcpy(&s.signal_bps, ptr_ + 8, sizeof(s.signal_bps));
            std::memcpy(&s.risk_score, ptr_ + 16, sizeof(s.risk_score));
            std::memcpy(&s.ts_ns, ptr_ + 24, sizeof(s.ts_ns));

            // Acquire fence: loads above must not be reordered after the seq2 load.
            std::atomic_thread_fence(std::memory_order_acquire);
            uint64_t seq2;
            std::memcpy(&seq2, ptr_, sizeof(seq2));

            if (seq1 == seq2) {
                return s;
            }
            // Writer updated the signal while we were reading; retry.
        }
        // Could not get a consistent read within the retry budget.
        // Return neutral (ts_ns == 0 → is_stale() == true → fail-open).
        return {};
    }

    // Returns true if a LONG taker arb is allowed by the model.
    bool allows_long() const noexcept {
        if (!ptr_)
            return true; // fail-open
        AlphaSignal s = read();
        if (is_stale(s))
            return true;
        return s.signal_bps >= signal_min_bps_ && s.risk_score < risk_max_;
    }

    // Returns true if a SHORT taker arb is allowed by the model.
    bool allows_short() const noexcept {
        if (!ptr_)
            return true;
        AlphaSignal s = read();
        if (is_stale(s))
            return true;
        return s.signal_bps <= -signal_min_bps_ && s.risk_score < risk_max_;
    }

    // Returns true if market-making is allowed (low adverse-selection risk).
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
        // Use steady_clock to avoid NTP-driven discontinuities in the staleness check.
        using namespace std::chrono;
        return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
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
