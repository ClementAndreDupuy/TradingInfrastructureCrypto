#include "core/feeds/common/book_manager.hpp"
#include "core/execution/market_maker.hpp"
#include "core/risk/kill_switch.hpp"
#include "core/shadow/shadow_engine.hpp"
#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <functional>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <stdexcept>
#include <vector>

using namespace trading;

namespace {

struct RecordedEvent {
    char type = 'D';
    uint64_t sequence = 0;
    Side side = Side::BID;
    double price = 0.0;
    double size = 0.0;
};


std::string resolve_fixture_path(const std::string& relative) {
    namespace fs = std::filesystem;

    const fs::path direct(relative);
    if (fs::exists(direct)) return direct.string();

    const fs::path from_file = fs::path(__FILE__).parent_path().parent_path() / "data" / "replay" / "binance_btcusdt_replay.csv";
    if (fs::exists(from_file)) return from_file.string();

    const fs::path from_cwd = fs::current_path() / "tests" / "data" / "replay" / "binance_btcusdt_replay.csv";
    if (fs::exists(from_cwd)) return from_cwd.string();

    const fs::path from_build = fs::current_path().parent_path() / "tests" / "data" / "replay" / "binance_btcusdt_replay.csv";
    if (fs::exists(from_build)) return from_build.string();

    throw std::runtime_error("Unable to open replay fixture: " + relative);
}

std::vector<RecordedEvent> load_events(const std::string& path) {
    std::ifstream in(resolve_fixture_path(path));
    if (!in.is_open()) {
        throw std::runtime_error("Unable to open replay fixture: " + path);
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
        event.type = fields[0][0];
        event.sequence = std::stoull(fields[1]);
        event.side = (fields[2] == "BID") ? Side::BID : Side::ASK;
        event.price = std::stod(fields[3]);
        event.size = std::stod(fields[4]);
        events.push_back(event);
    }

    return events;
}

class RecordedFeedHandler {
public:
    using SnapshotCallback = std::function<void(const Snapshot&)>;
    using DeltaCallback = std::function<void(const Delta&)>;

    void set_snapshot_callback(SnapshotCallback cb) { snapshot_cb_ = std::move(cb); }
    void set_delta_callback(DeltaCallback cb) { delta_cb_ = std::move(cb); }

    void replay(const std::vector<RecordedEvent>& events) {
        Snapshot snapshot;
        snapshot.symbol = "BTCUSDT";
        snapshot.exchange = Exchange::BINANCE;

        bool snapshot_sent = false;
        for (const auto& event : events) {
            if (event.type == 'S') {
                snapshot.sequence = event.sequence;
                if (event.side == Side::BID) snapshot.bids.emplace_back(event.price, event.size);
                else snapshot.asks.emplace_back(event.price, event.size);
                continue;
            }

            if (!snapshot_sent && snapshot_cb_) {
                snapshot_cb_(snapshot);
                snapshot_sent = true;
            }

            if (delta_cb_) {
                Delta delta;
                delta.side = event.side;
                delta.price = event.price;
                delta.size = event.size;
                delta.sequence = event.sequence;
                delta_cb_(delta);
            }
        }

        if (!snapshot_sent && snapshot_cb_) snapshot_cb_(snapshot);
    }

private:
    SnapshotCallback snapshot_cb_;
    DeltaCallback delta_cb_;
};

}  // namespace

TEST(FullPipelineReplayTest, FeedBookMakerShadowEndToEndWithReplayData) {
    const auto events = load_events("tests/data/replay/binance_btcusdt_replay.csv");

    BookManager binance_book("BTCUSDT", Exchange::BINANCE, 1.0, 20000);
    BookManager kraken_book("BTCUSDT", Exchange::KRAKEN, 1.0, 20000);

    ShadowConfig cfg;
    std::snprintf(cfg.log_path, sizeof(cfg.log_path), "/tmp/shadow_pipeline_replay.jsonl");
    ShadowEngine shadow_engine(binance_book, kraken_book, cfg);

    OrderManager order_manager(shadow_engine.binance_connector());
    KillSwitch kill_switch;

    MarketMakerConfig mm_cfg;
    std::strncpy(mm_cfg.symbol, "BTCUSDT", sizeof(mm_cfg.symbol) - 1);
    mm_cfg.exchange = Exchange::BINANCE;
    mm_cfg.order_qty = 0.1;
    mm_cfg.max_position = 0.2;
    mm_cfg.half_spread_bps = 1.0;

    NeuralAlphaMarketMaker maker(order_manager, binance_book, kill_switch, mm_cfg);
    maker.set_alpha_signal(10.0, 0.1);  // strong bullish + low risk

    RecordedFeedHandler feed;
    feed.set_snapshot_callback(binance_book.snapshot_handler());
    feed.set_delta_callback([&](const Delta& delta) {
        binance_book.delta_handler()(delta);
        maker.on_book_update();
        shadow_engine.check_fills();
    });

    feed.replay(events);

    EXPECT_TRUE(binance_book.is_ready());
    EXPECT_GT(order_manager.active_order_count(), 0u);
    EXPECT_GE(maker.position(), -mm_cfg.max_position);
    EXPECT_LE(maker.position(), mm_cfg.max_position);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
