#include "resource_shaper.hpp"
#include "logger.hpp"

#include <algorithm>

namespace isp {

ResourceShaper::ResourceShaper(ResourcePolicy policy, std::shared_ptr<IClock> clock)
    : policy_(std::move(policy)), clock_(std::move(clock)) {
    if (!clock_) {
        clock_ = std::make_shared<SystemClock>();
    }
    reset();
}

void ResourceShaper::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    state_ = SessionState::INACTIVE;
    current_bg_cpu_weight_ = 100;
    current_prot_cpu_weight_ = 100;
    current_bg_io_weight_ = 100;
    current_prot_io_weight_ = 100;
    aging_steps_count_ = 0;
    hysteresis_cancellations_count_ = 0;
}

void ResourceShaper::enter_active_locked(const std::chrono::steady_clock::time_point& now) {
    state_ = SessionState::ACTIVE;
    session_start_time_ = now;
    last_aging_time_ = now;

    // Enforce starvation floor on background starting weight
    uint32_t starting_bg = std::max(policy_.starvation_floor_cpu_weight, policy_.background_cpu_weight);
    current_bg_cpu_weight_ = starting_bg;
    current_prot_cpu_weight_ = policy_.protected_cpu_weight;

    current_bg_io_weight_ = std::max(policy_.starvation_floor_cpu_weight, policy_.background_cpu_weight);
    current_prot_io_weight_ = policy_.protected_io_weight;

    ISP_LOG_INFO("ResourceShaper: Entered ACTIVE session. Throttled background CPU weight to "
                 << current_bg_cpu_weight_ << " (floor: " << policy_.starvation_floor_cpu_weight
                 << "), boosted protected CPU weight to " << current_prot_cpu_weight_);
}

void ResourceShaper::enter_hysteresis_locked(const std::chrono::steady_clock::time_point& now) {
    state_ = SessionState::IN_HYSTERESIS;
    hysteresis_start_time_ = now;
    ISP_LOG_INFO("ResourceShaper: Interactive session absent. Entered IN_HYSTERESIS grace period ("
                 << policy_.hysteresis_delay_sec << "s delay before restoring normal resources).");
}

void ResourceShaper::enter_inactive_locked() {
    state_ = SessionState::INACTIVE;
    current_bg_cpu_weight_ = 100;
    current_prot_cpu_weight_ = 100;
    current_bg_io_weight_ = 100;
    current_prot_io_weight_ = 100;
    ISP_LOG_INFO("ResourceShaper: Hysteresis expired. Transitioned to INACTIVE. Restored baseline weights (100).");
}

void ResourceShaper::perform_aging_locked(const std::chrono::steady_clock::time_point& now) {
    if (policy_.aging_interval_sec == 0 || policy_.aging_increment == 0) {
        return;
    }

    auto elapsed_sec = std::chrono::duration_cast<std::chrono::seconds>(now - last_aging_time_).count();
    if (elapsed_sec < static_cast<int64_t>(policy_.aging_interval_sec)) {
        return;
    }

    int64_t intervals = elapsed_sec / policy_.aging_interval_sec;
    for (int64_t i = 0; i < intervals; ++i) {
        if (current_bg_cpu_weight_ < policy_.aging_ceiling_cpu_weight) {
            uint32_t new_weight = current_bg_cpu_weight_ + policy_.aging_increment;
            current_bg_cpu_weight_ = std::min(policy_.aging_ceiling_cpu_weight, new_weight);
            current_bg_io_weight_ = std::min(policy_.aging_ceiling_cpu_weight, current_bg_cpu_weight_);
            ++aging_steps_count_;
            ISP_LOG_INFO("ResourceShaper: [Aging Step " << aging_steps_count_ << "] Background CPU weight increased to "
                         << current_bg_cpu_weight_ << " (ceiling: " << policy_.aging_ceiling_cpu_weight << ")");
        }
    }

    last_aging_time_ += std::chrono::seconds(intervals * policy_.aging_interval_sec);
}

void ResourceShaper::update(bool session_active) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto now = clock_->now();

    switch (state_) {
        case SessionState::INACTIVE: {
            if (session_active) {
                enter_active_locked(now);
            }
            break;
        }

        case SessionState::ACTIVE: {
            if (session_active) {
                perform_aging_locked(now);
            } else {
                enter_hysteresis_locked(now);
            }
            break;
        }

        case SessionState::IN_HYSTERESIS: {
            if (session_active) {
                // Session reappeared! Cancel restoration and resume active session
                ++hysteresis_cancellations_count_;
                state_ = SessionState::ACTIVE;
                ISP_LOG_INFO("ResourceShaper: Session re-established during hysteresis. Pending restoration cancelled.");
                perform_aging_locked(now);
            } else {
                // Check if hysteresis timer has elapsed
                auto elapsed_hysteresis = std::chrono::duration_cast<std::chrono::seconds>(now - hysteresis_start_time_).count();
                if (elapsed_hysteresis >= static_cast<int64_t>(policy_.hysteresis_delay_sec)) {
                    enter_inactive_locked();
                } else {
                    // Still in grace period, aging continues
                    perform_aging_locked(now);
                }
            }
            break;
        }
    }
}

SessionState ResourceShaper::get_state() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

bool ResourceShaper::is_in_protection() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_ == SessionState::ACTIVE || state_ == SessionState::IN_HYSTERESIS;
}

uint32_t ResourceShaper::get_background_cpu_weight() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_bg_cpu_weight_;
}

uint32_t ResourceShaper::get_protected_cpu_weight() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_prot_cpu_weight_;
}

uint32_t ResourceShaper::get_background_io_weight() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_bg_io_weight_;
}

uint32_t ResourceShaper::get_protected_io_weight() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_prot_io_weight_;
}

uint32_t ResourceShaper::get_aging_steps() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return aging_steps_count_;
}

uint32_t ResourceShaper::get_hysteresis_cancellations() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return hysteresis_cancellations_count_;
}

int64_t ResourceShaper::get_remaining_hysteresis_seconds() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != SessionState::IN_HYSTERESIS) {
        return 0;
    }
    auto now = clock_->now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - hysteresis_start_time_).count();
    int64_t remaining = static_cast<int64_t>(policy_.hysteresis_delay_sec) - elapsed;
    return std::max<int64_t>(0, remaining);
}

void ResourceShaper::set_policy(const ResourcePolicy& policy) {
    std::lock_guard<std::mutex> lock(mutex_);
    policy_ = policy;
}

ResourcePolicy ResourceShaper::get_policy() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return policy_;
}

} // namespace isp
