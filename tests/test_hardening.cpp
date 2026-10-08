#include "test_framework.hpp"
#include "cgroup_manager.hpp"
#include "proc_table.hpp"
#include "daemon.hpp"
#include "clock.hpp"

#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <csignal>

namespace fs = std::filesystem;

TEST_CASE(Hardening_PidReuseProtection) {
    fs::path temp_dir = fs::temp_directory_path() / "isp_hardening_pid_reuse";
    std::error_code ec;
    fs::remove_all(temp_dir, ec);
    fs::create_directories(temp_dir, ec);

    fs::path proc_dir = temp_dir / "proc";
    fs::path cgroup_dir = temp_dir / "cgroup";
    fs::create_directories(proc_dir, ec);
    fs::create_directories(cgroup_dir, ec);

    // Create process 4001 in mock procfs
    fs::create_directories(proc_dir / "4001", ec);
    {
        std::ofstream comm(proc_dir / "4001" / "comm");
        comm << "mock_worker\n";
    }
    // stat file: field 1: pid, field 2: (comm), field 3..21, field 22: starttime
    // We put 21 tokens after ')'
    {
        std::ofstream stat(proc_dir / "4001" / "stat");
        stat << "4001 (mock_worker) S 1 4001 4001 0 0 4194304 100 0 0 0 10 20 0 0 20 0 1 0 50000 1000 0\n";
        // Field 22 after ')' is token index 19 (0-indexed). Let's verify starttime is parsed correctly:
        // Token 0: S, 1: 1, 2: 4001, 3: 4001, 4: 0, 5: 0, 6: 4194304, 7: 100, 8: 0, 9: 0,
        // 10: 0, 11: 10, 12: 20, 13: 0, 14: 0, 15: 20, 16: 0, 17: 1, 18: 0, 19: 50000
    }
    {
        std::ofstream cg(proc_dir / "4001" / "cgroup");
        cg << "0::/user.slice\n";
    }

    uint64_t st1 = isp::CgroupManager::get_process_starttime(4001, proc_dir.string());
    ASSERT_EQ(st1, 50000ULL);

    isp::ProcTable pt;
    ASSERT_TRUE(pt.refresh(proc_dir.string(), "/dev/video"));
    auto entry1 = pt.get_entry(4001);
    ASSERT_TRUE(entry1.has_value());
    ASSERT_EQ(entry1->start_time, 50000ULL);

    // Now simulate PID recycling: process 4001 is killed and a completely new process
    // is spawned with the same PID but later starttime (e.g. 75000)
    {
        std::ofstream stat(proc_dir / "4001" / "stat");
        stat << "4001 (other_proc) S 1 4001 4001 0 0 4194304 100 0 0 0 10 20 0 0 20 0 1 0 75000 1000 0\n";
    }
    {
        std::ofstream comm(proc_dir / "4001" / "comm");
        comm << "other_proc\n";
    }

    uint64_t st2 = isp::CgroupManager::get_process_starttime(4001, proc_dir.string());
    ASSERT_EQ(st2, 75000ULL);

    // Refresh proc table: should detect recycling and update entry
    ASSERT_TRUE(pt.refresh(proc_dir.string(), "/dev/video"));
    auto entry2 = pt.get_entry(4001);
    ASSERT_TRUE(entry2.has_value());
    ASSERT_EQ(entry2->start_time, 75000ULL);
    ASSERT_EQ(entry2->comm, "other_proc");

    // Clean up
    fs::remove_all(temp_dir, ec);
}

TEST_CASE(Hardening_StaleCgroupCleanup) {
    fs::path temp_dir = fs::temp_directory_path() / "isp_hardening_stale_cg";
    std::error_code ec;
    fs::remove_all(temp_dir, ec);
    fs::create_directories(temp_dir, ec);

    // Build mock stale cgroup tree left behind by a killed/crashed daemon
    fs::path cg_root = temp_dir / "sys_cgroup";
    fs::path slice_dir = cg_root / "protector.slice";
    fs::path prot_dir = slice_dir / "protected";
    fs::path bg_dir = slice_dir / "background";
    fs::path norm_dir = slice_dir / "normal";

    fs::create_directories(cg_root, ec);
    fs::create_directories(slice_dir, ec);
    fs::create_directories(prot_dir, ec);
    fs::create_directories(bg_dir, ec);
    fs::create_directories(norm_dir, ec);

    // Root cgroup procs
    std::ofstream root_procs(cg_root / "cgroup.procs");
    root_procs << "1\n";
    root_procs.close();

    // Stranded PIDs in stale groups
    {
        std::ofstream p_procs(prot_dir / "cgroup.procs");
        p_procs << "8001\n8002\n";
    }
    {
        std::ofstream b_procs(bg_dir / "cgroup.procs");
        b_procs << "8003\n";
    }
    {
        std::ofstream n_procs(norm_dir / "cgroup.procs");
    }

    // Initialize CgroupManager with dry_run = false (real fs operations against temp directory)
    isp::CgroupManager cgm(cg_root.string(), false, "/proc");

    // Invoke stale cleanup
    cgm.cleanup_stale_hierarchy();

    // Verify:
    // 1. Root cgroup received the stranded PIDs (8001, 8002, 8003)
    std::ifstream root_in(cg_root / "cgroup.procs");
    std::string line;
    std::vector<std::string> evacuated;
    while (std::getline(root_in, line)) {
        if (!line.empty()) {
            evacuated.push_back(line);
        }
    }

    bool found_8001 = false, found_8002 = false, found_8003 = false;
    for (const auto& pid : evacuated) {
        if (pid == "8001") found_8001 = true;
        if (pid == "8002") found_8002 = true;
        if (pid == "8003") found_8003 = true;
    }
    ASSERT_TRUE(found_8001);
    ASSERT_TRUE(found_8002);
    ASSERT_TRUE(found_8003);

    // 2. Child directories were cleanly removed
    ASSERT_FALSE(fs::exists(prot_dir));
    ASSERT_FALSE(fs::exists(bg_dir));
    ASSERT_FALSE(fs::exists(norm_dir));
    ASSERT_FALSE(fs::exists(slice_dir));

    fs::remove_all(temp_dir, ec);
}

TEST_CASE(Hardening_CrashAndRestartRecovery) {
    std::string test_pidfile = "/tmp/isp_hardening_recovery.pid";
    std::error_code ec;
    fs::remove(test_pidfile, ec);

    // 1. Simulate a previous crashed run leaving a stale file on disk with dead PID
    {
        std::ofstream f(test_pidfile);
        f << "999999\n"; // Stale pid file
    }
    ASSERT_TRUE(fs::exists(test_pidfile));

    // 2. New daemon starts up: should acquire lock cleanly despite existing dead file
    isp::ResourcePolicy policy;
    policy.dry_run = true;
    policy.poll_interval_ms = 50;

    isp::ProtectorDaemon daemon(policy, test_pidfile);
    ASSERT_TRUE(daemon.start());
    ASSERT_TRUE(daemon.is_running());

    // 3. Graceful shutdown
    daemon.stop();
    ASSERT_FALSE(daemon.is_running());
    ASSERT_FALSE(fs::exists(test_pidfile));
}

TEST_CASE(Hardening_RobustSignalHandling) {
    std::string test_pidfile = "/tmp/isp_hardening_signal.pid";
    isp::ResourcePolicy policy;
    policy.dry_run = true;
    policy.poll_interval_ms = 50;

    isp::ProtectorDaemon daemon(policy, test_pidfile);
    ASSERT_TRUE(daemon.start());
    ASSERT_TRUE(daemon.is_running());

    // Simulate async signal arrival
    isp::ProtectorDaemon::handle_signal(SIGTERM);
    ASSERT_EQ(isp::ProtectorDaemon::get_last_signal(), SIGTERM);

    // wait(2) should immediately detect the signal and shut down gracefully
    bool clean_timeout = daemon.wait(2);
    ASSERT_FALSE(clean_timeout); // Returns false because signal caused early exit
    ASSERT_FALSE(daemon.is_running());
    ASSERT_FALSE(fs::exists(test_pidfile));
}

TEST_CASE(Hardening_ConcurrentSafety_ProcTable) {
    // Multi-threaded race condition stress test for ProcTable (TSan validation)
    fs::path temp_dir = fs::temp_directory_path() / "isp_hardening_race_test";
    std::error_code ec;
    fs::remove_all(temp_dir, ec);
    fs::create_directories(temp_dir, ec);

    // Seed dummy proc entries
    for (int i = 1; i <= 20; ++i) {
        fs::path p = temp_dir / std::to_string(5000 + i);
        fs::create_directories(p, ec);
        std::ofstream comm(p / "comm");
        comm << "proc_" << i << "\n";
    }

    isp::ProcTable table;
    std::atomic<bool> keep_running{true};
    std::vector<std::thread> threads;

    // 2 writers continuously refreshing table
    for (int w = 0; w < 2; ++w) {
        threads.emplace_back([&table, &temp_dir, &keep_running]() {
            while (keep_running.load()) {
                table.refresh(temp_dir.string(), "/dev/video");
                std::this_thread::yield();
            }
        });
    }

    // 4 readers concurrently querying table
    for (int r = 0; r < 4; ++r) {
        threads.emplace_back([&table, &keep_running]() {
            while (keep_running.load()) {
                auto procs = table.get_by_class(isp::ProcessClass::NORMAL);
                auto entry = table.get_entry(5005);
                (void)procs.size();
                (void)entry.has_value();
                std::this_thread::yield();
            }
        });
    }

    // Run concurrent load for 300 ms
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    keep_running.store(false);

    for (auto& t : threads) {
        if (t.joinable()) {
            t.join();
        }
    }

    fs::remove_all(temp_dir, ec);
}
