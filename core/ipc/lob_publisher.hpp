#pragma once

// LOB feed publisher: C++ core → Python neural model via mmap ring buffer.
//
// Layout of /tmp/trt_lob_feed.bin:
//
//   Header (32 bytes):
//     offset  0 : uint64  magic     — 0x31304F424C545254 ("TRTLOB01")
//     offset  8 : uint32  version   — 1
//     offset 12 : uint32  capacity  — number of slots (10 000)
//     offset 16 : uint32  slot_size — 256
//     offset 20 : uint8[4] _pad
//     offset 24 : uint64  write_seq — atomic, monotonically increasing slot counter
//
//   Ring buffer: capacity × 256-byte slots at offset 32.
//
//   Each slot (256 bytes):
//     offset  0 : uint8    exchange_id  — 0=BINANCE 1=KRAKEN 2=OKX 3=COINBASE
//     offset  1 : char[15] symbol       — null-terminated
//     offset 16 : int64    timestamp_ns — local receive timestamp (ns)
//     offset 24 : double   mid_price
//     offset 32 : double[5] bid_price
//     offset 72 : double[5] bid_size
//     offset 112: double[5] ask_price
//     offset 152: double[5] ask_size
//     offset 192: uint8[64] _pad        — reserved
//
// Write protocol:
//   1. Write slot at index (write_seq % capacity).
//   2. Atomically store (write_seq + 1) with memory_order_release.
//
// Python reader compares its last_seq to write_seq to discover new slots.
// Pre-allocate bids_/asks_ on the BookManager; zero heap in hot path.

#include "../common/types.hpp"
#include <atomic>
#include <cstring>
#include <fcntl.h>
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

namespace trading {

static constexpr uint32_t LOB_FEED_CAPACITY  = 10'000;
static constexpr uint32_t LOB_FEED_SLOT_SIZE = 256;
static constexpr size_t   LOB_FEED_HEADER_SIZE = 32;
static constexpr size_t   LOB_FEED_FILE_SIZE =
    LOB_FEED_HEADER_SIZE + static_cast<size_t>(LOB_FEED_CAPACITY) * LOB_FEED_SLOT_SIZE;

// "TRTLOB01" as little-endian uint64
static constexpr uint64_t LOB_FEED_MAGIC   = 0x31304F424C545254ULL;
static constexpr uint32_t LOB_FEED_VERSION = 1;

// Offsets within the 32-byte header
static constexpr size_t HDR_OFF_MAGIC     = 0;
static constexpr size_t HDR_OFF_VERSION   = 8;
static constexpr size_t HDR_OFF_CAPACITY  = 12;
static constexpr size_t HDR_OFF_SLOT_SIZE = 16;
static constexpr size_t HDR_OFF_WRITE_SEQ = 24;

// Offsets within each slot
static constexpr size_t SLOT_OFF_EXCHANGE   = 0;
static constexpr size_t SLOT_OFF_SYMBOL     = 1;
static constexpr size_t SLOT_OFF_TS_NS      = 16;
static constexpr size_t SLOT_OFF_MID        = 24;
static constexpr size_t SLOT_OFF_BID_PRICE  = 32;
static constexpr size_t SLOT_OFF_BID_SIZE   = 72;
static constexpr size_t SLOT_OFF_ASK_PRICE  = 112;
static constexpr size_t SLOT_OFF_ASK_SIZE   = 152;

static inline uint8_t exchange_to_lob_id(Exchange e) noexcept {
    switch (e) {
    case Exchange::BINANCE:  return 0;
    case Exchange::KRAKEN:   return 1;
    case Exchange::OKX:      return 2;
    case Exchange::COINBASE: return 3;
    default:                 return 255;
    }
}

class LobPublisher {
  public:
    explicit LobPublisher(const std::string& path = "/tmp/trt_lob_feed.bin")
        : path_(path) {}

    ~LobPublisher() { close(); }

    LobPublisher(const LobPublisher&) = delete;
    LobPublisher& operator=(const LobPublisher&) = delete;

    // Create or truncate the mmap file and write the header.
    // Returns false on error; publisher silently skips publish() when not open.
    bool open() {
        fd_ = ::open(path_.c_str(), O_CREAT | O_RDWR, 0644);
        if (fd_ < 0)
            return false;

        if (::ftruncate(fd_, static_cast<off_t>(LOB_FEED_FILE_SIZE)) != 0) {
            ::close(fd_);
            fd_ = -1;
            return false;
        }

        ptr_ = static_cast<char*>(::mmap(
            nullptr, LOB_FEED_FILE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0));
        if (ptr_ == MAP_FAILED) {
            ::close(fd_);
            fd_ = -1;
            ptr_ = nullptr;
            return false;
        }

        // Initialise header fields (non-atomic; no reader yet)
        std::memcpy(ptr_ + HDR_OFF_MAGIC,     &LOB_FEED_MAGIC,   8);
        std::memcpy(ptr_ + HDR_OFF_VERSION,   &LOB_FEED_VERSION, 4);
        std::memcpy(ptr_ + HDR_OFF_CAPACITY,  &LOB_FEED_CAPACITY, 4);
        std::memcpy(ptr_ + HDR_OFF_SLOT_SIZE, &LOB_FEED_SLOT_SIZE, 4);

        // Zero write_seq
        seq_ptr_ = reinterpret_cast<std::atomic<uint64_t>*>(ptr_ + HDR_OFF_WRITE_SEQ);
        seq_ptr_->store(0, std::memory_order_relaxed);

        return true;
    }

    void close() {
        if (ptr_) {
            ::munmap(ptr_, LOB_FEED_FILE_SIZE);
            ptr_    = nullptr;
            seq_ptr_ = nullptr;
        }
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    bool is_open() const noexcept { return ptr_ != nullptr; }

    // Publish one L5 LOB snapshot.
    // top_bids / top_asks must have at least 5 elements (pre-allocated by caller).
    // Hot-path safe: no heap allocation; one memcpy per slot + atomic increment.
    void publish(Exchange exchange,
                 const char* symbol,
                 int64_t timestamp_ns,
                 double mid_price,
                 const std::vector<PriceLevel>& top_bids,
                 const std::vector<PriceLevel>& top_asks) noexcept {
        if (!ptr_) return;

        const uint64_t seq = seq_ptr_->load(std::memory_order_relaxed);
        const size_t slot_idx = static_cast<size_t>(seq % LOB_FEED_CAPACITY);
        char* s = ptr_ + LOB_FEED_HEADER_SIZE + slot_idx * LOB_FEED_SLOT_SIZE;

        // Zero the slot then fill fields at known offsets (no struct needed)
        std::memset(s, 0, LOB_FEED_SLOT_SIZE);

        const uint8_t ex_id = exchange_to_lob_id(exchange);
        std::memcpy(s + SLOT_OFF_EXCHANGE, &ex_id, 1);

        // Copy symbol (up to 14 chars + null terminator, into 15-byte field)
        std::strncpy(s + SLOT_OFF_SYMBOL, symbol, 14);

        std::memcpy(s + SLOT_OFF_TS_NS, &timestamp_ns, 8);
        std::memcpy(s + SLOT_OFF_MID,   &mid_price,    8);

        static constexpr size_t N = 5;
        const size_t n_bids = (top_bids.size() < N) ? top_bids.size() : N;
        const size_t n_asks = (top_asks.size() < N) ? top_asks.size() : N;

        for (size_t i = 0; i < n_bids; ++i) {
            std::memcpy(s + SLOT_OFF_BID_PRICE + i * 8, &top_bids[i].price, 8);
            std::memcpy(s + SLOT_OFF_BID_SIZE  + i * 8, &top_bids[i].size,  8);
        }
        for (size_t i = 0; i < n_asks; ++i) {
            std::memcpy(s + SLOT_OFF_ASK_PRICE + i * 8, &top_asks[i].price, 8);
            std::memcpy(s + SLOT_OFF_ASK_SIZE  + i * 8, &top_asks[i].size,  8);
        }

        // Release store — ensures slot bytes are visible before write_seq increment
        seq_ptr_->store(seq + 1, std::memory_order_release);
    }

  private:
    std::string path_;
    int fd_ = -1;
    char* ptr_ = nullptr;
    std::atomic<uint64_t>* seq_ptr_ = nullptr;
};

} // namespace trading
