import ctypes as C
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
class OverlayGeometryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc=shutil.which("cc")
        if not cc:raise unittest.SkipTest("host C compiler unavailable")
        cls.temp=tempfile.TemporaryDirectory();root=Path(__file__).parents[1]
        lib=Path(cls.temp.name)/"geometry.so"
        subprocess.run([cc,"-std=c11","-shared","-fPIC",str(root/"src/runtime/xr_overlay_geometry.c"),"-lm","-o",str(lib)],check=True)
        cls.lib=C.CDLL(str(lib));cls.lib.xo_gun_capacity.restype=C.c_uint
        cls.lib.xo_gun_build.argtypes=[C.c_int,C.c_int,C.POINTER(C.c_float),C.c_uint,C.POINTER(C.c_float)]
        cls.reference=json.loads((root/"tests/overlay_mesh_reference.json").read_text())["meshes"]
    @classmethod
    def tearDownClass(cls):cls.temp.cleanup()
    def test_every_mesh_matches_original_gl_bytes(self):
        capacity=self.lib.xo_gun_capacity();mesh=(C.c_float*(capacity*9))();rail=C.c_float()
        for key,reference in self.reference.items():
            with self.subTest(key=key):
                model,sight=map(int,key.split(","));n=self.lib.xo_gun_build(model,sight,mesh,capacity,C.byref(rail))
                self.assertEqual(n,reference["vertices"])
                self.assertEqual(hashlib.sha256(C.string_at(mesh,n*36)).hexdigest(),reference["sha256"])
    def test_small_destination_is_rejected_without_writing(self):
        mesh=(C.c_float*9)(*[123]*9);rail=C.c_float(99)
        self.assertEqual(self.lib.xo_gun_build(1,1,mesh,1,C.byref(rail)),0)
        self.assertEqual(list(mesh),[123]*9);self.assertEqual(rail.value,99)
if __name__=="__main__":unittest.main()
