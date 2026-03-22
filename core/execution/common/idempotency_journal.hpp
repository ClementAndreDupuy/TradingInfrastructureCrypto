#pragma once

#include "../../common/logging.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <limits>
#include <string>
#include <unistd.h>

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
            Entry *entry = find_or_create(op, client_order_id);
            if (!entry)
                return {};

            if (entry->state == JournalState::ACKED) {
                duplicate_ack_count_ = saturating_add(duplicate_ack_count_, 1);
            }

            if (entry->state == JournalState::ACKED || entry->state == JournalState::IN_FLIGHT) {
                JournalDecision out{};
                populate_decision(*entry, out);
                return out;
            }

            entry->state = JournalState::IN_FLIGHT;
            entry->attempt = saturating_add(entry->attempt, 1);
            entry->related_client_order_id = related_client_order_id;

            append_record(*entry);

            JournalDecision out{};
            populate_decision(*entry, out);
            return out;
        }

        void ack(JournalOperation op, uint64_t client_order_id, const char *venue_order_id = nullptr,
                 uint64_t related_client_order_id = 0) {
            Entry *entry = find_or_create(op, client_order_id);
            if (!entry)
                return;

            if (entry->state == JournalState::ACKED)
                duplicate_ack_count_ = saturating_add(duplicate_ack_count_, 1);

            entry->state = JournalState::ACKED;
            entry->related_client_order_id = related_client_order_id;
            if (venue_order_id) {
                std::strncpy(entry->venue_order_id, venue_order_id, sizeof(entry->venue_order_id) - 1);
                entry->venue_order_id[sizeof(entry->venue_order_id) - 1] = '\0';
            }

            append_record(*entry);
        }

        void fail(JournalOperation op, uint64_t client_order_id, uint64_t related_client_order_id = 0) {
            Entry *entry = find_or_create(op, client_order_id);
            if (!entry)
                return;

            entry->state = JournalState::FAILED;
            entry->related_client_order_id = related_client_order_id;
            append_record(*entry);
        }

        JournalDecision lookup(JournalOperation op, uint64_t client_order_id) const noexcept {
            const Entry *entry = find(op, client_order_id);
            if (!entry)
                return {};

            JournalDecision out{};
            populate_decision(*entry, out);
            return out;
        }

        uint32_t in_flight_count() const noexcept {
            uint32_t count = 0;
            for (const auto &entry: entries_) {
                if (entry.used && entry.state == JournalState::IN_FLIGHT)
                    ++count;
            }
            return count;
        }

        uint64_t duplicate_ack_count() const noexcept { return duplicate_ack_count_; }

        void recover() {
            for (auto &entry: entries_)
                entry = Entry{};
            duplicate_ack_count_ = 0;

            std::ifstream in(path_);
            if (!in.good())
                return;

            while (in.good()) {
                uint32_t version = 0;
                uint32_t op = 0;
                uint64_t client_order_id = 0;
                uint64_t related_client_order_id = 0;
                uint64_t attempt = 0;
                uint32_t state = 0;
                std::string venue_order_id;

                in >> version >> op >> client_order_id >> related_client_order_id >> attempt >> state >>
                        venue_order_id;
                if (!in.good())
                    break;
                if (version != 1)
                    continue;

                Entry *entry = find_or_create(static_cast<JournalOperation>(op), client_order_id);
                if (!entry)
                    continue;

                entry->state = static_cast<JournalState>(state);
                entry->attempt = attempt;
                entry->related_client_order_id = related_client_order_id;
                std::strncpy(entry->venue_order_id, venue_order_id.c_str(),
                             sizeof(entry->venue_order_id) - 1);
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

        static void populate_decision(const Entry &entry, JournalDecision &out) noexcept {
            out.state = entry.state;
            out.attempt = entry.attempt;
            out.related_client_order_id = entry.related_client_order_id;
            std::strncpy(out.venue_order_id, entry.venue_order_id, sizeof(out.venue_order_id) - 1);
            out.venue_order_id[sizeof(out.venue_order_id) - 1] = '\0';
        }

        Entry *find_or_create(JournalOperation op, uint64_t client_order_id) {
            for (auto &entry: entries_) {
                if (entry.used && entry.op == op && entry.client_order_id == client_order_id)
                    return &entry;
            }

            for (auto &entry: entries_) {
                if (!entry.used) {
                    entry.used = true;
                    entry.op = op;
                    entry.client_order_id = client_order_id;
                    return &entry;
                }
            }

            LOG_ERROR("Idempotency journal full", "path", path_.c_str());
            return nullptr;
        }

        const Entry *find(JournalOperation op, uint64_t client_order_id) const noexcept {
            for (const auto &entry: entries_) {
                if (entry.used && entry.op == op && entry.client_order_id == client_order_id)
                    return &entry;
            }
            return nullptr;
        }

        static uint64_t saturating_add(uint64_t value, uint64_t delta) noexcept {
            if (value > std::numeric_limits<uint64_t>::max() - delta)
                return std::numeric_limits<uint64_t>::max();
            return value + delta;
        }

        void append_record(const Entry &entry) const {
            int fd = open(path_.c_str(), O_WRONLY | O_APPEND | O_CREAT, 0644);
            if (fd < 0) {
                LOG_ERROR("Failed writing idempotency journal", "path", path_.c_str());
                return;
            }

            const char *venue_id = (entry.venue_order_id[0] != '\0') ? entry.venue_order_id : "-";
            char line[256] = {};
            const int len = std::snprintf(
                line, sizeof(line), "1 %u %llu %llu %llu %u %s\n", static_cast<unsigned>(entry.op),
                static_cast<unsigned long long>(entry.client_order_id),
                static_cast<unsigned long long>(entry.related_client_order_id),
                static_cast<unsigned long long>(entry.attempt), static_cast<unsigned>(entry.state),
                venue_id);
            if (len > 0) {
                const ssize_t wrote = ::write(fd, line, static_cast<size_t>(len));
                (void) wrote;
                const int sync_rc = ::fsync(fd);
                (void) sync_rc;
            }
            const int close_rc = ::close(fd);
            (void) close_rc;
        }

        std::string path_;
        std::array<Entry, MAX_RECORDS> entries_{};
        uint64_t duplicate_ack_count_ = 0;
    };
}
