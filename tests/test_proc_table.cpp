#include "test_framework.hpp"
#include "proc_table.hpp"

#include <filesystem>
#include <fstream>
#include <algorithm>

namespace fs = std::filesystem;

TEST_CASE(ProcTable_ProtectedProcess) {
    isp::ProcTable table;
    isp::ProcEntry entry;
    entry.pid = 1500;
    entry.ppid = 1;
    entry.comm = "fake_call";
    entry.cmdline = "./fake_call --device /dev/video10";
    entry.holds_video_device = true;

    table.upsert_entry(entry);

    ASSERT_EQ(table.classify_process(1500), isp::ProcessClass::PROTECTED);
    auto protected_list = table.get_by_class(isp::ProcessClass::PROTECTED);
    ASSERT_EQ(protected_list.size(), 1u);
    ASSERT_EQ(protected_list[0].pid, 1500);
}

TEST_CASE(ProcTable_AllowlistedProcess) {
    isp::ProcTable table;
    isp::ProcEntry ssh_entry;
    ssh_entry.pid = 820;
    ssh_entry.ppid = 1;
    ssh_entry.comm = "sshd";
    ssh_entry.cmdline = "/usr/sbin/sshd -D";

    table.upsert_entry(ssh_entry);

    auto entry_opt = table.get_entry(820);
    ASSERT_TRUE(entry_opt.has_value());
    ASSERT_TRUE(entry_opt->allowlisted);
    // Allowlisted processes must always stay NORMAL (never moved to background)
    ASSERT_EQ(table.classify_process(820), isp::ProcessClass::NORMAL);

    // Dynamic addition to allowlist
    isp::ProcEntry custom_proc;
    custom_proc.pid = 900;
    custom_proc.ppid = 1;
    custom_proc.comm = "custom_critical_daemon";
    table.upsert_entry(custom_proc);

    table.add_allowlist_entry("custom_critical_daemon");
    auto custom_opt = table.get_entry(900);
    ASSERT_TRUE(custom_opt.has_value());
    ASSERT_TRUE(custom_opt->allowlisted);
    ASSERT_EQ(table.classify_process(900), isp::ProcessClass::NORMAL);
}

TEST_CASE(ProcTable_BackgroundProcess) {
    isp::ProcTable table;

    // Background root process: stress-ng
    isp::ProcEntry root_stress;
    root_stress.pid = 2000;
    root_stress.ppid = 100;
    root_stress.comm = "stress-ng";
    root_stress.cmdline = "stress-ng --cpu 4";
    table.upsert_entry(root_stress);

    // Child worker: stress-ng-cpu
    isp::ProcEntry child_stress;
    child_stress.pid = 2001;
    child_stress.ppid = 2000;
    child_stress.comm = "stress-ng-cpu";
    child_stress.cmdline = "stress-ng-cpu [worker]";
    table.upsert_entry(child_stress);

    // Additional background workloads: tar and dd
    isp::ProcEntry tar_proc;
    tar_proc.pid = 2050;
    tar_proc.ppid = 100;
    tar_proc.comm = "tar";
    tar_proc.cmdline = "tar -czf backup.tar.gz /data";
    table.upsert_entry(tar_proc);

    ASSERT_EQ(table.classify_process(2000), isp::ProcessClass::BACKGROUND);
    ASSERT_EQ(table.classify_process(2001), isp::ProcessClass::BACKGROUND);
    ASSERT_EQ(table.classify_process(2050), isp::ProcessClass::BACKGROUND);

    auto bg_list = table.get_by_class(isp::ProcessClass::BACKGROUND);
    ASSERT_EQ(bg_list.size(), 3u);
}

TEST_CASE(ProcTable_NormalProcess) {
    isp::ProcTable table;
    isp::ProcEntry editor;
    editor.pid = 3000;
    editor.ppid = 100;
    editor.comm = "nano";
    editor.cmdline = "nano document.txt";
    table.upsert_entry(editor);

    ASSERT_EQ(table.classify_process(3000), isp::ProcessClass::NORMAL);
    auto normal_list = table.get_by_class(isp::ProcessClass::NORMAL);
    ASSERT_EQ(normal_list.size(), 1u);
    ASSERT_EQ(normal_list[0].pid, 3000);
}

TEST_CASE(ProcTable_BrowserLikeProcessTree) {
    isp::ProcTable table;

    // Main browser process
    isp::ProcEntry chrome_main;
    chrome_main.pid = 4000;
    chrome_main.ppid = 1; // Direct child of init/user session
    chrome_main.comm = "chrome";
    chrome_main.cmdline = "/opt/google/chrome/chrome";
    table.upsert_entry(chrome_main);

    // Browser GPU process
    isp::ProcEntry chrome_gpu;
    chrome_gpu.pid = 4001;
    chrome_gpu.ppid = 4000;
    chrome_gpu.comm = "chrome";
    chrome_gpu.cmdline = "/opt/google/chrome/chrome --type=gpu-process";
    table.upsert_entry(chrome_gpu);

    // Browser Renderer process (tab)
    isp::ProcEntry chrome_renderer;
    chrome_renderer.pid = 4002;
    chrome_renderer.ppid = 4000;
    chrome_renderer.comm = "chrome";
    chrome_renderer.cmdline = "/opt/google/chrome/chrome --type=renderer";
    table.upsert_entry(chrome_renderer);

    // Browser Media Utility process that actually holds /dev/video10 open
    isp::ProcEntry chrome_video;
    chrome_video.pid = 4003;
    chrome_video.ppid = 4000;
    chrome_video.comm = "chrome";
    chrome_video.cmdline = "/opt/google/chrome/chrome --type=utility --utility-sub-type=video_capture";
    chrome_video.holds_video_device = true;
    table.upsert_entry(chrome_video);

    // Grandchild worker spawned by renderer
    isp::ProcEntry chrome_worker;
    chrome_worker.pid = 4004;
    chrome_worker.ppid = 4002;
    chrome_worker.comm = "chrome";
    chrome_worker.cmdline = "/opt/google/chrome/chrome --type=worker";
    table.upsert_entry(chrome_worker);

    // Verification:
    // 1. Session root of the media process 4003 should be chrome_main (4000)
    ASSERT_EQ(table.get_session_root(4003), 4000);

    // 2. Descendants of 4000 should include 4001, 4002, 4003, 4004
    auto descendants = table.get_descendants(4000);
    ASSERT_EQ(descendants.size(), 4u);
    ASSERT_TRUE(std::find(descendants.begin(), descendants.end(), 4001) != descendants.end());
    ASSERT_TRUE(std::find(descendants.begin(), descendants.end(), 4002) != descendants.end());
    ASSERT_TRUE(std::find(descendants.begin(), descendants.end(), 4003) != descendants.end());
    ASSERT_TRUE(std::find(descendants.begin(), descendants.end(), 4004) != descendants.end());

    // 3. EVERY process in the browser tree must be PROTECTED
    ASSERT_EQ(table.classify_process(4000), isp::ProcessClass::PROTECTED);
    ASSERT_EQ(table.classify_process(4001), isp::ProcessClass::PROTECTED);
    ASSERT_EQ(table.classify_process(4002), isp::ProcessClass::PROTECTED);
    ASSERT_EQ(table.classify_process(4003), isp::ProcessClass::PROTECTED);
    ASSERT_EQ(table.classify_process(4004), isp::ProcessClass::PROTECTED);

    auto protected_list = table.get_by_class(isp::ProcessClass::PROTECTED);
    ASSERT_EQ(protected_list.size(), 5u);
}

TEST_CASE(ProcTable_ProcfsRefreshIntegration) {
    fs::path fake_proc = fs::temp_directory_path() / "isp_proctable_refresh_test";
    std::error_code ec;
    fs::remove_all(fake_proc, ec);
    fs::create_directories(fake_proc, ec);

    // Create process 5001: Video call app
    fs::create_directories(fake_proc / "5001" / "fd", ec);
    {
        std::ofstream comm(fake_proc / "5001" / "comm"); comm << "fake_call\n";
        std::ofstream status(fake_proc / "5001" / "status"); status << "Name:\tfake_call\nPPid:\t1\n";
        std::ofstream cg(fake_proc / "5001" / "cgroup"); cg << "0::/user.slice/app.scope\n";
        fs::create_symlink("/dev/video10", fake_proc / "5001" / "fd" / "3", ec);
    }

    // Create process 5002: Background compiler
    fs::create_directories(fake_proc / "5002" / "fd", ec);
    {
        std::ofstream comm(fake_proc / "5002" / "comm"); comm << "cc1plus\n";
        std::ofstream status(fake_proc / "5002" / "status"); status << "Name:\tcc1plus\nPPid:\t1\n";
        std::ofstream cg(fake_proc / "5002" / "cgroup"); cg << "0::/user.slice/build.scope\n";
        fs::create_symlink("/dev/null", fake_proc / "5002" / "fd" / "0", ec);
    }

    isp::ProcTable table;
    ASSERT_TRUE(table.refresh(fake_proc.string(), "/dev/video"));

    ASSERT_EQ(table.size(), 2u);
    ASSERT_EQ(table.classify_process(5001), isp::ProcessClass::PROTECTED);
    ASSERT_EQ(table.classify_process(5002), isp::ProcessClass::BACKGROUND);

    auto e1 = table.get_entry(5001);
    ASSERT_TRUE(e1.has_value());
    ASSERT_EQ(e1->current_cgroup, "/user.slice/app.scope");

    // Clean up
    fs::remove_all(fake_proc, ec);
}
