#pragma once

#include "types.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <stdexcept>
#include <string>
#include <string_view>

namespace trading {

struct VenueSymbols {
    std::string binance;
    std::string okx;
    std::string coinbase;
    std::string kraken_ws;
    std::string kraken_rest;

    // Returns the venue-specific symbol string for the given exchange.
    // Kraken uses the REST format (slash-separated) for order submission.
    [[nodiscard]] const std::string& for_exchange(Exchange ex) const noexcept {
        switch (ex) {
        case Exchange::BINANCE:
            return binance;
        case Exchange::OKX:
            return okx;
        case Exchange::COINBASE:
            return coinbase;
        case Exchange::KRAKEN:
            return kraken_rest;
        default:
            return binance;
        }
    }
};

// Converts a canonical symbol string (e.g. "BTCUSDT", "BTC-USDT", "BTC/USDT")
// into venue-specific formats for all supported exchanges.
//
// Throws std::invalid_argument if the symbol is empty.
class SymbolMapper {
  public:
    [[nodiscard]] static VenueSymbols map_all(std::string symbol) {
        const auto parsed = parse_symbol(std::move(symbol));

        VenueSymbols out;
        out.binance     = parsed.base + parsed.quote;
        out.okx         = parsed.base + "-" + parsed.quote;
        out.coinbase    = parsed.base + "-" + parsed.quote;
        out.kraken_ws   = parsed.base + "/" + parsed.quote;
        out.kraken_rest = out.kraken_ws;
        return out;
    }

    // Convenience: map a canonical symbol to a single venue's format.
    [[nodiscard]] static std::string map_for_exchange(Exchange ex, std::string symbol) {
        return map_all(std::move(symbol)).for_exchange(ex);
    }

  private:
    struct ParsedSymbol {
        std::string base;
        std::string quote;
    };

    static ParsedSymbol parse_symbol(std::string symbol) {
        if (symbol.empty()) {
            throw std::invalid_argument("SymbolMapper: symbol must not be empty");
        }

        std::transform(symbol.begin(), symbol.end(), symbol.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });

        // 1. Try explicit separators (-, /, _).
        constexpr std::array<char, 3> separators = {'-', '/', '_'};
        for (const char sep : separators) {
            const size_t pos = symbol.find(sep);
            if (pos != std::string::npos && pos > 0 && pos + 1 < symbol.size()) {
                return {symbol.substr(0, pos), symbol.substr(pos + 1)};
            }
        }

        // 2. Try well-known quote suffixes, longest first to avoid mis-splits
        //    (e.g. "FDUSD" before "USD", "USDT" before "USD").
        static constexpr std::array<std::string_view, 17> quote_suffixes = {
            "FDUSD", "USDT", "USDC", "USDK", "TUSD", "BUSD", "DAI", "USD",
            "EUR",   "GBP",  "JPY",  "TRY",  "AUD",  "CAD",  "CHF", "BTC", "ETH",
        };

        for (const std::string_view suffix : quote_suffixes) {
            if (symbol.size() > suffix.size() &&
                std::string_view(symbol).substr(symbol.size() - suffix.size()) == suffix) {
                return {symbol.substr(0, symbol.size() - suffix.size()), std::string(suffix)};
            }
        }

        // 3. Last resort: treat the final 3 characters as the quote currency.
        if (symbol.size() > 3) {
            return {symbol.substr(0, symbol.size() - 3), symbol.substr(symbol.size() - 3)};
        }

        // Symbol too short to split — return it as base with no quote.
        return {symbol, ""};
    }
};

} // namespace trading
