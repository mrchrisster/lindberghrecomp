import os
import sys
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

# The build's own panel size (xr_settings.h LG_XR_NATIVE_*, ec78b3b): the
# default and the fallback for invalid sizes. This harness is built for the host.
NATIVE="2160 2160" if sys.platform.startswith("linux") else "2064 2280"

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
            self.assertEqual(self.run_settings(d),NATIVE+" 120 1.000 3.000 0 0 0")

    def test_parent_file_then_run_overrides(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);(root/"elf").mkdir()
            (root/"vr.cfg").write_text("width 1728\nresolution 1920\nscale 2\nrefresh 90\n")
            result=self.run_settings(root/"elf",{"LINDBERGH_XR_SCALE":"3","LINDBERGH_XR_SET":"width=2064,refresh=120,dual=on,hand=left,laser=dot"})
            self.assertEqual(result,"2064 1920 120 2.000 3.000 1 1 1")
            saved=(root/"elf/vr.cfg").read_text()
            # LINDBERGH_XR_SET is for the run only: the file's values (or defaults) are saved
            self.assertIn("dual off",saved);self.assertIn("refresh 90",saved);self.assertIn("width 1728",saved)
            self.assertIn("hand right",saved);self.assertIn("laser laser",saved);self.assertIn("scale 2",saved)

    def test_invalid_sizes_restore_existing_defaults(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertEqual(self.run_settings(d,{"LINDBERGH_XR_SET":"width=-1,resolution=1,screen=0,scale=-2"}),NATIVE+" 120 1.000 3.000 0 0 0")

    MODE_KEYS={"mode":"classic","classic.shadow":"1024","classic.cutaways":"on","immersive.replays":"off",
               "immersive.overlays":"off","immersive.cutaways":"off","immersive.dolly":"200",
               "immersive.back":"60","immersive.sky":"stadium","immersive.shadow":"2048",
               "immersive.height":"0"}

    @staticmethod
    def parse(text):
        return dict(l.split(" ",1) for l in text.strip().splitlines())

    def test_old_file_keeps_its_values_and_gains_mode_defaults(self):
        old=("scale 1\nscreen 2.5\naim 15\nresolution 2208\nwidth 2064\nrefresh 120\nhand right\nlaser laser\ncoins auto\n"
             "sight none\ncrosshair on\ngun on\nhud_pitch 10\ndual off\nstats off\nfoveation high\noverlays off\nreplays off\n")
        with tempfile.TemporaryDirectory() as d:
            (Path(d)/"vr.cfg").write_text(old)
            self.run_settings(d)
            saved=self.parse((Path(d)/"vr.cfg").read_text())
            for k,v in self.parse(old).items():self.assertEqual(saved[k],v,k)
            for k,v in self.MODE_KEYS.items():self.assertEqual(saved[k],v,k)

    def test_mode_keys_round_trip(self):
        cfg={"mode":"immersive","classic.shadow":"4096","classic.cutaways":"off","immersive.replays":"on",
             "immersive.overlays":"on","immersive.cutaways":"on","immersive.dolly":"250",
             "immersive.back":"70","immersive.sky":"painted","immersive.shadow":"1024",
             "immersive.height":"-10"}
        with tempfile.TemporaryDirectory() as d:
            (Path(d)/"vr.cfg").write_text("".join(f"{k} {v}\n" for k,v in cfg.items()))
            self.run_settings(d)
            first=(Path(d)/"vr.cfg").read_text();saved=self.parse(first)
            for k,v in cfg.items():self.assertEqual(saved[k],v,k)
            self.run_settings(d)
            self.assertEqual((Path(d)/"vr.cfg").read_text(),first)

    def test_unknown_keys_ignored_and_bad_values_defaulted(self):
        with tempfile.TemporaryDirectory() as d:
            (Path(d)/"vr.cfg").write_text("bogus 12\nmode immersive\nimmersive.shadow 3000\nimmersive.stand_y 9\nimmersive.dolly -5\nfuture.key on\n")
            self.run_settings(d)
            saved=self.parse((Path(d)/"vr.cfg").read_text())
            self.assertNotIn("bogus",saved);self.assertNotIn("future.key",saved)
            self.assertEqual(saved["mode"],"immersive");self.assertEqual(saved["immersive.shadow"],"2048")
            self.assertNotIn("immersive.stand_y",saved);self.assertEqual(saved["immersive.dolly"],"200")

    def test_step_back_and_height_snap_to_choices(self):
        # an older panel's Step back (0 / 3 / 9 / 12 m) and odd heights become the nearest choice
        for back,want in (("0","50"),("30","50"),("90","80"),("120","80"),("65","60"),("-7","50"),("999","80")):
            with tempfile.TemporaryDirectory() as d:
                (Path(d)/"vr.cfg").write_text(f"immersive.back {back}\nimmersive.height {int(back)//4}\n")
                self.run_settings(d)
                saved=self.parse((Path(d)/"vr.cfg").read_text())
                self.assertEqual(saved["immersive.back"],want,back)
                self.assertIn(saved["immersive.height"],("-20","-10","0","10","20"))
        with tempfile.TemporaryDirectory() as d:
            (Path(d)/"vr.cfg").write_text("immersive.height 14\n")
            self.run_settings(d)
            self.assertEqual(self.parse((Path(d)/"vr.cfg").read_text())["immersive.height"],"10")

    def test_old_stand_and_sky_keys(self):
        # a file from before the stadium sky: the stands' stretch key is dropped, sky on = the stadium's
        with tempfile.TemporaryDirectory() as d:
            (Path(d)/"vr.cfg").write_text("mode immersive\nimmersive.stand_y 1.5\nimmersive.sky on\nimmersive.dolly 150\n")
            self.run_settings(d)
            saved=self.parse((Path(d)/"vr.cfg").read_text())
            self.assertNotIn("immersive.stand_y",saved);self.assertEqual(saved["immersive.sky"],"stadium")
            self.assertEqual(saved["immersive.dolly"],"150")
        for v in ("off","painted","stadium"):
            with tempfile.TemporaryDirectory() as d:
                (Path(d)/"vr.cfg").write_text("immersive.sky %s\n"%v)
                self.run_settings(d)
                self.assertEqual(self.parse((Path(d)/"vr.cfg").read_text())["immersive.sky"],v)

    def test_env_overrides_are_not_saved(self):
        with tempfile.TemporaryDirectory() as d:
            (Path(d)/"vr.cfg").write_text("replays on\nmode classic\n")
            self.run_settings(d,{"LINDBERGH_XR_SET":"replays=off,mode=immersive,immersive.dolly=300",
                                 "LINDBERGH_REPLAYS":"0","LINDBERGH_VT3_IMMERSIVE":"1"})
            saved=self.parse((Path(d)/"vr.cfg").read_text())
            self.assertEqual(saved["replays"],"on");self.assertEqual(saved["mode"],"classic")
            self.assertEqual(saved["immersive.dolly"],"200")

if __name__=="__main__":unittest.main()
