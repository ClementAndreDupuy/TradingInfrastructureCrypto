#include <pybind11/pybind11.h>
#include <pybind11/numpy.h>
#include <pybind11/stl.h>

#include "../core/common/types.hpp"
#include "../core/orderbook/orderbook.hpp"
#include "../core/risk/kill_switch.hpp"
#include "../core/feeds/binance/binance_feed_handler.hpp"
#include "../core/feeds/kraken/kraken_feed_handler.hpp"

#include <iomanip>
#include <memory>
#include <sstream>

namespace py = pybind11;
using namespace trading;

// ── GIL-safe callback factory ─────────────────────────────────────────────────
//
// Wraps a Python callable in a shared_ptr whose custom deleter acquires the GIL
// before destroying the py::object.  This guarantees the Python refcount
// decrement is always GIL-protected, even when the owning std::function is
// destroyed from a C++ background thread.
template <typename... Args>
static std::function<void(Args...)> make_safe_cb(py::object cb) {
    auto safe = std::shared_ptr<py::object>(
        new py::object(std::move(cb)),
        [](py::object* p) {
            py::gil_scoped_acquire gil;
            delete p;
        }
    );
    return [safe](Args... args) {
        py::gil_scoped_acquire gil;
        (*safe)(std::forward<Args>(args)...);
    };
}

// ── __repr__ helper ───────────────────────────────────────────────────────────

static std::string fmt_double(double v) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(8) << v;
    return oss.str();
}

PYBIND11_MODULE(trading_core, m) {
    m.doc() = "ThamesRiverTrading C++ pybind11 bridge — OrderBook, FeedHandlers, KillSwitch";

    // ── Enums ─────────────────────────────────────────────────────────────────
    // Do NOT call export_values() — these are C++ scoped enums (enum class).
    // Always access via qualified name: trading_core.Exchange.BINANCE.

    py::enum_<Exchange>(m, "Exchange")
        .value("BINANCE",  Exchange::BINANCE)
        .value("OKX",      Exchange::OKX)
        .value("COINBASE", Exchange::COINBASE)
        .value("KRAKEN",   Exchange::KRAKEN)
        .value("UNKNOWN",  Exchange::UNKNOWN);

    py::enum_<Side>(m, "Side")
        .value("BID", Side::BID)
        .value("ASK", Side::ASK);

    py::enum_<Result>(m, "Result")
        .value("SUCCESS",                Result::SUCCESS)
        .value("ERROR_INVALID_SEQUENCE", Result::ERROR_INVALID_SEQUENCE)
        .value("ERROR_INVALID_PRICE",    Result::ERROR_INVALID_PRICE)
        .value("ERROR_INVALID_SIZE",     Result::ERROR_INVALID_SIZE)
        .value("ERROR_SEQUENCE_GAP",     Result::ERROR_SEQUENCE_GAP)
        .value("ERROR_BOOK_CORRUPTED",   Result::ERROR_BOOK_CORRUPTED)
        .value("ERROR_CONNECTION_LOST",  Result::ERROR_CONNECTION_LOST);

    py::enum_<KillReason>(m, "KillReason")
        .value("MANUAL",           KillReason::MANUAL)
        .value("DRAWDOWN",         KillReason::DRAWDOWN)
        .value("CIRCUIT_BREAKER",  KillReason::CIRCUIT_BREAKER)
        .value("HEARTBEAT_MISSED", KillReason::HEARTBEAT_MISSED)
        .value("BOOK_CORRUPTED",   KillReason::BOOK_CORRUPTED);

    // ── Structs ───────────────────────────────────────────────────────────────

    py::class_<PriceLevel>(m, "PriceLevel")
        .def(py::init<>())
        .def(py::init<double, double>(), py::arg("price"), py::arg("size"))
        .def_readwrite("price", &PriceLevel::price)
        .def_readwrite("size",  &PriceLevel::size)
        .def("__eq__", [](const PriceLevel& a, const PriceLevel& b) {
            return a.price == b.price && a.size == b.size;
        })
        .def("__repr__", [](const PriceLevel& pl) {
            return "PriceLevel(price=" + fmt_double(pl.price)
                 + ", size=" + fmt_double(pl.size) + ")";
        });

    py::class_<Delta>(m, "Delta")
        .def(py::init<>())
        .def_readwrite("side",                  &Delta::side)
        .def_readwrite("price",                 &Delta::price)
        .def_readwrite("size",                  &Delta::size)
        .def_readwrite("sequence",              &Delta::sequence)
        .def_readwrite("timestamp_exchange_ns", &Delta::timestamp_exchange_ns)
        .def_readwrite("timestamp_local_ns",    &Delta::timestamp_local_ns)
        .def("__eq__", [](const Delta& a, const Delta& b) {
            return a.side == b.side && a.price == b.price
                && a.size == b.size && a.sequence == b.sequence;
        })
        .def("__repr__", [](const Delta& d) {
            std::ostringstream oss;
            oss << "Delta(side=" << (d.side == Side::BID ? "BID" : "ASK")
                << ", price=" << fmt_double(d.price)
                << ", size="  << fmt_double(d.size)
                << ", seq="   << d.sequence << ")";
            return oss.str();
        });

    py::class_<Snapshot>(m, "Snapshot")
        .def(py::init<>())
        .def_readwrite("symbol",                &Snapshot::symbol)
        .def_readwrite("exchange",              &Snapshot::exchange)
        .def_readwrite("sequence",              &Snapshot::sequence)
        .def_readwrite("bids",                  &Snapshot::bids)
        .def_readwrite("asks",                  &Snapshot::asks)
        .def_readwrite("timestamp_exchange_ns", &Snapshot::timestamp_exchange_ns)
        .def_readwrite("timestamp_local_ns",    &Snapshot::timestamp_local_ns)
        .def("__repr__", [](const Snapshot& s) {
            std::ostringstream oss;
            oss << "Snapshot(symbol=" << s.symbol
                << ", seq=" << s.sequence
                << ", bids=" << s.bids.size()
                << ", asks=" << s.asks.size() << ")";
            return oss.str();
        });

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
        .def("get_best_bid",   &OrderBook::get_best_bid)
        .def("get_best_ask",   &OrderBook::get_best_ask)
        .def("get_mid_price",  &OrderBook::get_mid_price)
        .def("get_spread",     &OrderBook::get_spread)
        .def("get_sequence",   &OrderBook::get_sequence)
        .def("is_initialized", &OrderBook::is_initialized)
        .def("get_top_levels",
             [](const OrderBook& book, size_t n) -> py::tuple {
                 std::vector<PriceLevel> bids, asks;
                 book.get_top_levels(n, bids, asks);
                 return py::make_tuple(bids, asks);
             },
             py::arg("n"),
             "Returns (bids, asks) as lists of PriceLevel, best-first.")
        .def("get_levels_array",
             [](const OrderBook& book, py::ssize_t n) -> py::tuple {
                 // Zero-copy into numpy: returns float64 arrays shape (n_actual, 2).
                 // Columns: [price, size].  Preferred over get_top_levels() for
                 // bulk research / vectorised backtesting — no Python object
                 // allocation per level.
                 std::vector<PriceLevel> bids, asks;
                 book.get_top_levels(static_cast<size_t>(n), bids, asks);

                 auto to_array = [](const std::vector<PriceLevel>& levels) {
                     py::array_t<double> arr({(py::ssize_t)levels.size(),
                                              (py::ssize_t)2});
                     auto buf = arr.mutable_unchecked<2>();
                     for (py::ssize_t i = 0; i < (py::ssize_t)levels.size(); ++i) {
                         buf(i, 0) = levels[i].price;
                         buf(i, 1) = levels[i].size;
                     }
                     return arr;
                 };
                 return py::make_tuple(to_array(bids), to_array(asks));
             },
             py::arg("n"),
             "Returns (bids, asks) as float64 numpy arrays shape (n_actual, 2). "
             "Columns: [price, size]. Best-first. Use this over get_top_levels() "
             "in research loops — avoids per-level Python object allocation.")
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
             "Hot-path check: returns True if kill switch is armed.")
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
    // Thread model: start() spawns a background WebSocket thread and blocks up
    // to 30 s waiting for STREAMING state.  The GIL is released for the entire
    // duration of start() and stop() so the background thread can acquire it
    // when invoking Python callbacks.  Callbacks are wrapped with a GIL-safe
    // shared_ptr deleter so their py::object is always released with the GIL.

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
                 h.set_snapshot_callback(
                     make_safe_cb<const Snapshot&>(std::move(cb)));
             },
             py::arg("callback"))
        .def("set_delta_callback",
             [](BinanceFeedHandler& h, py::object cb) {
                 h.set_delta_callback(
                     make_safe_cb<const Delta&>(std::move(cb)));
             },
             py::arg("callback"))
        .def("set_error_callback",
             [](BinanceFeedHandler& h, py::object cb) {
                 h.set_error_callback(
                     make_safe_cb<const std::string&>(std::move(cb)));
             },
             py::arg("callback"))
        .def("start", &BinanceFeedHandler::start,
             py::call_guard<py::gil_scoped_release>(),
             "Connect to Binance WebSocket and sync the order book. "
             "BLOCKS up to 30 s waiting for STREAMING state. "
             "Returns Result.SUCCESS or Result.ERROR_CONNECTION_LOST.")
        .def("stop", &BinanceFeedHandler::stop,
             py::call_guard<py::gil_scoped_release>(),
             "Disconnect and join the background WebSocket thread.")
        .def("is_running",      &BinanceFeedHandler::is_running)
        .def("get_sequence",    &BinanceFeedHandler::get_sequence)
        .def("process_message", &BinanceFeedHandler::process_message,
             py::arg("message"),
             "Parse and dispatch a raw WebSocket JSON message (for testing).")
        .def("__enter__",
             [](py::object self) -> py::object {
                 BinanceFeedHandler& h = self.cast<BinanceFeedHandler&>();
                 Result r;
                 {
                     py::gil_scoped_release release;
                     r = h.start();
                 }
                 if (r != Result::SUCCESS)
                     throw std::runtime_error("BinanceFeedHandler.start() failed");
                 return self;
             })
        .def("__exit__",
             [](BinanceFeedHandler& h, py::object, py::object, py::object) -> bool {
                 py::gil_scoped_release release;
                 h.stop();
                 return false;
             });

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
                 h.set_snapshot_callback(
                     make_safe_cb<const Snapshot&>(std::move(cb)));
             },
             py::arg("callback"))
        .def("set_delta_callback",
             [](KrakenFeedHandler& h, py::object cb) {
                 h.set_delta_callback(
                     make_safe_cb<const Delta&>(std::move(cb)));
             },
             py::arg("callback"))
        .def("set_error_callback",
             [](KrakenFeedHandler& h, py::object cb) {
                 h.set_error_callback(
                     make_safe_cb<const std::string&>(std::move(cb)));
             },
             py::arg("callback"))
        .def("start", &KrakenFeedHandler::start,
             py::call_guard<py::gil_scoped_release>(),
             "Connect to Kraken WebSocket v2 and sync the order book. "
             "BLOCKS up to 30 s waiting for STREAMING state. "
             "Returns Result.SUCCESS or Result.ERROR_CONNECTION_LOST.")
        .def("stop", &KrakenFeedHandler::stop,
             py::call_guard<py::gil_scoped_release>(),
             "Disconnect and join the background WebSocket thread.")
        .def("is_running",      &KrakenFeedHandler::is_running)
        .def("get_sequence",    &KrakenFeedHandler::get_sequence)
        .def("process_message", &KrakenFeedHandler::process_message,
             py::arg("message"),
             "Parse and dispatch a raw WebSocket JSON message (for testing).")
        .def("__enter__",
             [](py::object self) -> py::object {
                 KrakenFeedHandler& h = self.cast<KrakenFeedHandler&>();
                 Result r;
                 {
                     py::gil_scoped_release release;
                     r = h.start();
                 }
                 if (r != Result::SUCCESS)
                     throw std::runtime_error("KrakenFeedHandler.start() failed");
                 return self;
             })
        .def("__exit__",
             [](KrakenFeedHandler& h, py::object, py::object, py::object) -> bool {
                 py::gil_scoped_release release;
                 h.stop();
                 return false;
             });
}
