from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest
class ActionsTests(unittest.TestCase):
    def test_action_lifecycle_and_sampling(self):
        cc=shutil.which("cc")
        if not cc:self.skipTest("host C compiler unavailable")
        root=Path(__file__).resolve().parents[1]
        candidates=[Path(os.environ.get("LINDBERGH_TEST_OPENXR_HEADERS","/opt/homebrew/include")),
                    Path("/usr/include"),root.parent.parent/"deps/oxr/prefab/modules/headers/include"]
        include=next((p for p in candidates if (p/"openxr/openxr.h").exists()),None)
        if include is None:self.skipTest("OpenXR declarations unavailable")
        with tempfile.TemporaryDirectory() as d:
            binary=Path(d)/"actions"
            subprocess.run([cc,"-std=c11","-Wall","-Wextra","-Werror","-Wno-missing-field-initializers", "-I",str(include),
                            "-I",str(root/"src/runtime"),str(root/"tests/xr_actions_selftest.c"),str(root/"src/runtime/xr_actions.c"),
                            "-o",str(binary)],check=True)
            subprocess.run([str(binary)],check=True)
if __name__=="__main__":unittest.main()
