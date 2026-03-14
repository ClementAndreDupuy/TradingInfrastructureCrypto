#include "core/feeds/book_manager.hpp"
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace trading;

namespace {

struct RecordedEvent {
    char type = 'D';  // S or D
    uint64_t sequence = 0;
    Side side = Side::BID;
    double price = 0.0;
    double size = 0.0;
};

std::vector<RecordedEvent> load_events(const std::string& path) {
    std::ifstream in(path);
    if (!in.is_open()) {
        throw std::runtime_error("Failed to open replay file: " + path);
    }

    std::vector<RecordedEvent> events;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;

        std::stringstream ss(line);
        std::string token;
        std::vector<std::string> fields;
        while (std::getline(ss, token, ',')) fields.push_back(token);
        if (fields.size() != 5) continue;

        RecordedEvent event;
        event.type = fields[0].empty() ? 'D' : fields[0][0];
        event.sequence = std::stoull(fields[1]);
        event.side = (fields[2] == "BID") ? Side::BID : Side::ASK;
        event.price = std::stod(fields[3]);
        event.size = std::stod(fields[4]);
        events.push_back(event);
    }

    return events;
}

std::string book_state_bytes(const BookManager& manager, size_t levels = 8) {
    std::vector<PriceLevel> bids;
    std::vector<PriceLevel> asks;
    manager.get_top_levels(levels, bids, asks);

    std::string bytes;
    bytes.reserve(sizeof(uint64_t) + levels * 4 * sizeof(double));

    auto append_raw = [&bytes](const void* ptr, size_t len) {
        bytes.append(static_cast<const char*>(ptr), len);
    };

    uint64_t seq = manager.book().get_sequence();
    append_raw(&seq, sizeof(seq));

    auto append_level = [&append_raw](const PriceLevel& level) {
        append_raw(&level.price, sizeof(level.price));
        append_raw(&level.size, sizeof(level.size));
    };

    for (size_t i = 0; i < levels; ++i) {
        const PriceLevel bid = (i < bids.size()) ? bids[i] : PriceLevel{};
        const PriceLevel ask = (i < asks.size()) ? asks[i] : PriceLevel{};
        append_level(bid);
        append_level(ask);
    }

    return bytes;
}

std::string replay_once(const std::string& path) {
    const auto events = load_events(path);

    BookManager manager("BTCUSDT", Exchange::BINANCE, 1.0, 20000);
    auto snapshot_cb = manager.snapshot_handler();
    auto delta_cb = manager.delta_handler();

    Snapshot snapshot;
    snapshot.symbol = "BTCUSDT";
    snapshot.exchange = Exchange::BINANCE;

    bool snapshot_applied = false;
    for (const auto& event : events) {
        if (event.type == 'S') {
            snapshot.sequence = event.sequence;
            if (event.side == Side::BID) snapshot.bids.emplace_back(event.price, event.size);
            else snapshot.asks.emplace_back(event.price, event.size);
            continue;
        }

        if (!snapshot_applied) {
            snapshot_cb(snapshot);
            snapshot_applied = true;
        }

        Delta delta;
        delta.side = event.side;
        delta.price = event.price;
        delta.size = event.size;
        delta.sequence = event.sequence;
        delta_cb(delta);
    }

    if (!snapshot_applied) snapshot_cb(snapshot);
    return book_state_bytes(manager);
}

}  // namespace

TEST(FeedReplayTest, DeterministicByteForByteOrderBookState) {
    const std::string replay_file = "tests/data/replay/binance_btcusdt_replay.csv";

    const std::string first = replay_once(replay_file);
    const std::string second = replay_once(replay_file);

    ASSERT_FALSE(first.empty());
    EXPECT_EQ(first, second);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
