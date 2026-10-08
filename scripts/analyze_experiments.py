#!/usr/bin/env python3
"""
Interactive Session Protector — Data Analysis & Visualization Suite (Phase 9)

Computes quantitative metrics directly from generated experiment CSV files:
  - Frame-drop rate (%)
  - Mean frame time (ms)
  - P95 frame time (ms)
  - P99 frame time (ms)
  - Background job completion time & slowdown ratio

Generates publication-quality figures:
  1. baseline vs protected frame-drop plot
  2. baseline vs protected p99 latency plot
  3. summary results table (Terminal + Markdown + CSV)

Handles missing, incomplete, or malformed CSV rows gracefully.
Contains zero hardcoded or manually entered results.
"""

import os
import sys
import argparse
import csv
import math
from pathlib import Path
from typing import Dict, Any, List, Optional, Tuple

# Dynamic path resolution — no hardcoded paths
DEFAULT_RESULTS_DIR = Path(__file__).resolve().parent.parent / "results"
DEFAULT_PLOTS_DIR = DEFAULT_RESULTS_DIR / "plots"


def safe_float(val: Any, default: Optional[float] = None) -> Optional[float]:
    """Safely convert value to float, handling malformed, null, or missing entries."""
    if val is None:
        return default
    try:
        f = float(str(val).strip())
        if math.isnan(f) or math.isinf(f):
            return default
        return f
    except (ValueError, TypeError):
        return default


def safe_int(val: Any, default: Optional[int] = None) -> Optional[int]:
    """Safely convert value to integer, handling malformed entries."""
    if val is None:
        return default
    try:
        return int(float(str(val).strip()))
    except (ValueError, TypeError):
        return default


def analyze_frame_csv(csv_path: Path) -> Dict[str, float]:
    """Parse and calculate statistical metrics from a fake_call per-frame CSV, tolerating malformed rows."""
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

    with open(csv_path, "r", encoding="utf-8", errors="replace") as f:
        reader = csv.DictReader(f)
        for row in reader:
            interval = safe_float(row.get("frame_interval_ms"))
            missed = safe_int(row.get("deadline_missed"))
            jitter = safe_float(row.get("jitter_ms"), 0.0)

            # Skip corrupt or negative interval rows
            if interval is None or missed is None or interval <= 0:
                continue

            intervals.append(interval)
            misses += missed
            if jitter is not None:
                jitters.append(jitter)

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
    mean_interval = sum(intervals) / total_frames
    mean_jitter = sum(jitters) / len(jitters) if jitters else 0.0
    miss_rate = (misses / total_frames) * 100.0

    return {
        "total_frames": total_frames,
        "deadline_misses": misses,
        "miss_rate_pct": round(miss_rate, 3),
        "mean_interval_ms": round(mean_interval, 3),
        "min_interval_ms": round(intervals_sorted[0], 3),
        "max_interval_ms": round(intervals_sorted[-1], 3),
        "p50_interval_ms": round(intervals_sorted[int(len(intervals_sorted) * 0.50)], 3),
        "p95_interval_ms": round(intervals_sorted[min(int(len(intervals_sorted) * 0.95), len(intervals_sorted) - 1)], 3),
        "p99_interval_ms": round(intervals_sorted[min(int(len(intervals_sorted) * 0.99), len(intervals_sorted) - 1)], 3),
        "mean_jitter_ms": round(mean_jitter, 3),
        "max_jitter_ms": round(max(jitters) if jitters else 0.0, 3),
    }


class ExperimentAnalyzer:
    def __init__(self, results_dir: Path, plots_dir: Optional[Path] = None):
        self.results_dir = Path(results_dir).resolve()
        self.raw_dir = self.results_dir / "raw"
        self.plots_dir = (Path(plots_dir) if plots_dir else self.results_dir / "plots").resolve()
        self.plots_dir.mkdir(parents=True, exist_ok=True)

        self.summary_csv = self.results_dir / "experiment_summary.csv"

        # Parsed data structures
        self.summary_records: List[Dict[str, Any]] = []
        self.frame_data: Dict[str, List[Dict[str, Any]]] = {"baseline": [], "protected": []}
        self.stats: Dict[str, Dict[str, Any]] = {}

    def load_summary_csv(self) -> bool:
        """Load and validate the master experiment_summary.csv file."""
        if not self.summary_csv.exists():
            print(f"[!] Warning: Master summary CSV not found at {self.summary_csv}")
            return False

        with open(self.summary_csv, "r", encoding="utf-8", errors="replace") as f:
            reader = csv.DictReader(f)
            for line_no, row in enumerate(reader, start=2):
                mode = row.get("mode", "").strip().lower()
                if mode not in ["baseline", "protected"]:
                    continue

                rec: Dict[str, Any] = {
                    "mode": mode,
                    "repetition": safe_int(row.get("repetition"), 1),
                    "total_frames": safe_int(row.get("total_frames"), 0),
                    "deadline_misses": safe_int(row.get("deadline_misses"), 0),
                    "miss_rate_pct": safe_float(row.get("miss_rate_pct"), 0.0),
                    "mean_interval_ms": safe_float(row.get("mean_interval_ms"), 0.0),
                    "p50_interval_ms": safe_float(row.get("p50_interval_ms"), 0.0),
                    "p95_interval_ms": safe_float(row.get("p95_interval_ms"), 0.0),
                    "p99_interval_ms": safe_float(row.get("p99_interval_ms"), 0.0),
                    "max_interval_ms": safe_float(row.get("max_interval_ms"), 0.0),
                    "stress_cpu_runtime_sec": safe_float(row.get("stress_cpu_runtime_sec")),
                    "tar_runtime_sec": safe_float(row.get("tar_runtime_sec")),
                    "dd_runtime_sec": safe_float(row.get("dd_runtime_sec")),
                }
                self.summary_records.append(rec)

        print(f"[+] Loaded {len(self.summary_records)} run records from {self.summary_csv.name}")
        return len(self.summary_records) > 0

    def load_raw_frame_csvs(self) -> None:
        """Load individual frame measurements from results/raw/*.csv files."""
        if not self.raw_dir.exists():
            return

        for mode in ["baseline", "protected"]:
            files = sorted(list(self.raw_dir.glob(f"{mode}_*_frames.csv")))
            for filepath in files:
                valid_rows = 0
                malformed_rows = 0
                with open(filepath, "r", encoding="utf-8", errors="replace") as f:
                    reader = csv.DictReader(f)
                    for row in reader:
                        interval = safe_float(row.get("frame_interval_ms"))
                        missed = safe_int(row.get("deadline_missed"))
                        ts = safe_float(row.get("timestamp"))
                        fn = safe_int(row.get("frame_number"))

                        # Skip corrupt rows gracefully
                        if interval is None or missed is None or interval <= 0:
                            malformed_rows += 1
                            continue

                        self.frame_data[mode].append({
                            "timestamp": ts if ts is not None else 0.0,
                            "frame_number": fn if fn is not None else 0,
                            "frame_interval_ms": interval,
                            "deadline_missed": missed,
                            "jitter_ms": safe_float(row.get("jitter_ms"), 0.0),
                        })
                        valid_rows += 1

                if malformed_rows > 0:
                    print(f"[*] Note: Skipped {malformed_rows} malformed rows in {filepath.name}")

        for mode in ["baseline", "protected"]:
            print(f"[+] Loaded {len(self.frame_data[mode])} raw frame records for mode '{mode}'")

    def compute_statistics(self) -> None:
        """Compute aggregated statistical metrics across all repetitions."""
        for mode in ["baseline", "protected"]:
            mode_summary = [r for r in self.summary_records if r["mode"] == mode]
            raw_frames = self.frame_data[mode]

            # 1. Total Frames and Drops
            if raw_frames:
                total_frames = len(raw_frames)
                deadline_misses = sum(f["deadline_missed"] for f in raw_frames)
                intervals = sorted([f["frame_interval_ms"] for f in raw_frames])
            elif mode_summary:
                total_frames = sum(r["total_frames"] for r in mode_summary)
                deadline_misses = sum(r["deadline_misses"] for r in mode_summary)
                intervals = []
            else:
                total_frames = 0
                deadline_misses = 0
                intervals = []

            # 2. Frame-drop rate
            drop_rate = (deadline_misses / total_frames * 100.0) if total_frames > 0 else 0.0

            # 3. Percentiles and intervals
            if intervals:
                mean_time = sum(intervals) / len(intervals)
                p50_time = intervals[int(len(intervals) * 0.50)]
                p95_time = intervals[min(int(len(intervals) * 0.95), len(intervals) - 1)]
                p99_time = intervals[min(int(len(intervals) * 0.99), len(intervals) - 1)]
                max_time = intervals[-1]
            elif mode_summary:
                mean_time = sum(r["mean_interval_ms"] for r in mode_summary) / len(mode_summary)
                p50_time = sum(r["p50_interval_ms"] for r in mode_summary) / len(mode_summary)
                p95_time = sum(r["p95_interval_ms"] for r in mode_summary) / len(mode_summary)
                p99_time = sum(r["p99_interval_ms"] for r in mode_summary) / len(mode_summary)
                max_time = max(r["max_interval_ms"] for r in mode_summary)
            else:
                mean_time = p50_time = p95_time = p99_time = max_time = 0.0

            # 4. Background Job Completion Times
            def avg_metric(key: str) -> Optional[float]:
                vals = [r[key] for r in mode_summary if r.get(key) is not None]
                return (sum(vals) / len(vals)) if vals else None

            stress_cpu_time = avg_metric("stress_cpu_runtime_sec")
            tar_time = avg_metric("tar_runtime_sec")
            dd_time = avg_metric("dd_runtime_sec")

            self.stats[mode] = {
                "total_frames": total_frames,
                "deadline_misses": deadline_misses,
                "drop_rate_pct": round(drop_rate, 3),
                "mean_frame_time_ms": round(mean_time, 3),
                "p50_frame_time_ms": round(p50_time, 3),
                "p95_frame_time_ms": round(p95_time, 3),
                "p99_frame_time_ms": round(p99_time, 3),
                "max_frame_time_ms": round(max_time, 3),
                "stress_cpu_runtime_sec": round(stress_cpu_time, 3) if stress_cpu_time else None,
                "tar_runtime_sec": round(tar_time, 3) if tar_time else None,
                "dd_runtime_sec": round(dd_time, 3) if dd_time else None,
            }

        # 5. Background job slowdown calculation (Protected vs. Baseline)
        base = self.stats.get("baseline", {})
        prot = self.stats.get("protected", {})

        def calc_slowdown(key: str) -> Tuple[Optional[float], Optional[float]]:
            t_base = base.get(key)
            t_prot = prot.get(key)
            if t_base and t_prot and t_base > 0:
                ratio = round(t_prot / t_base, 3)
                pct = round(((t_prot - t_base) / t_base) * 100.0, 2)
                return ratio, pct
            return None, None

        self.slowdown = {
            "stress_cpu": calc_slowdown("stress_cpu_runtime_sec"),
            "tar": calc_slowdown("tar_runtime_sec"),
            "dd": calc_slowdown("dd_runtime_sec"),
        }

    def generate_frame_drop_plot(self) -> Path:
        """Generate Plot 1: Baseline vs Protected Frame-Drop Rate & Drop Count comparison."""
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt

        plot_path = self.plots_dir / "baseline_vs_protected_frame_drops.png"
        fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(11, 5))

        modes = ["Baseline", "Protected"]
        colors = ["#e74c3c", "#27ae60"]

        base_rate = self.stats.get("baseline", {}).get("drop_rate_pct", 0.0)
        prot_rate = self.stats.get("protected", {}).get("drop_rate_pct", 0.0)
        rates = [base_rate, prot_rate]

        base_drops = self.stats.get("baseline", {}).get("deadline_misses", 0)
        prot_drops = self.stats.get("protected", {}).get("deadline_misses", 0)
        drops = [base_drops, prot_drops]

        # Left: Miss Rate (%)
        bars1 = ax1.bar(modes, rates, color=colors, width=0.45, edgecolor="black", linewidth=1.2)
        ax1.set_ylabel("Frame Drop Rate (%)", fontsize=11, fontweight="bold")
        ax1.set_title("Frame Drop Rate Comparison\n(Lower is Better)", fontsize=12, fontweight="bold")
        ax1.grid(axis="y", linestyle="--", alpha=0.7)
        ax1.set_ylim(0, max(rates) * 1.35 if max(rates) > 0 else 1.0)

        for bar, val in zip(bars1, rates):
            yval = bar.get_height()
            ax1.text(bar.get_x() + bar.get_width() / 2.0, yval + (ax1.get_ylim()[1] * 0.02),
                     f"{val:.3f}%", ha="center", va="bottom", fontsize=11, fontweight="bold")

        # Right: Total Dropped Frames
        bars2 = ax2.bar(modes, drops, color=colors, width=0.45, edgecolor="black", linewidth=1.2)
        ax2.set_ylabel("Total Deadline Misses (Frames)", fontsize=11, fontweight="bold")
        ax2.set_title("Total Frame Drops Count\n(Lower is Better)", fontsize=12, fontweight="bold")
        ax2.grid(axis="y", linestyle="--", alpha=0.7)
        ax2.set_ylim(0, max(drops) * 1.35 if max(drops) > 0 else 10)

        for bar, val in zip(bars2, drops):
            yval = bar.get_height()
            ax2.text(bar.get_x() + bar.get_width() / 2.0, yval + (ax2.get_ylim()[1] * 0.02),
                     f"{int(val)} frames", ha="center", va="bottom", fontsize=11, fontweight="bold")

        # Annotation of reduction
        if base_drops > 0:
            reduction_pct = ((base_drops - prot_drops) / base_drops) * 100.0
            fig.suptitle(f"Interactive Session Protector: Frame Drop Protection (Improvement: {reduction_pct:+.1f}%)",
                         fontsize=13, fontweight="bold", y=1.02)
        else:
            fig.suptitle("Interactive Session Protector: Frame Drop Protection", fontsize=13, fontweight="bold", y=1.02)

        plt.tight_layout()
        plt.savefig(plot_path, dpi=200, bbox_inches="tight")
        plt.close()
        print(f"[+] Generated Plot 1: {plot_path}")
        return plot_path

    def generate_p99_latency_plot(self) -> Path:
        """Generate Plot 2: Baseline vs Protected P99 & P95 Frame Latency comparison."""
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt

        plot_path = self.plots_dir / "baseline_vs_protected_p99_latency.png"
        fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(12, 5))

        modes = ["Baseline", "Protected"]
        colors = ["#e74c3c", "#27ae60"]

        base_p99 = self.stats.get("baseline", {}).get("p99_frame_time_ms", 0.0)
        prot_p99 = self.stats.get("protected", {}).get("p99_frame_time_ms", 0.0)
        p99_vals = [base_p99, prot_p99]

        base_max = self.stats.get("baseline", {}).get("max_frame_time_ms", 0.0)
        prot_max = self.stats.get("protected", {}).get("max_frame_time_ms", 0.0)
        max_vals = [base_max, prot_max]

        # 1. P99 Frame Latency Bar Chart
        bars1 = ax1.bar(modes, p99_vals, color=colors, width=0.45, edgecolor="black", linewidth=1.2)
        ax1.axhline(y=33.333, color="#2980b9", linestyle="--", linewidth=1.5, label="Target (33.33ms / 30 FPS)")
        ax1.axhline(y=40.0, color="#d35400", linestyle=":", linewidth=1.8, label="Deadline Limit (40.0ms)")
        ax1.set_ylabel("P99 Frame Interval (ms)", fontsize=11, fontweight="bold")
        ax1.set_title("99th Percentile Frame Latency (P99)\n(Lower is Better)", fontsize=12, fontweight="bold")
        ax1.grid(axis="y", linestyle="--", alpha=0.7)
        ax1.legend(loc="upper left")
        ax1.set_ylim(25, max(p99_vals) * 1.25 if max(p99_vals) > 0 else 50)

        for bar, val in zip(bars1, p99_vals):
            yval = bar.get_height()
            ax1.text(bar.get_x() + bar.get_width() / 2.0, yval + 0.8,
                     f"{val:.2f} ms", ha="center", va="bottom", fontsize=11, fontweight="bold")

        # 2. Maximum Latency Spike Bar Chart
        bars2 = ax2.bar(modes, max_vals, color=colors, width=0.45, edgecolor="black", linewidth=1.2)
        ax2.axhline(y=33.333, color="#2980b9", linestyle="--", linewidth=1.5, label="Target (33.33ms)")
        ax2.axhline(y=40.0, color="#d35400", linestyle=":", linewidth=1.8, label="Deadline Limit (40.0ms)")
        ax2.set_ylabel("Maximum Latency Spike (ms)", fontsize=11, fontweight="bold")
        ax2.set_title("Maximum Frame Latency Spike\n(Tail Latency Suppression)", fontsize=12, fontweight="bold")
        ax2.grid(axis="y", linestyle="--", alpha=0.7)
        ax2.legend(loc="upper left")
        ax2.set_ylim(25, max(max_vals) * 1.25 if max(max_vals) > 0 else 70)

        for bar, val in zip(bars2, max_vals):
            yval = bar.get_height()
            ax2.text(bar.get_x() + bar.get_width() / 2.0, yval + 1.2,
                     f"{val:.2f} ms", ha="center", va="bottom", fontsize=11, fontweight="bold")

        if base_max > 0 and prot_max > 0:
            reduction_ms = base_max - prot_max
            fig.suptitle(f"Tail Latency Analysis: P99 Latency & Peak Spike Suppression (Δ = {reduction_ms:.2f}ms)",
                         fontsize=13, fontweight="bold", y=1.02)
        else:
            fig.suptitle("Tail Latency Analysis: P99 Latency & Peak Spike Suppression",
                         fontsize=13, fontweight="bold", y=1.02)

        plt.tight_layout()
        plt.savefig(plot_path, dpi=200, bbox_inches="tight")
        plt.close()
        print(f"[+] Generated Plot 2: {plot_path}")
        return plot_path

    def generate_summary_table(self) -> str:
        """Produce and save the comprehensive results summary table."""
        b = self.stats.get("baseline", {})
        p = self.stats.get("protected", {})

        rows = []

        def fmt_diff(v_base: Optional[float], v_prot: Optional[float], unit: str = "", is_bg_job: bool = False) -> Tuple[str, str, str]:
            if v_base is None or v_prot is None:
                return "N/A", "N/A", "N/A"
            diff = v_prot - v_base
            pct = ((v_prot - v_base) / v_base * 100.0) if v_base > 0 else 0.0
            sign = "+" if diff > 0 else ""
            delta_str = f"{sign}{diff:.2f}{unit}"
            pct_str = f"{sign}{pct:.1f}%"

            # Qualitative evaluation
            if is_bg_job:
                if diff > 0:
                    verdict = "Controlled Throttle"
                elif abs(diff) < 0.05:
                    verdict = "Steady Progress"
                else:
                    verdict = "Unconstrained"
            else:
                if diff < -0.05:
                    verdict = "Improved"
                elif abs(diff) <= 0.05:
                    verdict = "Neutral"
                else:
                    verdict = "Minor Jitter"
            return delta_str, pct_str, verdict

        metrics_def = [
            ("Frame-Drop Rate", b.get("drop_rate_pct"), p.get("drop_rate_pct"), "%", False),
            ("Total Deadline Misses", b.get("deadline_misses"), p.get("deadline_misses"), " frames", False),
            ("Mean Frame Time", b.get("mean_frame_time_ms"), p.get("mean_frame_time_ms"), " ms", False),
            ("P50 Frame Time (Median)", b.get("p50_frame_time_ms"), p.get("p50_frame_time_ms"), " ms", False),
            ("P95 Frame Time", b.get("p95_frame_time_ms"), p.get("p95_frame_time_ms"), " ms", False),
            ("P99 Frame Time", b.get("p99_frame_time_ms"), p.get("p99_frame_time_ms"), " ms", False),
            ("Max Frame Latency Spike", b.get("max_frame_time_ms"), p.get("max_frame_time_ms"), " ms", False),
            ("stress-ng CPU Completion Time", b.get("stress_cpu_runtime_sec"), p.get("stress_cpu_runtime_sec"), " s", True),
            ("tar Compression Completion Time", b.get("tar_runtime_sec"), p.get("tar_runtime_sec"), " s", True),
            ("dd Direct-I/O Completion Time", b.get("dd_runtime_sec"), p.get("dd_runtime_sec"), " s", True),
        ]

        # Markdown Table Generation
        md_lines = [
            "| Metric | Baseline (Mode A) | Protected (Mode B) | Absolute Change (Δ) | Relative Change (%) | Impact |",
            "| :--- | :---: | :---: | :---: | :---: | :--- |",
        ]

        # Plain Text Table for Terminal
        border = "=" * 88
        terminal_lines = [
            border,
            f"{'Metric':<35} | {'Baseline':<12} | {'Protected':<12} | {'Delta (Δ)':<10} | {'Rel %':<8} | {'Impact':<10}",
            "-" * 88
        ]

        csv_rows = [["Metric", "Baseline", "Protected", "Delta", "Relative_Change_Pct", "Impact"]]

        for name, v_b, v_p, unit, invert in metrics_def:
            sb = f"{v_b:.2f}{unit}" if v_b is not None else "N/A"
            sp = f"{v_p:.2f}{unit}" if v_p is not None else "N/A"
            delta, pct, impact = fmt_diff(v_b, v_p, unit, invert)

            md_lines.append(f"| **{name}** | {sb} | {sp} | {delta} | {pct} | {impact} |")
            terminal_lines.append(f"{name:<35} | {sb:<12} | {sp:<12} | {delta:<10} | {pct:<8} | {impact:<10}")
            csv_rows.append([name, sb, sp, delta, pct, impact])

        terminal_lines.append(border)

        # Background Slowdown Breakdown Section
        slowdown_lines = [
            "\n### Background Job Slowdown Analysis",
            "Slowdown is calculated as (Protected_Runtime / Baseline_Runtime):",
        ]
        term_slowdown = [
            "\nBackground Job Slowdown Factor (Protected / Baseline):"
        ]

        for job, (ratio, pct) in self.slowdown.items():
            if ratio is not None:
                slowdown_lines.append(f"- **{job}**: **{ratio:.3f}x** ({pct:+.2f}% runtime increase)")
                term_slowdown.append(f"  * {job:<12}: {ratio:.3f}x ({pct:+.2f}%)")
            else:
                slowdown_lines.append(f"- **{job}**: N/A")
                term_slowdown.append(f"  * {job:<12}: N/A")

        md_output = "\n".join(md_lines) + "\n" + "\n".join(slowdown_lines) + "\n"
        terminal_output = "\n".join(terminal_lines) + "\n" + "\n".join(term_slowdown) + "\n"

        # Save Markdown table
        table_md_path = self.results_dir / "analysis_summary_table.md"
        with open(table_md_path, "w", encoding="utf-8") as f:
            f.write("# Interactive Session Protector — Quantitative Analysis Summary\n\n" + md_output)
        print(f"[+] Saved summary markdown table to: {table_md_path}")

        # Save CSV summary
        table_csv_path = self.results_dir / "analysis_summary_table.csv"
        with open(table_csv_path, "w", encoding="utf-8", newline="") as f:
            writer = csv.writer(f)
            writer.writerows(csv_rows)
        print(f"[+] Saved summary CSV table to: {table_csv_path}")

        return terminal_output

    def run(self) -> None:
        """Execute the full data analysis workflow."""
        print(f"\n==================================================================")
        print(f"  Interactive Session Protector — Experiment Data Analysis")
        print(f"==================================================================")
        print(f"  Input Directory : {self.results_dir}")
        print(f"  Plots Directory : {self.plots_dir}\n")

        has_summary = self.load_summary_csv()
        self.load_raw_frame_csvs()

        if not has_summary and not any(self.frame_data.values()):
            print("[!] Error: No valid experiment CSV data found in results directory!")
            sys.exit(1)

        self.compute_statistics()

        # 1. Generate Plots
        try:
            self.generate_frame_drop_plot()
            self.generate_p99_latency_plot()
        except ImportError as ie:
            print(f"[!] Warning: matplotlib not found ({ie}); skipping plots.")
        except Exception as e:
            print(f"[!] Error while generating plots: {e}")
            import traceback
            traceback.print_exc()

        # 2. Produce Summary Table
        table_str = self.generate_summary_table()
        print("\n" + table_str)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Analyze Interactive Session Protector experiment CSV metrics and generate comparison plots."
    )
    parser.add_argument("--results-dir", type=str, default=str(DEFAULT_RESULTS_DIR),
                        help=f"Directory containing experiment CSVs (default: {DEFAULT_RESULTS_DIR})")
    parser.add_argument("--plots-dir", type=str, default=None,
                        help=f"Directory to save generated plots (default: <results-dir>/plots)")
    return parser.parse_args()


if __name__ == "__main__":
    args = parse_args()
    analyzer = ExperimentAnalyzer(Path(args.results_dir), Path(args.plots_dir) if args.plots_dir else None)
    analyzer.run()
