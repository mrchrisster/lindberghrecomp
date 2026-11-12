"""The start-up screen's title (splash_logo.c): the game's logo read from the
player's files and composited into our own picture, or our own title when the
file is missing - DXT decoding, the ABRS layout, the fit, the fallback."""
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest

AW, AH = 400, 300          # the test atlas: a 400x200 panel, parts below


def dds(w, h, block):
    """a DXT5 DDS (header + one block repeated) as the game's files embed it"""
    hdr = bytearray(128)
    hdr[0:4] = b"DDS "
    struct.pack_into("<7I", hdr, 4, 124, 0x81007, h, w, w * h, 0, 0)
    struct.pack_into("<I", hdr, 80, 4)
    hdr[84:88] = b"DXT5"
    return bytes(hdr) + block * ((w // 4) * (h // 4))


def dxt5_solid(rgb565, alpha=255):
    return bytes([alpha, alpha, 0, 0, 0, 0, 0, 0]) + struct.pack("<HHI", rgb565, 0, 0)


def abr(right=True, left=True):
    """an ABRS file: the 512x512 right part red, the 512x256 left part blue"""
    out = b"ABRS" + bytes(60) + b"texture_I_ADV_title_us\0"
    if right: out += dds(512, 512, dxt5_solid(0xF800))
    out += b"I_ADV_title_us2\0"
    if left: out += dds(512, 256, dxt5_solid(0x001F))
    return out


class SplashLogoTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc = shutil.which("cc")
        if not cc: raise unittest.SkipTest("host C compiler unavailable")
        cls.temp = tempfile.TemporaryDirectory()
        rt = Path(__file__).resolve().parents[1] / "src/runtime"
        src = Path(cls.temp.name) / "sl.c"
        src.write_text(r'''#include "splash_logo.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* sl compose <file> <out.rgba> : a grey panel, a green VR badge and a blue title in the parts row
 * sl dxt1 : one DXT1 block in the 3-colour + transparent mode */
int main(int argc, char **argv) {
    if (argc < 2) return 2;
    if (!strcmp(argv[1], "dxt1")) {
        unsigned char b[8] = { 0x00, 0x00, 0x1F, 0x00, 0xE4, 0xE4, 0xE4, 0xE4 };   /* c0 black <= c1 blue: idx 0,1,2,3 per row */
        unsigned char o[64];
        printf("%d", lg_dxt_decode(b, 8, 4, 4, 0, o));
        for (int i = 0; i < 4; i++) printf(" %d,%d,%d,%d", o[i * 4], o[i * 4 + 1], o[i * 4 + 2], o[i * 4 + 3]);
        printf(" short %d\n", lg_dxt_decode(b, 7, 4, 4, 0, o));
        return 0;
    }
    const int W = 400, H = 300;
    unsigned char *px = malloc(W * H * 4);
    for (int i = 0; i < W * H; i++) { unsigned char *p = px + i * 4; p[0] = p[1] = p[2] = 50; p[3] = 255; }
    for (int y = 200; y < 220; y++) for (int x = 0; x < 40; x++) { unsigned char *p = px + (y * W + x) * 4; p[0] = 0; p[1] = 255; p[2] = 0; }
    for (int y = 200; y < 230; y++) for (int x = 50; x < 150; x++) { unsigned char *p = px + (y * W + x) * 4; p[0] = 0; p[1] = 0; p[2] = 255; }
    static const int logo_at[4] = { 100, 20, 200, 100 }, vr[4] = { 0, 200, 40, 20 }, vr_at[2] = { -5, -5 },
                     title[4] = { 50, 200, 100, 30 }, title_at[2] = { 10, 10 };
    LgSplashPlaces pl = { logo_at, vr, vr_at, title, title_at, argv[2] };
    printf("%d\n", lg_splash_compose(px, W, H, &pl));
    FILE *f = fopen(argv[3], "wb"); fwrite(px, 1, W * H * 4, f); fclose(f);
    return 0;
}''')
        cls.bin = Path(cls.temp.name) / "sl"
        subprocess.run([cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(rt), str(src), str(rt / "splash_logo.c"),
                        "-o", str(cls.bin)], check=True)

    @classmethod
    def tearDownClass(cls): cls.temp.cleanup()

    def compose(self, data, env=None):
        with tempfile.TemporaryDirectory() as d:
            f, out = Path(d) / "logo.abr", Path(d) / "out.rgba"
            if data is not None: f.write_bytes(data)
            e = dict(os.environ); e.pop("LINDBERGH_SPLASH_LOGO", None); e.update(env or {})
            p = subprocess.run([str(self.bin), "compose", str(f), str(out)], text=True, capture_output=True, check=True, env=e)
            px = out.read_bytes()
        at = lambda x, y: tuple(px[(y * AW + x) * 4:(y * AW + x) * 4 + 3])
        return p.stdout.strip(), p.stderr, at

    def test_dxt1_transparent_mode(self):
        out = subprocess.run([str(self.bin), "dxt1"], text=True, capture_output=True, check=True).stdout.split()
        # c0 (black) <= c1 (blue): 0 black, 1 blue, 2 half blue, 3 transparent
        self.assertEqual(out[0], "0")
        self.assertEqual(out[1:5], ["0,0,0,255", "0,0,255,255", "0,0,127,255", "0,0,0,0"])
        self.assertEqual(out[-1], "-1")                                       # short data refused

    def test_logo_fitted_with_vr_beside_it(self):
        r, log, at = self.compose(abr())
        self.assertEqual(r, "1")
        self.assertIn("the game's title logo from", log)
        self.assertIn("(1024x512, shown 200x100) with VR", log)
        # the group (logo 200 + gap -5 + badge 40 = 235) centred on the place's centre (200): logo x 83..283
        self.assertEqual(at(84, 70), (0, 0, 255)); self.assertEqual(at(81, 70), (50, 50, 50))
        self.assertEqual(at(281, 25), (255, 0, 0)); self.assertEqual(at(285, 25), (50, 50, 50))
        self.assertEqual(at(250, 25), (255, 0, 0))                            # the right part: the whole height
        self.assertEqual(at(110, 70), (0, 0, 255))                            # the left part: the middle half
        self.assertEqual(at(110, 25), (50, 50, 50))                           # above it: the panel shows
        self.assertEqual(at(300, 100), (0, 255, 0))                           # VR: logo right - 5, bottom up 5
        self.assertEqual(at(316, 100), (0, 255, 0)); self.assertEqual(at(320, 100), (50, 50, 50))
        self.assertEqual(at(20, 20), (50, 50, 50))                            # no fallback title

    def test_missing_or_bad_file_gives_our_title(self):
        for data, why in ((None, "no "), (b"XXXX" + bytes(100), "not an ABRS"), (abr(left=False), "two textures"),
                          (abr()[:6000], "two textures")):
            r, log, at = self.compose(data)
            self.assertEqual(r, "0", why)
            self.assertIn("our own title", log)
            self.assertIn(why, log)
            self.assertEqual(at(200, 20), (0, 0, 255))                        # the title, centred on the place
            self.assertEqual(at(20, 20), (50, 50, 50))                        # (not at title_at's x)
            self.assertEqual(at(300, 25), (50, 50, 50))                       # no logo, no VR badge
            self.assertEqual(at(300, 100), (50, 50, 50))

    def test_env_forces_the_fallback(self):
        r, log, _ = self.compose(abr(), {"LINDBERGH_SPLASH_LOGO": "0"})
        self.assertEqual(r, "0")
        self.assertIn("LINDBERGH_SPLASH_LOGO=0", log)


if __name__ == "__main__":
    unittest.main()
