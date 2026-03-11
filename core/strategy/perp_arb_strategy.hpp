#pragma once

// Perpetual Futures Cross-Exchange Arbitrage Strategy
//
// Supports two complementary modes selected per tick:
//
//   TAKER  : When net spread > taker_threshold_bps, cross both sides simultaneously
//             with IOC orders. Both legs submitted in under 1 µs of each other.
//
//   MM     : When net spread > mm_spread_target_bps (but below taker threshold),
//             post limit orders inside the book on both exchanges. The filled
//             leg is the "anchor"; the other exchange is hedged via market order.
//
// Fee model (Binance Perp / Kraken Futures):
//   Maker: 2 bps each exchange → 4 bps round-trip
//   Taker: 5 bps each exchange → 10 bps round-trip
//
// Thread safety:
//   on_book_update() — single strategy thread
//   on_fill()        — may arrive from WS receive threads; uses atomic ops

#include "../common/types.hpp"
#include "../common/logging.hpp"
#include "../execution/exchange_connector.hpp"
#include "../execution/order_manager.hpp"
#include "../feeds/book_manager.hpp"
#include "../ipc/alpha_signal.hpp"
#include "../risk/arb_risk_manager.hpp"

#include <atomic>
#include <chrono>
#include <cstring>

namespace trading {

// ── Configuration ────────────────────────────────────────────────────────────

struct PerpArbConfig {
    char   binance_symbol[16]   = "BTCUSDT";   // Binance perp ticker
    char   kraken_symbol[16]    = "PI_XBTUSD";  // Kraken Futures product ID
    char   base_asset[8]        = "BTC";         // For risk book (normalized)

    double trade_qty            = 0.001;          // BTC per leg (≈$65 at $65k BTC)

    // Market-maker settings
    double mm_spread_target_bps = 6.0;           // Min spread to post quotes (must beat 4 bps MM cost)
    double quote_offset_ticks   = 1.0;            // Place quotes N ticks inside best bid/ask
    int64_t quote_ttl_ms        = 500;            // Cancel and re-quote after this interval

    // Taker arb threshold
    double taker_threshold_bps  = 12.0;          // Min net spread for taker trade (beats 10 bps cost)

    // Per-exchange fees (bps)
    double binance_maker_fee    = 2.0;
    double binance_taker_fee    = 5.0;
    double kraken_maker_fee     = 2.0;
    double kraken_taker_fee     = 5.0;

    // Hedge timeout: if hedge not submitted within this after anchor fill, force market
    int64_t hedge_timeout_ms    = 2000;
};

// ── Strategy ─────────────────────────────────────────────────────────────────

class PerpArbStrategy {
public:
    PerpArbStrategy(const PerpArbConfig& cfg,
                    BookManager&         binance_book,
                    BookManager&         kraken_book,
                    ExchangeConnector&   binance_connector,
                    ExchangeConnector&   kraken_connector,
                    ArbRiskManager&      risk,
                    OrderManager&        order_mgr,
                    AlphaSignalReader*   alpha = nullptr)
        : cfg_(cfg),
          binance_book_(binance_book),
          kraken_book_(kraken_book),
          binance_(binance_connector),
          kraken_(kraken_connector),
          risk_(risk),
          order_mgr_(order_mgr),
          alpha_(alpha) {}

    // Call from strategy thread on every book update (either exchange).
    void on_book_update() {
        if (!binance_book_.is_ready() || !kraken_book_.is_ready()) return;
        if (risk_.is_kill_switch_active())                          return;

        risk_.heartbeat();

        const double bin_bid = binance_book_.best_bid();
        const double bin_ask = binance_book_.best_ask();
        const double kra_bid = kraken_book_.best_bid();
        const double kra_ask = kraken_book_.best_ask();

        if (bin_bid <= 0.0 || bin_ask <= 0.0 || kra_bid <= 0.0 || kra_ask <= 0.0) return;

        // Check any pending hedge that might be overdue
        check_hedge_timeout();

        // Taker arb: buy cheaper exchange, sell more expensive exchange
        // Spread = (sell_best_bid - buy_best_ask) / buy_best_ask * 10000
        const double spread_buy_bin  = spread_bps(bin_ask, kra_bid);  // buy Binance, sell Kraken
        const double spread_buy_kra  = spread_bps(kra_ask, bin_bid);  // buy Kraken, sell Binance

        if (spread_buy_bin >= cfg_.taker_threshold_bps) {
            if (!alpha_ || alpha_->allows_long())
                run_taker_arb(bin_ask, kra_bid, Exchange::BINANCE, Exchange::KRAKEN);
            return;
        }
        if (spread_buy_kra >= cfg_.taker_threshold_bps) {
            if (!alpha_ || alpha_->allows_short())
                run_taker_arb(kra_ask, bin_bid, Exchange::KRAKEN, Exchange::BINANCE);
            return;
        }

        // Market maker: quote both sides when spread is wide enough
        const double mid = (bin_bid + bin_ask + kra_bid + kra_ask) / 4.0;
        const double avg_spread_bps = ((bin_ask - bin_bid) / bin_bid +
                                       (kra_ask - kra_bid) / kra_bid) / 2.0 * 10000.0;

        if (avg_spread_bps >= cfg_.mm_spread_target_bps && !hedge_pending_.active)
            if (!alpha_ || alpha_->allows_mm())
                run_market_maker(mid, bin_bid, bin_ask, kra_bid, kra_ask);
    }

    // Call from WS fill callback thread(s).
    void on_fill(const FillUpdate& fill) {
        const OrderState s = fill.new_state;
        if (s != OrderState::FILLED && s != OrderState::CANCELED &&
            s != OrderState::REJECTED) return;

        const uint64_t oid = fill.client_order_id;

        // ── Taker leg fills ──────────────────────────────────────────────────
        if (oid == taker_buy_oid_ || oid == taker_sell_oid_) {
            handle_taker_fill(fill);
            return;
        }

        // ── Market maker anchor fill ──────────────────────────────────────────
        if (quote_.active && (oid == quote_.bin_bid_oid || oid == quote_.kra_bid_oid ||
                              oid == quote_.bin_ask_oid || oid == quote_.kra_ask_oid)) {
            handle_mm_fill(fill);
            return;
        }

        // ── Hedge fill ────────────────────────────────────────────────────────
        if (hedge_pending_.active && oid == hedge_oid_) {
            handle_hedge_fill(fill);
        }
    }

    // Metrics
    double   realized_pnl()   const noexcept { return realized_pnl_.load(std::memory_order_acquire); }
    uint64_t total_trades()   const noexcept { return total_trades_.load(std::memory_order_acquire); }
    uint64_t total_rej()      const noexcept { return total_rej_.load(std::memory_order_acquire); }

private:
    // ── State ─────────────────────────────────────────────────────────────────

    struct QuoteState {
        uint64_t bin_bid_oid = 0;
        uint64_t bin_ask_oid = 0;
        uint64_t kra_bid_oid = 0;
        uint64_t kra_ask_oid = 0;
        double   bin_bid_px  = 0;
        double   bin_ask_px  = 0;
        double   kra_bid_px  = 0;
        double   kra_ask_px  = 0;
        int64_t  posted_ns   = 0;
        bool     active      = false;
    };

    struct HedgePending {
        Exchange hedge_ex;
        Side     hedge_side;
        double   hedge_qty    = 0;
        double   anchor_price = 0;  // filled price of the anchor leg
        int64_t  fill_ts_ns   = 0;
        uint64_t leg_id       = 0;
        bool     active       = false;
    };

    PerpArbConfig      cfg_;
    BookManager&       binance_book_;
    BookManager&       kraken_book_;
    ExchangeConnector& binance_;
    ExchangeConnector& kraken_;
    ArbRiskManager&    risk_;
    OrderManager&      order_mgr_;

    QuoteState   quote_;
    HedgePending hedge_pending_;

    // Taker tracking
    uint64_t taker_buy_oid_  = 0;
    uint64_t taker_sell_oid_ = 0;
    uint64_t taker_leg_id_   = 0;
    uint64_t hedge_oid_      = 0;

    AlphaSignalReader*    alpha_ = nullptr;

    std::atomic<double>   realized_pnl_{0.0};
    std::atomic<uint64_t> total_trades_{0};
    std::atomic<uint64_t> total_rej_{0};

    // ── Mode: Taker Arb ───────────────────────────────────────────────────────

    void run_taker_arb(double buy_px, double sell_px,
                       Exchange buy_ex, Exchange sell_ex) {
        if (taker_buy_oid_ != 0) return;  // already in flight

        cancel_stale_quotes();

        ArbOpportunity opp;
        std::strncpy(opp.symbol, cfg_.base_asset, 15);
        opp.buy_exchange  = buy_ex;
        opp.sell_exchange = sell_ex;
        opp.buy_price     = buy_px;
        opp.sell_price    = sell_px;
        opp.quantity      = cfg_.trade_qty;
        opp.reference_price = (binance_book_.mid_price() + kraken_book_.mid_price()) / 2.0;
        opp.buy_book_ts_ns  = opp.local_ts_ns = opp.sell_book_ts_ns = now_ns();

        RiskVerdict v = risk_.evaluate_arb(opp);
        if (v != RiskVerdict::APPROVED) {
            LOG_DEBUG("Taker arb rejected", "reason", verdict_to_string(v));
            total_rej_.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        taker_leg_id_ = risk_.open_leg(opp);
        if (taker_leg_id_ == 0) return;

        // Submit buy and sell simultaneously
        Order buy  = make_ioc(buy_ex,  symbol_for(buy_ex),  Side::ASK, buy_px,  cfg_.trade_qty);
        Order sell = make_ioc(sell_ex, symbol_for(sell_ex), Side::BID, sell_px, cfg_.trade_qty);

        taker_buy_oid_  = buy.client_order_id;
        taker_sell_oid_ = sell.client_order_id;

        ExchangeConnector& buy_conn  = (buy_ex  == Exchange::BINANCE) ? binance_ : kraken_;
        ExchangeConnector& sell_conn = (sell_ex == Exchange::BINANCE) ? binance_ : kraken_;

        buy_conn.submit_order(buy);
        sell_conn.submit_order(sell);

        LOG_INFO("Taker arb submitted",
                 "buy_ex",  exchange_to_string(buy_ex),
                 "sell_ex", exchange_to_string(sell_ex),
                 "buy_px",  buy_px,
                 "sell_px", sell_px,
                 "spread_bps", spread_bps(buy_px, sell_px));
    }

    void handle_taker_fill(const FillUpdate& fill) {
        const bool is_buy  = (fill.client_order_id == taker_buy_oid_);
        const bool filled  = (fill.new_state == OrderState::FILLED);
        const bool failed  = (fill.new_state == OrderState::REJECTED ||
                              fill.new_state == OrderState::CANCELED);

        if (filled) {
            if (is_buy)
                risk_.on_buy_filled(taker_leg_id_, fill.avg_fill_price, fill.cumulative_filled_qty);
            else
                risk_.on_sell_filled(taker_leg_id_, fill.avg_fill_price, fill.cumulative_filled_qty);
        } else if (failed) {
            risk_.on_leg_rejected(taker_leg_id_);
        }

        // Clear tracking when both legs resolved
        const bool buy_done  = (fill.client_order_id == taker_buy_oid_);
        const bool sell_done = (fill.client_order_id == taker_sell_oid_);
        if (buy_done)  taker_buy_oid_  = 0;
        if (sell_done) taker_sell_oid_ = 0;
        if (taker_buy_oid_ == 0 && taker_sell_oid_ == 0) {
            taker_leg_id_ = 0;
            total_trades_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // ── Mode: Market Maker ────────────────────────────────────────────────────

    void run_market_maker(double mid,
                          double bin_bid, double bin_ask,
                          double kra_bid, double kra_ask) {
        const int64_t ts = now_ns();

        // Re-quote only if TTL expired or no active quotes
        if (quote_.active && (ts - quote_.posted_ns) < cfg_.quote_ttl_ms * 1'000'000LL)
            return;

        cancel_stale_quotes();

        // Post bids/asks inside the book on both exchanges
        const double tick_b = binance_book_.book().tick_size();
        const double tick_k = kraken_book_.book().tick_size();

        const double bin_bid_px = bin_bid + cfg_.quote_offset_ticks * tick_b;
        const double kra_ask_px = kra_ask - cfg_.quote_offset_ticks * tick_k;

        // Only post if the synthetic spread we'd capture is positive after maker fees
        const double mm_round_trip_fees = (cfg_.binance_maker_fee + cfg_.kraken_maker_fee);
        const double captured_spread    = spread_bps(bin_bid_px, kra_ask_px);
        if (captured_spread < mm_round_trip_fees + cfg_.mm_spread_target_bps) return;

        Order bin_bid_ord = make_limit(Exchange::BINANCE, cfg_.binance_symbol,
                                       Side::BID, bin_bid_px, cfg_.trade_qty);
        Order kra_ask_ord = make_limit(Exchange::KRAKEN,  cfg_.kraken_symbol,
                                       Side::ASK, kra_ask_px, cfg_.trade_qty);

        quote_.bin_bid_oid = bin_bid_ord.client_order_id;
        quote_.kra_ask_oid = kra_ask_ord.client_order_id;
        quote_.bin_bid_px  = bin_bid_px;
        quote_.kra_ask_px  = kra_ask_px;
        quote_.posted_ns   = ts;
        quote_.active      = true;

        binance_.submit_order(bin_bid_ord);
        kraken_.submit_order(kra_ask_ord);

        LOG_DEBUG("MM quotes posted",
                  "bin_bid", bin_bid_px, "kra_ask", kra_ask_px,
                  "spread_bps", captured_spread);
    }

    void handle_mm_fill(const FillUpdate& fill) {
        if (fill.new_state != OrderState::FILLED) {
            // Quote rejected/canceled - clear and re-evaluate next tick
            if (fill.client_order_id == quote_.bin_bid_oid) quote_.bin_bid_oid = 0;
            if (fill.client_order_id == quote_.kra_ask_oid) quote_.kra_ask_oid = 0;
            if (fill.client_order_id == quote_.kra_bid_oid) quote_.kra_bid_oid = 0;
            if (fill.client_order_id == quote_.bin_ask_oid) quote_.bin_ask_oid = 0;
            if (!quote_.bin_bid_oid && !quote_.kra_ask_oid &&
                !quote_.kra_bid_oid && !quote_.bin_ask_oid)
                quote_.active = false;
            return;
        }

        // Determine which leg filled and set up hedge
        bool is_bin_bid = (fill.client_order_id == quote_.bin_bid_oid);
        bool is_kra_ask = (fill.client_order_id == quote_.kra_ask_oid);
        bool is_kra_bid = (fill.client_order_id == quote_.kra_bid_oid);
        bool is_bin_ask = (fill.client_order_id == quote_.bin_ask_oid);

        if (is_bin_bid || is_kra_bid) {
            // Bought on Binance/Kraken → hedge: sell on the other exchange
            hedge_pending_.hedge_ex    = is_bin_bid ? Exchange::KRAKEN  : Exchange::BINANCE;
            hedge_pending_.hedge_side  = Side::ASK;
            hedge_pending_.anchor_price = fill.avg_fill_price;
        } else if (is_kra_ask || is_bin_ask) {
            // Sold on Kraken/Binance → hedge: buy on the other exchange
            hedge_pending_.hedge_ex    = is_kra_ask ? Exchange::BINANCE : Exchange::KRAKEN;
            hedge_pending_.hedge_side  = Side::BID;
            hedge_pending_.anchor_price = fill.avg_fill_price;
        } else { return; }

        hedge_pending_.hedge_qty = fill.cumulative_filled_qty;
        hedge_pending_.fill_ts_ns = now_ns();
        hedge_pending_.active     = true;

        // Cancel opposing quote
        cancel_stale_quotes();

        // Submit hedge immediately
        submit_hedge();

        LOG_INFO("MM anchor filled, hedge submitted",
                 "anchor_px", fill.avg_fill_price,
                 "hedge_ex",  exchange_to_string(hedge_pending_.hedge_ex),
                 "hedge_side", side_to_string(hedge_pending_.hedge_side));
    }

    void submit_hedge() {
        if (!hedge_pending_.active) return;

        const char* sym = (hedge_pending_.hedge_ex == Exchange::BINANCE)
                          ? cfg_.binance_symbol : cfg_.kraken_symbol;
        Order hedge = make_market(hedge_pending_.hedge_ex, sym,
                                  hedge_pending_.hedge_side, hedge_pending_.hedge_qty);
        hedge_oid_ = hedge.client_order_id;

        ExchangeConnector& conn = (hedge_pending_.hedge_ex == Exchange::BINANCE)
                                  ? binance_ : kraken_;
        conn.submit_order(hedge);
    }

    void handle_hedge_fill(const FillUpdate& fill) {
        if (fill.new_state == OrderState::FILLED) {
            double buy_px, sell_px;
            double fee_bps;

            if (hedge_pending_.hedge_side == Side::BID) {
                // We sold anchor (maker), bought hedge (taker)
                sell_px = hedge_pending_.anchor_price;
                buy_px  = fill.avg_fill_price;
                fee_bps = cfg_.kraken_maker_fee + cfg_.binance_taker_fee;
            } else {
                // We bought anchor (maker), sold hedge (taker)
                buy_px  = hedge_pending_.anchor_price;
                sell_px = fill.avg_fill_price;
                fee_bps = cfg_.binance_maker_fee + cfg_.kraken_taker_fee;
            }

            double qty      = fill.cumulative_filled_qty;
            double gross    = (sell_px - buy_px) * qty;
            double fee_cost = (fee_bps / 10000.0) * buy_px * qty;
            double net_pnl  = gross - fee_cost;

            double old_pnl = realized_pnl_.load(std::memory_order_acquire);
            realized_pnl_.store(old_pnl + net_pnl, std::memory_order_release);
            total_trades_.fetch_add(1, std::memory_order_relaxed);

            LOG_INFO("MM round-trip complete",
                     "net_pnl",   net_pnl,
                     "gross_pnl", gross,
                     "fee_cost",  fee_cost,
                     "total_pnl", realized_pnl_.load(std::memory_order_relaxed));
        } else {
            LOG_ERROR("Hedge failed", "state", order_state_to_string(fill.new_state),
                      "reason", fill.reject_reason);
        }

        hedge_pending_.active = false;
        hedge_oid_            = 0;
        quote_.active         = false;
    }

    // ── Quote management ──────────────────────────────────────────────────────

    void cancel_stale_quotes() {
        if (!quote_.active) return;

        if (quote_.bin_bid_oid) { binance_.cancel_order(quote_.bin_bid_oid); quote_.bin_bid_oid = 0; }
        if (quote_.bin_ask_oid) { binance_.cancel_order(quote_.bin_ask_oid); quote_.bin_ask_oid = 0; }
        if (quote_.kra_bid_oid) { kraken_.cancel_order(quote_.kra_bid_oid);  quote_.kra_bid_oid = 0; }
        if (quote_.kra_ask_oid) { kraken_.cancel_order(quote_.kra_ask_oid);  quote_.kra_ask_oid = 0; }

        quote_.active = false;
    }

    void check_hedge_timeout() {
        if (!hedge_pending_.active || hedge_oid_ != 0) return;

        int64_t elapsed_ms = (now_ns() - hedge_pending_.fill_ts_ns) / 1'000'000LL;
        if (elapsed_ms > cfg_.hedge_timeout_ms) {
            LOG_WARN("Hedge timeout, force-submitting", "elapsed_ms", elapsed_ms);
            submit_hedge();
        }
    }

    // ── Order builders ────────────────────────────────────────────────────────

    Order make_limit(Exchange ex, const char* symbol, Side side,
                     double price, double qty) {
        Order o;
        o.client_order_id = order_mgr_.next_client_id();
        std::strncpy(o.symbol, symbol, 15);
        o.exchange  = ex;
        o.side      = side;
        o.type      = OrderType::LIMIT;
        o.tif       = TimeInForce::GTC;
        o.price     = price;
        o.quantity  = qty;
        o.state     = OrderState::NEW;
        o.submit_ts_ns = now_ns();
        order_mgr_.add_order(o);
        return o;
    }

    Order make_ioc(Exchange ex, const char* symbol, Side side,
                   double price, double qty) {
        Order o = make_limit(ex, symbol, side, price, qty);
        o.tif   = TimeInForce::IOC;
        return o;
    }

    Order make_market(Exchange ex, const char* symbol, Side side, double qty) {
        Order o;
        o.client_order_id = order_mgr_.next_client_id();
        std::strncpy(o.symbol, symbol, 15);
        o.exchange  = ex;
        o.side      = side;
        o.type      = OrderType::MARKET;
        o.tif       = TimeInForce::IOC;
        o.quantity  = qty;
        o.state     = OrderState::NEW;
        o.submit_ts_ns = now_ns();
        order_mgr_.add_order(o);
        return o;
    }

    // ── Utilities ─────────────────────────────────────────────────────────────

    static double spread_bps(double buy_px, double sell_px) noexcept {
        return (sell_px - buy_px) / buy_px * 10000.0;
    }

    static int64_t now_ns() noexcept {
        using namespace std::chrono;
        return duration_cast<nanoseconds>(
            high_resolution_clock::now().time_since_epoch()).count();
    }

    const char* symbol_for(Exchange ex) const noexcept {
        return (ex == Exchange::BINANCE) ? cfg_.binance_symbol : cfg_.kraken_symbol;
    }
};

}  // namespace trading
