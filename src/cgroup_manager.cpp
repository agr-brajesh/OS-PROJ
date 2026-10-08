#include "cgroup_manager.hpp"
#include "logger.hpp"

#include <fstream>
#include <sstream>
#include <algorithm>
#include <unistd.h>

namespace fs = std::filesystem;

namespace isp {

CgroupManager::CgroupManager(std::string cgroup_root, bool dry_run, std::string proc_root)
    : cgroup_root_(std::move(cgroup_root)),
      dry_run_(dry_run),
      proc_root_(std::move(proc_root)) {}

CgroupManager::~CgroupManager() {
    cleanup();
}

CgroupManager::CgroupManager(CgroupManager&& other) noexcept {
    std::lock_guard<std::mutex> lock(other.mutex_);
    cgroup_root_ = std::move(other.cgroup_root_);
    dry_run_ = other.dry_run_;
    proc_root_ = std::move(other.proc_root_);
    original_cgroups_ = std::move(other.original_cgroups_);
    action_log_ = std::move(other.action_log_);
}

CgroupManager& CgroupManager::operator=(CgroupManager&& other) noexcept {
    if (this != &other) {
        std::scoped_lock lock(mutex_, other.mutex_);
        cgroup_root_ = std::move(other.cgroup_root_);
        dry_run_ = other.dry_run_;
        proc_root_ = std::move(other.proc_root_);
        original_cgroups_ = std::move(other.original_cgroups_);
        action_log_ = std::move(other.action_log_);
    }
    return *this;
}

std::string CgroupManager::get_slice_path() const {
    return (fs::path(cgroup_root_) / SLICE_NAME).string();
}

std::string CgroupManager::get_group_path(const std::string& group_name) const {
    return (fs::path(cgroup_root_) / SLICE_NAME / group_name).string();
}

void CgroupManager::log_action(const std::string& action) {
    action_log_.push_back(action);
    if (dry_run_) {
        ISP_LOG_INFO(action);
    } else {
        ISP_LOG_DEBUG(action);
    }
}

bool CgroupManager::create_dir(const fs::path& dir_path) {
    if (dry_run_) {
        log_action("[DRY-RUN] Create directory: " + dir_path.string());
        return true;
    }

    std::error_code ec;
    if (fs::exists(dir_path, ec)) {
        return true;
    }
    if (!fs::create_directories(dir_path, ec)) {
        ISP_LOG_WARN("CgroupManager: failed to create directory " << dir_path.string() << " (" << ec.message() << ")");
        return false;
    }
    log_action("Created directory: " + dir_path.string());
    return true;
}

bool CgroupManager::remove_dir(const fs::path& dir_path) {
    if (dry_run_) {
        log_action("[DRY-RUN] Remove directory: " + dir_path.string());
        return true;
    }

    std::error_code ec;
    if (!fs::exists(dir_path, ec)) {
        return true;
    }

    // Standard cgroup v2 rmdir
    if (fs::remove(dir_path, ec) && !ec) {
        log_action("Removed directory: " + dir_path.string());
        return true;
    }

    // In mock/test directories containing regular files, clear regular files and retry rmdir
    if (ec == std::errc::directory_not_empty) {
        ec.clear();
        for (const auto& entry : fs::directory_iterator(dir_path, ec)) {
            if (entry.is_regular_file()) {
                std::error_code rm_ec;
                fs::remove(entry.path(), rm_ec);
            }
        }
        ec.clear();
        if (fs::remove(dir_path, ec) && !ec) {
            log_action("Removed directory: " + dir_path.string());
            return true;
        }
    }

    ISP_LOG_DEBUG("CgroupManager: could not remove directory " << dir_path.string() << " (" << ec.message() << ")");
    return false;
}

bool CgroupManager::write_file(const fs::path& file_path, const std::string& content) {
    if (dry_run_) {
        log_action("[DRY-RUN] Write \"" + content + "\" to " + file_path.string());
        return true;
    }

    std::ofstream out(file_path);
    if (!out.is_open()) {
        ISP_LOG_WARN("CgroupManager: failed to open for writing: " << file_path.string());
        return false;
    }
    out << content;
    if (!out) {
        ISP_LOG_WARN("CgroupManager: failed writing to: " << file_path.string());
        return false;
    }
    log_action("Wrote \"" + content + "\" to " + file_path.string());
    return true;
}

bool CgroupManager::init_hierarchy() {
    std::lock_guard<std::mutex> lock(mutex_);
    fs::path slice_path = fs::path(cgroup_root_) / SLICE_NAME;

    // 1. Create protector.slice
    if (!create_dir(slice_path)) {
        return false;
    }

    // 2. Enable subtree controllers in protector.slice
    fs::path subtree_ctl = slice_path / "cgroup.subtree_control";
    write_file(subtree_ctl, "+cpu +io +memory +pids");

    // 3. Create child cgroups
    std::vector<std::string> groups = {GROUP_PROTECTED, GROUP_BACKGROUND, GROUP_NORMAL};
    for (const auto& grp : groups) {
        fs::path grp_path = slice_path / grp;
        if (!create_dir(grp_path)) {
            return false;
        }
    }

    return true;
}

bool CgroupManager::set_cpu_weight(const std::string& group_name, uint32_t weight) {
    std::lock_guard<std::mutex> lock(mutex_);
    // Linux cgroups v2 valid range is 1..10000
    uint32_t clamped = std::clamp(weight, 1u, 10000u);
    fs::path path = fs::path(get_group_path(group_name)) / "cpu.weight";
    return write_file(path, std::to_string(clamped));
}

bool CgroupManager::set_io_weight(const std::string& group_name, uint32_t weight) {
    std::lock_guard<std::mutex> lock(mutex_);
    // Linux cgroups v2 io.weight range is 1..10000
    uint32_t clamped = std::clamp(weight, 1u, 10000u);
    fs::path path = fs::path(get_group_path(group_name)) / "io.weight";
    return write_file(path, std::to_string(clamped));
}

bool CgroupManager::set_memory_high_bytes(const std::string& group_name, uint64_t bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    fs::path path = fs::path(get_group_path(group_name)) / "memory.high";
    return write_file(path, std::to_string(bytes));
}

bool CgroupManager::set_memory_high_percent(const std::string& group_name, uint32_t percent) {
    uint64_t total_ram = get_total_ram_bytes();
    uint32_t clamped_pct = std::clamp(percent, 1u, 100u);
    uint64_t limit_bytes = (total_ram * clamped_pct) / 100;
    return set_memory_high_bytes(group_name, limit_bytes);
}

bool CgroupManager::apply_policy(const ResourcePolicy& policy) {
    bool ok = true;
    // Set protected weights
    ok &= set_cpu_weight(GROUP_PROTECTED, policy.protected_cpu_weight);
    ok &= set_io_weight(GROUP_PROTECTED, policy.protected_io_weight);

    // Set background weights & memory throttling limit
    ok &= set_cpu_weight(GROUP_BACKGROUND, policy.background_cpu_weight);
    ok &= set_memory_high_percent(GROUP_BACKGROUND, policy.background_memory_high_percent);

    // Set normal baseline weights (standard Linux default is 100)
    ok &= set_cpu_weight(GROUP_NORMAL, 100);
    ok &= set_io_weight(GROUP_NORMAL, 100);

    return ok;
}

std::string CgroupManager::read_process_cgroup(pid_t pid) const {
    fs::path cg_path = fs::path(proc_root_) / std::to_string(pid) / "cgroup";
    std::ifstream file(cg_path);
    if (!file.is_open()) {
        return "/";
    }
    std::string line;
    if (std::getline(file, line)) {
        size_t colon_pos = line.rfind(':');
        if (colon_pos != std::string::npos && colon_pos + 1 < line.size()) {
            return line.substr(colon_pos + 1);
        }
        return line;
    }
    return "/";
}

bool CgroupManager::move_process(pid_t pid,
                                const std::string& target_group,
                                const std::string& hint_original_cgroup) {
    std::lock_guard<std::mutex> lock(mutex_);

    // 1. Record original cgroup if not already tracked
    if (original_cgroups_.find(pid) == original_cgroups_.end()) {
        std::string orig_cg = hint_original_cgroup;
        if (orig_cg.empty() || orig_cg == "/") {
            orig_cg = read_process_cgroup(pid);
        }
        original_cgroups_[pid] = orig_cg;
    }

    std::string orig = original_cgroups_[pid];
    fs::path target_procs = fs::path(get_group_path(target_group)) / "cgroup.procs";

    if (dry_run_) {
        log_action("[DRY-RUN] Move PID " + std::to_string(pid) + " to " + target_procs.string() + " (original: " + orig + ")");
        return true;
    }

    std::ofstream procs_file(target_procs);
    if (!procs_file.is_open()) {
        ISP_LOG_WARN("CgroupManager: could not open " << target_procs.string() << " to migrate PID " << pid);
        return false;
    }
    procs_file << pid;
    if (!procs_file) {
        // Likely process exited (ESRCH) or permission issue (EACCES)
        ISP_LOG_WARN("CgroupManager: failed writing PID " << pid << " to " << target_procs.string());
        return false;
    }

    log_action("Moved PID " + std::to_string(pid) + " to " + target_group);
    return true;
}

bool CgroupManager::restore_process(pid_t pid) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = original_cgroups_.find(pid);
    if (it == original_cgroups_.end()) {
        return true; // Not tracked
    }

    std::string orig_cg = it->second;
    // Format path to original cgroup.procs
    fs::path orig_rel = orig_cg;
    if (!orig_rel.empty() && orig_rel.string().front() == '/') {
        orig_rel = orig_rel.string().substr(1);
    }
    fs::path target_procs = fs::path(cgroup_root_) / orig_rel / "cgroup.procs";

    if (dry_run_) {
        log_action("[DRY-RUN] Restore PID " + std::to_string(pid) + " to " + orig_cg + " (" + target_procs.string() + ")");
        original_cgroups_.erase(it);
        return true;
    }

    std::ofstream procs_file(target_procs);
    if (procs_file.is_open()) {
        procs_file << pid;
    }
    // Regardless of whether write succeeded (process may have died), erase from tracked list
    log_action("Restored PID " + std::to_string(pid) + " to " + orig_cg);
    original_cgroups_.erase(it);
    return true;
}

void CgroupManager::restore_all() {
    std::vector<pid_t> pids_to_restore;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pids_to_restore.reserve(original_cgroups_.size());
        for (const auto& [pid, _] : original_cgroups_) {
            pids_to_restore.push_back(pid);
        }
    }

    for (pid_t pid : pids_to_restore) {
        restore_process(pid);
    }
}

bool CgroupManager::cleanup() {
    // 1. Restore all tracked processes first
    restore_all();

    std::lock_guard<std::mutex> lock(mutex_);
    fs::path slice_path = fs::path(cgroup_root_) / SLICE_NAME;

    // 2. Remove child cgroups
    std::vector<std::string> groups = {GROUP_PROTECTED, GROUP_BACKGROUND, GROUP_NORMAL};
    for (const auto& grp : groups) {
        remove_dir(slice_path / grp);
    }

    // 3. Remove slice directory
    remove_dir(slice_path);

    return true;
}

std::vector<std::string> CgroupManager::get_action_log() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return action_log_;
}

void CgroupManager::clear_action_log() {
    std::lock_guard<std::mutex> lock(mutex_);
    action_log_.clear();
}

std::unordered_map<pid_t, std::string> CgroupManager::get_tracked_migrations() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return original_cgroups_;
}

uint64_t CgroupManager::get_total_ram_bytes() const {
    fs::path meminfo_path = fs::path(proc_root_) / "meminfo";
    std::ifstream file(meminfo_path);
    if (file.is_open()) {
        std::string line;
        while (std::getline(file, line)) {
            if (line.rfind("MemTotal:", 0) == 0) {
                std::istringstream iss(line.substr(9));
                uint64_t kb = 0;
                if (iss >> kb) {
                    return kb * 1024ULL;
                }
            }
        }
    }

    // POSIX fallback
    long pages = sysconf(_SC_PHYS_PAGES);
    long page_size = sysconf(_SC_PAGE_SIZE);
    if (pages > 0 && page_size > 0) {
        return static_cast<uint64_t>(pages) * static_cast<uint64_t>(page_size);
    }

    // Safe 4 GB fallback
    return 4ULL * 1024ULL * 1024ULL * 1024ULL;
}

} // namespace isp
