#pragma once

#include "../../../common/types.hpp"

#include <array>
#include <cstddef>

namespace trading {
    struct VenueOrderEntry {
        uint64_t client_order_id;
        char venue_order_id[48];
        Exchange exchange;
        char symbol[16];
        bool active;
    };

    class VenueOrderMap {
    public:
        static constexpr size_t MAX_ENTRIES = 512;

        bool upsert(uint64_t client_order_id, const char *venue_order_id, Exchange exchange,
                    const char *symbol) noexcept {
            VenueOrderEntry *slot = find(client_order_id);
            if (!slot)
                slot = find_free_slot();
            if (!slot)
                return false;

            slot->client_order_id = client_order_id;
            copy_str(slot->venue_order_id, venue_order_id);
            slot->exchange = exchange;
            copy_str(slot->symbol, symbol);
            slot->active = true;
            return true;
        }

        const VenueOrderEntry *get(uint64_t client_order_id) const noexcept {
            for (const auto &entry: entries_) {
                if (entry.active && entry.client_order_id == client_order_id)
                    return &entry;
            }
            return nullptr;
        }

        VenueOrderEntry *find(uint64_t client_order_id) noexcept {
            for (auto &entry: entries_) {
                if (entry.active && entry.client_order_id == client_order_id)
                    return &entry;
            }
            return nullptr;
        }

        bool erase(uint64_t client_order_id) noexcept {
            VenueOrderEntry *slot = find(client_order_id);
            if (!slot)
                return false;
            *slot = {};
            return true;
        }

        void clear() noexcept {
            for (auto &entry: entries_)
                entry = {};
        }

        template<typename Fn>
        void for_each_active(Fn &&fn) const noexcept {
            for (const auto &entry: entries_) {
                if (entry.active)
                    fn(entry);
            }
        }

    private:
        std::array<VenueOrderEntry, MAX_ENTRIES> entries_{};

        VenueOrderEntry *find_free_slot() noexcept {
            for (auto &entry: entries_) {
                if (!entry.active)
                    return &entry;
            }
            return nullptr;
        }

        template<size_t N>
        static void copy_str(char (&dst)[N], const char *src) noexcept {
            if (!src) {
                dst[0] = '\0';
                return;
            }

            size_t i = 0;
            while (i + 1 < N && src[i] != '\0') {
                dst[i] = src[i];
                ++i;
            }
            dst[i] = '\0';
        }
    };
}
