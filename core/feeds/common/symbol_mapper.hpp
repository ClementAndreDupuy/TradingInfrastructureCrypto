#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <string>

namespace trading {

struct VenueSymbols {
    std::string binance;
    std::string okx;
    std::string coinbase;
    std::string kraken_ws;
    std::string kraken_rest;
};

class SymbolMapper {
  public:
    static auto map_all(std::string symbol) -> VenueSymbols {
        const auto parsed = parse_symbol(std::move(symbol));

        VenueSymbols out;
        out.binance = parsed.base + parsed.quote;
        out.okx = parsed.base + "-" + parsed.quote;
        out.coinbase = parsed.base + "-" + parsed.quote;
        out.kraken_ws = parsed.base + "/" + parsed.quote;
        out.kraken_rest = out.kraken_ws;
        return out;
    }

  private:
    struct ParsedSymbol {
        std::string base;
        std::string quote;
    };

    static auto parse_symbol(std::string symbol) -> ParsedSymbol {
        std::transform(symbol.begin(), symbol.end(), symbol.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });

        constexpr std::array<char, 3> separators = {'-', '/', '_'};
        for (char sep : separators) {
            const size_t pos = symbol.find(sep);
            if (pos != std::string::npos && pos > 0 && pos + 1 < symbol.size()) {
                return {symbol.substr(0, pos), symbol.substr(pos + 1)};
            }
        }

        static constexpr std::array<const char*, 17> quote_suffixes = {
            "FDUSD", "USDT", "USDC", "USDK", "TUSD", "BUSD", "DAI", "USD", "EUR",
            "GBP",   "JPY",  "TRY",  "AUD",  "CAD",  "CHF", "BTC", "ETH",
        };

        for (const char* suffix : quote_suffixes) {
            const std::string quote(suffix);
            if (symbol.size() > quote.size() &&
                symbol.compare(symbol.size() - quote.size(), quote.size(), quote) == 0) {
                return {symbol.substr(0, symbol.size() - quote.size()), quote};
            }
        }

        if (symbol.size() > 3) {
            return {symbol.substr(0, symbol.size() - 3), symbol.substr(symbol.size() - 3)};
        }

        return {symbol, ""};
    }
};

} // namespace trading
