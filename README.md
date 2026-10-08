# Interactive Session Protector

An Operating Systems project designed to protect latency-sensitive interactive sessions (such as video calls, online classes, telemedicine, or interviews) from resource starvation caused by heavy background workloads (compilation, compression, batch I/O, CPU stress).

---

## Architecture Overview

```
Linux Processes
      │
      ▼
Session Detector (monitors open descriptors for /dev/video*)
      │
      ▼
Process Classifier (Allowlist + rule-based classification)
      │
      ▼
Cgroup v2 Manager (manages hierarchy & migrations)
      │
      ▼
Resource Shaper (dynamic CPU weight, IO weight, memory.high, IO priority)
      ├── Starvation Floor (prevents total stall of background jobs)
      ├── Aging (+10 weight every 15s)
      ├── Aging Ceiling (caps background weight)
      └── Hysteresis (delays restoration to handle intermittent drops)
      │
      ▼
Protected Session (Low latency, zero frame drops)
   + Background Workloads (Smooth progress without starvation)
```

---

## Phase Status

- [x] **Phase 0: Environment Setup & Verification**
  - Ubuntu Linux environment verified
  - C++17 (`g++` with `-Wall -Wextra`) & CMake
  - `cgroups v2` unified hierarchy with `cpu`, `io`, and `memory` controllers
  - Webcam test device (`/dev/video10`)
  - Stress workloads (`stress-ng`, `tar`, `dd`) & Python analysis suite
- [x] **Phase 1: Build & Foundation (CMake, CLI, Logging)**
  - Clean modular architecture (`include/`, `src/`, `tests/`)
  - CMake C++17 build with `-Wall -Wextra -Wpedantic`
  - Targets: `protector`, `fake_call`, `unit_tests`
  - Zero-dependency key=value configuration parser
- [x] **Phase 2: Session Detection (`/dev/video*` procfs scanner)**
  - Injectable `procRoot` support for testing
  - Robust scanning of `/proc/<pid>/fd/*` symlinks
  - Graceful handling of disappeared processes & permission errors
  - Multi-process detection and metadata lookup (`comm`, `status`, `PPid`)
- [x] **Phase 3: Process Classification & Allowlist**
  - Mutex-protected `ProcTable` with safe process lifecycle tracking
  - Parent/child tree walking (`get_ancestors`, `get_descendants`, `get_session_root`)
  - Browser-style multi-process tree protection
  - Three-class taxonomy (`PROTECTED`, `NORMAL`, `BACKGROUND`)
  - Strict allowlist & never-touch policy enforcement
- [x] **Phase 4: Cgroups v2 Manager (Creation, Migration, Restoration)**
  - `protector.slice` hierarchy (`protected`, `background`, `normal`)
  - Dynamic shaping: `cpu.weight`, `io.weight`, `memory.high`
  - PID migration tracking with original cgroup recording
  - Complete restoration of processes to original cgroups upon cleanup
  - Strict `--dry-run` default with transparent action audit logs
- [ ] **Phase 5: Resource Shaper State Machine (Fairness & Aging)**
- [ ] **Phase 6: Multi-Threaded Daemon Engine (Monitor + Shaper)**
- [ ] **Phase 7: Fake Video Call Benchmark App (30 FPS metric collector)**
- [ ] **Phase 8: Benchmarking & Quantitative Evaluation Suite**
- [ ] **Phase 9: Hardening, Sanitizers & Final Documentation**

---

## Building and Testing (Phase 1)

```bash
# Configure and build all targets
cmake -B build -S .
cmake --build build

# Run unit tests
./build/unit_tests

# Run protector daemon skeleton
./build/protector --config config/protector.conf --dry-run

# Run fake call benchmark skeleton
./build/fake_call --device /dev/video10 --fps 30
```

---

## Environment Verification

To verify that your environment meets all requirements:

```bash
bash scripts/verify_env.sh
```

For full reproduction instructions, see [`docs/ENVIRONMENT_SETUP.md`](docs/ENVIRONMENT_SETUP.md).
