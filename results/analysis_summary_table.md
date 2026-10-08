# Interactive Session Protector — Quantitative Analysis Summary

| Metric | Baseline (Mode A) | Protected (Mode B) | Absolute Change (Δ) | Relative Change (%) | Impact |
| :--- | :---: | :---: | :---: | :---: | :--- |
| **Frame-Drop Rate** | 0.44% | 0.39% | -0.05% | -12.4% | Improved |
| **Total Deadline Misses** | 8.00 frames | 7.00 frames | -1.00 frames | -12.5% | Improved |
| **Mean Frame Time** | 33.33 ms | 33.33 ms | +0.00 ms | +0.0% | Neutral |
| **P50 Frame Time (Median)** | 33.33 ms | 33.33 ms | -0.00 ms | -0.0% | Neutral |
| **P95 Frame Time** | 34.35 ms | 34.62 ms | +0.27 ms | +0.8% | Minor Jitter |
| **P99 Frame Time** | 37.47 ms | 37.88 ms | +0.41 ms | +1.1% | Minor Jitter |
| **Max Frame Latency Spike** | 56.77 ms | 44.48 ms | -12.28 ms | -21.6% | Improved |
| **stress-ng CPU Completion Time** | 22.48 s | 23.75 s | +1.27 s | +5.6% | Controlled Throttle |
| **tar Compression Completion Time** | 0.75 s | 0.74 s | -0.01 s | -0.8% | Steady Progress |
| **dd Direct-I/O Completion Time** | 0.45 s | 0.34 s | -0.10 s | -23.1% | Unconstrained |

### Background Job Slowdown Analysis
Slowdown is calculated as (Protected_Runtime / Baseline_Runtime):
- **stress_cpu**: **1.056x** (+5.65% runtime increase)
- **tar**: **0.992x** (-0.80% runtime increase)
- **dd**: **0.769x** (-23.15% runtime increase)
