#pragma once

#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/mman.h>
#include <unistd.h>

namespace trading {
    struct RegimeSignal {
        double p_calm;
        double p_trending;
        double p_shock;
        double p_illiquid;
        int64_t ts_ns;
    };

    class RegimeSignalReader {
    public:
        static constexpr size_t FILE_SIZE = 48;
        static constexpr int k_max_retries = 16;

        explicit RegimeSignalReader(const std::string &path) : path_(path) {
        }

        ~RegimeSignalReader() { close(); }

        RegimeSignalReader(const RegimeSignalReader &) = delete;

        RegimeSignalReader &operator=(const RegimeSignalReader &) = delete;

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

        RegimeSignal read() {
            if (!ptr_ && !open()) {
                return {};
            }

            for (int i = 0; i < k_max_retries; ++i) {
                uint64_t seq1 = 0;
                std::memcpy(&seq1, ptr_, sizeof(seq1));
                if (seq1 & 1ULL)
                    continue;

                std::atomic_thread_fence(std::memory_order_acquire);

                RegimeSignal s;
                std::memcpy(&s.p_calm, ptr_ + 8, sizeof(double));
                std::memcpy(&s.p_trending, ptr_ + 16, sizeof(double));
                std::memcpy(&s.p_shock, ptr_ + 24, sizeof(double));
                std::memcpy(&s.p_illiquid, ptr_ + 32, sizeof(double));
                std::memcpy(&s.ts_ns, ptr_ + 40, sizeof(int64_t));

                std::atomic_thread_fence(std::memory_order_acquire);

                uint64_t seq2 = 0;
                std::memcpy(&seq2, ptr_, sizeof(seq2));
                if (seq1 == seq2 && !(seq2 & 1ULL))
                    return s;
            }

            return {};
        }

        bool is_stale(const RegimeSignal &s, int64_t stale_ns) const noexcept {
            if (s.ts_ns == 0)
                return true;
            int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            return (now - s.ts_ns) > stale_ns;
        }

    private:
        std::string path_;
        int fd_;
        const char *ptr_;
    };
} 
