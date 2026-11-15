"""The lockstep protocol (src/runtime/lockstep.c) and the state-hash log
compare tool, without a network or a game: netplay_selftest.c joins two
sessions by a simulated link (delay, jitter, loss, reordering) and checks both
see the same input every frame, the delay they agree on, the handshake's
mismatch message and desync detection."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = Path(__file__).parent
RUNTIME = HERE.parent / "src/runtime"


class NetplayTests(unittest.TestCase):
    def test_lockstep_selftest(self):
        cc = shutil.which("cc")
        if not cc:
            raise unittest.SkipTest("host C compiler unavailable")
        with tempfile.TemporaryDirectory() as t:
            exe = Path(t) / "nps"
            subprocess.run([cc, "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror", "-I", str(RUNTIME),
                            str(RUNTIME / "lockstep.c"), str(HERE / "netplay_selftest.c"), "-o", str(exe), "-lm"],
                           check=True)
            r = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr + r.stdout)
            self.assertIn("all passed", r.stdout)

    def test_state_hash_diff(self):
        tool = HERE.parent / "tools/state_hash_diff.py"
        with tempfile.TemporaryDirectory() as t:
            a, b = Path(t) / "a.log", Path(t) / "b.log"
            a.write_text("x\n[hash] frame 60: 00aa\n[hash] frame 120: 00bb\n[hash] frame 180: 00cc\n")
            b.write_text("[hash] frame 60: 00aa\nnoise\n[hash] frame 120: 00bd\n[hash] frame 180: 00ce\n")
            r = subprocess.run([sys.executable, str(tool), str(a), str(b)], capture_output=True, text=True)
            self.assertEqual(r.returncode, 1)
            self.assertIn("first divergence at frame 120", r.stdout)
            r = subprocess.run([sys.executable, str(tool), str(a), str(a)], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0)
            self.assertIn("identical", r.stdout)


if __name__ == "__main__":
    unittest.main()
