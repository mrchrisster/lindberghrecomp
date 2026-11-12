"""Virtua Tennis 3's save file (vt3_save.c): one card record per character, safe writes."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

REC = 0x264
HDR = 32
ENT = 24 + REC


class Vt3SaveTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc = shutil.which("cc")
        if not cc: raise unittest.SkipTest("host C compiler unavailable")
        cls.temp = tempfile.TemporaryDirectory()
        rt = Path(__file__).resolve().parents[1] / "src/runtime"
        src = Path(cls.temp.name) / "sv.c"
        src.write_text(r'''#include "vt3_save.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* sv <file> open | store <ch> <fill> [<ch> <fill> ...] : prints the open result, the count, each character's first byte */
int main(int argc, char **argv) {
    int r = vt3_save_open(argv[1]);
    printf("%d %d", r, vt3_save_count());
    for (int i = 3; i + 1 < argc; i += 2) {
        unsigned char rec[VT3_SAVE_REC];
        memset(rec, atoi(argv[i + 1]), sizeof rec);
        rec[0] = (unsigned char)atoi(argv[i]);
        printf(" w%d", vt3_save_store(atoi(argv[i]), rec));
    }
    for (int ch = 0; ch < 40; ch++) {
        const unsigned char *p = vt3_save_find(ch);
        if (p) printf(" %d:%d", ch, p[1]);
    }
    printf("\n");
    return 0;
}''')
        cls.bin = Path(cls.temp.name) / "sv"
        subprocess.run([cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(rt), str(src), str(rt / "vt3_save.c"),
                        "-o", str(cls.bin)], check=True)

    @classmethod
    def tearDownClass(cls): cls.temp.cleanup()

    def sv(self, path, *args):
        p = subprocess.run([str(self.bin), str(path), "x", *map(str, args)], text=True, capture_output=True, check=True)
        return p.stdout.strip(), p.stderr

    def test_missing_file_is_fresh_and_no_file_until_stored(self):
        with tempfile.TemporaryDirectory() as d:
            f = Path(d) / "vt3_save.bin"
            out, log = self.sv(f)
            self.assertEqual(out, "0 0")
            self.assertIn("no save yet", log)
            self.assertFalse(f.exists())

    def test_store_and_load_per_character(self):
        with tempfile.TemporaryDirectory() as d:
            f = Path(d) / "vt3_save.bin"
            self.assertEqual(self.sv(f, 3, 7)[0], "0 0 w0 3:7")
            self.assertEqual(f.stat().st_size, HDR + ENT)
            self.assertFalse((Path(d) / "vt3_save.bin.tmp").exists())
            self.assertEqual(self.sv(f, 5, 9)[0], "1 1 w0 3:7 5:9")              # a second character
            self.assertTrue((Path(d) / "vt3_save.bin.bak").exists())            # the previous file kept
            self.assertEqual((Path(d) / "vt3_save.bin.bak").stat().st_size, HDR + ENT)
            self.assertEqual(self.sv(f, 3, 11)[0], "1 2 w0 3:11 5:9")             # replaced, not added
            self.assertEqual(self.sv(f)[0], "1 2 3:11 5:9")
            self.assertEqual(f.read_bytes()[:8], b"VT3SAVE\0")

    def test_corrupt_file_falls_back_to_backup_and_is_kept_aside(self):
        with tempfile.TemporaryDirectory() as d:
            f = Path(d) / "vt3_save.bin"
            self.sv(f, 3, 7); self.sv(f, 3, 8)                                   # .bak has 7, file 8
            b = bytearray(f.read_bytes()); b[HDR + 24 + 100] ^= 0xFF; f.write_bytes(bytes(b))
            out, log = self.sv(f)
            self.assertEqual(out, "2 1 3:7")
            self.assertIn("unreadable (checksum)", log)
            aside = [p for p in os.listdir(d) if p.startswith("vt3_save.bin.unreadable-")]
            self.assertEqual(len(aside), 1)
            self.assertFalse(f.exists())                                          # never written over
            self.assertEqual(self.sv(f, 3, 9)[0], "2 1 w0 3:9")                   # no file: the backup again; the next store writes a good file
            self.assertEqual(self.sv(f)[0], "1 1 3:9")

    def test_corrupt_file_and_backup_start_fresh(self):
        for bad in (b"garbage", b"", None):
            with tempfile.TemporaryDirectory() as d:
                f = Path(d) / "vt3_save.bin"
                self.sv(f, 3, 7)
                good = f.read_bytes()
                f.write_bytes(bad if bad is not None else good[:-5])              # junk, empty, truncated
                out, log = self.sv(f)
                self.assertEqual(out, "3 0", bad)
                self.assertIn("starting a fresh save", log)
                self.assertTrue(any(p.startswith("vt3_save.bin.unreadable-") for p in os.listdir(d)))

    def test_missing_file_with_backup_loads_the_backup(self):
        with tempfile.TemporaryDirectory() as d:
            f = Path(d) / "vt3_save.bin"
            self.sv(f, 3, 7); self.sv(f, 3, 8)
            f.unlink()                                                            # a write cut between its two renames
            out, log = self.sv(f)
            self.assertEqual(out, "2 1 3:7")
            self.assertIn("loaded from the backup", log)

    def test_record_checksum_and_length_checked(self):
        with tempfile.TemporaryDirectory() as d:
            f = Path(d) / "vt3_save.bin"
            self.sv(f, 3, 7)
            good = f.read_bytes()
            f.write_bytes(good + b"\0")                                            # extra byte
            self.assertIn("wrong length", self.sv(f)[1])
            b = bytearray(good); b[12] = 0x10; f.write_bytes(bytes(b))            # another record size
            self.assertIn("wrong record size", self.sv(f)[1])


if __name__ == "__main__":
    unittest.main()
