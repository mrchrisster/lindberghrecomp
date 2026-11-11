"""Classic / Immersive mode values: env > vr.cfg > the mode's default (world_mode.c)."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ENV_KEYS = ["LINDBERGH_VT3_IMMERSIVE", "LINDBERGH_REPLAYS", "LINDBERGH_ARCADE_OVERLAYS", "LINDBERGH_VT3_CUTAWAYS",
            "LINDBERGH_SHADOW_SCALE", "LINDBERGH_VT3_STAND_Y", "LINDBERGH_VT3_DOLLY", "LINDBERGH_VT3_BACK",
            "LINDBERGH_VT3_SKY", "LINDBERGH_XR_SET", "LINDBERGH_XR_SCALE"]


class WorldModeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc = shutil.which("cc")
        if not cc: raise unittest.SkipTest("host C compiler unavailable")
        cls.temp = tempfile.TemporaryDirectory()
        rt = Path(__file__).resolve().parents[1] / "src/runtime"
        src = Path(cls.temp.name) / "wm.c"
        src.write_text(r'''#include "world_mode.h"
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv) {
    world_mode_init(argc > 1 ? atoi(argv[1]) : 1);
    const WmValues *v = world_mode_values();
    printf("%d %d %d %d %d %.2f %.0f %.0f %d %.1f", v->immersive, v->replays, v->overlays, v->cutaways,
           v->shadow, v->stand_y, v->dolly, v->back, v->sky, v->shadow_scale);
    if (argc > 2) {     /* a menu change: immersive replays on, classic replays off; then the live values */
        LgXrSettings s = LG_XR_SETTINGS_DEFAULT; xr_settings_load(&s);
        s.imm_replays = 1; s.replays = 0; s.imm_dolly = 50; s.imm_shadow = 4096;
        world_mode_update(&s);
        printf(" | %d %d %.0f %d", world_mode_immersive(), world_mode_replays(), world_mode_dolly(), world_mode_values()->shadow);
    }
    printf("\n");
    return 0;
}''')
        cls.bin = Path(cls.temp.name) / "wm"
        for name in ("wm", "wm_desk"):
            subprocess.run([cc, "-std=c11", "-Wall", "-Wextra", "-Werror", *(["-DLINDBERGH_OPENXR"] if name == "wm" else []),
                            "-I", str(rt), str(src), str(rt / "world_mode.c"), str(rt / "xr_settings.c"),
                            "-o", str(Path(cls.temp.name) / name)], check=True)
        cls.desk = Path(cls.temp.name) / "wm_desk"

    @classmethod
    def tearDownClass(cls): cls.temp.cleanup()

    def run_wm(self, cfg=None, env=None, args=("1",), binary=None):
        with tempfile.TemporaryDirectory() as d:
            if cfg is not None: (Path(d) / "vr.cfg").write_text(cfg)
            e = {k: v for k, v in os.environ.items() if k not in ENV_KEYS}
            e.update(env or {})
            p = subprocess.run([str(binary or self.bin), *args], cwd=d, env=e, text=True, capture_output=True, check=True)
            return p.stdout.strip(), p.stderr

    def test_defaults_are_classic(self):
        out, log = self.run_wm()
        self.assertEqual(out, "0 1 1 1 1024 1.50 200 60 1 0.0")
        self.assertIn("[world] mode classic (default)", log)

    def test_vr_cfg_mode_immersive_has_immersive_defaults(self):
        out, log = self.run_wm("mode immersive\nreplays on\noverlays on\n")
        self.assertEqual(out, "1 0 0 0 2048 1.50 200 60 1 2.0")
        self.assertIn("[world] mode immersive (vr.cfg) dolly 200 (default)", log)

    def test_env_pins_the_mode_and_wins_over_vr_cfg(self):
        self.assertEqual(self.run_wm("mode classic\n", {"LINDBERGH_VT3_IMMERSIVE": "1"})[0].split()[0], "1")
        self.assertEqual(self.run_wm("mode immersive\n", {"LINDBERGH_VT3_IMMERSIVE": "0"})[0].split()[0], "0")
        self.assertEqual(self.run_wm("mode immersive\n", {"LINDBERGH_VT3_IMMERSIVE": ""})[0].split()[0], "1")

    def test_env_values_win_over_vr_cfg(self):
        cfg = "mode immersive\nimmersive.replays on\nimmersive.dolly 100\nimmersive.sky off\nimmersive.shadow 4096\n"
        out, log = self.run_wm(cfg)
        self.assertEqual(out, "1 1 0 0 4096 1.50 100 60 0 4.0")
        self.assertIn("dolly 100 (vr.cfg)", log)
        out, log = self.run_wm(cfg, {"LINDBERGH_REPLAYS": "0", "LINDBERGH_VT3_DOLLY": "250", "LINDBERGH_VT3_SKY": "1",
                                     "LINDBERGH_SHADOW_SCALE": "2", "LINDBERGH_VT3_STAND_Y": "1",
                                     "LINDBERGH_VT3_CUTAWAYS": "1", "LINDBERGH_ARCADE_OVERLAYS": "1", "LINDBERGH_VT3_BACK": "0"})
        self.assertEqual(out, "1 0 1 1 2048 1.00 250 0 1 2.0")
        self.assertIn("dolly 250 (env)", log)
        self.assertIn("replays off (env)", log)

    def test_classic_reads_the_old_keys(self):
        out, _ = self.run_wm("replays off\noverlays off\nclassic.cutaways off\nclassic.shadow 2048\nimmersive.replays on\n")
        self.assertEqual(out, "0 0 0 0 2048 1.50 200 60 1 2.0")

    def test_a_game_without_world_mode_ignores_the_stored_mode(self):
        self.assertEqual(self.run_wm("mode immersive\n", args=("0",))[0].split()[0], "0")
        self.assertEqual(self.run_wm("mode immersive\n", {"LINDBERGH_VT3_IMMERSIVE": "1"}, args=("0",))[0].split()[0], "1")

    def test_desktop_reads_env_only(self):
        out, _ = self.run_wm("mode immersive\nreplays off\n", binary=self.desk)
        self.assertEqual(out.split()[:2], ["0", "1"])
        self.assertEqual(self.run_wm(None, {"LINDBERGH_VT3_IMMERSIVE": "1"}, binary=self.desk)[0].split()[0], "1")

    def test_live_update_keeps_mode_and_shadow(self):
        out, _ = self.run_wm("mode immersive\n", args=("1", "update"))
        self.assertEqual(out.split(" | ")[1], "1 1 50 2048")       # replays now on, dolly 50, shadow as started
        out, _ = self.run_wm("mode immersive\n", {"LINDBERGH_REPLAYS": "0"}, args=("1", "update"))
        self.assertEqual(out.split(" | ")[1], "1 0 50 2048")       # env still wins
        out, _ = self.run_wm("mode classic\n", args=("1", "update"))
        self.assertEqual(out.split(" | ")[1], "0 0 50 1024")       # classic: its own replays (now off)


if __name__ == "__main__": unittest.main()
