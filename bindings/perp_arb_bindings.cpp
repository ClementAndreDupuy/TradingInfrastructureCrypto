// pybind11 bindings for the perp-arb simulation engine.
//
// Build:
//   pip3 install pybind11
//   python3 bindings/setup.py build_ext --inplace
//
// Usage from Python:
//   import perp_arb_sim as cpp
//   cfg      = cpp.PerpArbConfig()
//   risk_cfg = cpp.ArbRiskConfig()
//   engine   = cpp.PerpArbSim(cfg, risk_cfg)
//   engine.on_tick(ts_ns, "BINANCE", bid, ask, bid_sz, ask_sz)
//   trades  = engine.trades()
//   metrics = engine.metrics()

#include "perp_arb_sim.hpp"
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

namespace py = pybind11;
using namespace pybind11::literals;
using namespace trading;

PYBIND11_MODULE(perp_arb_sim, m) {
    m.doc() = "Perp-arb simulation engine (C++ hot-path components via pybind11)";

    // ── PerpArbConfig ─────────────────────────────────────────────────────────
    py::class_<PerpArbConfig>(m, "PerpArbConfig")
        .def(py::init<>())
        .def_readwrite("trade_qty",             &PerpArbConfig::trade_qty)
        .def_readwrite("mm_spread_target_bps",  &PerpArbConfig::mm_spread_target_bps)
        .def_readwrite("quote_offset_ticks",    &PerpArbConfig::quote_offset_ticks)
        .def_readwrite("quote_ttl_ms",          &PerpArbConfig::quote_ttl_ms)
        .def_readwrite("taker_threshold_bps",   &PerpArbConfig::taker_threshold_bps)
        .def_readwrite("binance_maker_fee",     &PerpArbConfig::binance_maker_fee)
        .def_readwrite("binance_taker_fee",     &PerpArbConfig::binance_taker_fee)
        .def_readwrite("kraken_maker_fee",      &PerpArbConfig::kraken_maker_fee)
        .def_readwrite("kraken_taker_fee",      &PerpArbConfig::kraken_taker_fee)
        .def_readwrite("hedge_timeout_ms",      &PerpArbConfig::hedge_timeout_ms)
        .def("__repr__", [](const PerpArbConfig& c) {
            return "<PerpArbConfig trade_qty=" + std::to_string(c.trade_qty)
                 + " taker_bps=" + std::to_string(c.taker_threshold_bps)
                 + " mm_bps="    + std::to_string(c.mm_spread_target_bps) + ">";
        });

    // ── ArbRiskConfig ─────────────────────────────────────────────────────────
    py::class_<ArbRiskConfig>(m, "ArbRiskConfig")
        .def(py::init<>())
        .def_readwrite("max_abs_position_per_symbol",  &ArbRiskConfig::max_abs_position_per_symbol)
        .def_readwrite("max_cross_exchange_exposure",  &ArbRiskConfig::max_cross_exchange_exposure)
        .def_readwrite("max_notional_per_symbol",      &ArbRiskConfig::max_notional_per_symbol)
        .def_readwrite("max_notional_per_exchange",    &ArbRiskConfig::max_notional_per_exchange)
        .def_readwrite("max_portfolio_notional",       &ArbRiskConfig::max_portfolio_notional)
        .def_readwrite("max_drawdown_usd",             &ArbRiskConfig::max_drawdown_usd)
        .def_readwrite("max_orders_per_second",        &ArbRiskConfig::max_orders_per_second)
        .def_readwrite("max_orders_per_minute",        &ArbRiskConfig::max_orders_per_minute)
        .def_readwrite("min_spread_bps",               &ArbRiskConfig::min_spread_bps)
        .def_readwrite("min_profit_usd",               &ArbRiskConfig::min_profit_usd)
        .def_readwrite("max_book_age_ns",              &ArbRiskConfig::max_book_age_ns)
        .def_readwrite("max_open_arb_legs",            &ArbRiskConfig::max_open_arb_legs)
        .def_readwrite("max_price_deviation_bps",      &ArbRiskConfig::max_price_deviation_bps)
        .def_readwrite("circuit_breaker_count",        &ArbRiskConfig::circuit_breaker_count)
        .def_readwrite("circuit_breaker_loss_usd",     &ArbRiskConfig::circuit_breaker_loss_usd)
        .def("__repr__", [](const ArbRiskConfig& c) {
            return "<ArbRiskConfig min_spread=" + std::to_string(c.min_spread_bps)
                 + " min_profit=" + std::to_string(c.min_profit_usd)
                 + " max_dd="     + std::to_string(c.max_drawdown_usd) + ">";
        });

    // ── SimTrade ──────────────────────────────────────────────────────────────
    py::class_<SimTrade>(m, "SimTrade")
        .def_readonly("open_ns",      &SimTrade::open_ns)
        .def_readonly("close_ns",     &SimTrade::close_ns)
        .def_readonly("buy_ex",       &SimTrade::buy_ex)
        .def_readonly("sell_ex",      &SimTrade::sell_ex)
        .def_readonly("buy_price",    &SimTrade::buy_price)
        .def_readonly("sell_price",   &SimTrade::sell_price)
        .def_readonly("qty",          &SimTrade::qty)
        .def_readonly("gross_pnl",    &SimTrade::gross_pnl)
        .def_readonly("fee_usd",      &SimTrade::fee_usd)
        .def_readonly("net_pnl",      &SimTrade::net_pnl)
        .def_readonly("mode",         &SimTrade::mode)
        .def_readonly("risk_verdict", &SimTrade::risk_verdict)
        .def("hold_time_ms", [](const SimTrade& t) { return hold_time_ms(t); })
        .def("to_dict", [](const SimTrade& t) {
            return py::dict(
                "open_ns"_a      = t.open_ns,
                "close_ns"_a     = t.close_ns,
                "buy_exchange"_a = t.buy_ex,
                "sell_exchange"_a= t.sell_ex,
                "buy_price"_a    = t.buy_price,
                "sell_price"_a   = t.sell_price,
                "qty"_a          = t.qty,
                "gross_pnl"_a    = t.gross_pnl,
                "fee"_a          = t.fee_usd,
                "net_pnl"_a      = t.net_pnl,
                "hold_time_ms"_a = hold_time_ms(t),
                "mode"_a         = t.mode
            );
        })
        .def("__repr__", [](const SimTrade& t) {
            return "<SimTrade " + t.mode + " net_pnl=" + std::to_string(t.net_pnl) + ">";
        });

    // ── SimMetrics ────────────────────────────────────────────────────────────
    py::class_<SimMetrics>(m, "SimMetrics")
        .def_readonly("total_trades",       &SimMetrics::total_trades)
        .def_readonly("taker_trades",       &SimMetrics::taker_trades)
        .def_readonly("mm_trades",          &SimMetrics::mm_trades)
        .def_readonly("total_net_pnl",      &SimMetrics::total_net_pnl)
        .def_readonly("avg_net_pnl",        &SimMetrics::avg_net_pnl)
        .def_readonly("sharpe_annualized",  &SimMetrics::sharpe_annualized)
        .def_readonly("win_rate",           &SimMetrics::win_rate)
        .def_readonly("avg_win",            &SimMetrics::avg_win)
        .def_readonly("avg_loss",           &SimMetrics::avg_loss)
        .def_readonly("profit_factor",      &SimMetrics::profit_factor)
        .def_readonly("max_drawdown_usd",   &SimMetrics::max_drawdown_usd)
        .def_readonly("avg_hold_time_ms",   &SimMetrics::avg_hold_time_ms)
        .def_readonly("gross_pnl",          &SimMetrics::gross_pnl)
        .def_readonly("total_fees",         &SimMetrics::total_fees)
        .def_readonly("risk_rejections",    &SimMetrics::risk_rejections)
        .def("to_dict", [](const SimMetrics& m) {
            return py::dict(
                "total_trades"_a      = m.total_trades,
                "taker_trades"_a      = m.taker_trades,
                "mm_trades"_a         = m.mm_trades,
                "total_net_pnl"_a     = m.total_net_pnl,
                "avg_net_pnl"_a       = m.avg_net_pnl,
                "sharpe_annualized"_a = m.sharpe_annualized,
                "win_rate"_a          = m.win_rate,
                "avg_win"_a           = m.avg_win,
                "avg_loss"_a          = m.avg_loss,
                "profit_factor"_a     = m.profit_factor,
                "max_drawdown_usd"_a  = m.max_drawdown_usd,
                "avg_hold_time_ms"_a  = m.avg_hold_time_ms,
                "gross_pnl"_a         = m.gross_pnl,
                "total_fees"_a        = m.total_fees,
                "risk_rejections"_a   = m.risk_rejections
            );
        });

    // ── PerpArbSim ────────────────────────────────────────────────────────────
    py::class_<PerpArbSim>(m, "PerpArbSim")
        .def(py::init<const PerpArbConfig&, const ArbRiskConfig&, int64_t>(),
             py::arg("cfg"), py::arg("risk_cfg"),
             py::arg("latency_ns") = 5'000'000LL)
        .def("on_tick", &PerpArbSim::on_tick,
             py::arg("ts_ns"), py::arg("exchange"),
             py::arg("best_bid"), py::arg("best_ask"),
             py::arg("bid_size"), py::arg("ask_size"),
             "Feed one book-top tick. exchange must be 'BINANCE' or 'KRAKEN'.")
        .def("trades",    &PerpArbSim::trades,
             py::return_value_policy::reference_internal)
        .def("metrics",   &PerpArbSim::metrics)
        .def("rejections",&PerpArbSim::rejections)
        .def("trades_as_dicts", [](const PerpArbSim& sim) {
            py::list result;
            for (const auto& t : sim.trades())
                result.append(py::dict(
                    "open_ns"_a      = t.open_ns,
                    "close_ns"_a     = t.close_ns,
                    "buy_exchange"_a = t.buy_ex,
                    "sell_exchange"_a= t.sell_ex,
                    "buy_price"_a    = t.buy_price,
                    "sell_price"_a   = t.sell_price,
                    "qty"_a          = t.qty,
                    "gross_pnl"_a    = t.gross_pnl,
                    "fee"_a          = t.fee_usd,
                    "net_pnl"_a      = t.net_pnl,
                    "hold_time_ms"_a = hold_time_ms(t),
                    "mode"_a         = t.mode
                ));
            return result;
        }, "Return all trades as a list of dicts (ready for pl.DataFrame()).");
}