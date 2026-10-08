#!/usr/bin/env python3
"""
Interactive Session Protector — Experiment Automation Runner (Phase 8)

Compares:
  - MODE A: Baseline (Protector disabled)
  - MODE B: Protected (Protector enabled)

Each run executes:
  - Fixed-duration interactive session: fake_call @ 30 FPS holding /dev/video10
  - Fixed-size background jobs:
      1. stress-ng CPU workload (fixed bogo-ops)
      2. tar compression workload (fixed dataset)
      3. dd direct-I/O workload (fixed block write)
  - Multiple configurable repetitions

Generates:
  - Per-run frame metrics CSVs (raw/<mode>_rep<N>_frames.csv)
  - Per-run summary CSVs (raw/<mode>_rep<N>_summary.csv)
  - Master aggregate experiment summary CSV (experiment_summary.csv)
  - JSON metadata and environment report (experiment_metadata.json)
  - Automated comparative visualization plots (if matplotlib available)
"""

import os
import sys
import time
import argparse
import subprocess
import signal
import shutil
import json
import csv
from pathlib import Path
from typing import Dict, Any, List, Optional

# Resolve root directory dynamically — never hardcode paths
PROJECT_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_BUILD_DIR = PROJECT_ROOT / "build"
DEFAULT_OUTPUT_DIR = PROJECT_ROOT / "results"
DEFAULT_CONFIG_PATH = PROJECT_ROOT / "config" / "protector.conf"


def ensure_video_device(device_path: str) -> None:
    """Ensure the webcam test device exists and is readable/writable."""
    can_open = False
    if os.path.exists(device_path):
        try:
            fd = os.open(device_path, os.O_RDWR)
            os.close(fd)
            can_open = True
        except Exception:
            try:
                fd = os.open(device_path, os.O_RDONLY)
                os.close(fd)
                can_open = True
            except Exception:
                can_open = False

    if not can_open:
        print(f"[*] Creating/reinitializing video device node {device_path}...")
        try:
            if os.path.exists(device_path):
                os.remove(device_path)
        except Exception:
            pass
        Path(device_path).parent.mkdir(parents=True, exist_ok=True)
        with open(device_path, "wb") as f:
            f.write(os.urandom(1024 * 100))
        try:
            os.chmod(device_path, 0o666)
        except Exception:
            pass


def prepare_tar_dataset(dataset_dir: Path, size_mb: int) -> None:
    """Create a deterministic fixed-size dataset of compressible text and binary files."""
    if dataset_dir.exists() and any(dataset_dir.iterdir()):
        return

    dataset_dir.mkdir(parents=True, exist_ok=True)
    num_files = max(1, size_mb // 5)
    file_size_bytes = (size_mb * 1024 * 1024) // num_files

    sample_text = (
        "Operating Systems Project: Interactive Session Protector benchmark dataset.\n"
        "Evaluating latency-sensitive video calls under background resource contention.\n"
        "Testing cgroups v2 resource shaping, aging, starvation floors, and hysteresis.\n"
    ).encode("utf-8")

    for i in range(num_files):
        target_file = dataset_dir / f"dataset_part_{i:03d}.dat"
        with open(target_file, "wb") as f:
            written = 0
            while written < file_size_bytes:
                chunk = sample_text * min(100, (file_size_bytes - written) // len(sample_text) + 1)
                f.write(chunk[:file_size_bytes - written])
                written += min(len(chunk), file_size_bytes - written)


def kill_lingering_processes() -> None:
    """Safely terminate any leftover background stress or protector processes."""
    targets = ["stress-ng", "tar", "dd", "fake_call", "protector"]
    for prog in targets:
        subprocess.run(["pkill", "-9", "-f", prog], check=False,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(0.5)


def analyze_frame_csv(csv_path: Path) -> Dict[str, float]:
    """Parse and calculate statistical metrics from a fake_call per-frame CSV."""
    if not csv_path.exists():
        return {
            "total_frames": 0,
            "deadline_misses": 0,
            "miss_rate_pct": 0.0,
            "mean_interval_ms": 0.0,
            "min_interval_ms": 0.0,
            "max_interval_ms": 0.0,
            "p50_interval_ms": 0.0,
            "p95_interval_ms": 0.0,
            "p99_interval_ms": 0.0,
            "mean_jitter_ms": 0.0,
            "max_jitter_ms": 0.0,
        }

    intervals: List[float] = []
    misses = 0
    jitters: List[float] = []

    with open(csv_path, "r", newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            try:
                interval = float(row.get("frame_interval_ms", 0.0))
                missed = int(row.get("deadline_missed", 0))
                jitter = float(row.get("jitter_ms", 0.0))

                intervals.append(interval)
                misses += missed
                jitters.append(jitter)
            except (ValueError, KeyError):
                continue

    total_frames = len(intervals)
    if total_frames == 0:
        return {
            "total_frames": 0,
            "deadline_misses": 0,
            "miss_rate_pct": 0.0,
            "mean_interval_ms": 0.0,
            "min_interval_ms": 0.0,
            "max_interval_ms": 0.0,
            "p50_interval_ms": 0.0,
            "p95_interval_ms": 0.0,
            "p99_interval_ms": 0.0,
            "mean_jitter_ms": 0.0,
            "max_jitter_ms": 0.0,
        }

    intervals_sorted = sorted(intervals)
    jitters_sorted = sorted(jitters)

    def percentile(arr: List[float], p: float) -> float:
        idx = int(len(arr) * p)
        return arr[min(idx, len(arr) - 1)]

    mean_interval = sum(intervals) / total_frames
    mean_jitter = sum(jitters) / total_frames
    miss_rate = (misses / total_frames) * 100.0

    return {
        "total_frames": total_frames,
        "deadline_misses": misses,
        "miss_rate_pct": round(miss_rate, 3),
        "mean_interval_ms": round(mean_interval, 3),
        "min_interval_ms": round(intervals_sorted[0], 3),
        "max_interval_ms": round(intervals_sorted[-1], 3),
        "p50_interval_ms": round(percentile(intervals_sorted, 0.50), 3),
        "p95_interval_ms": round(percentile(intervals_sorted, 0.95), 3),
        "p99_interval_ms": round(percentile(intervals_sorted, 0.99), 3),
        "mean_jitter_ms": round(mean_jitter, 3),
        "max_jitter_ms": round(jitters_sorted[-1], 3),
    }


class ExperimentRunner:
    def __init__(self, args: argparse.Namespace):
        self.args = args
        self.build_dir = Path(args.build_dir).resolve()
        self.output_dir = Path(args.output_dir).resolve()
        self.raw_dir = self.output_dir / "raw"
        self.logs_dir = self.output_dir / "logs"
        self.scratch_dir = Path(f"/tmp/isp_exp_{os.getpid()}")

        self.fake_call_bin = self.build_dir / "fake_call"
        self.protector_bin = self.build_dir / "protector"
        self.config_path = Path(args.config).resolve() if args.config else DEFAULT_CONFIG_PATH

        self.records: List[Dict[str, Any]] = []

    def check_prerequisites(self) -> None:
        """Verify required binaries and system commands exist."""
        if not self.fake_call_bin.exists():
            raise FileNotFoundError(f"fake_call binary not found at: {self.fake_call_bin}. Please run 'cmake --build build' first.")
        if not self.protector_bin.exists():
            raise FileNotFoundError(f"protector binary not found at: {self.protector_bin}. Please run 'cmake --build build' first.")

        for cmd in ["stress-ng", "tar", "dd"]:
            if not shutil.which(cmd):
                raise RuntimeError(f"Required command '{cmd}' not found in PATH.")

        self.output_dir.mkdir(parents=True, exist_ok=True)
        self.raw_dir.mkdir(parents=True, exist_ok=True)
        self.logs_dir.mkdir(parents=True, exist_ok=True)
        self.scratch_dir.mkdir(parents=True, exist_ok=True)

        ensure_video_device(self.args.device)

    def run_single_experiment(self, mode: str, rep: int) -> Dict[str, Any]:
        """Execute a single repetition of an experiment under the specified mode."""
        print(f"\n==================================================================")
        print(f"  [RUN] Mode: {mode.upper()} | Repetition: {rep}/{self.args.repetitions} | Duration: {self.args.duration}s")
        print(f"==================================================================")

        kill_lingering_processes()

        # Paths for this run
        run_tag = f"{mode}_rep{rep}"
        frame_csv = self.raw_dir / f"{run_tag}_frames.csv"
        run_summary_csv = self.raw_dir / f"{run_tag}_summary.csv"
        daemon_log = self.logs_dir / f"{run_tag}_daemon.log"
        call_log = self.logs_dir / f"{run_tag}_call.log"
        stress_log = self.logs_dir / f"{run_tag}_stress.log"

        tar_dataset_dir = self.scratch_dir / "tar_data"
        tar_archive = self.scratch_dir / f"tar_archive_{run_tag}.tar.gz"
        dd_file = self.scratch_dir / f"dd_test_{run_tag}.bin"

        prepare_tar_dataset(tar_dataset_dir, self.args.tar_size_mb)

        daemon_proc = None
        pidfile = f"/tmp/protector_{run_tag}.pid"

        # 1. Start Protector Daemon (if Protected mode)
        if mode == "protected":
            daemon_cmd = [
                str(self.protector_bin),
                "--config", str(self.config_path),
                "--pidfile", pidfile,
            ]
            if self.args.dry_run_daemon:
                daemon_cmd.append("--dry-run")
            elif self.args.real_daemon:
                daemon_cmd.append("--real")

            print(f"[*] Starting Protector daemon ({'DRY-RUN' if self.args.dry_run_daemon else 'REAL CGROUP'})...")
            with open(daemon_log, "w") as df:
                daemon_proc = subprocess.Popen(
                    daemon_cmd,
                    stdout=df,
                    stderr=subprocess.STDOUT
                )
            time.sleep(1.0)
            if daemon_proc.poll() is not None:
                print(f"[!] Warning: Protector daemon exited prematurely! Check log: {daemon_log}")

        # 2. Prepare background workload commands
        stress_cmd = [
            "stress-ng",
            "--cpu", str(self.args.stress_cpus),
            "--cpu-ops", str(self.args.cpu_ops),
            "--metrics-brief"
        ]

        tar_cmd = [
            "tar",
            "-czf", str(tar_archive),
            "-C", str(tar_dataset_dir),
            "."
        ]

        dd_cmd = [
            "dd",
            "if=/dev/zero",
            f"of={dd_file}",
            "bs=1M",
            f"count={self.args.dd_blocks}",
            "conv=fdatasync"
        ]

        call_cmd = [
            str(self.fake_call_bin),
            "--device", self.args.device,
            "--fps", str(self.args.fps),
            "--duration", str(self.args.duration),
            "--output", str(frame_csv),
            "--tolerance", str(self.args.tolerance)
        ]

        print(f"[*] Launching concurrent workloads...")
        print(f"    - stress-ng (CPU: {self.args.stress_cpus} cores, {self.args.cpu_ops} ops)")
        print(f"    - tar (compressing {self.args.tar_size_mb} MB dataset)")
        print(f"    - dd (writing {self.args.dd_blocks} MB with fdatasync)")
        print(f"    - fake_call ({self.args.duration}s @ {self.args.fps} FPS on {self.args.device})")

        # 3. Launch background processes
        t_start = time.time()

        stress_f = open(stress_log, "w")
        t_stress_start = time.time()
        stress_proc = subprocess.Popen(stress_cmd, stdout=stress_f, stderr=subprocess.STDOUT)

        t_tar_start = time.time()
        tar_proc = subprocess.Popen(tar_cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

        t_dd_start = time.time()
        dd_proc = subprocess.Popen(dd_cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

        # 4. Launch interactive video call
        call_f = open(call_log, "w")
        fake_call_proc = subprocess.Popen(call_cmd, stdout=call_f, stderr=subprocess.STDOUT)

        # Tracking completion times of background jobs
        stress_runtime: Optional[float] = None
        tar_runtime: Optional[float] = None
        dd_runtime: Optional[float] = None

        # 5. Monitor execution until fake_call finishes
        while fake_call_proc.poll() is None:
            now = time.time()
            if stress_runtime is None and stress_proc.poll() is not None:
                stress_runtime = round(now - t_stress_start, 3)
            if tar_runtime is None and tar_proc.poll() is not None:
                tar_runtime = round(now - t_tar_start, 3)
            if dd_runtime is None and dd_proc.poll() is not None:
                dd_runtime = round(now - t_dd_start, 3)
            time.sleep(0.1)

        fake_call_proc.wait()
        call_f.close()

        # Check completed background jobs
        now = time.time()
        if stress_runtime is None:
            if stress_proc.poll() is not None:
                stress_runtime = round(now - t_stress_start, 3)
            else:
                stress_proc.kill()
                stress_runtime = round(self.args.duration, 3)
        if tar_runtime is None:
            if tar_proc.poll() is not None:
                tar_runtime = round(now - t_tar_start, 3)
            else:
                tar_proc.kill()
                tar_runtime = round(self.args.duration, 3)
        if dd_runtime is None:
            if dd_proc.poll() is not None:
                dd_runtime = round(now - t_dd_start, 3)
            else:
                dd_proc.kill()
                dd_runtime = round(self.args.duration, 3)

        stress_f.close()

        # 6. Teardown Protector Daemon
        if daemon_proc:
            print(f"[*] Stopping Protector daemon...")
            try:
                daemon_proc.send_signal(signal.SIGINT)
                daemon_proc.wait(timeout=5.0)
            except Exception:
                daemon_proc.kill()

        kill_lingering_processes()

        # Cleanup test artifacts
        if tar_archive.exists():
            tar_archive.unlink()
        if dd_file.exists():
            dd_file.unlink()

        # 7. Analyze metrics
        metrics = analyze_frame_csv(frame_csv)
        print(f"[*] Results for {run_tag}:")
        print(f"    - Frames Recorded       : {metrics['total_frames']} (Expected ~{int(self.args.fps * self.args.duration)})")
        print(f"    - Deadline Misses (Drops): {metrics['deadline_misses']} ({metrics['miss_rate_pct']}%)")
        print(f"    - Frame Interval P95    : {metrics['p95_interval_ms']} ms")
        print(f"    - Frame Interval P99    : {metrics['p99_interval_ms']} ms")
        print(f"    - Max Frame Interval    : {metrics['max_interval_ms']} ms")
        print(f"    - stress-ng Completion  : {stress_runtime}s")
        print(f"    - tar Completion        : {tar_runtime}s")
        print(f"    - dd Completion         : {dd_runtime}s")

        record = {
            "mode": mode,
            "repetition": rep,
            "target_fps": self.args.fps,
            "duration_sec": self.args.duration,
            "total_frames": metrics["total_frames"],
            "deadline_misses": metrics["deadline_misses"],
            "miss_rate_pct": metrics["miss_rate_pct"],
            "mean_interval_ms": metrics["mean_interval_ms"],
            "min_interval_ms": metrics["min_interval_ms"],
            "max_interval_ms": metrics["max_interval_ms"],
            "p50_interval_ms": metrics["p50_interval_ms"],
            "p95_interval_ms": metrics["p95_interval_ms"],
            "p99_interval_ms": metrics["p99_interval_ms"],
            "mean_jitter_ms": metrics["mean_jitter_ms"],
            "max_jitter_ms": metrics["max_jitter_ms"],
            "stress_cpu_runtime_sec": stress_runtime,
            "tar_runtime_sec": tar_runtime,
            "dd_runtime_sec": dd_runtime,
            "device": self.args.device,
            "deadline_tolerance": self.args.tolerance,
        }

        # Write per-run summary CSV
        with open(run_summary_csv, "w", newline="") as sf:
            writer = csv.DictWriter(sf, fieldnames=list(record.keys()))
            writer.writeheader()
            writer.writerow(record)

        self.records.append(record)
        return record

    def save_master_summary(self) -> Path:
        """Export master aggregate CSV across all runs."""
        summary_csv = self.output_dir / "experiment_summary.csv"
        if not self.records:
            return summary_csv

        fieldnames = list(self.records[0].keys())
        with open(summary_csv, "w", newline="") as f:
            writer = csv.DictWriter(f, fieldnames=fieldnames)
            writer.writeheader()
            for r in self.records:
                writer.writerow(r)

        print(f"\n[+] Master experiment summary saved to: {summary_csv}")
        return summary_csv

    def save_metadata(self) -> Path:
        """Export experiment configuration and host environment metadata to JSON."""
        metadata_file = self.output_dir / "experiment_metadata.json"
        uname = os.uname() if hasattr(os, "uname") else None

        data = {
            "timestamp": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
            "configuration": {
                "duration_sec": self.args.duration,
                "repetitions": self.args.repetitions,
                "modes": self.args.modes,
                "fps": self.args.fps,
                "device": self.args.device,
                "tolerance": self.args.tolerance,
                "cpu_ops": self.args.cpu_ops,
                "stress_cpus": self.args.stress_cpus,
                "tar_size_mb": self.args.tar_size_mb,
                "dd_blocks": self.args.dd_blocks,
                "poll_interval_ms": self.args.poll_interval_ms,
            },
            "environment": {
                "sysname": uname.sysname if uname else sys.platform,
                "release": uname.release if uname else "unknown",
                "machine": uname.machine if uname else "unknown",
                "cpu_cores": os.cpu_count(),
            },
            "summary_records": self.records,
        }

        with open(metadata_file, "w") as f:
            json.dump(data, f, indent=2)

        print(f"[+] Experiment metadata saved to: {metadata_file}")
        return metadata_file

    def print_comparison_table(self) -> None:
        """Print an ASCII comparison table summarizing Mode A vs Mode B."""
        if not self.records:
            return

        modes = set(r["mode"] for r in self.records)
        grouped: Dict[str, List[Dict[str, Any]]] = {m: [] for m in modes}
        for r in self.records:
            grouped[r["mode"]].append(r)

        print("\n" + "=" * 76)
        print("                   EXPERIMENT COMPARISON SUMMARY")
        print("=" * 76)
        header = f"{'Metric':<32} | {'Baseline (Mode A)':<18} | {'Protected (Mode B)':<18}"
        print(header)
        print("-" * 76)

        def avg(mode: str, key: str) -> float:
            items = grouped.get(mode, [])
            if not items:
                return 0.0
            return sum(float(x.get(key, 0.0)) for x in items) / len(items)

        metrics_to_show = [
            ("Total Frames Recorded", "total_frames", "{:.1f}"),
            ("Deadline Misses (Drops)", "deadline_misses", "{:.1f}"),
            ("Deadline Miss Rate (%)", "miss_rate_pct", "{:.2f}%"),
            ("Mean Frame Interval (ms)", "mean_interval_ms", "{:.2f} ms"),
            ("P95 Frame Interval (ms)", "p95_interval_ms", "{:.2f} ms"),
            ("P99 Frame Interval (ms)", "p99_interval_ms", "{:.2f} ms"),
            ("Max Frame Interval (ms)", "max_interval_ms", "{:.2f} ms"),
            ("Mean Jitter (ms)", "mean_jitter_ms", "{:.2f} ms"),
            ("stress-ng CPU Runtime (s)", "stress_cpu_runtime_sec", "{:.2f} s"),
            ("tar Compression Runtime (s)", "tar_runtime_sec", "{:.2f} s"),
            ("dd Direct I/O Runtime (s)", "dd_runtime_sec", "{:.2f} s"),
        ]

        for label, key, fmt in metrics_to_show:
            val_base = avg("baseline", key)
            val_prot = avg("protected", key)
            s_base = fmt.format(val_base) if "baseline" in grouped else "N/A"
            s_prot = fmt.format(val_prot) if "protected" in grouped else "N/A"
            print(f"{label:<32} | {s_base:<18} | {s_prot:<18}")

        print("=" * 76 + "\n")

    def run(self) -> None:
        """Run the full experiment matrix across all configured modes and repetitions."""
        self.check_prerequisites()

        modes = [m.strip().lower() for m in self.args.modes.split(",") if m.strip()]
        total_runs = len(modes) * self.args.repetitions
        run_count = 0

        print(f"\n[+] Starting Interactive Session Protector Experiment Automation")
        print(f"    Total Runs: {total_runs} ({len(modes)} modes x {self.args.repetitions} repetitions)")
        print(f"    Duration per run: {self.args.duration}s")
        print(f"    Results directory: {self.output_dir}\n")

        for rep in range(1, self.args.repetitions + 1):
            for mode in modes:
                run_count += 1
                try:
                    self.run_single_experiment(mode, rep)
                except KeyboardInterrupt:
                    print("\n[!] Experiment aborted by user.")
                    kill_lingering_processes()
                    self.save_master_summary()
                    self.save_metadata()
                    return
                except Exception as e:
                    print(f"\n[!] Error during run {mode} rep {rep}: {e}")
                    import traceback
                    traceback.print_exc()

                if run_count < total_runs:
                    print("[*] Cooling down 2 seconds before next run...")
                    time.sleep(2.0)

        # Cleanup scratch
        try:
            shutil.rmtree(self.scratch_dir, ignore_errors=True)
        except Exception:
            pass

        self.save_master_summary()
        self.save_metadata()
        self.print_comparison_table()
        self.generate_plots()

    def generate_plots(self) -> None:
        """Generate comparative visualization plots from the collected experiment metrics."""
        try:
            import matplotlib
            matplotlib.use("Agg")
            import matplotlib.pyplot as plt
            import pandas as pd
        except ImportError:
            print("[*] Note: matplotlib or pandas not available; skipping plot generation.")
            return

        plots_dir = self.output_dir / "plots"
        plots_dir.mkdir(parents=True, exist_ok=True)
        summary_file = self.output_dir / "experiment_summary.csv"
        if not summary_file.exists():
            return

        try:
            df_summary = pd.read_csv(summary_file)
            if df_summary.empty:
                return

            color_map = {"baseline": "#e74c3c", "protected": "#2ecc71"}
            modes = df_summary["mode"].unique()

            # Plot 1: Summary Bar Chart
            fig, axes = plt.subplots(1, 3, figsize=(16, 5))

            # 1a: Miss Rate
            miss_means = df_summary.groupby("mode")["miss_rate_pct"].mean()
            cols = [color_map.get(m, "#3498db") for m in miss_means.index]
            axes[0].bar(miss_means.index, miss_means.values, color=cols, width=0.45)
            axes[0].set_title("Deadline Miss Rate (%) [Lower is Better]")
            axes[0].set_ylabel("Miss Rate (%)")
            axes[0].grid(axis="y", linestyle="--", alpha=0.7)

            # 1b: P95 Interval
            p95_means = df_summary.groupby("mode")["p95_interval_ms"].mean()
            cols = [color_map.get(m, "#3498db") for m in p95_means.index]
            axes[1].bar(p95_means.index, p95_means.values, color=cols, width=0.45)
            axes[1].axhline(y=33.333, color="blue", linestyle="--", label="Target (33.33ms)")
            axes[1].axhline(y=40.0, color="orange", linestyle=":", label="Deadline Limit (40ms)")
            axes[1].set_title("P95 Frame Interval (ms) [Target = 33.33ms]")
            axes[1].set_ylabel("P95 Interval (ms)")
            axes[1].legend()
            axes[1].grid(axis="y", linestyle="--", alpha=0.7)

            # 1c: Background CPU Runtime
            cpu_means = df_summary.groupby("mode")["stress_cpu_runtime_sec"].mean()
            cols = [color_map.get(m, "#3498db") for m in cpu_means.index]
            axes[2].bar(cpu_means.index, cpu_means.values, color=cols, width=0.45)
            axes[2].set_title("Background CPU Runtime (s)")
            axes[2].set_ylabel("Runtime (s)")
            axes[2].grid(axis="y", linestyle="--", alpha=0.7)

            plt.tight_layout()
            summary_plot_path = plots_dir / "summary_metrics_comparison.png"
            plt.savefig(summary_plot_path, dpi=150)
            plt.close()
            print(f"[+] Saved summary comparison plot to: {summary_plot_path}")

            # Plot 2: Frame Interval CDF from raw data
            if self.raw_dir.exists():
                fig, ax = plt.subplots(figsize=(8, 5))
                for mode in modes:
                    frame_files = list(self.raw_dir.glob(f"{mode}_*_frames.csv"))
                    all_intervals: List[float] = []
                    for ff in frame_files:
                        try:
                            fdf = pd.read_csv(ff)
                            if "frame_interval_ms" in fdf:
                                all_intervals.extend(fdf["frame_interval_ms"].dropna().tolist())
                        except Exception:
                            pass
                    if all_intervals:
                        sorted_vals = sorted(all_intervals)
                        probs = [i / len(sorted_vals) for i in range(len(sorted_vals))]
                        ax.plot(sorted_vals, probs, label=f"{mode.capitalize()} Mode",
                                color=color_map.get(mode, "#3498db"), linewidth=2)

                ax.axvline(x=33.333, color="blue", linestyle="--", label="Nominal Target (33.33ms)")
                ax.axvline(x=40.0, color="orange", linestyle=":", label="Deadline Threshold (40ms)")
                ax.set_xlim(25, 55)
                ax.set_xlabel("Frame Interval (ms)")
                ax.set_ylabel("Cumulative Probability")
                ax.set_title("Frame Interval Cumulative Distribution Function (CDF)")
                ax.legend()
                ax.grid(True, linestyle="--", alpha=0.7)
                plt.tight_layout()
                cdf_plot_path = plots_dir / "frame_interval_cdf.png"
                plt.savefig(cdf_plot_path, dpi=150)
                plt.close()
                print(f"[+] Saved CDF plot to: {cdf_plot_path}")

        except Exception as pe:
            print(f"[*] Note: Plot generation encountered minor issue ({pe}); plots skipped.")



def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Automated Experiment Runner for Interactive Session Protector (Baseline vs. Protected)"
    )
    parser.add_argument("--duration", type=int, default=60,
                        help="Run duration in seconds for each fake_call session (default: 60)")
    parser.add_argument("--repetitions", type=int, default=3,
                        help="Number of repetitions per mode (default: 3)")
    parser.add_argument("--modes", type=str, default="baseline,protected",
                        help="Comma-separated modes to evaluate (default: baseline,protected)")
    parser.add_argument("--device", type=str, default="/dev/video10",
                        help="Path to webcam device (default: /dev/video10)")
    parser.add_argument("--fps", type=float, default=30.0,
                        help="Target FPS for fake video call (default: 30.0)")
    parser.add_argument("--tolerance", type=float, default=1.2,
                        help="Deadline tolerance ratio (default: 1.2, threshold = 40.0ms @ 30 FPS)")
    parser.add_argument("--output-dir", type=str, default=str(DEFAULT_OUTPUT_DIR),
                        help=f"Directory to store CSVs, logs, and plots (default: {DEFAULT_OUTPUT_DIR})")
    parser.add_argument("--build-dir", type=str, default=str(DEFAULT_BUILD_DIR),
                        help=f"CMake build directory containing binaries (default: {DEFAULT_BUILD_DIR})")
    parser.add_argument("--config", type=str, default=str(DEFAULT_CONFIG_PATH),
                        help=f"Protector configuration file path (default: {DEFAULT_CONFIG_PATH})")
    parser.add_argument("--cpu-ops", type=int, default=300000,
                        help="Fixed bogo-ops count for stress-ng CPU workload (default: 300000)")
    parser.add_argument("--stress-cpus", type=int, default=0,
                        help="Number of CPU workers for stress-ng (0 = all online CPUs, default: 0)")
    parser.add_argument("--tar-size-mb", type=int, default=50,
                        help="Size in MB of dataset to generate for tar compression workload (default: 50)")
    parser.add_argument("--dd-blocks", type=int, default=150,
                        help="Number of 1MB blocks for dd direct-I/O workload (default: 150)")
    parser.add_argument("--poll-interval-ms", type=int, default=200,
                        help="Daemon monitor polling interval in ms (default: 200)")
    parser.add_argument("--real-daemon", action="store_true", default=True,
                        help="Run daemon with real cgroup v2 operations (default: True when root)")
    parser.add_argument("--dry-run-daemon", action="store_true", default=False,
                        help="Force daemon to run in dry-run simulation mode")
    parser.add_argument("--quick", action="store_true", default=False,
                        help="Quick test run for validation (duration: 10s, repetitions: 1, reduced workloads)")

    args = parser.parse_args()

    # Apply quick settings override if requested
    if args.quick:
        args.duration = 10
        args.repetitions = 1
        args.cpu_ops = 50000
        args.tar_size_mb = 10
        args.dd_blocks = 20

    if args.stress_cpus <= 0:
        args.stress_cpus = os.cpu_count() or 2

    return args


if __name__ == "__main__":
    runner = ExperimentRunner(parse_arguments())
    runner.run()
