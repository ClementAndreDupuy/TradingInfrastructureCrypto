#pragma once

#include "../common/logging.hpp"
#include "../common/types.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace trading {

class LobPublisher {
  public:
    static constexpr const char* k_default_path = "/tmp/trt_lob_feed.bin";
    static constexpr size_t k_capacity = 10000;
    static constexpr size_t k_slot_size = 256;
    static constexpr size_t k_header_size = 64;

    struct alignas(1) LobSlot {
        uint8_t exchange_id = 255;
        char symbol[15] = {};
        int64_t timestamp_ns = 0;
        double mid_price = 0.0;
        double bid_price[5] = {};
        double bid_size[5] = {};
        double ask_price[5] = {};
        double ask_size[5] = {};
        char reserved[64] = {};
    };

    static_assert(sizeof(LobSlot) == k_slot_size, "LobSlot must be exactly 256 bytes");

    LobPublisher() = default;
    explicit LobPublisher(std::string path) : path_(std::move(path)) {}
    ~LobPublisher() { close(); }

    LobPublisher(const LobPublisher&) = delete;
    LobPublisher& operator=(const LobPublisher&) = delete;

    bool open() {
        if (is_open()) {
            return true;
        }

        const size_t total_size = k_header_size + k_capacity * sizeof(LobSlot);
        fd_ = ::open(path_.c_str(), O_CREAT | O_RDWR, 0644);
        if (fd_ < 0) {
            LOG_WARN("LobPublisher open failed", "path", path_.c_str());
            return false;
        }
        if (::ftruncate(fd_, static_cast<off_t>(total_size)) != 0) {
            LOG_WARN("LobPublisher ftruncate failed", "path", path_.c_str());
            ::close(fd_);
            fd_ = -1;
            return false;
        }

        void* mapped = ::mmap(nullptr, total_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        if (mapped == MAP_FAILED) {
            LOG_WARN("LobPublisher mmap failed", "path", path_.c_str());
            ::close(fd_);
            fd_ = -1;
            return false;
        }

        mapped_size_ = total_size;
        base_ = static_cast<std::byte*>(mapped);
        init_header();
        return true;
    }

    void close() {
        if (base_ != nullptr) {
            ::munmap(base_, mapped_size_);
            base_ = nullptr;
        }
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
        header_ = nullptr;
        slots_ = nullptr;
        mapped_size_ = 0;
    }

    bool is_open() const noexcept { return base_ != nullptr && header_ != nullptr; }

    void publish(Exchange exchange, const std::string& symbol, int64_t timestamp_ns,
                 double mid_price, const std::vector<PriceLevel>& bids,
                 const std::vector<PriceLevel>& asks) noexcept {
        if (!is_open()) {
            return;
        }

        const uint64_t write_seq = header_->write_seq.load(std::memory_order_relaxed);
        LobSlot& slot = slots_[write_seq % k_capacity];

        slot.exchange_id = static_cast<uint8_t>(exchange);
        std::memset(slot.symbol, 0, sizeof(slot.symbol));
        if (!symbol.empty()) {
            std::strncpy(slot.symbol, symbol.c_str(), sizeof(slot.symbol) - 1);
        }
        slot.timestamp_ns = timestamp_ns;
        slot.mid_price = mid_price;

        for (size_t i = 0; i < 5; ++i) {
            slot.bid_price[i] = (i < bids.size()) ? bids[i].price : 0.0;
            slot.bid_size[i] = (i < bids.size()) ? bids[i].size : 0.0;
            slot.ask_price[i] = (i < asks.size()) ? asks[i].price : 0.0;
            slot.ask_size[i] = (i < asks.size()) ? asks[i].size : 0.0;
        }

        header_->write_seq.store(write_seq + 1, std::memory_order_release);
    }

  private:
    struct alignas(64) Header {
        char magic[8];
        uint32_t version;
        uint32_t slot_size;
        uint32_t capacity;
        uint32_t reserved0;
        std::atomic<uint64_t> write_seq;
        char reserved1[32];
    };

    static_assert(sizeof(Header) == k_header_size, "LobPublisher header must be 64 bytes");

    void init_header() {
        header_ = reinterpret_cast<Header*>(base_);
        slots_ = reinterpret_cast<LobSlot*>(base_ + k_header_size);

        std::memset(base_, 0, mapped_size_);
        std::memcpy(header_->magic, "TRTLOB01", 8);
        header_->version = 1;
        header_->slot_size = static_cast<uint32_t>(sizeof(LobSlot));
        header_->capacity = static_cast<uint32_t>(k_capacity);
        header_->reserved0 = 0;
        header_->write_seq.store(0, std::memory_order_release);
    }

    std::string path_ = k_default_path;
    int fd_ = -1;
    std::byte* base_ = nullptr;
    size_t mapped_size_ = 0;
    Header* header_ = nullptr;
    LobSlot* slots_ = nullptr;
};

} // namespace trading
