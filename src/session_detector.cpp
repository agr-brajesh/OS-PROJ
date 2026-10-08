#include "session_detector.hpp"
#include "logger.hpp"

#include <filesystem>
#include <fstream>
#include <algorithm>
#include <cctype>
#include <set>

namespace fs = std::filesystem;

namespace isp {

SessionDetector::SessionDetector(std::string proc_root, std::string video_device_prefix)
    : proc_root_(std::move(proc_root)), video_device_prefix_(std::move(video_device_prefix)) {}

std::vector<pid_t> SessionDetector::find_session_pids() const {
    return findSessionPids(proc_root_, video_device_prefix_);
}

std::vector<pid_t> SessionDetector::findSessionPids(const std::string& procRoot,
                                                    const std::string& videoDevicePrefix) {
    std::set<pid_t> session_pids;
    std::error_code ec;

    fs::path root_path(procRoot);
    if (!fs::exists(root_path, ec) || !fs::is_directory(root_path, ec)) {
        ISP_LOG_DEBUG("Procfs root does not exist or is not a directory: " << procRoot);
        return {};
    }

    auto dir_opts = fs::directory_options::skip_permission_denied;
    fs::directory_iterator it(root_path, dir_opts, ec);
    fs::directory_iterator end_it;

    if (ec) {
        ISP_LOG_WARN("Failed to open procfs directory: " << procRoot << " (" << ec.message() << ")");
        return {};
    }

    while (it != end_it) {
        const auto& entry = *it;
        std::string filename = entry.path().filename().string();

        // Check if directory name is numeric PID
        if (!filename.empty() && std::all_of(filename.begin(), filename.end(), [](unsigned char c) {
            return std::isdigit(c);
        })) {
            pid_t pid = 0;
            try {
                pid = static_cast<pid_t>(std::stol(filename));
            } catch (...) {
                pid = 0;
            }

            if (pid > 0) {
                if (is_holding_device(pid, procRoot, videoDevicePrefix)) {
                    session_pids.insert(pid);
                }
            }
        }

        // Safely advance iterator to handle process disappearance or permission changes
        it.increment(ec);
        if (ec) {
            // If an entry disappeared while iterating, clear error and continue
            ec.clear();
        }
    }

    return std::vector<pid_t>(session_pids.begin(), session_pids.end());
}

bool SessionDetector::is_holding_device(pid_t pid,
                                       const std::string& procRoot,
                                       const std::string& devicePrefix) {
    std::error_code ec;
    fs::path fd_dir = fs::path(procRoot) / std::to_string(pid) / "fd";

    if (!fs::exists(fd_dir, ec) || !fs::is_directory(fd_dir, ec) || ec) {
        return false;
    }

    auto dir_opts = fs::directory_options::skip_permission_denied;
    fs::directory_iterator it(fd_dir, dir_opts, ec);
    fs::directory_iterator end_it;

    if (ec) {
        // Permission denied or process exited
        return false;
    }

    while (it != end_it) {
        const auto& fd_entry = *it;
        std::error_code sym_ec;

        if (fd_entry.is_symlink(sym_ec) && !sym_ec) {
            fs::path target = fs::read_symlink(fd_entry.path(), sym_ec);
            if (!sym_ec) {
                std::string target_str = target.string();
                if (target_str.find(devicePrefix) == 0) {
                    return true;
                }
            }
        }

        it.increment(ec);
        if (ec) {
            ec.clear();
        }
    }

    return false;
}

std::string SessionDetector::get_process_name(pid_t pid, const std::string& procRoot) {
    fs::path comm_path = fs::path(procRoot) / std::to_string(pid) / "comm";
    std::ifstream comm_file(comm_path);
    if (comm_file.is_open()) {
        std::string comm;
        if (std::getline(comm_file, comm)) {
            while (!comm.empty() && (comm.back() == '\n' || comm.back() == '\r')) {
                comm.pop_back();
            }
            if (!comm.empty()) {
                return comm;
            }
        }
    }

    // Fallback: /proc/<pid>/cmdline
    fs::path cmdline_path = fs::path(procRoot) / std::to_string(pid) / "cmdline";
    std::ifstream cmd_file(cmdline_path);
    if (cmd_file.is_open()) {
        std::string cmd;
        if (std::getline(cmd_file, cmd, '\0')) {
            auto slash_pos = cmd.find_last_of('/');
            if (slash_pos != std::string::npos) {
                return cmd.substr(slash_pos + 1);
            }
            return cmd;
        }
    }

    return "";
}

pid_t SessionDetector::get_process_ppid(pid_t pid, const std::string& procRoot) {
    fs::path status_path = fs::path(procRoot) / std::to_string(pid) / "status";
    std::ifstream status_file(status_path);
    if (!status_file.is_open()) {
        return 0;
    }

    std::string line;
    while (std::getline(status_file, line)) {
        if (line.rfind("PPid:", 0) == 0) {
            size_t tab = line.find_first_of(" \t");
            if (tab != std::string::npos) {
                try {
                    return static_cast<pid_t>(std::stol(line.substr(tab)));
                } catch (...) {
                    return 0;
                }
            }
        }
    }

    return 0;
}

} // namespace isp
