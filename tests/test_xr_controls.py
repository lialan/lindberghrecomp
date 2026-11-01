import ctypes as C
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

class State(C.Structure):
    _fields_=[("gun_hand",C.c_int),("prev_trigger",C.c_int*2),("prev_grip",C.c_int*2),("prev_start",C.c_int*2),
              ("grip_at",C.c_int64*2),("joined_at",C.c_int64),("credit_frames",C.c_uint)]
class Frame(C.Structure):
    _fields_=[("time",C.c_int64),("dual",C.c_int),("auto_coins",C.c_int),("blocked",C.c_int),
              *[(name,C.c_int*2) for name in ("trigger","grip","lower","upper","stick","aimed","start_override","firing")],("fire_supplied",C.c_int)]
class Cabinet(C.Structure):
    _fields_=[("held",C.c_uint),("coins",C.c_int)]

class ControlsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc=shutil.which("cc")
        if not cc:raise unittest.SkipTest("host C compiler unavailable")
        cls.temp=tempfile.TemporaryDirectory();runtime=Path(__file__).parents[1]/"src/runtime"
        library=Path(cls.temp.name)/"controls.so"
        subprocess.run([cc,"-std=c11","-Wall","-Wextra","-Werror","-shared","-fPIC","-I",str(runtime),
                        str(runtime/"xr_controls.c"),"-o",str(library)],check=True)
        cls.lib=C.CDLL(str(library))
        cls.lib.lg_xr_controls_select.argtypes=[C.POINTER(State),C.POINTER(C.c_int),C.c_int]
        cls.lib.lg_xr_controls_update.argtypes=[C.POINTER(State),C.POINTER(Frame)]
        cls.lib.lg_xr_controls_update.restype=Cabinet
    @classmethod
    def tearDownClass(cls):cls.temp.cleanup()
    def setUp(self):self.state=State();self.state.gun_hand=1
    def step(self,ms,**values):
        f=Frame();f.time=1000000000+ms*1000000;f.aimed[:]=[1,1]
        for name,value in values.items():
            if isinstance(value,(list,tuple)):getattr(f,name)[:]=value
            else:setattr(f,name,value)
        self.lib.lg_xr_controls_select(C.byref(self.state),f.trigger,f.dual)
        return self.lib.lg_xr_controls_update(C.byref(self.state),C.byref(f))
    def test_single_gun_switches_only_on_other_hand_pull(self):
        self.assertEqual(self.step(0,trigger=[1,0]).held,1<<4)
        self.assertEqual(self.state.gun_hand,0)
        self.step(10,trigger=[1,1]);self.assertEqual(self.state.gun_hand,1)
        self.step(20,trigger=[1,1]);self.assertEqual(self.state.gun_hand,1)
    def test_grip_reload_is_a_pulse_and_can_be_rearmed(self):
        self.assertEqual(self.step(0,grip=[0,1]).held,1<<5)
        self.assertEqual(self.step(119,grip=[0,1]).held,1<<5)
        self.assertEqual(self.step(120,grip=[0,1]).held,0)
        self.step(200);self.assertEqual(self.step(210,grip=[0,1]).held,1<<5)
    def test_offscreen_trigger_reloads_instead_of_firing(self):
        self.assertEqual(self.step(0,trigger=[0,1],aimed=[0,1]).held,1<<5)
        self.assertEqual(self.step(10,trigger=[0,1],aimed=[1,1]).held,1<<4)
    def test_dual_players_keep_their_own_actions_and_reload(self):
        r=self.step(0,dual=1,trigger=[1,0],grip=[0,1],lower=[1,0],upper=[0,1])
        self.assertEqual(self.state.gun_hand,1)
        self.assertEqual(r.held,(1<<8)|(1<<10)|(1<<5)|(1<<7))
        self.state.gun_hand=0
        r=self.step(500,dual=1,trigger=[0,1],lower=[0,1])
        self.assertEqual(r.held,(1<<8)|(1<<10))
    def test_second_player_credit_precedes_delayed_start(self):
        r=self.step(0,dual=1,auto_coins=1,trigger=[1,0]);self.assertEqual(r.coins,1)
        self.assertFalse(r.held&2)
        self.assertFalse(self.step(149,dual=1,auto_coins=1,trigger=[1,0]).held&2)
        self.assertTrue(self.step(150,dual=1,auto_coins=1,trigger=[1,0]).held&2)
        self.assertFalse(self.step(450,dual=1,auto_coins=1,trigger=[1,0]).held&2)
        self.step(1000,dual=1,auto_coins=1)
        self.assertEqual(self.step(1010,dual=1,auto_coins=1,trigger=[1,0]).coins,0)
        self.step(3100,dual=1,auto_coins=1)
        self.assertEqual(self.step(3110,dual=1,auto_coins=1,trigger=[1,0]).coins,1)
    def test_menu_blocks_a_previously_scheduled_join_start(self):
        self.step(0,dual=1,auto_coins=1,trigger=[1,0])
        r=self.step(150,dual=1,auto_coins=1,blocked=1,trigger=[1,0])
        self.assertEqual((r.held,r.coins),(0,0))

    def test_start_credits_are_edge_triggered(self):
        self.assertEqual(self.step(0,auto_coins=1,stick=[1,0]).coins,1)
        self.assertEqual(self.step(10,auto_coins=1,stick=[1,0]).coins,0)
        self.step(20,auto_coins=1)
        self.assertEqual(self.step(30,auto_coins=1,stick=[0,1]).coins,1)
    def test_startup_credit_occurs_once(self):
        self.assertEqual(sum(self.step(i,auto_coins=1).coins for i in range(500)),1)
    def test_gl_firing_result_preserves_panel_callback_order(self):
        r=self.step(0,blocked=1,fire_supplied=1,firing=[1,0])
        self.assertEqual(r.held,1<<4)
        r=self.step(10,trigger=[0,1],fire_supplied=1,firing=[0,0])
        self.assertEqual(r.held,0)
    def test_panel_suppresses_cabinet_buttons(self):
        r=self.step(0,blocked=1,dual=1,trigger=[1,1],grip=[1,1],lower=[1,1],upper=[1,1],stick=[1,1])
        self.assertEqual((r.held,r.coins),(0,0))

if __name__=="__main__":unittest.main()
