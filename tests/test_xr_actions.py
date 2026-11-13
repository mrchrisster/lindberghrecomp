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
        # openxr/openxr.h: LINDBERGH_TEST_OPENXR_HEADERS, Homebrew / system
        # headers, or the Quest build's loader headers ($DEPS, default the
        # folder next to the repository that holds this submodule: ../deps).
        deps=Path(os.environ["DEPS"]) if os.environ.get("DEPS") else root.parent.parent/"deps"
        candidates=[Path(p) for p in (os.environ.get("LINDBERGH_TEST_OPENXR_HEADERS"),) if p]+[
                    Path("/opt/homebrew/include"),Path("/usr/local/include"),Path("/usr/include"),
                    deps/"oxr/prefab/modules/headers/include"]
        include=next((p for p in candidates if (p/"openxr/openxr.h").exists()),None)
        if include is None:self.skipTest("openxr/openxr.h not found (set LINDBERGH_TEST_OPENXR_HEADERS, or build ../deps with quest/deps.sh)")
        with tempfile.TemporaryDirectory() as d:
            binary=Path(d)/"actions"
            subprocess.run([cc,"-std=c11","-Wall","-Wextra","-Werror","-Wno-missing-field-initializers", "-I",str(include),
                            "-I",str(root/"src/runtime"),str(root/"tests/xr_actions_selftest.c"),str(root/"src/runtime/xr_actions.c"),
                            "-o",str(binary)],check=True)
            subprocess.run([str(binary)],check=True)
if __name__=="__main__":unittest.main()
