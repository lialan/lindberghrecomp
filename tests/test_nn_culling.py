"""The shared NN hook changes CPU side planes, never the render projection."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

class CullingTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc=shutil.which("cc")
        if not cc:raise unittest.SkipTest("host C compiler unavailable")
        cls.temp=tempfile.TemporaryDirectory();folder=Path(cls.temp.name);root=Path(__file__).parents[1]
        (folder/"recomp_imports.h").write_text("#define HLE_IMPORTS(X)\n")
        harness=folder/"culling.c"
        harness.write_text(r'''
#include "lindbergh_rt.h"
#include "nn_culling.h"
#include <assert.h>
#include <math.h>
#include <string.h>
static uint8_t memory[65536];uint8_t *g_guest_base=memory;
static HleHandler installed;static unsigned installs,calls;static int fail;
static float factor;static uint32_t expected_x,expected_y;
static void builder(CPU *c) {
    assert(rd32(0x100)==expected_x&&rd32(0x114)==expected_y);
    assert(pop32(c)==0xDEADBEEFu);++calls;c->eax=123;wr32(0x200,calls);
}
HleHandler dispatch_lifted(uint32_t va) {return va==0x400?builder:NULL;}
int guest_override_va(const char *label,uint32_t va,HleHandler hook) {
    assert(!strcmp(label,"nnTest")&&va==0x400);++installs;if(fail)return 0;installed=hook;return 1;
}
static uint32_t widened(uint32_t bits) {float f;memcpy(&f,&bits,4);f/=factor;memcpy(&bits,&f,4);return bits;}
int main(int argc,char **argv) {
    assert(argc==4);
    const char *immersive=getenv("TEST_IMMERSIVE");
    if(immersive)assert(nn_culling_default_widen()==strtof(immersive,NULL));
    factor=strtof(argv[1],NULL);const int want=atoi(argv[2]);fail=atoi(argv[3]);
    const float fallback=2;const int result=nn_culling_install("nnTest",0x400,0x100,0x114,fallback);
    assert(result==want);
    if(!want){assert(!installed&&calls==0);return 0;}
    assert(installs==1&&installed);
    assert(!nn_culling_install("nnTest",0x400,0x100,0x114,fallback)&&installs==1);
    const uint32_t values[][2]={{0x3fc00000,0xc0100000},{0x80000000,0x00000000},{0x7f800000,0xff800000},{0x7fc12345,0x3f800000}};
    CPU c={0};c.esp=0x8000;
    for(unsigned i=0;i<sizeof(values)/sizeof(values[0]);++i){const uint32_t x=values[i][0],y=values[i][1];wr32(0x100,x);wr32(0x114,y);
        expected_x=widened(x);expected_y=widened(y);installed(&c);
        assert(c.esp==0x8000&&c.eax==123&&rd32(0x200)==i+1);
        assert(rd32(0x100)==x&&rd32(0x114)==y);
    }
    return 0;
}
''')
        cls.binary=folder/"culling"
        subprocess.run([cc,"-std=c11","-Wall","-Wextra","-Werror","-DRECOMP_GUEST_BASE=1",
            "-I",str(folder),"-I",str(root/"src/runtime"),"-I",str(root/"pcrecomp/runtime/recomp32_cpu"),
            str(harness),str(root/"src/runtime/nn_culling.c"),"-lm","-o",str(cls.binary)],check=True)
    @classmethod
    def tearDownClass(cls):cls.temp.cleanup()
    def run_case(self,setting,factor,want,fail=0,extra=None):
        env=dict(os.environ);env.pop("LINDBERGH_CULL_WIDEN",None)
        for key in ("LINDBERGH_STEREO","LINDBERGH_VULKAN_FAKE_YAW","TEST_IMMERSIVE"):env.pop(key,None)
        if extra:env.update(extra)
        if setting is not None:env["LINDBERGH_CULL_WIDEN"]=setting
        result=subprocess.run([str(self.binary),str(factor),str(want),str(fail)],env=env,capture_output=True,text=True)
        self.assertEqual(result.returncode,0,result.stderr)
    def test_default_and_override_restore_projection_and_guest_stack(self):
        self.run_case(None,2,1);self.run_case("4",4,1)
    def test_immersive_defaults_and_explicit_cabinet_override(self):
        self.run_case("1",1,0,extra={"TEST_IMMERSIVE":"1"})
        self.run_case("1",1,0,extra={"LINDBERGH_STEREO":"1","TEST_IMMERSIVE":"4"})
        self.run_case("1",1,0,extra={"LINDBERGH_VULKAN_FAKE_YAW":"0","TEST_IMMERSIVE":"4"})

    def test_invalid_disabled_and_failed_install_leave_guest_untouched(self):
        for setting in ("1","0","-2","nan","inf","4junk"):
            with self.subTest(setting=setting):self.run_case(setting,1,0)
        self.run_case("4",4,0,fail=1)
