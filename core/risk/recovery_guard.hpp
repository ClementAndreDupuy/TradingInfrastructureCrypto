#pragma once

#include "kill_switch.hpp"

#include "../common/logging.hpp"
#include <cstdint>

namespace trading {

struct RecoveryGuardConfig {
    uint32_t max_in_flight_ops = 16;
    uint32_t max_duplicate_acks = 8;
    uint32_t max_cancel_replace_races = 4;
};

class RecoveryGuard {
  public:
    RecoveryGuard(const RecoveryGuardConfig& cfg, KillSwitch& kill_switch)
        : cfg_(cfg), kill_switch_(kill_switch) {}

    bool check(uint32_t in_flight_ops, uint32_t duplicate_acks,
               uint32_t cancel_replace_races) noexcept {
        if (in_flight_ops > cfg_.max_in_flight_ops || duplicate_acks > cfg_.max_duplicate_acks ||
            cancel_replace_races > cfg_.max_cancel_replace_races) {
            LOG_ERROR("[RecoveryGuard] limit breached", "in_flight_ops", in_flight_ops,
                      "max_in_flight_ops", cfg_.max_in_flight_ops, "duplicate_acks", duplicate_acks,
                      "max_duplicate_acks", cfg_.max_duplicate_acks, "cancel_replace_races",
                      cancel_replace_races, "max_cancel_replace_races",
                      cfg_.max_cancel_replace_races);
            kill_switch_.trigger(KillReason::CIRCUIT_BREAKER);
            return false;
        }
        return true;
    }

  private:
    RecoveryGuardConfig cfg_;
    KillSwitch& kill_switch_;
};

} // namespace trading
