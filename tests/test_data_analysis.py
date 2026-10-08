#!/usr/bin/env python3
"""
Unit test for data analysis module: verifies graceful handling of malformed and missing rows.
"""

import os
import sys
import tempfile
import unittest
from pathlib import Path

# Add scripts directory to module path
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "scripts"))
from analyze_experiments import safe_float, safe_int, analyze_frame_csv, ExperimentAnalyzer


class TestDataAnalysis(unittest.TestCase):
    def test_safe_conversions(self):
        # Floats
        self.assertEqual(safe_float("33.333"), 33.333)
        self.assertEqual(safe_float("  42.5  "), 42.5)
        self.assertIsNone(safe_float("invalid"))
        self.assertIsNone(safe_float("nan"))
        self.assertIsNone(safe_float("inf"))
        self.assertIsNone(safe_float(None))
        self.assertEqual(safe_float("bad", default=0.0), 0.0)

        # Integers
        self.assertEqual(safe_int("100"), 100)
        self.assertEqual(safe_int("100.0"), 100)
        self.assertIsNone(safe_int("abc"))
        self.assertIsNone(safe_int(None))
        self.assertEqual(safe_int("corrupt", default=1), 1)

    def test_malformed_csv_parsing(self):
        with tempfile.NamedTemporaryFile("w", delete=False, suffix=".csv") as f:
            temp_path = Path(f.name)
            # Mix valid rows with malformed, empty, and corrupt rows
            f.write("timestamp,frame_number,frame_interval_ms,deadline_missed,jitter_ms\n")
            f.write("0.000,1,33.333,0,0.000\n")
            f.write("0.033,2,33.500,0,0.167\n")
            f.write("CORRUPT_TIMESTAMP,NaN,INVALID,0,0.000\n")  # Bad row 1
            f.write("\n")                                       # Empty row
            f.write(",,,,\n")                                   # Missing values
            f.write("0.066,3,48.200,1,14.867\n")               # Missed deadline
            f.write("0.099,4,-5.000,0,0.000\n")                 # Negative interval (invalid)
            f.write("0.133,5,33.333,0,0.000\n")

        try:
            metrics = analyze_frame_csv(temp_path)
            # Only valid positive intervals: 33.333, 33.500, 48.200, 33.333 (4 frames)
            self.assertEqual(metrics["total_frames"], 4)
            self.assertEqual(metrics["deadline_misses"], 1)
            self.assertAlmostEqual(metrics["miss_rate_pct"], 25.0, places=1)
            self.assertAlmostEqual(metrics["max_interval_ms"], 48.2, places=1)
            self.assertAlmostEqual(metrics["min_interval_ms"], 33.333, places=2)
        finally:
            if temp_path.exists():
                temp_path.unlink()

    def test_missing_csv_file(self):
        non_existent = Path("/tmp/non_existent_file_12345.csv")
        metrics = analyze_frame_csv(non_existent)
        self.assertEqual(metrics["total_frames"], 0)
        self.assertEqual(metrics["deadline_misses"], 0)
        self.assertEqual(metrics["miss_rate_pct"], 0.0)


if __name__ == "__main__":
    unittest.main()
