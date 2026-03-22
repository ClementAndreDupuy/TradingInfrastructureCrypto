#pragma once

#include <cmath>
#include <string>

namespace trading {
    inline auto tick_from_string(const std::string &s) -> double {
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

        const bool is_power10 =
                frac.back() == '1' && frac.find_first_not_of('0') == frac.size() - 1;
        if (is_power10) {
            return std::pow(10.0, -static_cast<int>(frac.size()));
        }

        return std::stod(s);
    }
}
