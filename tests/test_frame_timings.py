import importlib.util
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("frame_timings", Path(__file__).parents[1] / "tools/vulkan/frame_timings.py")
metrics = importlib.util.module_from_spec(spec)
spec.loader.exec_module(metrics)


class FrameTimingTests(unittest.TestCase):
    def trace(self, intervals, frames=None):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        path = Path(directory.name) / "timings.csv"
        ticks = 0
        rows = ["# boundary=cpu_present_return", "# clock_hz=1000000", "frame,present_ticks", "0,0"]
        for i, ms in enumerate(intervals, 1):
            ticks += round(ms * 1000)
            rows.append(f"{frames[i-1] if frames else i},{ticks}")
        path.write_text("\n".join(rows)+"\n")
        return path

    def test_steady_sixty(self):
        result = metrics.summarize(self.trace([1000/60]*120))
        self.assertAlmostEqual(result["average_cpu_cadence_fps"], 60, places=2)
        self.assertEqual(result["late_intervals"], 0)
        self.assertEqual(result["longest_late_burst"], 0)

    def test_bursts_hidden_by_average(self):
        result = metrics.summarize(self.trace([16]*98+[26,40]))
        self.assertEqual(result["cpu_interval_ms"]["p99"], 26)
        self.assertEqual(result["cpu_interval_ms"]["max"], 40)
        self.assertEqual(result["over_25_ms"], 2)
        self.assertEqual(result["longest_late_burst"], 2)
        self.assertGreater(result["average_cpu_cadence_fps"], 60)

    def test_explicit_combat_window_retains_its_stalls(self):
        path = self.trace([100,16,40,16,100])
        result = metrics.summarize(path, first_frame=1, last_frame=4)
        self.assertEqual(result["first_frame"], 1)
        self.assertEqual(result["last_frame"], 4)
        self.assertEqual(result["cpu_interval_ms"]["max"], 40)
        with self.assertRaises(ValueError):
            metrics.summarize(path, first_frame=1, last_frame=9)

    def test_missing_frames_are_not_timing_samples(self):
        with self.assertRaises(ValueError):
            metrics.summarize(self.trace([16,16], [1,3]))

    def test_nonmonotonic_clock_rejected(self):
        with self.assertRaises(ValueError):
            metrics.summarize(self.trace([16,0]))


if __name__ == "__main__":
    unittest.main()
