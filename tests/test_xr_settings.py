import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

class XrSettingsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler=shutil.which("cc")
        if not compiler:raise unittest.SkipTest("host C compiler unavailable")
        cls.temp=tempfile.TemporaryDirectory()
        root=Path(__file__).resolve().parents[1]
        harness=Path(cls.temp.name)/"settings.c"
        harness.write_text(r'''#include "xr_settings.h"
#include <stdio.h>
int main(void) {
 LgXrSettings s=LG_XR_SETTINGS_DEFAULT;xr_settings_load(&s);
 printf("%d %d %d %.3f %.3f %d %d %d\n",s.width,s.resolution,s.refresh,s.scale,s.screen,s.dual,s.left_handed,s.laser);
 xr_settings_save(&s);return 0;
}''')
        cls.binary=Path(cls.temp.name)/"settings"
        subprocess.run([compiler,"-I",str(root/"src/runtime"),str(harness),str(root/"src/runtime/xr_settings.c"),"-o",str(cls.binary)],check=True)

    @classmethod
    def tearDownClass(cls):cls.temp.cleanup()

    def run_settings(self,root,extra=None):
        env=os.environ.copy()
        env.pop("LINDBERGH_XR_SCALE",None);env.pop("LINDBERGH_XR_SET",None)
        env.update(extra or {})
        return subprocess.check_output([str(self.binary)],cwd=root,env=env,text=True).strip()

    def test_defaults_match_gl_quality(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertEqual(self.run_settings(d),"2064 2280 120 1.000 3.000 0 0 0")

    def test_parent_file_then_run_overrides(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);(root/"elf").mkdir()
            (root/"vr.cfg").write_text("width 1728\nresolution 1920\nscale 2\nrefresh 90\n")
            result=self.run_settings(root/"elf",{"LINDBERGH_XR_SCALE":"3","LINDBERGH_XR_SET":"width=2064,refresh=120,dual=on,hand=left,laser=dot"})
            self.assertEqual(result,"2064 1920 120 2.000 3.000 1 1 1")
            self.assertIn("dual on",(root/"elf/vr.cfg").read_text())

    def test_invalid_sizes_restore_existing_defaults(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertEqual(self.run_settings(d,{"LINDBERGH_XR_SET":"width=-1,resolution=1,screen=0,scale=-2"}),"0 2280 120 1.000 3.000 0 0 0")

if __name__=="__main__":unittest.main()
