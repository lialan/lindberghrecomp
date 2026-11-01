import csv
import importlib.util
from pathlib import Path
import tempfile
import unittest

spec=importlib.util.spec_from_file_location("submission_timings",Path(__file__).parents[1]/"tools/vulkan/submission_timings.py")
metrics=importlib.util.module_from_spec(spec);spec.loader.exec_module(metrics)

class SubmissionTimingTests(unittest.TestCase):
    def trace(self, rows):
        directory=tempfile.TemporaryDirectory();self.addCleanup(directory.cleanup)
        path=Path(directory.name)/"submissions.csv"
        fields=["submission","frame","slot","gpu_valid",*metrics.NS_FIELDS,"upload_bytes","retained_resources","record_thread_valid"]
        with path.open("w") as out:
            writer=csv.DictWriter(out,fieldnames=fields);writer.writeheader()
            for submission,frame,changes in rows:
                row={k:1000000 for k in metrics.NS_FIELDS};row.update(submission=submission,frame=frame,slot=0,gpu_valid=1,record_thread_valid=1,upload_bytes=1048576,retained_resources=1);row.update(changes);writer.writerow(row)
        return path

    def test_reordered_retirement_and_multiple_submissions(self):
        result=metrics.summarize(self.trace([(3,11,{}),(1,10,{}),(2,10,{"gpu_ns":4000000})]),10,11)
        self.assertEqual(result["submissions"],3)
        self.assertEqual(result["per_frame_ms"]["gpu"]["max"],5)
        self.assertEqual(result["worst_gpu_frames_ms"][0]["frame"],10)
        self.assertEqual(result["per_frame_upload_MiB"]["mean"],1.5)

    def test_missing_duplicate_and_invalid_results(self):
        for rows in [[(1,10,{})],[(1,10,{}),(1,11,{})],[(1,10,{"gpu_valid":0}),(2,11,{})],
                     [(1,10,{"record_thread_valid":0}),(2,11,{})],[(1,10,{"gpu_ns":"nan"}),(2,11,{})],
                     [(1,10,{"retire_ns":-1}),(2,11,{})]]:
            with self.subTest(rows=rows),self.assertRaises(ValueError):metrics.summarize(self.trace(rows),10,11)

    def test_explicit_window_keeps_spike_and_excludes_initial_handoff(self):
        result=metrics.summarize(self.trace([(1,0,{"record_thread_valid":0}),(2,10,{}),(3,11,{"gpu_ns":80000000})]),10,11)
        self.assertEqual(result["per_frame_ms"]["gpu"]["max"],80)
        self.assertEqual(result["frames"],2)

if __name__=="__main__":unittest.main()
