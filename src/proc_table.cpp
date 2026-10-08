#include "proc_table.hpp"
#include "session_detector.hpp"
#include "logger.hpp"

#include <filesystem>
#include <fstream>
#include <algorithm>
#include <queue>
#include <unordered_set>

namespace fs = std::filesystem;

namespace isp {

ProcTable::ProcTable() = default;

ProcTable::ProcTable(ClassificationRules rules)
    : rules_(std::move(rules)) {}

bool ProcTable::refresh(const std::string& procRoot, const std::string& videoDevicePrefix) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::error_code ec;

    fs::path root_path(procRoot);
    if (!fs::exists(root_path, ec) || !fs::is_directory(root_path, ec)) {
        ISP_LOG_DEBUG("ProcTable: root does not exist: " << procRoot);
        return false;
    }

    auto now = std::chrono::steady_clock::now();
    std::unordered_set<pid_t> active_pids;

    auto dir_opts = fs::directory_options::skip_permission_denied;
    fs::directory_iterator it(root_path, dir_opts, ec);
    fs::directory_iterator end_it;

    if (ec) {
        ISP_LOG_WARN("ProcTable: failed to iterate: " << procRoot << " (" << ec.message() << ")");
        return false;
    }

    while (it != end_it) {
        std::string filename = it->path().filename().string();
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
                active_pids.insert(pid);
                ProcEntry& entry = entries_[pid];
                entry.pid = pid;
                entry.last_seen = now;

                // Process name from comm
                entry.comm = SessionDetector::get_process_name(pid, procRoot);

                // PPID
                entry.ppid = SessionDetector::get_process_ppid(pid, procRoot);

                // Cmdline
                fs::path cmd_path = it->path() / "cmdline";
                std::ifstream cmd_file(cmd_path);
                if (cmd_file.is_open()) {
                    std::string cmd;
                    if (std::getline(cmd_file, cmd, '\0')) {
                        entry.cmdline = cmd;
                    }
                }

                // Cgroup
                fs::path cgroup_file = it->path() / "cgroup";
                std::ifstream cg_stream(cgroup_file);
                if (cg_stream.is_open()) {
                    std::string cg_line;
                    if (std::getline(cg_stream, cg_line)) {
                        // In cgroup v2, format is 0::<path>
                        size_t colon_pos = cg_line.rfind(':');
                        if (colon_pos != std::string::npos && colon_pos + 1 < cg_line.size()) {
                            entry.current_cgroup = cg_line.substr(colon_pos + 1);
                        } else {
                            entry.current_cgroup = cg_line;
                        }
                        if (entry.original_cgroup.empty() || entry.original_cgroup == "/") {
                            entry.original_cgroup = entry.current_cgroup;
                        }
                    }
                }

                // Check webcam usage
                entry.holds_video_device = SessionDetector::is_holding_device(pid, procRoot, videoDevicePrefix);

                // Allowlist evaluation
                entry.allowlisted = is_allowlisted_locked(entry);
            }
        }

        it.increment(ec);
        if (ec) {
            ec.clear();
        }
    }

    // Prune disappeared processes
    for (auto map_it = entries_.begin(); map_it != entries_.end(); ) {
        if (active_pids.find(map_it->first) == active_pids.end()) {
            map_it = entries_.erase(map_it);
        } else {
            ++map_it;
        }
    }

    // Rebuild tree & classify
    rebuild_tree_locked();
    classify_all_locked();

    return true;
}

void ProcTable::rebuild_tree_locked() {
    for (auto& [pid, entry] : entries_) {
        entry.children.clear();
    }

    for (auto& [pid, entry] : entries_) {
        if (entry.ppid > 0) {
            auto parent_it = entries_.find(entry.ppid);
            if (parent_it != entries_.end()) {
                parent_it->second.children.push_back(pid);
            }
        }
    }
}

bool ProcTable::matches_pattern(const std::string& text, const std::string& pattern) const {
    if (pattern.empty() || text.empty()) {
        return false;
    }
    std::string lower_text = text;
    std::transform(lower_text.begin(), lower_text.end(), lower_text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    std::string lower_pattern = pattern;
    std::transform(lower_pattern.begin(), lower_pattern.end(), lower_pattern.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    return lower_text.find(lower_pattern) != std::string::npos;
}

bool ProcTable::is_allowlisted_locked(const ProcEntry& entry) const {
    if (entry.pid <= 1) {
        return true; // init / systemd is always allowlisted
    }
    for (const auto& allow : rules_.allowlist) {
        if (matches_pattern(entry.comm, allow) || matches_pattern(entry.cmdline, allow)) {
            return true;
        }
    }
    return false;
}

bool ProcTable::is_background_locked(const ProcEntry& entry) const {
    if (entry.allowlisted) {
        return false;
    }
    for (const auto& pattern : rules_.background_patterns) {
        if (matches_pattern(entry.comm, pattern) || matches_pattern(entry.cmdline, pattern)) {
            return true;
        }
    }
    return false;
}

bool ProcTable::is_protected_locked(const ProcEntry& entry) const {
    if (entry.allowlisted) {
        return false;
    }
    if (entry.holds_video_device) {
        return true;
    }
    for (const auto& pattern : rules_.protected_patterns) {
        if (matches_pattern(entry.comm, pattern) || matches_pattern(entry.cmdline, pattern)) {
            return true;
        }
    }
    return false;
}

std::vector<pid_t> ProcTable::get_descendants_locked(pid_t root_pid) const {
    std::vector<pid_t> descendants;
    std::unordered_set<pid_t> visited;
    std::queue<pid_t> q;

    q.push(root_pid);
    visited.insert(root_pid);

    while (!q.empty()) {
        pid_t curr = q.front();
        q.pop();

        auto it = entries_.find(curr);
        if (it != entries_.end()) {
            for (pid_t child : it->second.children) {
                if (visited.insert(child).second) {
                    descendants.push_back(child);
                    q.push(child);
                }
            }
        }
    }

    return descendants;
}

std::vector<pid_t> ProcTable::get_ancestors_locked(pid_t pid) const {
    std::vector<pid_t> ancestors;
    std::unordered_set<pid_t> visited;
    pid_t curr = pid;

    while (true) {
        auto it = entries_.find(curr);
        if (it == entries_.end() || it->second.ppid <= 0) {
            break;
        }
        pid_t parent = it->second.ppid;
        if (!visited.insert(parent).second) {
            break; // Loop cycle protection
        }
        ancestors.push_back(parent);
        curr = parent;
    }

    return ancestors;
}

pid_t ProcTable::get_session_root_locked(pid_t pid) const {
    pid_t curr = pid;
    std::unordered_set<pid_t> visited;

    while (true) {
        visited.insert(curr);
        auto it = entries_.find(curr);
        if (it == entries_.end()) {
            return curr;
        }

        pid_t parent = it->second.ppid;
        // Stop if parent is init (1) or missing or allowlisted
        if (parent <= 1 || visited.count(parent)) {
            return curr;
        }

        auto parent_it = entries_.find(parent);
        if (parent_it == entries_.end() || parent_it->second.allowlisted) {
            return curr;
        }

        curr = parent;
    }
}

void ProcTable::classify_all_locked() {
    // Phase 1: Set initial baseline classification
    for (auto& [pid, entry] : entries_) {
        entry.allowlisted = is_allowlisted_locked(entry);
        if (entry.allowlisted) {
            entry.classification = ProcessClass::NORMAL;
        } else {
            entry.classification = ProcessClass::NORMAL;
        }
    }

    // Phase 2: Identify and protect interactive session trees (e.g. browser parent/child)
    std::unordered_set<pid_t> protected_pids;
    for (auto& [pid, entry] : entries_) {
        if (entry.allowlisted) {
            continue;
        }
        if (entry.holds_video_device || is_protected_locked(entry)) {
            pid_t session_root = get_session_root_locked(pid);
            if (entries_.count(session_root) && !entries_[session_root].allowlisted) {
                protected_pids.insert(session_root);
                auto descendants = get_descendants_locked(session_root);
                for (pid_t d : descendants) {
                    if (entries_.count(d) && !entries_[d].allowlisted) {
                        protected_pids.insert(d);
                    }
                }
            } else {
                protected_pids.insert(pid);
            }
        }
    }

    for (pid_t p : protected_pids) {
        entries_[p].classification = ProcessClass::PROTECTED;
    }

    // Phase 3: Identify background workloads (e.g. stress-ng, tar, dd)
    std::unordered_set<pid_t> background_pids;
    for (auto& [pid, entry] : entries_) {
        if (entry.classification == ProcessClass::PROTECTED || entry.allowlisted) {
            continue;
        }
        if (is_background_locked(entry)) {
            background_pids.insert(pid);
            auto descendants = get_descendants_locked(pid);
            for (pid_t d : descendants) {
                if (entries_.count(d) && !entries_[d].allowlisted &&
                    entries_[d].classification != ProcessClass::PROTECTED) {
                    background_pids.insert(d);
                }
            }
        }
    }

    for (pid_t b : background_pids) {
        entries_[b].classification = ProcessClass::BACKGROUND;
    }
}

void ProcTable::classify_all() {
    std::lock_guard<std::mutex> lock(mutex_);
    classify_all_locked();
}

ProcessClass ProcTable::classify_process(pid_t pid) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(pid);
    if (it != entries_.end()) {
        return it->second.classification;
    }
    return ProcessClass::NORMAL;
}

std::optional<ProcEntry> ProcTable::get_entry(pid_t pid) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(pid);
    if (it != entries_.end()) {
        return it->second;
    }
    return std::nullopt;
}

std::vector<ProcEntry> ProcTable::get_all_entries() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ProcEntry> result;
    result.reserve(entries_.size());
    for (const auto& [pid, entry] : entries_) {
        result.push_back(entry);
    }
    return result;
}

std::vector<ProcEntry> ProcTable::get_by_class(ProcessClass pc) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ProcEntry> result;
    for (const auto& [pid, entry] : entries_) {
        if (entry.classification == pc) {
            result.push_back(entry);
        }
    }
    return result;
}

size_t ProcTable::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
}

std::vector<pid_t> ProcTable::get_descendants(pid_t pid) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return get_descendants_locked(pid);
}

std::vector<pid_t> ProcTable::get_ancestors(pid_t pid) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return get_ancestors_locked(pid);
}

pid_t ProcTable::get_session_root(pid_t pid) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return get_session_root_locked(pid);
}

void ProcTable::set_rules(ClassificationRules rules) {
    std::lock_guard<std::mutex> lock(mutex_);
    rules_ = std::move(rules);
    classify_all_locked();
}

ClassificationRules ProcTable::get_rules() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return rules_;
}

void ProcTable::add_allowlist_entry(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    rules_.allowlist.push_back(name);
    classify_all_locked();
}

void ProcTable::add_background_pattern(const std::string& pattern) {
    std::lock_guard<std::mutex> lock(mutex_);
    rules_.background_patterns.push_back(pattern);
    classify_all_locked();
}

void ProcTable::add_protected_pattern(const std::string& pattern) {
    std::lock_guard<std::mutex> lock(mutex_);
    rules_.protected_patterns.push_back(pattern);
    classify_all_locked();
}

void ProcTable::upsert_entry(const ProcEntry& entry) {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_[entry.pid] = entry;
    rebuild_tree_locked();
    classify_all_locked();
}

bool ProcTable::remove_entry(pid_t pid) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(pid);
    if (it != entries_.end()) {
        entries_.erase(it);
        rebuild_tree_locked();
        classify_all_locked();
        return true;
    }
    return false;
}

void ProcTable::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.clear();
}

} // namespace isp
