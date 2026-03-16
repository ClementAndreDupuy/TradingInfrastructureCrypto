#pragma once

#include "../common/types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace trading {

template <typename T, size_t Capacity> struct FixedTable {
    std::array<T, Capacity> items{};
    size_t size = 0;

    bool push(const T& value) {
        if (size >= Capacity)
            return false;
        items[size++] = value;
        return true;
    }

    void clear() { size = 0; }
};

struct ReconciledOrder {
    uint64_t client_order_id = 0;
    char venue_order_id[64] = {};
    char symbol[16] = {};
    Side side = Side::BID;
    double quantity = 0.0;
    double filled_quantity = 0.0;
    double price = 0.0;
    OrderState state = OrderState::PENDING;
};

struct ReconciledFill {
    uint64_t client_order_id = 0;
    char venue_order_id[64] = {};
    char venue_trade_id[64] = {};
    char symbol[16] = {};
    Exchange exchange = Exchange::UNKNOWN;
    Side side = Side::BID;
    double quantity = 0.0;
    double price = 0.0;
    double notional = 0.0;
    double fee = 0.0;
    char fee_asset[16] = {};
    int64_t exchange_ts_ns = 0;
};

struct ReconciledBalance {
    char asset[16] = {};
    double total = 0.0;
    double available = 0.0;
};

struct ReconciledPosition {
    char symbol[16] = {};
    double quantity = 0.0;
    double avg_entry_price = 0.0;
};

struct ReconciliationSnapshot {
    FixedTable<ReconciledOrder, 128> open_orders;
    FixedTable<ReconciledFill, 512> fills;
    FixedTable<ReconciledBalance, 64> balances;
    FixedTable<ReconciledPosition, 64> positions;

    void clear() {
        open_orders.clear();
        fills.clear();
        balances.clear();
        positions.clear();
    }
};

inline void copy_cstr(char* dst, size_t dst_size, const std::string& src) {
    if (dst_size == 0)
        return;
    std::strncpy(dst, src.c_str(), dst_size - 1);
    dst[dst_size - 1] = '\0';
}

} // namespace trading
