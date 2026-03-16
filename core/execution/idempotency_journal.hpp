#pragma once

#include "../common/logging.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>

namespace trading {

enum class JournalOperation : uint8_t {
    SUBMIT = 0,
    CANCEL = 1,
    REPLACE = 2,
};

enum class JournalState : uint8_t {
    NONE = 0,
    IN_FLIGHT = 1,
    ACKED = 2,
    FAILED = 3,
};

struct JournalDecision {
    JournalState state = JournalState::NONE;
    uint64_t attempt = 0;
    uint64_t related_client_order_id = 0;
    char venue_order_id[64] = {};

    bool should_send_to_venue() const noexcept { return state == JournalState::IN_FLIGHT; }
    bool already_acked() const noexcept { return state == JournalState::ACKED; }
};

class IdempotencyJournal {
  public:
    static constexpr size_t MAX_RECORDS = 256;

    explicit IdempotencyJournal(std::string path) : path_(std::move(path)) { recover(); }

    JournalDecision begin(JournalOperation op, uint64_t client_order_id,
                          uint64_t related_client_order_id = 0) {
        Entry* entry = find_or_create(op, client_order_id);
        if (!entry)
            return {};

        if (entry->state == JournalState::ACKED || entry->state == JournalState::IN_FLIGHT) {
            JournalDecision out;
            out.state = entry->state;
            out.attempt = entry->attempt;
            out.related_client_order_id = entry->related_client_order_id;
            copy_venue_id(*entry, out.venue_order_id, sizeof(out.venue_order_id));
            return out;
        }

        entry->state = JournalState::IN_FLIGHT;
        entry->attempt = saturating_add(entry->attempt, 1);
        entry->related_client_order_id = related_client_order_id;

        append_record(op, client_order_id, related_client_order_id, entry->attempt, entry->state,
                      entry->venue_order_id);

        JournalDecision out;
        out.state = JournalState::IN_FLIGHT;
        out.attempt = entry->attempt;
        out.related_client_order_id = related_client_order_id;
        copy_venue_id(*entry, out.venue_order_id, sizeof(out.venue_order_id));
        return out;
    }

    void ack(JournalOperation op, uint64_t client_order_id, const char* venue_order_id = nullptr,
             uint64_t related_client_order_id = 0) {
        Entry* entry = find_or_create(op, client_order_id);
        if (!entry)
            return;

        entry->state = JournalState::ACKED;
        entry->related_client_order_id = related_client_order_id;
        if (venue_order_id) {
            std::strncpy(entry->venue_order_id, venue_order_id, sizeof(entry->venue_order_id) - 1);
            entry->venue_order_id[sizeof(entry->venue_order_id) - 1] = '\0';
        }

        append_record(op, client_order_id, entry->related_client_order_id, entry->attempt,
                      entry->state, entry->venue_order_id);
    }

    void fail(JournalOperation op, uint64_t client_order_id, uint64_t related_client_order_id = 0) {
        Entry* entry = find_or_create(op, client_order_id);
        if (!entry)
            return;

        entry->state = JournalState::FAILED;
        entry->related_client_order_id = related_client_order_id;
        append_record(op, client_order_id, entry->related_client_order_id, entry->attempt,
                      entry->state, entry->venue_order_id);
    }

    JournalDecision lookup(JournalOperation op, uint64_t client_order_id) const noexcept {
        const Entry* entry = find(op, client_order_id);
        if (!entry)
            return {};
        JournalDecision out;
        out.state = entry->state;
        out.attempt = entry->attempt;
        out.related_client_order_id = entry->related_client_order_id;
        copy_venue_id(*entry, out.venue_order_id, sizeof(out.venue_order_id));
        return out;
    }

    uint32_t in_flight_count() const noexcept {
        uint32_t count = 0;
        for (const auto& e : entries_) {
            if (e.used && e.state == JournalState::IN_FLIGHT)
                ++count;
        }
        return count;
    }

    void recover() {
        for (auto& e : entries_)
            e = Entry{};

        std::ifstream in(path_);
        if (!in.good())
            return;

        uint32_t version = 0;
        while (in.good()) {
            LogRecord r{};
            in >> version >> r.op >> r.client_order_id >> r.related_client_order_id >> r.attempt >>
                r.state >> r.venue_order_id;
            if (!in.good())
                break;
            if (version != 1)
                continue;

            Entry* entry = find_or_create(static_cast<JournalOperation>(r.op), r.client_order_id);
            if (!entry)
                continue;
            entry->state = static_cast<JournalState>(r.state);
            entry->attempt = r.attempt;
            entry->related_client_order_id = r.related_client_order_id;
            std::strncpy(entry->venue_order_id, r.venue_order_id, sizeof(entry->venue_order_id) - 1);
            entry->venue_order_id[sizeof(entry->venue_order_id) - 1] = '\0';
        }
    }

  private:
    struct Entry {
        bool used = false;
        JournalOperation op = JournalOperation::SUBMIT;
        uint64_t client_order_id = 0;
        uint64_t related_client_order_id = 0;
        uint64_t attempt = 0;
        JournalState state = JournalState::NONE;
        char venue_order_id[64] = {};
    };

    struct LogRecord {
        uint32_t op = 0;
        uint64_t client_order_id = 0;
        uint64_t related_client_order_id = 0;
        uint64_t attempt = 0;
        uint32_t state = 0;
        char venue_order_id[64] = "-";
    };

    static void copy_venue_id(const Entry& entry, char* out, size_t out_size) noexcept {
        if (!out || out_size == 0)
            return;
        std::strncpy(out, entry.venue_order_id, out_size - 1);
        out[out_size - 1] = '\0';
    }

    Entry* find_or_create(JournalOperation op, uint64_t client_order_id) {
        for (auto& e : entries_) {
            if (e.used && e.op == op && e.client_order_id == client_order_id)
                return &e;
        }
        for (auto& e : entries_) {
            if (!e.used) {
                e.used = true;
                e.op = op;
                e.client_order_id = client_order_id;
                e.attempt = 0;
                e.related_client_order_id = 0;
                e.state = JournalState::NONE;
                e.venue_order_id[0] = '\0';
                return &e;
            }
        }
        LOG_ERROR("Idempotency journal full", "path", path_.c_str());
        return nullptr;
    }

    const Entry* find(JournalOperation op, uint64_t client_order_id) const noexcept {
        for (const auto& e : entries_) {
            if (e.used && e.op == op && e.client_order_id == client_order_id)
                return &e;
        }
        return nullptr;
    }

    static uint64_t saturating_add(uint64_t value, uint64_t delta) noexcept {
        if (value > std::numeric_limits<uint64_t>::max() - delta)
            return std::numeric_limits<uint64_t>::max();
        return value + delta;
    }

    void append_record(JournalOperation op, uint64_t client_order_id, uint64_t related_client_order_id,
                       uint64_t attempt, JournalState state, const char* venue_order_id) const {
        std::ofstream out(path_, std::ios::app);
        if (!out.good()) {
            LOG_ERROR("Failed writing idempotency journal", "path", path_.c_str());
            return;
        }
        const char* venue_id = (venue_order_id && venue_order_id[0] != '\0') ? venue_order_id : "-";
        out << 1 << ' ' << static_cast<uint32_t>(op) << ' ' << client_order_id << ' '
            << related_client_order_id << ' ' << attempt << ' ' << static_cast<uint32_t>(state) << ' '
            << venue_id << '\n';
        out.flush();
    }

    std::string path_;
    std::array<Entry, MAX_RECORDS> entries_{};
};

} // namespace trading
