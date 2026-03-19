#pragma once

#include <cmath>
#include <string>

namespace trading {

// Parse a tick size string returned by exchange symbol-info endpoints.
//
// Exchange tick sizes are always powers of 10 (e.g. "0.01000000", "0.001",
// "0.10000000").  Using std::stod() on these introduces floating-point
// parsing noise that accumulates in price_to_index() divisions.  Instead,
// count the significant decimal digits in the string and return 10^-n exactly
// — the same representation Kraken uses via pair_decimals.
//
// Falls back to stod() for non-power-of-10 values (e.g. "0.25", "5.0").
inline auto tick_from_string(const std::string& s) -> double {
    auto dot = s.find('.');
    if (dot == std::string::npos) {
        return std::stod(s);
    }

    std::string frac = s.substr(dot + 1);
    while (!frac.empty() && frac.back() == '0') {
        frac.pop_back();
    }

    if (frac.empty()) {
        return std::stod(s);
    }

    // Power-of-10 pattern: trailing char is '1', all preceding are '0'.
    const bool is_power10 =
        frac.back() == '1' && frac.find_first_not_of('0') == frac.size() - 1;
    if (is_power10) {
        return std::pow(10.0, -static_cast<int>(frac.size()));
    }

    return std::stod(s);
}

} // namespace trading
