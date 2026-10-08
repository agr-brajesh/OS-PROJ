#pragma once

#include "types.hpp"
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <optional>
#include <chrono>

namespace isp {

struct ProcEntry {
    pid_t pid{0};
    pid_t ppid{0};
    std::string comm;
    std::string cmdline;
    std::string original_cgroup{"/"};
    std::string current_cgroup{"/"};
    ProcessClass classification{ProcessClass::NORMAL};
    bool holds_video_device{false};
    bool allowlisted{false};
    uint64_t start_time{0};
    std::vector<pid_t> children;
    std::chrono::steady_clock::time_point last_seen{std::chrono::steady_clock::now()};
};

struct ClassificationRules {
    std::vector<std::string> allowlist{
        "systemd", "sshd", "init", "systemd-journal", "Xorg", "wayland",
        "pipewire", "pulseaudio", "dbus-daemon", "bash", "zsh", "protector", "systemd-udevd"
    };
    std::vector<std::string> background_patterns{
        "stress-ng", "tar", "dd", "gzip", "bzip2", "xz", "cc1", "cc1plus",
        "make", "ninja", "rustc", "ffmpeg"
    };
    std::vector<std::string> protected_patterns{
        "fake_call", "chrome", "firefox", "brave", "zoom", "teams", "slack", "obs"
    };
};

class ProcTable {
public:
    ProcTable();
    explicit ProcTable(ClassificationRules rules);

    // Safely refresh table by scanning procfs (injectable procRoot)
    bool refresh(const std::string& procRoot = "/proc",
                 const std::string& videoDevicePrefix = "/dev/video");

    // Thread-safe accessors
    std::optional<ProcEntry> get_entry(pid_t pid) const;
    std::vector<ProcEntry> get_all_entries() const;
    std::vector<ProcEntry> get_by_class(ProcessClass pc) const;
    size_t size() const;

    // Process tree walking
    std::vector<pid_t> get_descendants(pid_t pid) const;
    std::vector<pid_t> get_ancestors(pid_t pid) const;
    pid_t get_session_root(pid_t pid) const;

    // Classification trigger
    void classify_all();
    ProcessClass classify_process(pid_t pid) const;

    // Rule management
    void set_rules(ClassificationRules rules);
    ClassificationRules get_rules() const;
    void add_allowlist_entry(const std::string& name);
    void add_background_pattern(const std::string& pattern);
    void add_protected_pattern(const std::string& pattern);

    // Direct manual insertion for testbeds
    void upsert_entry(const ProcEntry& entry);
    bool remove_entry(pid_t pid);
    void clear();

private:
    mutable std::mutex mutex_;
    std::unordered_map<pid_t, ProcEntry> entries_;
    ClassificationRules rules_;

    // Internal methods (assumes mutex_ is held)
    void rebuild_tree_locked();
    void classify_all_locked();
    bool matches_pattern(const std::string& text, const std::string& pattern) const;
    bool is_allowlisted_locked(const ProcEntry& entry) const;
    bool is_background_locked(const ProcEntry& entry) const;
    bool is_protected_locked(const ProcEntry& entry) const;
    std::vector<pid_t> get_descendants_locked(pid_t pid) const;
    std::vector<pid_t> get_ancestors_locked(pid_t pid) const;
    pid_t get_session_root_locked(pid_t pid) const;
};

} // namespace isp
