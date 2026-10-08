#include "test_framework.hpp"
#include "resource_shaper.hpp"
#include "clock.hpp"
#include "ioprio.hpp"

TEST_CASE(ResourceShaper_EnterProtectionOnSessionAppears) {
    auto clock = std::make_shared<isp::MockClock>();
    isp::ResourcePolicy policy;
    policy.protected_cpu_weight = 800;
    policy.background_cpu_weight = 20;
    policy.starvation_floor_cpu_weight = 5;

    isp::ResourceShaper shaper(policy, clock);

    // Initial state: INACTIVE
    ASSERT_EQ(shaper.get_state(), isp::SessionState::INACTIVE);
    ASSERT_FALSE(shaper.is_in_protection());
    ASSERT_EQ(shaper.get_background_cpu_weight(), 100u);
    ASSERT_EQ(shaper.get_protected_cpu_weight(), 100u);

    // Session appears
    shaper.update(/*session_active=*/true);

    // State becomes ACTIVE with shaped weights
    ASSERT_EQ(shaper.get_state(), isp::SessionState::ACTIVE);
    ASSERT_TRUE(shaper.is_in_protection());
    ASSERT_EQ(shaper.get_background_cpu_weight(), 20u);
    ASSERT_EQ(shaper.get_protected_cpu_weight(), 800u);
}

TEST_CASE(ResourceShaper_StarvationFloorClamping) {
    auto clock = std::make_shared<isp::MockClock>();
    isp::ResourcePolicy policy;
    policy.background_cpu_weight = 2; // Below floor!
    policy.starvation_floor_cpu_weight = 10;

    isp::ResourceShaper shaper(policy, clock);
    shaper.update(/*session_active=*/true);

    // Must never drop below starvation floor (10)
    ASSERT_EQ(shaper.get_background_cpu_weight(), 10u);
}

TEST_CASE(ResourceShaper_AgingProgression) {
    auto clock = std::make_shared<isp::MockClock>();
    isp::ResourcePolicy policy;
    policy.background_cpu_weight = 20;
    policy.aging_interval_sec = 15;
    policy.aging_increment = 10;
    policy.aging_ceiling_cpu_weight = 100;

    isp::ResourceShaper shaper(policy, clock);
    shaper.update(/*session_active=*/true);
    ASSERT_EQ(shaper.get_background_cpu_weight(), 20u);
    ASSERT_EQ(shaper.get_aging_steps(), 0u);

    // Advance by 10s (not yet reached 15s interval)
    clock->advance_seconds(10);
    shaper.update(/*session_active=*/true);
    ASSERT_EQ(shaper.get_background_cpu_weight(), 20u);
    ASSERT_EQ(shaper.get_aging_steps(), 0u);

    // Advance by another 5s (total 15s) -> First aging step
    clock->advance_seconds(5);
    shaper.update(/*session_active=*/true);
    ASSERT_EQ(shaper.get_background_cpu_weight(), 30u);
    ASSERT_EQ(shaper.get_aging_steps(), 1u);

    // Advance by 15s -> Second aging step
    clock->advance_seconds(15);
    shaper.update(/*session_active=*/true);
    ASSERT_EQ(shaper.get_background_cpu_weight(), 40u);
    ASSERT_EQ(shaper.get_aging_steps(), 2u);

    // Advance by 30s (2 intervals at once)
    clock->advance_seconds(30);
    shaper.update(/*session_active=*/true);
    ASSERT_EQ(shaper.get_background_cpu_weight(), 60u);
    ASSERT_EQ(shaper.get_aging_steps(), 4u);
}

TEST_CASE(ResourceShaper_AgingCeilingCapping) {
    auto clock = std::make_shared<isp::MockClock>();
    isp::ResourcePolicy policy;
    policy.background_cpu_weight = 20;
    policy.aging_interval_sec = 15;
    policy.aging_increment = 20;
    policy.aging_ceiling_cpu_weight = 50; // Ceiling is 50

    isp::ResourceShaper shaper(policy, clock);
    shaper.update(/*session_active=*/true);
    ASSERT_EQ(shaper.get_background_cpu_weight(), 20u);

    // Step 1: 20 + 20 = 40
    clock->advance_seconds(15);
    shaper.update(/*session_active=*/true);
    ASSERT_EQ(shaper.get_background_cpu_weight(), 40u);

    // Step 2: 40 + 20 = 60, but capped at 50!
    clock->advance_seconds(15);
    shaper.update(/*session_active=*/true);
    ASSERT_EQ(shaper.get_background_cpu_weight(), 50u);

    // Step 3: Advance further, remains capped at 50
    clock->advance_seconds(60);
    shaper.update(/*session_active=*/true);
    ASSERT_EQ(shaper.get_background_cpu_weight(), 50u);
}

TEST_CASE(ResourceShaper_HysteresisCancellationOnSessionReopen) {
    auto clock = std::make_shared<isp::MockClock>();
    isp::ResourcePolicy policy;
    policy.hysteresis_delay_sec = 8;
    policy.background_cpu_weight = 20;

    isp::ResourceShaper shaper(policy, clock);
    shaper.update(/*session_active=*/true);
    ASSERT_EQ(shaper.get_state(), isp::SessionState::ACTIVE);

    // Session momentarily drops
    shaper.update(/*session_active=*/false);
    ASSERT_EQ(shaper.get_state(), isp::SessionState::IN_HYSTERESIS);
    ASSERT_TRUE(shaper.is_in_protection()); // Resources remain protected during grace period
    ASSERT_EQ(shaper.get_background_cpu_weight(), 20u);
    ASSERT_EQ(shaper.get_remaining_hysteresis_seconds(), 8);

    // 3 seconds elapse
    clock->advance_seconds(3);
    shaper.update(/*session_active=*/false);
    ASSERT_EQ(shaper.get_state(), isp::SessionState::IN_HYSTERESIS);
    ASSERT_EQ(shaper.get_remaining_hysteresis_seconds(), 5);

    // Session reopens before 8s expiration!
    shaper.update(/*session_active=*/true);
    ASSERT_EQ(shaper.get_state(), isp::SessionState::ACTIVE);
    ASSERT_EQ(shaper.get_hysteresis_cancellations(), 1u);
    ASSERT_TRUE(shaper.is_in_protection());

    // Advance clock past original 8s mark: session remains active and protected
    clock->advance_seconds(10);
    shaper.update(/*session_active=*/true);
    ASSERT_EQ(shaper.get_state(), isp::SessionState::ACTIVE);
    ASSERT_TRUE(shaper.is_in_protection());
}

TEST_CASE(ResourceShaper_FullHysteresisRestoration) {
    auto clock = std::make_shared<isp::MockClock>();
    isp::ResourcePolicy policy;
    policy.hysteresis_delay_sec = 8;
    policy.background_cpu_weight = 20;
    policy.protected_cpu_weight = 800;

    isp::ResourceShaper shaper(policy, clock);
    shaper.update(/*session_active=*/true);
    ASSERT_EQ(shaper.get_state(), isp::SessionState::ACTIVE);
    ASSERT_EQ(shaper.get_background_cpu_weight(), 20u);

    // Session ends
    shaper.update(/*session_active=*/false);
    ASSERT_EQ(shaper.get_state(), isp::SessionState::IN_HYSTERESIS);

    // Advance past full hysteresis delay (8s)
    clock->advance_seconds(8);
    shaper.update(/*session_active=*/false);

    // Must transition to INACTIVE and restore baseline weights (100)
    ASSERT_EQ(shaper.get_state(), isp::SessionState::INACTIVE);
    ASSERT_FALSE(shaper.is_in_protection());
    ASSERT_EQ(shaper.get_background_cpu_weight(), 100u);
    ASSERT_EQ(shaper.get_protected_cpu_weight(), 100u);
}

TEST_CASE(ResourceShaper_IoPrioritySupport) {
    // Dry-run mode validation
    ASSERT_TRUE(isp::set_process_ioprio(1234, isp::IoPriorityClass::IDLE, 7, /*dry_run=*/true));
    ASSERT_TRUE(isp::set_process_ioprio(1234, isp::IoPriorityClass::BEST_EFFORT, 0, /*dry_run=*/true));
    ASSERT_TRUE(isp::set_process_ioprio(1234, isp::IoPriorityClass::REAL_TIME, 0, /*dry_run=*/true));
}
