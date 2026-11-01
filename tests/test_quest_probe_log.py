import importlib.util
from pathlib import Path
import unittest
spec=importlib.util.spec_from_file_location("quest_probe",Path(__file__).parents[1]/"tools/vulkan/test_quest_probe.py")
probe=importlib.util.module_from_spec(spec)
spec.loader.exec_module(probe)

class ProbeLogTests(unittest.TestCase):
    def test_rolling_snapshot_retains_authenticated_start(self):
        log=probe.FreshProbeLog("fresh")
        start="11-01 04:05:00.000 123 124 I LindberghVulkanProbe: START fresh: native"
        first="11-01 04:05:00.500 123 124 I LindberghVulkanProbe: FIRST: stereo"
        passed="11-01 04:05:01.500 123 124 I LindberghVulkanProbe: PASS: native"
        self.assertIn(start,log.observe(start+"\n"+first))
        result=log.observe(first+"\n"+passed)
        self.assertIn(start,result)
        self.assertIn(passed,result)
        self.assertEqual(result.count(first),1)

    def test_unrelated_or_historical_pass_is_rejected(self):
        log=probe.FreshProbeLog("fresh")
        stale="11-01 04:04:59.000 123 124 I LindberghVulkanProbe: PASS: stale"
        wrong="11-01 04:04:59.100 123 124 I LindberghVulkanProbe: START other: native"
        self.assertEqual(log.observe(wrong+"\n"+stale),"")
        start="11-01 04:05:00.000 123 124 I LindberghVulkanProbe: START fresh: native"
        log.observe(start)
        self.assertNotIn("PASS:",log.observe(stale+"\n"+start))

if __name__=="__main__":unittest.main()
