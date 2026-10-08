#pragma once

#include "types.hpp"
#include "clock.hpp"

#include <memory>
#include <mutex>
#include <cstdint>

namespace isp {

class ResourceShaper {
public:
    explicit ResourceShaper(ResourcePolicy policy = ResourcePolicy{},
                            std::shared_ptr<IClock> clock = std::make_shared<SystemClock>());

    // State machine update step (called on every monitor poll / tick)
    void update(bool session_active);

    // Queries
    SessionState get_state() const;
    bool is_in_protection() const;
    uint32_t get_background_cpu_weight() const;
    uint32_t get_protected_cpu_weight() const;
    uint32_t get_background_io_weight() const;
    uint32_t get_protected_io_weight() const;

    // Metrics & status
    uint32_t get_aging_steps() const;
    uint32_t get_hysteresis_cancellations() const;
    int64_t get_remaining_hysteresis_seconds() const;

    // Configuration & reset
    void set_policy(const ResourcePolicy& policy);
    ResourcePolicy get_policy() const;
    void reset();

    // Clock accessor
    std::shared_ptr<IClock> get_clock() const { return clock_; }
    void set_clock(std::shared_ptr<IClock> clock) { clock_ = std::move(clock); }

private:
    ResourcePolicy policy_;
    std::shared_ptr<IClock> clock_;
    mutable std::mutex mutex_;

    SessionState state_{SessionState::INACTIVE};

    uint32_t current_bg_cpu_weight_{100};
    uint32_t current_prot_cpu_weight_{100};
    uint32_t current_bg_io_weight_{100};
    uint32_t current_prot_io_weight_{100};

    std::chrono::steady_clock::time_point session_start_time_{};
    std::chrono::steady_clock::time_point last_aging_time_{};
    std::chrono::steady_clock::time_point hysteresis_start_time_{};

    uint32_t aging_steps_count_{0};
    uint32_t hysteresis_cancellations_count_{0};

    // Internal state transition logic (mutex_ must be held)
    void enter_active_locked(const std::chrono::steady_clock::time_point& now);
    void enter_hysteresis_locked(const std::chrono::steady_clock::time_point& now);
    void enter_inactive_locked();
    void perform_aging_locked(const std::chrono::steady_clock::time_point& now);
};

} // namespace isp
