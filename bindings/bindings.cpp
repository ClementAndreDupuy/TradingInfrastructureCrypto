#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/functional.h>

#include "../core/common/types.hpp"
#include "../core/orderbook/orderbook.hpp"
#include "../core/risk/kill_switch.hpp"
#include "../core/feeds/binance/binance_feed_handler.hpp"
#include "../core/feeds/kraken/kraken_feed_handler.hpp"

namespace py = pybind11;
using namespace trading;

PYBIND11_MODULE(trading_core, m) {
    m.doc() = "ThamesRiverTrading C++ pybind11 bridge — OrderBook, FeedHandlers, KillSwitch";

    // ── Enums ─────────────────────────────────────────────────────────────────

    py::enum_<Exchange>(m, "Exchange")
        .value("BINANCE",  Exchange::BINANCE)
        .value("OKX",      Exchange::OKX)
        .value("COINBASE", Exchange::COINBASE)
        .value("KRAKEN",   Exchange::KRAKEN)
        .value("UNKNOWN",  Exchange::UNKNOWN)
        .export_values();

    py::enum_<Side>(m, "Side")
        .value("BID", Side::BID)
        .value("ASK", Side::ASK)
        .export_values();

    py::enum_<Result>(m, "Result")
        .value("SUCCESS",               Result::SUCCESS)
        .value("ERROR_INVALID_SEQUENCE", Result::ERROR_INVALID_SEQUENCE)
        .value("ERROR_INVALID_PRICE",   Result::ERROR_INVALID_PRICE)
        .value("ERROR_INVALID_SIZE",    Result::ERROR_INVALID_SIZE)
        .value("ERROR_SEQUENCE_GAP",    Result::ERROR_SEQUENCE_GAP)
        .value("ERROR_BOOK_CORRUPTED",  Result::ERROR_BOOK_CORRUPTED)
        .value("ERROR_CONNECTION_LOST", Result::ERROR_CONNECTION_LOST)
        .export_values();

    py::enum_<KillReason>(m, "KillReason")
        .value("MANUAL",           KillReason::MANUAL)
        .value("DRAWDOWN",         KillReason::DRAWDOWN)
        .value("CIRCUIT_BREAKER",  KillReason::CIRCUIT_BREAKER)
        .value("HEARTBEAT_MISSED", KillReason::HEARTBEAT_MISSED)
        .value("BOOK_CORRUPTED",   KillReason::BOOK_CORRUPTED)
        .export_values();

    // ── Structs ───────────────────────────────────────────────────────────────

    py::class_<PriceLevel>(m, "PriceLevel")
        .def(py::init<>())
        .def(py::init<double, double>(), py::arg("price"), py::arg("size"))
        .def_readwrite("price", &PriceLevel::price)
        .def_readwrite("size",  &PriceLevel::size)
        .def("__repr__", [](const PriceLevel& pl) {
            return "PriceLevel(price=" + std::to_string(pl.price)
                 + ", size=" + std::to_string(pl.size) + ")";
        });

    py::class_<Delta>(m, "Delta")
        .def(py::init<>())
        .def_readwrite("side",                  &Delta::side)
        .def_readwrite("price",                 &Delta::price)
        .def_readwrite("size",                  &Delta::size)
        .def_readwrite("sequence",              &Delta::sequence)
        .def_readwrite("timestamp_exchange_ns", &Delta::timestamp_exchange_ns)
        .def_readwrite("timestamp_local_ns",    &Delta::timestamp_local_ns);

    py::class_<Snapshot>(m, "Snapshot")
        .def(py::init<>())
        .def_readwrite("symbol",               &Snapshot::symbol)
        .def_readwrite("exchange",             &Snapshot::exchange)
        .def_readwrite("sequence",             &Snapshot::sequence)
        .def_readwrite("bids",                 &Snapshot::bids)
        .def_readwrite("asks",                 &Snapshot::asks)
        .def_readwrite("timestamp_exchange_ns", &Snapshot::timestamp_exchange_ns)
        .def_readwrite("timestamp_local_ns",   &Snapshot::timestamp_local_ns);

    // ── OrderBook ─────────────────────────────────────────────────────────────

    py::class_<OrderBook>(m, "OrderBook")
        .def(py::init<const std::string&, Exchange, double, size_t>(),
             py::arg("symbol"),
             py::arg("exchange"),
             py::arg("tick_size")  = 1.0,
             py::arg("max_levels") = 20000)
        .def("apply_snapshot", &OrderBook::apply_snapshot,
             py::arg("snapshot"),
             "Apply a full book snapshot, re-centering the price grid.")
        .def("apply_delta", &OrderBook::apply_delta,
             py::arg("delta"),
             "Apply an incremental book update.")
        .def("get_best_bid",  &OrderBook::get_best_bid)
        .def("get_best_ask",  &OrderBook::get_best_ask)
        .def("get_mid_price", &OrderBook::get_mid_price)
        .def("get_spread",    &OrderBook::get_spread)
        .def("get_sequence",  &OrderBook::get_sequence)
        .def("is_initialized", &OrderBook::is_initialized)
        .def("get_top_levels",
             [](const OrderBook& book, size_t n) -> py::tuple {
                 std::vector<PriceLevel> bids, asks;
                 book.get_top_levels(n, bids, asks);
                 return py::make_tuple(bids, asks);
             },
             py::arg("n"),
             "Returns (bids, asks) as lists of PriceLevel, best-first.")
        .def_property_readonly("tick_size",  &OrderBook::tick_size)
        .def_property_readonly("max_levels", &OrderBook::max_levels)
        .def_property_readonly("base_price", &OrderBook::base_price)
        .def_property_readonly("symbol",     &OrderBook::symbol)
        .def_property_readonly("exchange",   &OrderBook::exchange);

    // ── KillSwitch ────────────────────────────────────────────────────────────

    py::class_<KillSwitch>(m, "KillSwitch")
        .def(py::init<int64_t>(),
             py::arg("heartbeat_timeout_ns") = KillSwitch::DEFAULT_HEARTBEAT_TIMEOUT_NS)
        .def("is_active",       &KillSwitch::is_active,
             "Hot-path check: returns true if kill switch is armed.")
        .def("trigger",         &KillSwitch::trigger,    py::arg("reason"))
        .def("reset",           &KillSwitch::reset,
             "Operator-only reset after manual intervention.")
        .def("heartbeat",       &KillSwitch::heartbeat,
             "Call from hot-path loop at < 1 s intervals.")
        .def("check_heartbeat", &KillSwitch::check_heartbeat,
             "Call from monitoring thread; triggers kill if heartbeat stalled.")
        .def("get_reason",      &KillSwitch::get_reason)
        .def_static("reason_to_string", &KillSwitch::reason_to_string,
                    py::arg("reason"));

    // ── BinanceFeedHandler ────────────────────────────────────────────────────
    //
    // Callbacks are invoked from the background WebSocket thread, so each
    // wrapper acquires the GIL before calling into Python.  start()/stop()
    // release the GIL so the background thread can acquire it.

    py::class_<BinanceFeedHandler>(m, "BinanceFeedHandler")
        .def(py::init<const std::string&, const std::string&,
                      const std::string&, const std::string&, const std::string&>(),
             py::arg("symbol"),
             py::arg("api_key")    = "",
             py::arg("api_secret") = "",
             py::arg("api_url")    = "https://api.binance.com",
             py::arg("ws_url")     = "wss://stream.binance.com:9443/ws")
        .def("set_snapshot_callback",
             [](BinanceFeedHandler& h, py::object cb) {
                 h.set_snapshot_callback([cb](const Snapshot& snap) {
                     py::gil_scoped_acquire gil;
                     cb(snap);
                 });
             },
             py::arg("callback"))
        .def("set_delta_callback",
             [](BinanceFeedHandler& h, py::object cb) {
                 h.set_delta_callback([cb](const Delta& delta) {
                     py::gil_scoped_acquire gil;
                     cb(delta);
                 });
             },
             py::arg("callback"))
        .def("set_error_callback",
             [](BinanceFeedHandler& h, py::object cb) {
                 h.set_error_callback([cb](const std::string& err) {
                     py::gil_scoped_acquire gil;
                     cb(err);
                 });
             },
             py::arg("callback"))
        .def("start", &BinanceFeedHandler::start,
             py::call_guard<py::gil_scoped_release>(),
             "Connect to Binance WebSocket and begin streaming. Non-blocking.")
        .def("stop", &BinanceFeedHandler::stop,
             py::call_guard<py::gil_scoped_release>(),
             "Disconnect and join the background WebSocket thread.")
        .def("is_running",      &BinanceFeedHandler::is_running)
        .def("get_sequence",    &BinanceFeedHandler::get_sequence)
        .def("process_message", &BinanceFeedHandler::process_message,
             py::arg("message"),
             "Parse and dispatch a raw WebSocket JSON message (for testing).");

    // ── KrakenFeedHandler ─────────────────────────────────────────────────────

    py::class_<KrakenFeedHandler>(m, "KrakenFeedHandler")
        .def(py::init<const std::string&, const std::string&,
                      const std::string&, const std::string&, const std::string&>(),
             py::arg("symbol"),
             py::arg("api_key")    = "",
             py::arg("api_secret") = "",
             py::arg("api_url")    = "https://api.kraken.com",
             py::arg("ws_url")     = "wss://ws.kraken.com/v2")
        .def("set_snapshot_callback",
             [](KrakenFeedHandler& h, py::object cb) {
                 h.set_snapshot_callback([cb](const Snapshot& snap) {
                     py::gil_scoped_acquire gil;
                     cb(snap);
                 });
             },
             py::arg("callback"))
        .def("set_delta_callback",
             [](KrakenFeedHandler& h, py::object cb) {
                 h.set_delta_callback([cb](const Delta& delta) {
                     py::gil_scoped_acquire gil;
                     cb(delta);
                 });
             },
             py::arg("callback"))
        .def("set_error_callback",
             [](KrakenFeedHandler& h, py::object cb) {
                 h.set_error_callback([cb](const std::string& err) {
                     py::gil_scoped_acquire gil;
                     cb(err);
                 });
             },
             py::arg("callback"))
        .def("start", &KrakenFeedHandler::start,
             py::call_guard<py::gil_scoped_release>(),
             "Connect to Kraken WebSocket v2 and begin streaming. Non-blocking.")
        .def("stop", &KrakenFeedHandler::stop,
             py::call_guard<py::gil_scoped_release>(),
             "Disconnect and join the background WebSocket thread.")
        .def("is_running",      &KrakenFeedHandler::is_running)
        .def("get_sequence",    &KrakenFeedHandler::get_sequence)
        .def("process_message", &KrakenFeedHandler::process_message,
             py::arg("message"),
             "Parse and dispatch a raw WebSocket JSON message (for testing).");
}
