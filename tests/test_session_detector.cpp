#include "test_framework.hpp"
#include "session_detector.hpp"

#include <filesystem>
#include <fstream>
#include <algorithm>

namespace fs = std::filesystem;

class FakeProcFsFixture {
public:
    FakeProcFsFixture(const std::string& subfolder) {
        root_path_ = fs::temp_directory_path() / ("isp_test_proc_" + subfolder);
        std::error_code ec;
        fs::remove_all(root_path_, ec);
        fs::create_directories(root_path_, ec);
    }

    ~FakeProcFsFixture() {
        std::error_code ec;
        fs::remove_all(root_path_, ec);
    }

    const fs::path& path() const {
        return root_path_;
    }

    std::string str() const {
        return root_path_.string();
    }

    void add_process(pid_t pid,
                     const std::string& name,
                     pid_t ppid,
                     const std::vector<std::pair<int, std::string>>& fds) {
        fs::path pid_dir = root_path_ / std::to_string(pid);
        fs::path fd_dir = pid_dir / "fd";
        std::error_code ec;
        fs::create_directories(fd_dir, ec);

        // comm
        std::ofstream comm_out(pid_dir / "comm");
        comm_out << name << "\n";

        // status
        std::ofstream status_out(pid_dir / "status");
        status_out << "Name:\t" << name << "\n"
                   << "PPid:\t" << ppid << "\n";

        // symlinks in fd
        for (const auto& [fd_num, target] : fds) {
            fs::path link_path = fd_dir / std::to_string(fd_num);
            fs::create_symlink(target, link_path, ec);
        }
    }

    void remove_process(pid_t pid) {
        std::error_code ec;
        fs::remove_all(root_path_ / std::to_string(pid), ec);
    }

private:
    fs::path root_path_;
};

TEST_CASE(SessionDetector_DetectHoldingVideo10) {
    FakeProcFsFixture fixture("detect_video10");
    fixture.add_process(1001, "fake_call_app", 500, {
        {0, "/dev/null"},
        {1, "/dev/pts/1"},
        {3, "/dev/video10"}
    });

    auto pids = isp::findSessionPids(fixture.str(), "/dev/video");
    ASSERT_EQ(pids.size(), 1u);
    ASSERT_EQ(pids[0], 1001);

    ASSERT_TRUE(isp::SessionDetector::is_holding_device(1001, fixture.str(), "/dev/video10"));
    ASSERT_EQ(isp::SessionDetector::get_process_name(1001, fixture.str()), "fake_call_app");
    ASSERT_EQ(isp::SessionDetector::get_process_ppid(1001, fixture.str()), 500);
}

TEST_CASE(SessionDetector_IgnoreProcessWithoutVideoDevice) {
    FakeProcFsFixture fixture("ignore_no_video");
    fixture.add_process(1002, "compilation_worker", 400, {
        {0, "/dev/null"},
        {1, "/dev/pts/2"},
        {2, "/tmp/build.log"},
        {3, "/usr/include/stdio.h"}
    });

    auto pids = isp::findSessionPids(fixture.str(), "/dev/video");
    ASSERT_EQ(pids.size(), 0u);
    ASSERT_FALSE(isp::SessionDetector::is_holding_device(1002, fixture.str(), "/dev/video"));
}

TEST_CASE(SessionDetector_MultipleProcessesMixed) {
    FakeProcFsFixture fixture("multiple_processes");
    // Process holding /dev/video10
    fixture.add_process(1001, "video_conf", 100, {
        {0, "/dev/null"},
        {4, "/dev/video10"}
    });

    // Process holding /dev/video0
    fixture.add_process(2048, "webcam_recorder", 100, {
        {5, "/dev/video0"}
    });

    // Background process without video
    fixture.add_process(3099, "stress_worker", 100, {
        {0, "/dev/null"},
        {1, "/dev/urandom"}
    });

    // Create non-numeric noise entries in fake proc
    std::error_code ec;
    fs::create_directories(fixture.path() / "sys", ec);
    fs::create_directories(fixture.path() / "net", ec);
    std::ofstream cpuinfo(fixture.path() / "cpuinfo");
    cpuinfo << "processor : 0\n";

    // Scan for any /dev/video*
    auto pids = isp::findSessionPids(fixture.str(), "/dev/video");
    ASSERT_EQ(pids.size(), 2u);
    ASSERT_TRUE(std::find(pids.begin(), pids.end(), 1001) != pids.end());
    ASSERT_TRUE(std::find(pids.begin(), pids.end(), 2048) != pids.end());
    ASSERT_TRUE(std::find(pids.begin(), pids.end(), 3099) == pids.end());

    // Scan specifically for /dev/video10
    auto pids_specific = isp::findSessionPids(fixture.str(), "/dev/video10");
    ASSERT_EQ(pids_specific.size(), 1u);
    ASSERT_EQ(pids_specific[0], 1001);
}

TEST_CASE(SessionDetector_DisappearedProcessHandledSafely) {
    FakeProcFsFixture fixture("disappeared_process");
    fixture.add_process(4001, "transient_proc", 1, {
        {3, "/dev/video10"}
    });
    fixture.add_process(4002, "stable_proc", 1, {
        {3, "/dev/video10"}
    });

    // Remove 4001's fd directory completely (simulating process exiting mid-operation)
    std::error_code ec;
    fs::remove_all(fixture.path() / "4001" / "fd", ec);

    // Call findSessionPids - should not throw, should handle missing fd directory safely
    auto pids = isp::findSessionPids(fixture.str(), "/dev/video10");
    ASSERT_EQ(pids.size(), 1u);
    ASSERT_EQ(pids[0], 4002);

    // Remove entire process directory 4002 as well
    fixture.remove_process(4002);
    auto pids_empty = isp::findSessionPids(fixture.str(), "/dev/video10");
    ASSERT_EQ(pids_empty.size(), 0u);
}

TEST_CASE(SessionDetector_NonExistentAndEmptyProcRoot) {
    // Non-existent directory
    auto pids_nonexist = isp::findSessionPids("/tmp/non_existent_proc_dir_xyz_123", "/dev/video");
    ASSERT_EQ(pids_nonexist.size(), 0u);

    // Empty directory
    FakeProcFsFixture empty_fixture("empty_proc");
    auto pids_empty = isp::findSessionPids(empty_fixture.str(), "/dev/video");
    ASSERT_EQ(pids_empty.size(), 0u);
}
