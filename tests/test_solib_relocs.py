"""tools/recomp/solib.py's output for the dlopened libraries: a relocation
table and the file's check - never the library's bytes (the app reads the
player's copy at start-up, solib.c; tests/selftest covers that side).
Needs pyelftools and capstone (the recompiler's venv)."""
import hashlib
import os
from pathlib import Path
import struct
import sys
import tempfile
import types
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools.recomp import solib  # noqa: E402

BASE = 0xB4000000


class SolibRelocTests(unittest.TestCase):
    def test_apply_reloc_each_type(self):
        img = bytearray(64)
        struct.pack_into("<4I", img, 0x10, 0x10, 4, 0, 0)
        solib.apply_reloc(img, BASE, 0x10, solib.R_386_RELATIVE, 0)
        solib.apply_reloc(img, BASE, 0x14, solib.R_386_32, 0xF3000000)
        solib.apply_reloc(img, BASE, 0x18, solib.R_386_PC32, BASE)
        solib.apply_reloc(img, BASE, 0x1C, solib.R_386_JMP_SLOT, 0xB4000080)
        self.assertEqual(struct.unpack_from("<4I", img, 0x10),
                         (BASE + 0x10, 0xF3000004, 0xFFFFFFE8, 0xB4000080))

    def test_table_has_no_library_bytes(self):
        with tempfile.TemporaryDirectory() as d:
            exe = Path(d, "game", "vt3_Lindbergh_HD")
            so = Path(d, "lib", "libX.so")
            exe.parent.mkdir(); so.parent.mkdir()
            exe.write_bytes(b"\x7fELF")
            marker = bytes(range(7, 247))          # a run that must not reach the C
            raw = b"\x7fELF" + marker + bytes(16)
            so.write_bytes(raw)
            img = bytearray(4096); img[:len(raw)] = raw
            solib.apply_reloc(img, BASE, 0x100, solib.R_386_RELATIVE, 0)
            lib = types.SimpleNamespace(path=str(so), name="libX.so", base=BASE, size=4096, img=img,
                                        relocs=[(0x100, solib.R_386_RELATIVE, 0)], init=0x40,
                                        init_array=(None, 0))
            Path(d, "recomp_solib_img.c").write_text("stale")
            solib.write_relocs(d, [lib], str(exe))
            self.assertFalse(Path(d, "recomp_solib_img.c").exists())
            c = Path(d, "recomp_solib_relocs.c").read_text()
            self.assertIn('"libX.so", "../lib/libX.so"', c)
            self.assertIn(hashlib.sha256(raw).hexdigest(), c)
            self.assertIn(hashlib.sha256(bytes(img)).hexdigest(), c)
            self.assertIn("{ 0x100u, 8, 0x00000000u }", c)
            self.assertNotIn(",".join(str(b) for b in marker[:16]), c)
            self.assertNotIn("unsigned char", c)
            self.assertEqual(Path(d, "recomp_solib.txt").read_text(),
                             "../lib/libX.so %d %s\n" % (len(raw), hashlib.sha256(raw).hexdigest()))


if __name__ == "__main__":
    unittest.main()
