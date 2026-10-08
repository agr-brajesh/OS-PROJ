#include "config.hpp"
#include "logger.hpp"
#include "types.hpp"
#include "daemon.hpp"

#include <iostream>
#include <string>
#include <vector>
#include <chrono>
#include <thread>

void print_usage(const char* prog_name) {
    std::cout << "Usage: " << prog_name << " [options]\n\n"
              << "Options:\n"
              << "  -c, --config <file>     Path to configuration file\n"
              << "  -d, --dry-run           Run in dry-run mode [default]\n"
              << "  --real                  Enable real cgroup writes and process migration\n"
              << "  -p, --pidfile <file>    Path to PID lockfile (default: /tmp/protector.pid)\n"
              << "  -t, --timeout <sec>     Run for specified seconds then exit (for testing)\n"
              << "  -v, --verbose           Enable debug-level logging\n"
              << "  -h, --help              Show this help message and exit\n";
}

int main(int argc, char* argv[]) {
    std::string config_file = "config/protector.conf";
    std::string pidfile = "/tmp/protector.pid";
    bool dry_run = true; // Default to true for safety
    bool verbose = false;
    int timeout_sec = 0;

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
        } else if (arg == "-p" || arg == "--pidfile") {
            if (i + 1 < argc) {
                pidfile = argv[++i];
            }
        } else if (arg == "-d" || arg == "--dry-run") {
            dry_run = true;
        } else if (arg == "--real") {
            dry_run = false;
        } else if (arg == "-t" || arg == "--timeout") {
            if (i + 1 < argc) {
                timeout_sec = std::stoi(argv[++i]);
            }
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

    ISP_LOG_INFO("Configuration summary:");
    ISP_LOG_INFO("  video_device: " << policy.video_device);
    ISP_LOG_INFO("  cgroup_mount: " << policy.cgroup_mount);
    ISP_LOG_INFO("  proc_mount:   " << policy.proc_mount);
    ISP_LOG_INFO("  poll_ms:      " << policy.poll_interval_ms);
    ISP_LOG_INFO("  hysteresis:   " << policy.hysteresis_delay_sec << "s");
    ISP_LOG_INFO("  mode:         " << (policy.dry_run ? "DRY-RUN (SIMULATION)" : "REAL (ACTIVE)"));

    isp::ProtectorDaemon daemon(policy, pidfile);

    if (!daemon.start()) {
        ISP_LOG_ERROR("Daemon failed to start");
        return 1;
    }

    if (timeout_sec > 0) {
        ISP_LOG_INFO("Running in timed mode for " << timeout_sec << " seconds...");
        std::this_thread::sleep_for(std::chrono::seconds(timeout_sec));
        daemon.stop();
    } else {
        // Run indefinitely until signal
        daemon.wait();
    }

    return 0;
}
