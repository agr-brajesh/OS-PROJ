# Environment Setup & Reproduction Guide
## Project: Interactive Session Protector (Phase 0)

This guide documents the complete system prerequisites, installation steps, configuration commands, and verification procedures for the **Interactive Session Protector** development environment.

---

## 1. System Requirements

| Component | Minimum Requirement | Recommended |
| :--- | :--- | :--- |
| **Operating System** | Ubuntu Linux 22.04 LTS or 24.04 LTS (VM or WSL2) | Ubuntu 24.04 LTS |
| **Kernel** | Linux Kernel >= 5.15 with cgroup v2 support | Linux Kernel >= 6.x |
| **Language** | C++17 | ISO C++17 standard |
| **Compiler** | GCC / g++ >= 11 (with `-Wall -Wextra`) | GCC 13.x |
| **Build System** | CMake >= 3.16 | CMake 3.28+ |
| **Resource Management** | Linux `cgroups v2` unified hierarchy | `cpu`, `io`, `memory`, `pids` controllers |
| **Webcam Simulation** | `v4l2loopback` kernel module | `/dev/video10` (character device) |
| **Stress Workloads** | `stress-ng`, `tar`, `dd` | Standard Linux utilities |
| **Evaluation Stack** | Python >= 3.10, `pandas`, `matplotlib` | Python 3.12+ |

---

## 2. Automated One-Step Setup

If setting up on a clean Ubuntu system, you can run the automated provisioning script:

```bash
chmod +x scripts/setup_env.sh scripts/verify_env.sh
sudo ./scripts/setup_env.sh
```

This script automatically updates the package index, installs all necessary compilers and libraries, configures the `v4l2loopback` device at `/dev/video10`, configures cgroups v2 controllers, and runs the verification suite.

---

## 3. Manual Reproduction Commands (Step-by-Step)

If you prefer to configure the environment manually or need to install packages individually:

### Step 3.1: Update Package Index
```bash
sudo apt-get update -y
```

### Step 3.2: Install Compiler, Build Tools & Workload Generators
```bash
sudo apt-get install -y --no-install-recommends \
    build-essential \
    g++ \
    cmake \
    make \
    stress-ng \
    tar \
    coreutils \
    v4l-utils
```

### Step 3.3: Install Python & Data Science Packages
```bash
sudo apt-get install -y --no-install-recommends \
    python3 \
    python3-pip \
    python3-pandas \
    python3-matplotlib
```

### Step 3.4: Configure cgroups v2
Check that the unified cgroup v2 hierarchy is active:
```bash
stat -f -c %T /sys/fs/cgroup
```
*Expected output:* `cgroup2fs`

Verify that `cpu`, `io`, and `memory` controllers are supported:
```bash
cat /sys/fs/cgroup/cgroup.controllers
```
*Expected output:* contains `cpu io memory` (along with `pids`, `cpuset`, etc.)

To enable subtree control for these controllers:
```bash
sudo sh -c 'echo "+cpu +io +memory +pids" > /sys/fs/cgroup/cgroup.subtree_control'
```

### Step 3.5: Configure Webcam Test Device (`/dev/video10`)

Install kernel headers and the `v4l2loopback` DKMS package:
```bash
sudo apt-get install -y v4l2loopback-dkms v4l2loopback-utils
```

Load the module with device index 10:
```bash
sudo modprobe v4l2loopback video_nr=10 card_label="InteractiveSessionTest" exclusive_caps=1
```

Set permissions so non-root test processes can open `/dev/video10`:
```bash
sudo chmod 0666 /dev/video10
```

To persist the module configuration across reboots:
```bash
sudo sh -c 'echo "v4l2loopback" > /etc/modules-load.d/v4l2loopback.conf'
sudo sh -c 'echo "options v4l2loopback video_nr=10 card_label=\"InteractiveSessionTest\" exclusive_caps=1" > /etc/modprobe.d/v4l2loopback.conf'
```

*(Note for kernel environments without out-of-tree module support: a simulated character device node `/dev/video10` can also be created via `sudo mknod /dev/video10 c 81 10 && sudo chmod 0666 /dev/video10` for testing descriptor open semantics).*

---

## 4. Environment Verification

To verify that all four requirements are satisfied, run:

```bash
bash scripts/verify_env.sh
```

### Checklist Performed by `verify_env.sh`:
1. **cgroup v2 Mount**: Confirms `cgroup2fs` at `/sys/fs/cgroup` and verifies availability of `cpu`, `io`, and `memory` controllers.
2. **Webcam Device**: Confirms that `/dev/video10` exists, is a character device (`-c`), and is read/write accessible.
3. **Toolchain**: Tests `cmake --version`, `g++ --version`, and compiles and executes an inline C++17 program using `std::string_view`, `std::optional`, and `std::variant` with `-std=c++17 -Wall -Wextra`.
4. **Required Commands**: Verifies existence of `stress-ng`, `tar`, `dd`, `python3`, `make`, `modprobe`, and checks Python `pandas` / `matplotlib` availability.

---

## 5. Troubleshooting & FAQ

- **Issue:** `cgroup.controllers` is missing `io` or `memory`.
  - **Resolution:** In some systemd environments, controllers must be delegated. Ensure the root control file has `+cpu +io +memory` enabled via `echo "+cpu +io +memory" | sudo tee /sys/fs/cgroup/cgroup.subtree_control`.
- **Issue:** `modprobe: FATAL: Module v4l2loopback not found`.
  - **Resolution:** Ensure kernel headers match your running kernel: `sudo apt install -y linux-headers-$(uname -r)` and reinstall `v4l2loopback-dkms`.
- **Issue:** Permission denied accessing `/dev/video10`.
  - **Resolution:** Run `sudo chmod 666 /dev/video10` or add your user account to the `video` group (`sudo usermod -aG video $USER`).
