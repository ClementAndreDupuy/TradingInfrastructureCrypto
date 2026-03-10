#pragma once

// Shadow Trading Engine — Paper trading with live code path.
//
// Wraps any ExchangeConnector, intercepts submit_order/cancel_order, and
// simulates fills using live order book data. Identical decision logic to
// live; only the order submission is intercepted.
//
// Fill simulation rules:
//   LIMIT BID : fills when best_ask <= order.price  (book crosses)
//   LIMIT ASK : fills when best_bid >= order.price
//   MARKET    : fills immediately at current best ask/bid
//   IOC       : same as LIMIT but cancels if not immediately fillable
//
// Fee accounting: uses the configured maker/taker rates to compute
// realistic net P&L, matching what live trading would produce.
//
// Decision log: every order submission is written to JSONL file with
// full book state, timestamps, and verdict.
//
// Thread safety: submit_order/cancel_order are called from the strategy
// thread; check_fills() is called from the same thread on each book update.

#include "../common/types.hpp"
#include "../common/logging.hpp"
#include "../execution/exchange_connector.hpp"
#include "../execution/order_manager.hpp"
#include "../feeds/book_manager.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>

namespace trading {

struct ShadowConfig {
    double binance_maker_fee_bps = 2.0;
    double binance_taker_fee_bps = 5.0;
    double kraken_maker_fee_bps  = 2.0;
    double kraken_taker_fee_bps  = 5.0;
    char   log_path[256]         = "shadow_decisions.jsonl";
};

// ── Virtual order slot ────────────────────────────────────────────────────────

struct ShadowOrder {
    uint64_t   client_order_id = 0;
    char       symbol[16]      = {};
    Exchange   exchange        = Exchange::UNKNOWN;
    Side       side;
    OrderType  type;
    TimeInForce tif;
    double     price           = 0;
    double     quantity        = 0;
    double     filled_qty      = 0;
    int64_t    submit_ts_ns    = 0;
    bool       active          = false;
    bool       is_maker        = false;  // set when posted limit rests in book
};

// ── Shadow connector (wraps a real connector, intercepts orders) ──────────────

class ShadowConnector : public ExchangeConnector {
public:
    static constexpr size_t MAX_SHADOW_ORDERS = 256;

    explicit ShadowConnector(Exchange ex,
                             const ShadowConfig& cfg,
                             BookManager& book)
        : ex_(ex), cfg_(cfg), book_(book) {
        log_fp_ = std::fopen(cfg.log_path, "a");
    }

    ~ShadowConnector() {
        if (log_fp_) std::fclose(log_fp_);
    }

    Exchange exchange_id() const override { return ex_; }
    bool     is_connected() const override { return true; }

    ConnectorResult connect()    override { return ConnectorResult::OK; }
    void            disconnect() override {}

    ConnectorResult submit_order(const Order& order) override {
        // Find free slot
        ShadowOrder* slot = nullptr;
        for (auto& s : orders_) {
            if (!s.active) { slot = &s; break; }
        }
        if (!slot) {
            LOG_ERROR("Shadow order pool full");
            return ConnectorResult::ERROR_UNKNOWN;
        }

        *slot = {};
        slot->client_order_id = order.client_order_id;
        std::strncpy(slot->symbol, order.symbol, 15);
        slot->exchange    = order.exchange;
        slot->side        = order.side;
        slot->type        = order.type;
        slot->tif         = order.tif;
        slot->price       = order.price;
        slot->quantity    = order.quantity;
        slot->submit_ts_ns = now_ns();
        slot->active      = true;
        slot->is_maker    = false;

        // For IOC market orders, fill immediately at current best
        if (order.type == OrderType::MARKET ||
            (order.tif == TimeInForce::IOC && is_immediately_fillable(*slot))) {
            fill_at_best(*slot);
            return ConnectorResult::OK;
        }

        // Check if limit is immediately fillable (aggressor = taker)
        if (order.type == OrderType::LIMIT && is_immediately_fillable(*slot)) {
            fill_at_best(*slot);
            return ConnectorResult::OK;
        }

        // Otherwise order rests — mark as maker when/if it fills
        slot->is_maker = true;
        log_order(*slot, "RESTING");
        return ConnectorResult::OK;
    }

    ConnectorResult cancel_order(uint64_t client_id) override {
        for (auto& s : orders_) {
            if (s.active && s.client_order_id == client_id) {
                log_order(s, "CANCELED");
                emit_cancel(s);
                s.active = false;
                return ConnectorResult::OK;
            }
        }
        return ConnectorResult::ERROR_UNKNOWN;
    }

    ConnectorResult cancel_all(const char* symbol) override {
        for (auto& s : orders_) {
            if (s.active && std::strncmp(s.symbol, symbol, 15) == 0) {
                emit_cancel(s);
                s.active = false;
            }
        }
        return ConnectorResult::OK;
    }

    ConnectorResult reconcile() override { return ConnectorResult::OK; }

    // Called from strategy thread on every book update.
    // Checks all resting limit orders for fill eligibility.
    void check_fills() {
        for (auto& s : orders_) {
            if (!s.active) continue;
            if (s.type == OrderType::MARKET) { fill_at_best(s); continue; }
            if (is_immediately_fillable(s))  { fill_at_best(s); }
        }
    }

    // Metrics
    double   total_pnl()     const noexcept { return total_pnl_.load(std::memory_order_acquire); }
    uint64_t total_fills()   const noexcept { return total_fills_.load(std::memory_order_acquire); }
    uint32_t active_orders() const noexcept {
        uint32_t n = 0;
        for (const auto& s : orders_) if (s.active) ++n;
        return n;
    }

private:
    Exchange              ex_;
    ShadowConfig          cfg_;
    BookManager&          book_;
    std::FILE*            log_fp_ = nullptr;
    std::array<ShadowOrder, MAX_SHADOW_ORDERS> orders_{};

    std::atomic<double>   total_pnl_{0.0};
    std::atomic<uint64_t> total_fills_{0};

    bool is_immediately_fillable(const ShadowOrder& s) const noexcept {
        if (s.type == OrderType::MARKET) return true;
        if (s.side == Side::BID) {
            double best_ask = book_.best_ask();
            return (best_ask > 0.0 && s.price >= best_ask);
        } else {
            double best_bid = book_.best_bid();
            return (best_bid > 0.0 && s.price <= best_bid);
        }
    }

    void fill_at_best(ShadowOrder& s) {
        double fill_px = (s.side == Side::BID)
                         ? book_.best_ask()   // buy at ask
                         : book_.best_bid();  // sell at bid
        if (fill_px <= 0.0) {
            // No liquidity — treat as rejected
            emit_reject(s, "no_liquidity");
            s.active = false;
            return;
        }

        s.filled_qty = s.quantity;
        s.active     = false;

        // Compute fee based on maker/taker
        double fee_bps = fee_for(s);
        double fee     = (fee_bps / 10000.0) * fill_px * s.quantity;

        // For tracking P&L per fill: positive for sells, negative for buys
        double sign    = (s.side == Side::ASK) ? 1.0 : -1.0;
        double contrib = sign * fill_px * s.quantity - sign * fee;
        double old_pnl = total_pnl_.load(std::memory_order_acquire);
        total_pnl_.store(old_pnl + contrib, std::memory_order_release);
        total_fills_.fetch_add(1, std::memory_order_relaxed);

        log_fill(s, fill_px, fee_bps, fee);
        emit_fill(s, fill_px);
    }

    double fee_for(const ShadowOrder& s) const noexcept {
        bool maker = s.is_maker;
        if (s.exchange == Exchange::BINANCE)
            return maker ? cfg_.binance_maker_fee_bps : cfg_.binance_taker_fee_bps;
        else
            return maker ? cfg_.kraken_maker_fee_bps  : cfg_.kraken_taker_fee_bps;
    }

    void emit_fill(const ShadowOrder& s, double fill_px) {
        FillUpdate u;
        u.client_order_id      = s.client_order_id;
        u.fill_price           = fill_px;
        u.fill_qty             = s.quantity;
        u.cumulative_filled_qty = s.quantity;
        u.avg_fill_price       = fill_px;
        u.new_state            = OrderState::FILLED;
        u.local_ts_ns          = now_ns();
        ExchangeConnector::emit_fill(u);
    }

    void emit_cancel(const ShadowOrder& s) {
        FillUpdate u;
        u.client_order_id = s.client_order_id;
        u.new_state       = OrderState::CANCELED;
        u.local_ts_ns     = now_ns();
        ExchangeConnector::emit_fill(u);
    }

    void emit_reject(const ShadowOrder& s, const char* reason) {
        FillUpdate u;
        u.client_order_id = s.client_order_id;
        u.new_state       = OrderState::REJECTED;
        u.local_ts_ns     = now_ns();
        std::strncpy(u.reject_reason, reason, 63);
        ExchangeConnector::emit_fill(u);
    }

    // ── Decision logging ──────────────────────────────────────────────────────

    void log_order(const ShadowOrder& s, const char* event) {
        if (!log_fp_) return;
        double best_bid = book_.best_bid();
        double best_ask = book_.best_ask();
        std::fprintf(log_fp_,
            "{\"event\":\"%s\",\"ts_ns\":%lld,\"exchange\":\"%s\","
            "\"symbol\":\"%s\",\"side\":\"%s\",\"type\":\"%s\","
            "\"price\":%.2f,\"qty\":%.6f,"
            "\"book_bid\":%.2f,\"book_ask\":%.2f,"
            "\"client_oid\":%llu}\n",
            event,
            (long long)now_ns(),
            exchange_to_string(s.exchange),
            s.symbol,
            s.side == Side::BID ? "BID" : "ASK",
            s.type == OrderType::LIMIT ? "LIMIT" : "MARKET",
            s.price, s.quantity,
            best_bid, best_ask,
            (unsigned long long)s.client_order_id);
        std::fflush(log_fp_);
    }

    void log_fill(const ShadowOrder& s, double fill_px, double fee_bps, double fee) {
        if (!log_fp_) return;
        std::fprintf(log_fp_,
            "{\"event\":\"FILL\",\"ts_ns\":%lld,\"exchange\":\"%s\","
            "\"symbol\":\"%s\",\"side\":\"%s\",\"maker\":%s,"
            "\"fill_px\":%.2f,\"qty\":%.6f,"
            "\"fee_bps\":%.2f,\"fee_usd\":%.6f,"
            "\"cumulative_pnl\":%.6f,\"client_oid\":%llu}\n",
            (long long)now_ns(),
            exchange_to_string(s.exchange),
            s.symbol,
            s.side == Side::BID ? "BID" : "ASK",
            s.is_maker ? "true" : "false",
            fill_px, s.quantity,
            fee_bps, fee,
            total_pnl_.load(std::memory_order_relaxed),
            (unsigned long long)s.client_order_id);
        std::fflush(log_fp_);
    }

    static int64_t now_ns() noexcept {
        using namespace std::chrono;
        return duration_cast<nanoseconds>(
            high_resolution_clock::now().time_since_epoch()).count();
    }
};

// ── ShadowEngine — orchestrates the whole shadow session ─────────────────────

class ShadowEngine {
public:
    ShadowEngine(BookManager& binance_book,
                 BookManager& kraken_book,
                 const ShadowConfig& cfg = {})
        : binance_shadow_(Exchange::BINANCE, cfg, binance_book),
          kraken_shadow_(Exchange::KRAKEN,   cfg, kraken_book),
          binance_book_(binance_book),
          kraken_book_(kraken_book) {}

    ShadowConnector& binance_connector() { return binance_shadow_; }
    ShadowConnector& kraken_connector()  { return kraken_shadow_;  }

    // Call from strategy thread on every book update, after strategy runs.
    void check_fills() {
        binance_shadow_.check_fills();
        kraken_shadow_.check_fills();
    }

    // Combined net P&L across both connectors (buy side negative, sell side positive).
    double net_pnl() const noexcept {
        return binance_shadow_.total_pnl() + kraken_shadow_.total_pnl();
    }

    uint64_t total_fills() const noexcept {
        return binance_shadow_.total_fills() + kraken_shadow_.total_fills();
    }

    void print_summary() const {
        LOG_INFO("Shadow session summary",
                 "net_pnl",     net_pnl(),
                 "total_fills", total_fills(),
                 "bin_active",  binance_shadow_.active_orders(),
                 "kra_active",  kraken_shadow_.active_orders());
    }

private:
    ShadowConnector binance_shadow_;
    ShadowConnector kraken_shadow_;
    BookManager&    binance_book_;
    BookManager&    kraken_book_;
};

}  // namespace trading
