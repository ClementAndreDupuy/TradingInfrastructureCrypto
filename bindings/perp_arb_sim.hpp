#pragma once

// Self-contained C++ simulation engine for the perp-arb strategy.
//
// Uses the real OrderBook (flat-array, cache-friendly) and ArbRiskManager
// (position limits, drawdown, circuit breaker) from the hot path.
// Strategy logic mirrors perp_arb_strategy.hpp exactly.
// Exposed to Python via pybind11 in perp_arb_bindings.cpp.
//
// Key difference from production: fills are simulated against the current
// book snapshot with a configurable latency offset instead of being sent
// to a real exchange.

#include "../core/feeds/book_manager.hpp"
#include "../core/risk/arb_risk_manager.hpp"
#include "../core/strategy/perp_arb_strategy.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numeric>
#include <string>
#include <vector>

namespace trading {

// ── Result types ─────────────────────────────────────────────────────────────

struct SimTrade {
    int64_t     open_ns    = 0;
    int64_t     close_ns   = 0;
    std::string buy_ex;
    std::string sell_ex;
    double      buy_price  = 0.0;
    double      sell_price = 0.0;
    double      qty        = 0.0;
    double      gross_pnl  = 0.0;
    double      fee_usd    = 0.0;
    double      net_pnl    = 0.0;
    std::string mode;   // "TAKER" | "MM"
    std::string risk_verdict;  // "APPROVED" or rejection reason
};

struct SimMetrics {
    int    total_trades       = 0;
    int    taker_trades       = 0;
    int    mm_trades          = 0;
    double total_net_pnl      = 0.0;
    double avg_net_pnl        = 0.0;
    double sharpe_annualized  = 0.0;
    double win_rate           = 0.0;
    double avg_win            = 0.0;
    double avg_loss           = 0.0;
    double profit_factor      = 0.0;
    double max_drawdown_usd   = 0.0;
    double avg_hold_time_ms   = 0.0;
    double gross_pnl          = 0.0;
    double total_fees         = 0.0;
    int    risk_rejections    = 0;
};

inline double hold_time_ms(const SimTrade& t) {
    return static_cast<double>(t.close_ns - t.open_ns) / 1'000'000.0;
}

// ── PerpArbSim ───────────────────────────────────────────────────────────────

class PerpArbSim {
public:
    // latency_ns: simulated one-way order submission latency (default 5 ms)
    PerpArbSim(const PerpArbConfig& cfg, const ArbRiskConfig& risk_cfg,
               int64_t latency_ns = 5'000'000LL)
        : cfg_(cfg),
          risk_(risk_cfg),
          latency_ns_(latency_ns),
          bin_book_("BTCUSDT",   Exchange::BINANCE, 0.1, 10000),
          kra_book_("PI_XBTUSD", Exchange::KRAKEN,  0.5, 10000),
          bin_snap_fn_(bin_book_.snapshot_handler()),
          kra_snap_fn_(kra_book_.snapshot_handler()),
          bin_ts_(0), kra_ts_(0),
          pnl_(0.0), peak_pnl_(0.0), max_dd_(0.0) {}

    // Feed one book-top tick into the simulation.
    void on_tick(int64_t ts_ns, const std::string& exchange,
                 double best_bid, double best_ask,
                 double bid_size,  double ask_size) {
        // Build a single-level snapshot from the tick
        Snapshot snap;
        snap.sequence            = 0;
        snap.timestamp_local_ns  = ts_ns;
        snap.timestamp_exchange_ns = ts_ns;

        if (exchange == "BINANCE") {
            snap.symbol   = "BTCUSDT";
            snap.exchange = Exchange::BINANCE;
            snap.bids.emplace_back(best_bid, bid_size);
            snap.asks.emplace_back(best_ask, ask_size);
            bin_snap_fn_(snap);
            bin_ts_ = ts_ns;
        } else {
            snap.symbol   = "PI_XBTUSD";
            snap.exchange = Exchange::KRAKEN;
            snap.bids.emplace_back(best_bid, bid_size);
            snap.asks.emplace_back(best_ask, ask_size);
            kra_snap_fn_(snap);
            kra_ts_ = ts_ns;
        }

        risk_.heartbeat();
        check_pending_fills(ts_ns);

        if (!bin_book_.is_ready() || !kra_book_.is_ready()) return;
        if (std::abs(bin_ts_ - kra_ts_) > 1'000'000'000LL)  return;
        if (risk_.is_kill_switch_active())                    return;
        if (risk_.is_circuit_breaker_active())                return;

        evaluate(ts_ns);
    }

    const std::vector<SimTrade>& trades()    const { return trades_; }
    int                          rejections() const { return risk_rejections_; }

    SimMetrics metrics() const {
        SimMetrics m;
        m.total_trades    = static_cast<int>(trades_.size());
        m.risk_rejections = risk_rejections_;
        if (trades_.empty()) return m;

        std::vector<double> pnls;
        pnls.reserve(trades_.size());
        double gross = 0, fees = 0;
        double sum_hold = 0;
        int wins = 0;
        double sum_win = 0, sum_loss = 0;

        for (const auto& t : trades_) {
            pnls.push_back(t.net_pnl);
            gross     += t.gross_pnl;
            fees      += t.fee_usd;
            sum_hold  += hold_time_ms(t);
            if (t.net_pnl > 0) { wins++;  sum_win  += t.net_pnl; }
            else                {          sum_loss += t.net_pnl; }
            if (t.mode == "TAKER") m.taker_trades++;
            else                   m.mm_trades++;
        }

        double mean = std::accumulate(pnls.begin(), pnls.end(), 0.0) / pnls.size();
        double var  = 0;
        for (double v : pnls) var += (v - mean) * (v - mean);
        double std  = (pnls.size() > 1) ? std::sqrt(var / (pnls.size() - 1)) : 1e-9;
        if (std < 1e-9) std = 1e-9;

        m.total_net_pnl    = std::accumulate(pnls.begin(), pnls.end(), 0.0);
        m.avg_net_pnl      = mean;
        m.sharpe_annualized = (mean / std) * std::sqrt(252.0 * 24.0);
        m.win_rate         = static_cast<double>(wins) / pnls.size();
        m.avg_win          = wins > 0               ? sum_win  / wins                              : 0;
        m.avg_loss         = (pnls.size() - wins) > 0 ? sum_loss / (pnls.size() - wins)          : 0;
        m.profit_factor    = (sum_loss < 0)         ? sum_win / std::abs(sum_loss)                 : 1e9;
        m.max_drawdown_usd = max_dd_;
        m.avg_hold_time_ms = sum_hold / trades_.size();
        m.gross_pnl        = gross;
        m.total_fees       = fees;
        return m;
    }

private:
    // ── Pending order ────────────────────────────────────────────────────────

    struct PendingOrder {
        int64_t  oid;
        int64_t  eligible_ns;  // ts when latency has elapsed
        Exchange exchange;
        Side     side;
        OrderType type;
        double   price;
        double   qty;
        bool     done    = false;
        bool     filled  = false;
        double   fill_px = 0.0;
        int64_t  fill_ns = 0;
    };

    // ── In-flight state ──────────────────────────────────────────────────────

    struct TakerState {
        bool     active    = false;
        int64_t  open_ns   = 0;
        int64_t  buy_oid   = 0;
        int64_t  sell_oid  = 0;
        Exchange buy_ex    = Exchange::BINANCE;
        Exchange sell_ex   = Exchange::KRAKEN;
        double   buy_fee   = 0.0;  // bps
        double   sell_fee  = 0.0;  // bps
        uint64_t leg_id    = 0;
    };

    struct MMState {
        bool     active       = false;
        int64_t  open_ns      = 0;
        int64_t  quote_ns     = 0;
        int64_t  anchor_oid   = 0;
        Exchange anchor_ex    = Exchange::BINANCE;
        double   anchor_fee   = 0.0;  // bps
    };

    // ── Members ──────────────────────────────────────────────────────────────

    PerpArbConfig  cfg_;
    ArbRiskManager risk_;
    int64_t        latency_ns_;

    BookManager bin_book_;
    BookManager kra_book_;
    std::function<void(const Snapshot&)> bin_snap_fn_;
    std::function<void(const Snapshot&)> kra_snap_fn_;

    int64_t bin_ts_;
    int64_t kra_ts_;

    double  pnl_;
    double  peak_pnl_;
    double  max_dd_;

    int64_t next_oid_ = 1;
    std::vector<PendingOrder> pending_;

    TakerState taker_;
    MMState    mm_;

    std::vector<SimTrade> trades_;
    int risk_rejections_ = 0;

    // ── Strategy evaluation (mirrors PerpArbStrategy::on_book_update) ────────

    void evaluate(int64_t ts_ns) {
        if (taker_.active || mm_.active) return;  // already in flight

        const double bin_bid = bin_book_.best_bid();
        const double bin_ask = bin_book_.best_ask();
        const double kra_bid = kra_book_.best_bid();
        const double kra_ask = kra_book_.best_ask();

        if (bin_bid <= 0 || bin_ask <= 0 || kra_bid <= 0 || kra_ask <= 0) return;

        // ── Taker arb ────────────────────────────────────────────────────────
        const double spread_buy_bin = spread_bps(bin_ask, kra_bid);  // buy Binance, sell Kraken
        const double spread_buy_kra = spread_bps(kra_ask, bin_bid);  // buy Kraken, sell Binance

        if (spread_buy_bin >= cfg_.taker_threshold_bps) {
            run_taker(ts_ns, bin_ask, kra_bid, Exchange::BINANCE, Exchange::KRAKEN);
            return;
        }
        if (spread_buy_kra >= cfg_.taker_threshold_bps) {
            run_taker(ts_ns, kra_ask, bin_bid, Exchange::KRAKEN, Exchange::BINANCE);
            return;
        }

        // ── Market maker ─────────────────────────────────────────────────────
        const double avg_spread = ((bin_ask - bin_bid) / bin_bid +
                                   (kra_ask - kra_bid) / kra_bid) / 2.0 * 10000.0;
        if (avg_spread >= cfg_.mm_spread_target_bps)
            run_mm(ts_ns, bin_bid, bin_ask, kra_bid, kra_ask);
    }

    // ── Taker arb ────────────────────────────────────────────────────────────

    void run_taker(int64_t ts_ns,
                   double buy_px, double sell_px,
                   Exchange buy_ex, Exchange sell_ex) {
        // Position guard (simple: max_abs_position_per_symbol from risk config)
        const double pos_limit = risk_.config().max_abs_position_per_symbol;
        if (std::abs(risk_.get_position(buy_ex,  "BTC") + cfg_.trade_qty) > pos_limit) return;
        if (std::abs(risk_.get_position(sell_ex, "BTC") - cfg_.trade_qty) > pos_limit) return;

        // Pre-trade risk check
        ArbOpportunity opp = make_opp("BTC", buy_ex, sell_ex,
                                      buy_px, sell_px, ts_ns);
        RiskVerdict v = risk_.evaluate_arb(opp);
        if (v != RiskVerdict::APPROVED) {
            risk_rejections_++;
            return;
        }
        uint64_t leg_id = risk_.open_leg(opp);
        if (leg_id == 0) return;

        int64_t exec_ns = ts_ns + latency_ns_;

        taker_.active   = true;
        taker_.open_ns  = ts_ns;
        taker_.buy_ex   = buy_ex;
        taker_.sell_ex  = sell_ex;
        taker_.buy_fee  = (buy_ex  == Exchange::BINANCE) ? cfg_.binance_taker_fee : cfg_.kraken_taker_fee;
        taker_.sell_fee = (sell_ex == Exchange::BINANCE) ? cfg_.binance_taker_fee : cfg_.kraken_taker_fee;
        taker_.leg_id   = leg_id;

        taker_.buy_oid  = emit_ioc(exec_ns, buy_ex,  Side::BID, buy_px,  cfg_.trade_qty);
        taker_.sell_oid = emit_ioc(exec_ns, sell_ex, Side::ASK, sell_px, cfg_.trade_qty);
    }

    void check_taker_fills(int64_t ts_ns) {
        if (!taker_.active) return;

        PendingOrder* buy  = find_order(taker_.buy_oid);
        PendingOrder* sell = find_order(taker_.sell_oid);
        if (!buy || !sell) { taker_.active = false; return; }

        try_ioc_fill(*buy,  ts_ns, book_for(taker_.buy_ex));
        try_ioc_fill(*sell, ts_ns, book_for(taker_.sell_ex));

        if (!buy->done || !sell->done) return;

        if (buy->filled && sell->filled) {
            // Both legs filled: record trade
            double gross = (sell->fill_px - buy->fill_px) * cfg_.trade_qty;
            double mid   = (buy->fill_px + sell->fill_px) / 2.0;
            double fee   = ((taker_.buy_fee + taker_.sell_fee) / 10000.0) * mid * cfg_.trade_qty;
            double net   = gross - fee;

            record_trade("TAKER", taker_.open_ns, ts_ns,
                         taker_.buy_ex, taker_.sell_ex,
                         buy->fill_px, sell->fill_px,
                         gross, fee, net, v_string(RiskVerdict::APPROVED));

            risk_.on_buy_filled(taker_.leg_id, buy->fill_px, cfg_.trade_qty);
            risk_.on_sell_filled(taker_.leg_id, sell->fill_px, cfg_.trade_qty);
        } else if (buy->filled && !sell->filled) {
            // Leg 2 missed: emergency close at current ask (loss)
            double close_px = book_for(taker_.buy_ex).best_ask();
            if (close_px > 0) {
                double loss = (buy->fill_px - close_px) * cfg_.trade_qty;
                pnl_ += loss;
                update_drawdown();
            }
            risk_.on_leg_rejected(taker_.leg_id);
        } else {
            risk_.on_leg_rejected(taker_.leg_id);
        }

        taker_.active = false;
        cleanup_order(taker_.buy_oid);
        cleanup_order(taker_.sell_oid);
    }

    // ── Market maker ─────────────────────────────────────────────────────────

    void run_mm(int64_t ts_ns,
                double bin_bid, double bin_ask,
                double kra_bid, double kra_ask) {
        const double tick_b      = bin_book_.book().tick_size();
        const double bin_bid_px  = bin_bid + cfg_.quote_offset_ticks * tick_b;
        const double kra_ask_px  = kra_ask - cfg_.quote_offset_ticks * bin_book_.book().tick_size();

        const double mm_fees     = cfg_.binance_maker_fee + cfg_.kraken_maker_fee;
        const double captured    = spread_bps(bin_bid_px, kra_ask_px);
        if (captured < mm_fees + cfg_.mm_spread_target_bps) return;

        int64_t exec_ns = ts_ns + latency_ns_;

        mm_.active      = true;
        mm_.open_ns     = ts_ns;
        mm_.quote_ns    = ts_ns;
        mm_.anchor_ex   = Exchange::BINANCE;
        mm_.anchor_fee  = cfg_.binance_maker_fee;
        mm_.anchor_oid  = emit_limit(exec_ns, Exchange::BINANCE, Side::BID,
                                     bin_bid_px, cfg_.trade_qty);
    }

    void check_mm_fills(int64_t ts_ns) {
        if (!mm_.active) return;

        // TTL check: cancel stale quotes
        if ((ts_ns - mm_.quote_ns) > cfg_.quote_ttl_ms * 1'000'000LL) {
            cleanup_order(mm_.anchor_oid);
            mm_.active = false;
            return;
        }

        PendingOrder* anchor = find_order(mm_.anchor_oid);
        if (!anchor || anchor->done) { mm_.active = false; return; }

        // Limit BID fills when best_ask crosses down to anchor price
        const BookManager& book = book_for(mm_.anchor_ex);
        const double best_ask   = book.best_ask();
        if (best_ask > 0 && best_ask <= anchor->price && ts_ns >= anchor->eligible_ns) {
            anchor->fill_px = best_ask;
            anchor->fill_ns = ts_ns;
            anchor->filled  = true;
            anchor->done    = true;

            // Immediate market hedge on Kraken: sell at Kraken bid
            const double hedge_px  = kra_book_.best_bid();
            const double hedge_fee = cfg_.kraken_taker_fee;

            if (hedge_px > 0) {
                double gross = (hedge_px - anchor->fill_px) * cfg_.trade_qty;
                double mid   = (anchor->fill_px + hedge_px) / 2.0;
                double fee   = ((mm_.anchor_fee + hedge_fee) / 10000.0) * mid * cfg_.trade_qty;
                double net   = gross - fee;

                record_trade("MM", mm_.open_ns, ts_ns + latency_ns_,
                             mm_.anchor_ex, Exchange::KRAKEN,
                             anchor->fill_px, hedge_px,
                             gross, fee, net, "APPROVED");
            }

            mm_.active = false;
        }
    }

    // ── Pending fill simulation ───────────────────────────────────────────────

    void check_pending_fills(int64_t ts_ns) {
        check_taker_fills(ts_ns);
        check_mm_fills(ts_ns);
    }

    void try_ioc_fill(PendingOrder& o, int64_t ts_ns, const BookManager& book) {
        if (o.done) return;
        if (ts_ns < o.eligible_ns) return;

        if (o.side == Side::BID) {
            const double ask = book.best_ask();
            if (ask > 0 && o.price >= ask) {
                o.fill_px = ask; o.fill_ns = ts_ns; o.filled = true;
            }
        } else {
            const double bid = book.best_bid();
            if (bid > 0 && o.price <= bid) {
                o.fill_px = bid; o.fill_ns = ts_ns; o.filled = true;
            }
        }
        o.done = true;  // IOC: done whether filled or not
    }

    // ── Order helpers ─────────────────────────────────────────────────────────

    int64_t emit_ioc(int64_t eligible_ns, Exchange ex, Side side,
                     double price, double qty) {
        PendingOrder o;
        o.oid         = next_oid_++;
        o.eligible_ns = eligible_ns;
        o.exchange    = ex;
        o.side        = side;
        o.type        = OrderType::LIMIT;
        o.price       = price;
        o.qty         = qty;
        pending_.push_back(o);
        return o.oid;
    }

    int64_t emit_limit(int64_t eligible_ns, Exchange ex, Side side,
                       double price, double qty) {
        return emit_ioc(eligible_ns, ex, side, price, qty);  // reuse same struct; GTC handled in check_mm_fills
    }

    PendingOrder* find_order(int64_t oid) {
        for (auto& o : pending_)
            if (o.oid == oid) return &o;
        return nullptr;
    }

    void cleanup_order(int64_t oid) {
        pending_.erase(
            std::remove_if(pending_.begin(), pending_.end(),
                           [oid](const PendingOrder& o) { return o.oid == oid; }),
            pending_.end());
    }

    // ── Trade recording ───────────────────────────────────────────────────────

    void record_trade(const std::string& mode,
                      int64_t open_ns, int64_t close_ns,
                      Exchange buy_ex, Exchange sell_ex,
                      double buy_px, double sell_px,
                      double gross, double fee, double net,
                      const std::string& verdict) {
        SimTrade t;
        t.open_ns     = open_ns;
        t.close_ns    = close_ns;
        t.buy_ex      = exchange_to_string(buy_ex);
        t.sell_ex     = exchange_to_string(sell_ex);
        t.buy_price   = buy_px;
        t.sell_price  = sell_px;
        t.qty         = cfg_.trade_qty;
        t.gross_pnl   = gross;
        t.fee_usd     = fee;
        t.net_pnl     = net;
        t.mode        = mode;
        t.risk_verdict = verdict;
        trades_.push_back(t);

        pnl_ += net;
        update_drawdown();
    }

    void update_drawdown() {
        peak_pnl_ = std::max(peak_pnl_, pnl_);
        max_dd_   = std::min(max_dd_,   pnl_ - peak_pnl_);
    }

    // ── Utilities ─────────────────────────────────────────────────────────────

    static double spread_bps(double buy_px, double sell_px) noexcept {
        if (buy_px <= 0) return 0;
        return (sell_px - buy_px) / buy_px * 10000.0;
    }

    const BookManager& book_for(Exchange ex) const {
        return ex == Exchange::BINANCE ? bin_book_ : kra_book_;
    }

    ArbOpportunity make_opp(const char* sym,
                            Exchange buy_ex, Exchange sell_ex,
                            double buy_px, double sell_px,
                            int64_t ts_ns) const {
        ArbOpportunity opp;
        std::strncpy(opp.symbol, sym, 15);
        opp.buy_exchange     = buy_ex;
        opp.sell_exchange    = sell_ex;
        opp.buy_price        = buy_px;
        opp.sell_price       = sell_px;
        opp.quantity         = cfg_.trade_qty;
        opp.reference_price  = (bin_book_.mid_price() + kra_book_.mid_price()) / 2.0;
        opp.buy_book_ts_ns   = bin_ts_;
        opp.sell_book_ts_ns  = kra_ts_;
        opp.local_ts_ns      = ts_ns;
        return opp;
    }

    static std::string v_string(RiskVerdict v) {
        return verdict_to_string(v);
    }
};


}  // namespace trading