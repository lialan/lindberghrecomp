"""Shared settings policy: GL and native Vulkan must keep identical actions."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

class PanelTests(unittest.TestCase):
    def test_settings_actions_and_panel_geometry(self):
        cc=shutil.which("cc")
        if not cc:self.skipTest("host C compiler unavailable")
        runtime=Path(__file__).parents[1]/"src/runtime"
        with tempfile.TemporaryDirectory() as temp:
            folder=Path(temp)
            source=folder/"panel.c"
            source.write_text(r'''
#include "xr_panel.h"
#include <assert.h>
#include <math.h>
#include <string.h>
int main(void) {
    LgXrSettings cfg=LG_XR_SETTINGS_DEFAULT;
    XpPanel p={0};int hand=1;float rates[]={72,90,120};
    assert(XP_COUNT==XM_ROWS);
    assert(xp_menu_button(&p,1,0)&&p.open&&p.hover==-1&&p.dirty);
    assert(!xp_menu_button(&p,1,1)&&p.open);
    float head[]={0,1.6f,0},q[]={0,0,0,1};xp_place(&p,head,q);
    float d[3],hit[3],u,v,m[16];
    for(int i=0;i<3;++i)d[i]=p.c[i]-head[i];
    assert(xp_hit(&p,head,d,&u,&v,hit));
    assert(fabsf(u-.5f)<1e-5f&&fabsf(v-.5f)<1e-5f);
    xp_matrix(&p,m);
    for(int i=0;i<3;++i){assert(m[12+i]==p.c[i]);assert(m[i]==p.right[i]*p.hw);}
    for(int i=0;i<3;++i)d[i]=-d[i];
    assert(!xp_hit(&p,head,d,&u,&v,hit));
    int64_t held=0;
    assert(!xp_hold_button(&p,1,0,0,100,&held));
    assert(!xp_hold_button(&p,1,1,0,1000000099,&held));
    assert(xp_hold_button(&p,1,1,0,1000000100,&held)&&!p.open);
    assert(!xp_hold_button(&p,1,1,0,2000000100,&held));
    assert(!xp_hold_button(&p,1,0,1,3000000100,&held));
    assert(!xp_hold_button(&p,1,1,1,4000000100,&held));
    unsigned flags=xp_choose(&p,&cfg,&hand,XP_HAND,rates,3);
    assert((flags&XP_HAND_CHANGED)&&hand==0&&cfg.left_handed);
    flags=xp_choose(&p,&cfg,&hand,XP_RATE,rates,3);
    assert((flags&XP_RATE_CHANGED)&&cfg.refresh==72);
    xp_choose(&p,&cfg,&hand,XP_RES,rates,3);
    assert(cfg.width==0&&cfg.resolution==1216);
    for(int i=0;i<5;++i)xp_choose(&p,&cfg,&hand,XP_RES,rates,3);
    assert(cfg.width==2064&&cfg.resolution==2280);
    xp_choose(&p,&cfg,&hand,XP_COIN,rates,3);assert(p.coin==1);
    xp_choose(&p,&cfg,&hand,XP_TEST,rates,3);assert(p.test_frames==12);
    xp_choose(&p,&cfg,&hand,XP_SERVICE,rates,3);assert(p.service_frames==12);
    flags=xp_choose(&p,&cfg,&hand,XP_RESET,rates,3);assert(p.reset_armed&&!(flags&XP_RESTART));
    assert(!xp_choose(&p,&cfg,&hand,-1,rates,3)&&p.reset_armed);
    XmItem items[XP_COUNT];xp_items(&p,&cfg,hand,1,2280,2064,items);
    assert(strstr(items[XP_RESET].value,"again"));
    flags=xp_choose(&p,&cfg,&hand,XP_RESET,rates,3);assert(flags&XP_RESTART);
    xp_choose(&p,&cfg,&hand,XP_RESET,rates,3);
    xp_choose(&p,&cfg,&hand,XP_SIGHT,rates,3);assert(!p.reset_armed);
    assert(!(xp_choose(&p,&cfg,&hand,XP_RESET,rates,3)&XP_RESTART));
    cfg.resolution=1216;xp_items(&p,&cfg,hand,1,2280,2064,items);
    assert(strstr(items[XP_RES].value,"restart"));
    cfg.dual=1;xp_items(&p,&cfg,hand,0,2280,2064,items);
    assert(!strcmp(items[XP_HAND].label,"Player 1 hand"));
    assert(!strcmp(items[XP_HUD].value,"With the screen"));
    return 0;
}
''')
            binary=folder/"panel"
            subprocess.run([cc,"-std=c11","-Wall","-Wextra","-Werror","-I",str(runtime),str(source),
                            str(runtime/"xr_panel.c"),str(runtime/"stereo_projection.c"),"-lm","-o",str(binary)],check=True)
            subprocess.run([str(binary)],check=True)
