#include "config.hpp"
#include "logger.hpp"
#include "types.hpp"
#include "session_detector.hpp"
#include "proc_table.hpp"
#include "cgroup_manager.hpp"

#include <iostream>
#include <string>
#include <vector>

void print_usage(const char* prog_name) {
    std::cout << "Usage: " << prog_name << " [options]\n\n"
              << "Options:\n"
              << "  -c, --config <file>   Path to configuration file\n"
              << "  -d, --dry-run         Run in dry-run mode [default]\n"
              << "  --real                Enable real cgroup writes and process migration\n"
              << "  -v, --verbose         Enable debug-level logging\n"
              << "  -h, --help            Show this help message and exit\n";
}

int main(int argc, char* argv[]) {
    std::string config_file = "config/protector.conf";
    bool dry_run = true; // Default to true for safety
    bool verbose = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
        } else if (arg == "-c" || arg == "--config") {
            if (i + 1 < argc) {
                config_file = argv[++i];
            } else {
                std::cerr << "Error: --config requires a file argument\n";
                return 1;
            }
        } else if (arg == "-d" || arg == "--dry-run") {
            dry_run = true;
        } else if (arg == "--real") {
            dry_run = false;
        } else if (arg == "-v" || arg == "--verbose") {
            verbose = true;
        } else {
            std::cerr << "Unknown option: " << arg << "\n";
            print_usage(argv[0]);
            return 1;
        }
    }

    if (verbose) {
        isp::Logger::instance().set_level(isp::LogLevel::DEBUG);
    }

    ISP_LOG_INFO("Interactive Session Protector daemon initializing...");

    isp::Config config;
    if (config.load_from_file(config_file)) {
        ISP_LOG_INFO("Loaded configuration from: " << config_file);
    } else {
        ISP_LOG_WARN("Could not read configuration from " << config_file << ", using defaults");
    }

    config.set("dry_run", dry_run ? "true" : "false");
    isp::ResourcePolicy policy = config.to_resource_policy();

    ISP_LOG_INFO("Configuration loaded:");
    ISP_LOG_INFO("  video_device: " << policy.video_device);
    ISP_LOG_INFO("  cgroup_mount: " << policy.cgroup_mount);
    ISP_LOG_INFO("  proc_mount:   " << policy.proc_mount);
    ISP_LOG_INFO("  poll_ms:      " << policy.poll_interval_ms);
    ISP_LOG_INFO("  hysteresis:   " << policy.hysteresis_delay_sec << "s");
    ISP_LOG_INFO("  mode:         " << (policy.dry_run ? "DRY-RUN (SIMULATION)" : "REAL (ACTIVE)"));

    // Initialize cgroup hierarchy
    isp::CgroupManager cgroup_mgr(policy.cgroup_mount, policy.dry_run, policy.proc_mount);
    if (!cgroup_mgr.init_hierarchy()) {
        ISP_LOG_ERROR("Failed to initialize cgroup hierarchy");
        return 1;
    }
    cgroup_mgr.apply_policy(policy);

    // Initial process table snapshot and classification
    isp::ProcTable proc_table;
    if (proc_table.refresh(policy.proc_mount, policy.video_device)) {
        auto protected_procs = proc_table.get_by_class(isp::ProcessClass::PROTECTED);
        auto background_procs = proc_table.get_by_class(isp::ProcessClass::BACKGROUND);
        auto normal_procs = proc_table.get_by_class(isp::ProcessClass::NORMAL);

        ISP_LOG_INFO("Process table refreshed (" << proc_table.size() << " total processes):");
        ISP_LOG_INFO("  PROTECTED:  " << protected_procs.size());
        for (const auto& p : protected_procs) {
            ISP_LOG_INFO("    -> PID " << p.pid << " [" << p.comm << "]");
            cgroup_mgr.move_process(p.pid, isp::CgroupManager::GROUP_PROTECTED, p.current_cgroup);
        }
        ISP_LOG_INFO("  BACKGROUND: " << background_procs.size());
        for (const auto& p : background_procs) {
            ISP_LOG_INFO("    -> PID " << p.pid << " [" << p.comm << "]");
            cgroup_mgr.move_process(p.pid, isp::CgroupManager::GROUP_BACKGROUND, p.current_cgroup);
        }
        ISP_LOG_INFO("  NORMAL:     " << normal_procs.size());
    }

    ISP_LOG_INFO("Cleaning up and restoring initial cgroups...");
    cgroup_mgr.cleanup();
    ISP_LOG_INFO("Interactive Session Protector finished cleanly.");

    return 0;
}
