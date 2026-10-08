#include "test_framework.hpp"
#include "daemon.hpp"

#include <filesystem>
#include <fstream>
#include <thread>
#include <chrono>

namespace fs = std::filesystem;

TEST_CASE(Daemon_LifecycleAndPidfileLocking) {
    std::string pidfile1 = "/tmp/isp_test_pidfile_1.pid";
    isp::ResourcePolicy policy;
    policy.dry_run = true;
    policy.poll_interval_ms = 50;

    isp::ProtectorDaemon d1(policy, pidfile1);
    ASSERT_TRUE(d1.start());
    ASSERT_TRUE(d1.is_running());
    ASSERT_TRUE(fs::exists(pidfile1));

    // Try starting a second daemon with the same pidfile -> Must fail
    isp::ProtectorDaemon d2(policy, pidfile1);
    ASSERT_FALSE(d2.start());
    ASSERT_FALSE(d2.is_running());

    // Stop first daemon
    d1.stop();
    ASSERT_FALSE(d1.is_running());
    ASSERT_FALSE(fs::exists(pidfile1));

    // Now second daemon should be able to acquire lock
    ASSERT_TRUE(d2.start());
    ASSERT_TRUE(d2.is_running());
    d2.stop();
    ASSERT_FALSE(fs::exists(pidfile1));
}

TEST_CASE(Daemon_MonitorAndShaperCoordination) {
    fs::path fake_proc = fs::temp_directory_path() / "isp_daemon_coord_test";
    std::error_code ec;
    fs::remove_all(fake_proc, ec);
    fs::create_directories(fake_proc, ec);

    isp::ResourcePolicy policy;
    policy.dry_run = true;
    policy.poll_interval_ms = 50;
    policy.hysteresis_delay_sec = 2;
    policy.proc_mount = fake_proc.string();
    policy.video_device = "/dev/video10";

    std::string pidfile = "/tmp/isp_test_coord.pid";
    isp::ProtectorDaemon daemon(policy, pidfile);

    ASSERT_TRUE(daemon.start());
    ASSERT_EQ(daemon.get_session_state(), isp::SessionState::INACTIVE);

    // 1. Create a process holding /dev/video10
    fs::create_directories(fake_proc / "6001" / "fd", ec);
    {
        std::ofstream comm(fake_proc / "6001" / "comm"); comm << "fake_call\n";
        std::ofstream status(fake_proc / "6001" / "status"); status << "Name:\tfake_call\nPPid:\t1\n";
        fs::create_symlink("/dev/video10", fake_proc / "6001" / "fd" / "3", ec);
    }

    // Give monitor & shaper threads a moment (150ms) to detect and transition
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    ASSERT_EQ(daemon.get_session_state(), isp::SessionState::ACTIVE);
    ASSERT_EQ(daemon.get_active_sessions_count(), 1u);
    ASSERT_EQ(daemon.get_current_bg_cpu_weight(), policy.background_cpu_weight);

    // 2. Remove video session (session dropped)
    fs::remove_all(fake_proc / "6001", ec);

    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    // Must transition to IN_HYSTERESIS
    ASSERT_EQ(daemon.get_session_state(), isp::SessionState::IN_HYSTERESIS);

    daemon.stop();
    ASSERT_FALSE(daemon.is_running());

    fs::remove_all(fake_proc, ec);
}
