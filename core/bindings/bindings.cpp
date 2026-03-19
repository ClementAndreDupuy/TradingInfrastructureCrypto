#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "../common/symbol_mapper.hpp"
#include "../common/types.hpp"
#include "../feeds/binance/binance_feed_handler.hpp"
#include "../feeds/coinbase/coinbase_feed_handler.hpp"
#include "../feeds/kraken/kraken_feed_handler.hpp"
#include "../feeds/okx/okx_feed_handler.hpp"
#include "../orderbook/orderbook.hpp"
#include "../risk/kill_switch.hpp"

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
template <typename... Args> static std::function<void(Args...)> make_safe_cb(py::object cb) {
    auto safe = std::shared_ptr<py::object>(new py::object(std::move(cb)), [](py::object* p) {
        py::gil_scoped_acquire gil;
        delete p;
    });
    return [safe](Args... args) {
        py::gil_scoped_acquire gil;
        try {
            (*safe)(std::forward<Args>(args)...);
        } catch (const py::error_already_set& e) {
            // Python exception in callback — print traceback and clear, do NOT rethrow.
            // Re-raising from a C++ background WebSocket thread would terminate the process.
            // The kill switch should be triggered by the caller if the callback is critical.
            PyErr_PrintEx(0);
        } catch (const std::exception& e) {
            fprintf(stderr, "[trading_core] callback exception: %s\n", e.what());
        }
    };
}

// ── __repr__ helper ───────────────────────────────────────────────────────────

static std::string fmt_double(double v) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(8) << v;
    return oss.str();
}

// ── Feed handler binding helper ───────────────────────────────────────────────
//
// Both BinanceFeedHandler and KrakenFeedHandler expose an identical Python
// interface.  This template registers all shared methods so the concrete
// handler blocks below only need to supply the constructor (which differs by
// default URL values) and the exchange-specific docstring.
//
// Thread model: start() spawns a background WebSocket thread and blocks up to
// 30 s waiting for STREAMING state.  The GIL is released for the entire
// duration of start() and stop() so the background thread can acquire it when
// invoking Python callbacks.  Callbacks are wrapped with a GIL-safe shared_ptr
// deleter so their py::object is always released with the GIL held.
template <typename Handler>
static py::class_<Handler> bind_feed_handler(py::module_& m, const char* class_name,
                                             const char* class_doc) {
    return py::class_<Handler>(m, class_name, class_doc)
        .def(
            "set_snapshot_callback",
            [](Handler& h, py::object cb) {
                h.set_snapshot_callback(make_safe_cb<const Snapshot&>(std::move(cb)));
            },
            py::arg("callback"),
            py::keep_alive<1, 2>(), // keep callback alive as long as handler is alive
            "Register a callback invoked on each full book snapshot. "
            "The handler holds a reference to the callable for its lifetime.")
        .def(
            "set_delta_callback",
            [](Handler& h, py::object cb) {
                h.set_delta_callback(make_safe_cb<const Delta&>(std::move(cb)));
            },
            py::arg("callback"), py::keep_alive<1, 2>(),
            "Register a callback invoked on each incremental book update.")
        .def(
            "set_error_callback",
            [](Handler& h, py::object cb) {
                h.set_error_callback(make_safe_cb<const std::string&>(std::move(cb)));
            },
            py::arg("callback"), py::keep_alive<1, 2>(),
            "Register a callback invoked on WebSocket errors. Argument is an error string.")
        .def("start", &Handler::start, py::call_guard<py::gil_scoped_release>(),
             "Connect to WebSocket and sync the order book. "
             "BLOCKS up to 30 s waiting for STREAMING state. "
             "Returns Result.SUCCESS or Result.ERROR_CONNECTION_LOST.")
        .def("stop", &Handler::stop, py::call_guard<py::gil_scoped_release>(),
             "Disconnect and join the background WebSocket thread.")
        .def("is_running", &Handler::is_running)
        .def("get_sequence", &Handler::get_sequence)
        .def("process_message", &Handler::process_message, py::arg("message"),
             "TESTING ONLY — Parse and dispatch a raw WebSocket JSON message directly. "
             "Bypasses the normal feed state machine. Do not call in production; "
             "calling with fabricated data will corrupt the order book state.")
        .def("__enter__",
             [class_name](py::object self) -> py::object {
                 Handler& h = self.cast<Handler&>();
                 Result r;
                 {
                     py::gil_scoped_release release;
                     r = h.start();
                 }
                 if (r != Result::SUCCESS)
                     throw std::runtime_error(std::string(class_name) + ".start() failed");
                 return self;
             })
        .def("__exit__", [](Handler& h, py::object, py::object, py::object) -> bool {
            py::gil_scoped_release release;
            h.stop();
            return false;
        })
        .def_property_readonly("tick_size", &Handler::tick_size,
            "Price tick size fetched from the exchange symbol-info endpoint during start(). "
            "Returns 0.0 if the call has not yet been made or failed. "
            "Pass this value as tick_size when constructing the OrderBook.");
}

PYBIND11_MODULE(trading_core, m) {
    m.doc() = "ThamesRiverTrading C++ pybind11 bridge v1.0.0 — OrderBook, FeedHandlers, "
              "KillSwitch. Requires pybind11>=2.11, Python>=3.10, libwebsockets>=4.0.";
    m.attr("__version__") = "1.0.0";

    // ── Enums ─────────────────────────────────────────────────────────────────
    // Do NOT call export_values() — these are C++ scoped enums (enum class).
    // Always access via qualified name: trading_core.Exchange.BINANCE.

    py::enum_<Exchange>(m, "Exchange")
        .value("BINANCE", Exchange::BINANCE)
        .value("OKX", Exchange::OKX)
        .value("COINBASE", Exchange::COINBASE)
        .value("KRAKEN", Exchange::KRAKEN)
        .value("UNKNOWN", Exchange::UNKNOWN);

    py::enum_<Side>(m, "Side").value("BID", Side::BID).value("ASK", Side::ASK);

    py::enum_<Result>(m, "Result")
        .value("SUCCESS", Result::SUCCESS)
        .value("ERROR_INVALID_SEQUENCE", Result::ERROR_INVALID_SEQUENCE)
        .value("ERROR_INVALID_PRICE", Result::ERROR_INVALID_PRICE)
        .value("ERROR_INVALID_SIZE", Result::ERROR_INVALID_SIZE)
        .value("ERROR_SEQUENCE_GAP", Result::ERROR_SEQUENCE_GAP)
        .value("ERROR_BOOK_CORRUPTED", Result::ERROR_BOOK_CORRUPTED)
        .value("ERROR_CONNECTION_LOST", Result::ERROR_CONNECTION_LOST);

    py::enum_<KillReason>(m, "KillReason")
        .value("MANUAL", KillReason::MANUAL)
        .value("DRAWDOWN", KillReason::DRAWDOWN)
        .value("CIRCUIT_BREAKER", KillReason::CIRCUIT_BREAKER)
        .value("HEARTBEAT_MISSED", KillReason::HEARTBEAT_MISSED)
        .value("BOOK_CORRUPTED", KillReason::BOOK_CORRUPTED);

    // ── SymbolMapper ──────────────────────────────────────────────────────────

    py::class_<VenueSymbols>(m, "VenueSymbols",
        "Venue-specific symbol strings produced by SymbolMapper.map_all().")
        .def_readonly("binance",     &VenueSymbols::binance)
        .def_readonly("okx",         &VenueSymbols::okx)
        .def_readonly("coinbase",    &VenueSymbols::coinbase)
        .def_readonly("kraken_ws",   &VenueSymbols::kraken_ws)
        .def_readonly("kraken_rest", &VenueSymbols::kraken_rest)
        .def("for_exchange", &VenueSymbols::for_exchange, py::arg("exchange"),
             "Return the venue-specific symbol string for the given Exchange.")
        .def("__repr__", [](const VenueSymbols& vs) {
            return "VenueSymbols(binance=" + vs.binance + ", okx=" + vs.okx +
                   ", coinbase=" + vs.coinbase + ", kraken_ws=" + vs.kraken_ws + ")";
        });

    py::class_<SymbolMapper>(m, "SymbolMapper",
        "Converts a canonical symbol (e.g. 'BTCUSDT', 'BTC-USDT', 'BTC/USDT') "
        "to venue-specific formats. All methods are static.")
        .def_static("map_all", &SymbolMapper::map_all, py::arg("symbol"),
                    "Map a canonical symbol to all venue formats. "
                    "Returns VenueSymbols. Raises ValueError on empty symbol.")
        .def_static("map_for_exchange", &SymbolMapper::map_for_exchange,
                    py::arg("exchange"), py::arg("symbol"),
                    "Convenience: map a canonical symbol to a single venue's format.");

    // ── Structs ───────────────────────────────────────────────────────────────

    py::class_<PriceLevel>(m, "PriceLevel")
        .def(py::init<>())
        .def(py::init<double, double>(), py::arg("price"), py::arg("size"))
        .def_readwrite("price", &PriceLevel::price)
        .def_readwrite("size", &PriceLevel::size)
        .def("__eq__", [](const PriceLevel& a,
                          const PriceLevel& b) { return a.price == b.price && a.size == b.size; })
        .def("__repr__", [](const PriceLevel& pl) {
            return "PriceLevel(price=" + fmt_double(pl.price) + ", size=" + fmt_double(pl.size) +
                   ")";
        });

    py::class_<Delta>(m, "Delta")
        .def(py::init<>())
        .def_readwrite("side", &Delta::side)
        .def_readwrite("price", &Delta::price)
        .def_readwrite("size", &Delta::size)
        .def_readwrite("sequence", &Delta::sequence)
        .def_readwrite("timestamp_exchange_ns", &Delta::timestamp_exchange_ns)
        .def_readwrite("timestamp_local_ns", &Delta::timestamp_local_ns)
        .def("__eq__",
             [](const Delta& a, const Delta& b) {
                 return a.side == b.side && a.price == b.price && a.size == b.size &&
                        a.sequence == b.sequence;
             })
        .def("__repr__", [](const Delta& d) {
            std::ostringstream oss;
            oss << "Delta(side=" << (d.side == Side::BID ? "BID" : "ASK")
                << ", price=" << fmt_double(d.price) << ", size=" << fmt_double(d.size)
                << ", seq=" << d.sequence << ")";
            return oss.str();
        });

    py::class_<Snapshot>(m, "Snapshot")
        .def(py::init<>())
        .def_readwrite("symbol", &Snapshot::symbol)
        .def_readwrite("exchange", &Snapshot::exchange)
        .def_readwrite("sequence", &Snapshot::sequence)
        .def_readwrite("bids", &Snapshot::bids)
        .def_readwrite("asks", &Snapshot::asks)
        .def_readwrite("checksum", &Snapshot::checksum)
        .def_readwrite("checksum_present", &Snapshot::checksum_present)
        .def_readwrite("timestamp_exchange_ns", &Snapshot::timestamp_exchange_ns)
        .def_readwrite("timestamp_local_ns", &Snapshot::timestamp_local_ns)
        .def("__repr__", [](const Snapshot& s) {
            std::ostringstream oss;
            oss << "Snapshot(symbol=" << s.symbol << ", seq=" << s.sequence
                << ", bids=" << s.bids.size() << ", asks=" << s.asks.size() << ")";
            return oss.str();
        });

    // ── OrderBook ─────────────────────────────────────────────────────────────

    py::class_<OrderBook>(m, "OrderBook")
        .def(py::init<const std::string&, Exchange, double, size_t>(), py::arg("symbol"),
             py::arg("exchange"), py::arg("tick_size") = 1.0, py::arg("max_levels") = 20000)
        .def("apply_snapshot", &OrderBook::apply_snapshot, py::arg("snapshot"),
             "Apply a full book snapshot, re-centering the price grid.")
        .def("apply_delta", &OrderBook::apply_delta, py::arg("delta"),
             "Apply an incremental book update.")
        .def("get_best_bid", &OrderBook::get_best_bid)
        .def("get_best_ask", &OrderBook::get_best_ask)
        .def("get_mid_price", &OrderBook::get_mid_price)
        .def("get_spread", &OrderBook::get_spread)
        .def("get_sequence", &OrderBook::get_sequence)
        .def("is_initialized", &OrderBook::is_initialized)
        .def(
            "get_top_levels",
            [](const OrderBook& book, size_t n) -> py::tuple {
                std::vector<PriceLevel> bids, asks;
                book.get_top_levels(n, bids, asks);
                return py::make_tuple(bids, asks);
            },
            py::arg("n"), "Returns (bids, asks) as lists of PriceLevel, best-first.")
        .def(
            "get_levels_array",
            [](const OrderBook& book, py::ssize_t n) -> py::tuple {
                // Zero-copy into numpy: returns float64 arrays shape (n_actual, 2).
                // Columns: [price, size].  Preferred over get_top_levels() for
                // bulk research / vectorised backtesting — no Python object
                // allocation per level.
                std::vector<PriceLevel> bids, asks;
                book.get_top_levels(static_cast<size_t>(n), bids, asks);

                auto to_array = [](const std::vector<PriceLevel>& levels) {
                    py::array_t<double> arr({(py::ssize_t)levels.size(), (py::ssize_t)2});
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
        .def_property_readonly("tick_size", &OrderBook::tick_size)
        .def_property_readonly("max_levels", &OrderBook::max_levels)
        .def_property_readonly("base_price", &OrderBook::base_price)
        .def_property_readonly("symbol", &OrderBook::symbol)
        .def_property_readonly("exchange", &OrderBook::exchange);

    // ── KillSwitch ────────────────────────────────────────────────────────────

    py::class_<KillSwitch>(m, "KillSwitch")
        .def(py::init<int64_t>(),
             py::arg("heartbeat_timeout_ns") = KillSwitch::DEFAULT_HEARTBEAT_TIMEOUT_NS)
        .def("is_active", &KillSwitch::is_active,
             "Hot-path check: returns True if kill switch is armed.")
        .def("trigger", &KillSwitch::trigger, py::arg("reason"))
        .def("reset", &KillSwitch::reset, "Operator-only reset after manual intervention.")
        .def("heartbeat", &KillSwitch::heartbeat, "Call from hot-path loop at < 1 s intervals.")
        .def("check_heartbeat", &KillSwitch::check_heartbeat,
             "Call from monitoring thread; triggers kill if heartbeat stalled.")
        .def("get_reason", &KillSwitch::get_reason)
        .def_static("reason_to_string", &KillSwitch::reason_to_string, py::arg("reason"));

    // ── BinanceFeedHandler ────────────────────────────────────────────────────

    bind_feed_handler<BinanceFeedHandler>(
        m, "BinanceFeedHandler",
        "Binance spot order book feed handler using the diff-depth WebSocket stream.")
        .def(py::init<const std::string&, const std::string&, const std::string&,
                      const std::string&, const std::string&>(),
             py::arg("symbol"), py::arg("api_key") = "", py::arg("api_secret") = "",
             py::arg("api_url") = "https://api.binance.com",
             py::arg("ws_url") = "wss://stream.binance.com:9443/ws");

    // ── KrakenFeedHandler ─────────────────────────────────────────────────────

    bind_feed_handler<KrakenFeedHandler>(
        m, "KrakenFeedHandler",
        "Kraken order book feed handler using the WebSocket v2 book channel.")
        .def(py::init<const std::string&, const std::string&, const std::string&,
                      const std::string&, const std::string&>(),
             py::arg("symbol"), py::arg("api_key") = "", py::arg("api_secret") = "",
             py::arg("api_url") = "https://api.kraken.com",
             py::arg("ws_url") = "wss://ws.kraken.com/v2");

    // ── OkxFeedHandler ────────────────────────────────────────────────────────

    bind_feed_handler<OkxFeedHandler>(
        m, "OkxFeedHandler", "OKX order book feed handler using the WebSocket v5 public channel.")
        .def(py::init<const std::string&, const std::string&, const std::string&>(),
             py::arg("symbol"), py::arg("api_url") = "https://www.okx.com",
             py::arg("ws_url") = "wss://ws.okx.com:8443/ws/v5/public");

    // ── CoinbaseFeedHandler ───────────────────────────────────────────────────

    bind_feed_handler<CoinbaseFeedHandler>(
        m, "CoinbaseFeedHandler",
        "Coinbase Advanced Trade order book feed handler using the WebSocket channel.")
        .def(py::init<const std::string&, const std::string&, const std::string&>(),
             py::arg("symbol"),
             py::arg("ws_url") = "wss://advanced-trade-ws.coinbase.com",
             py::arg("api_url") = "https://api.exchange.coinbase.com");
}
