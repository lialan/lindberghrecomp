import importlib.util
from pathlib import Path
import tempfile
import unittest
import os
import subprocess
import shutil

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


class RecorderLifecycleTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc = shutil.which("cc")
        if not cc:
            raise unittest.SkipTest("host C compiler unavailable")
        cls.directory = tempfile.TemporaryDirectory()
        folder = Path(cls.directory.name)
        runtime = Path(__file__).parents[1] / "src/runtime"
        harness = folder / "record.c"
        harness.write_text("""#include "frame_timing.h"
#include <stdio.h>
#include <stdlib.h>
int main(void) {
    frame_timing_init("synthetic");
    for (unsigned i=0;i<8;++i) frame_timing_record(i,1000000+i*16000,1000000);
    FILE *out=fopen(getenv("LINDBERGH_FRAME_TIMINGS"),"r");
    if (!out) return 1;
    fclose(out);return 0;
}
""")
        cls.binary = folder / "record"
        subprocess.run([cc,"-std=c11","-Wall","-Wextra","-Werror","-I",str(runtime),
                        str(harness),str(runtime/"frame_timing.c"),"-o",str(cls.binary)],check=True)

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    def capture(self, capacity=10, invalid=False):
        folder = tempfile.TemporaryDirectory()
        self.addCleanup(folder.cleanup)
        path = Path(folder.name) / ("missing/trace.csv" if invalid else "trace.csv")
        env = dict(os.environ, LINDBERGH_FRAME_TIMINGS=str(path), LINDBERGH_TIMING_FROM="2",
                   LINDBERGH_TIMING_TO="6", LINDBERGH_TIMING_SAMPLES=str(capacity))
        result = subprocess.run([str(self.binary)],env=env,text=True,capture_output=True)
        return path,result

    def test_window_saved_before_process_exit(self):
        path,result = self.capture()
        self.assertEqual(result.returncode,0,result.stderr)
        metadata,rows,_ = metrics.read_trace(path)
        self.assertFalse(path.with_name(path.name+".tmp").exists())
        self.assertEqual((rows[0][0],rows[-1][0]),(2,6))
        self.assertEqual(metadata["truncated"],"0")

    def test_capacity_flush_is_explicitly_truncated(self):
        path,result = self.capture(capacity=3)
        self.assertEqual(result.returncode,0,result.stderr)
        metadata,rows,_ = metrics.read_trace(path)
        self.assertEqual((rows[0][0],rows[-1][0]),(2,4))
        self.assertEqual(metadata["truncated"],"1")

    def test_failed_write_does_not_double_free_at_exit(self):
        _,result = self.capture(invalid=True)
        self.assertEqual(result.returncode,1,result.stderr)
        self.assertIn("cannot write",result.stderr)


if __name__ == "__main__":
    unittest.main()
