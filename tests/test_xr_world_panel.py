"""The world-mode game's panel pages (xr_world_panel.c): rows, values, clicks."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class WorldPanelTests(unittest.TestCase):
    def test_pages_items_and_choices(self):
        cc = shutil.which("cc")
        if not cc: self.skipTest("host C compiler unavailable")
        rt = Path(__file__).parents[1] / "src/runtime"
        with tempfile.TemporaryDirectory() as temp:
            src = Path(temp) / "wp.c"
            src.write_text(r'''
#include "xr_world_panel.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static int has(const int *r, int n, int id) { for (int i = 0; i < n; i++) if (r[i] == id) return 1; return 0; }
static int find(const XwState *w, const LgXrSettings *c, int id) { int r[XW_ROWS_MAX]; int n = xw_page_rows(w, c, r);
    for (int i = 0; i < n; i++) if (r[i] == id) return i; return -1; }
int main(void) {
    LgXrSettings c = LG_XR_SETTINGS_DEFAULT;
    XwState w; memset(&w, 0, sizeof w);
    world_mode_resolve(&c, 0, 1, 0, &w.eff, NULL);
    int r[XW_ROWS_MAX], n;
    XmItem it[XW_COUNT];
    /* main page, Classic running: Mode, Advanced, the shared rows less stats/overlays/replays; no Restart */
    n = xw_page_rows(&w, &c, r);
    assert(r[0] == XW_ROW(XW_MODE) && r[1] == XW_ROW(XW_ADVANCED) && !has(r, n, XW_ROW(XW_RESTART)));
    assert(!has(r, n, XP_STATS) && !has(r, n, XP_OVERLAYS) && !has(r, n, XP_REPLAYS) && has(r, n, XP_RESET) && r[n - 1] == XP_CLOSE);
    assert(n <= XW_ROWS_MAX && n - 6 <= XM_ROWS);        /* minus the gun rows a lever cabinet drops */
    xw_items(&w, &c, it);
    assert(!strcmp(it[XW_MODE].value, "Classic (2D screen)"));
    assert(!strcmp(it[XW_ADVANCED].label, "Advanced settings (Classic) >") && !it[XW_ADVANCED].value[0]);
    /* Mode: saved, "(on restart)", Restart now shows */
    unsigned fx = xw_choose(&w, &c, XW_MODE);
    assert((fx & XP_SAVE) && (fx & XW_PAGE_CHANGED) && !(fx & XW_LIVE_CHANGED) && c.world_mode == 1);   /* live = 0 */
    xw_items(&w, &c, it);
    assert(!strcmp(it[XW_MODE].value, "Immersive (3D world) (on restart)"));
    assert(find(&w, &c, XW_ROW(XW_RESTART)) == 1);
    assert(xw_choose(&w, &c, XW_RESTART) == XP_RESTART);
    /* Advanced goes to the saved mode's page */
    assert(xw_choose(&w, &c, XW_ADVANCED) == XW_PAGE_CHANGED && w.page == XW_PAGE_IMMERSIVE);
    n = xw_page_rows(&w, &c, r);
    assert(n == 12 && r[0] == XW_ROW(XW_BACK) && r[1] == XW_ROW(XW_DOLLY) && r[3] == XW_ROW(XW_HEIGHT) && has(r, n, XP_STATS) && r[n - 1] == XP_CLOSE);
    xw_items(&w, &c, it);
    assert(!strcmp(it[XW_DOLLY].value, "20 m") && !strcmp(it[XW_STEPBACK].value, "6 m"));
    assert(!strcmp(it[XW_HEIGHT].label, "Camera height") && !strcmp(it[XW_HEIGHT].value, "Standard"));
    assert(!strcmp(it[XW_REPLAYS].value, "Off") && !strcmp(it[XW_CUTAWAYS].value, "Off") && !strcmp(it[XW_OVERLAYS].value, "Hidden"));
    assert(!strcmp(it[XW_SKY].value, "Stadium") && !strcmp(it[XW_SHADOW].value, "High 2048"));
    assert(!strcmp(it[XW_SKY].label, "Sky"));
    assert(!strcmp(xw_hint(&w), "Applies when Immersive mode runs."));
    /* the non-running mode's page: saved, not live */
    fx = xw_choose(&w, &c, XW_REPLAYS);
    assert(fx == XP_SAVE && c.imm_replays == 1 && c.replays == 1);
    /* cycles */
    xw_choose(&w, &c, XW_SKY); assert(c.imm_sky == LG_SKY_PAINTED);
    xw_items(&w, &c, it); assert(!strcmp(it[XW_SKY].value, "Painted"));
    xw_choose(&w, &c, XW_SKY); assert(c.imm_sky == LG_SKY_OFF);
    xw_items(&w, &c, it); assert(!strcmp(it[XW_SKY].value, "Off"));
    xw_choose(&w, &c, XW_SKY); assert(c.imm_sky == LG_SKY_STADIUM);
    xw_choose(&w, &c, XW_DOLLY); assert(c.imm_dolly == 250);
    xw_choose(&w, &c, XW_DOLLY); xw_choose(&w, &c, XW_DOLLY); assert(c.imm_dolly == 0);
    xw_choose(&w, &c, XW_STEPBACK); assert(c.imm_back == 70);
    xw_choose(&w, &c, XW_STEPBACK); assert(c.imm_back == 80);
    xw_choose(&w, &c, XW_STEPBACK); assert(c.imm_back == 50);
    xw_items(&w, &c, it); assert(!strcmp(it[XW_STEPBACK].value, "5 m"));
    xw_choose(&w, &c, XW_STEPBACK); assert(c.imm_back == 60);
    c.imm_back = 120; xw_choose(&w, &c, XW_STEPBACK); assert(c.imm_back == 50);   /* off the list: from its nearest (8 m) */
    xw_choose(&w, &c, XW_HEIGHT); assert(c.imm_height == 10);
    xw_items(&w, &c, it); assert(!strcmp(it[XW_HEIGHT].value, "1 m higher"));
    xw_choose(&w, &c, XW_HEIGHT); assert(c.imm_height == 20);
    xw_choose(&w, &c, XW_HEIGHT); assert(c.imm_height == -20);
    xw_items(&w, &c, it); assert(!strcmp(it[XW_HEIGHT].value, "2 m lower"));
    xw_choose(&w, &c, XW_HEIGHT); assert(c.imm_height == -10);
    xw_choose(&w, &c, XW_SHADOW); assert(c.imm_shadow == 4096);
    xw_choose(&w, &c, XW_SHADOW); assert(c.imm_shadow == 1024);
    /* restore: two pulls */
    fx = xw_choose(&w, &c, XW_RESTORE); assert(w.restore_armed && !(fx & XP_SAVE));
    xw_items(&w, &c, it); assert(!strcmp(it[XW_RESTORE].value, "Pull again to restore"));
    fx = xw_choose(&w, &c, XW_RESTORE); assert((fx & XP_SAVE) && !w.restore_armed);
    assert(c.imm_sky == LG_SKY_STADIUM && c.imm_dolly == 200 && c.imm_back == 60 && c.imm_height == 0 && c.imm_shadow == 2048 && c.imm_replays == 0);
    xw_choose(&w, &c, XW_RESTORE); xw_choose(&w, &c, XW_SKY); assert(!w.restore_armed);    /* another row disarms */
    assert(xw_choose(&w, &c, XW_BACK) == XW_PAGE_CHANGED && w.page == XW_PAGE_MAIN);
    /* the running mode's page: live; a pinned row shows the run's value and ignores clicks */
    c.world_mode = 0; w.page = XW_PAGE_CLASSIC;
    fx = xw_choose(&w, &c, XW_CUTAWAYS); assert((fx & XW_LIVE_CHANGED) && c.classic_cutaways == 0);
    n = xw_page_rows(&w, &c, r);
    assert(n == 8 && !has(r, n, XW_ROW(XW_DOLLY)) && !has(r, n, XW_ROW(XW_SKY)));
    assert(xw_choose(&w, &c, XW_SKY) == 0);             /* not a Classic row */
    w.pins = 1u << WM_REPLAYS; w.eff.replays = 0;
    xw_items(&w, &c, it); assert(!strcmp(it[XW_REPLAYS].value, "Off (env.txt)"));
    assert(xw_choose(&w, &c, XW_REPLAYS) == 0 && c.replays == 1);
    w.page = XW_PAGE_IMMERSIVE;                          /* the pin is the running (Classic) mode's only */
    xw_items(&w, &c, it); assert(!strstr(it[XW_REPLAYS].value, "env"));
    /* shadow on restart, on the running mode's page */
    w.page = XW_PAGE_CLASSIC; w.eff.shadow = 1024; c.classic_shadow = 2048;
    xw_items(&w, &c, it); assert(!strcmp(it[XW_SHADOW].value, "High 2048 (on restart)"));
    /* a pinned mode: no Restart row, clicks ignored */
    w.page = XW_PAGE_MAIN; w.pins = 1u << WM_MODE; w.running = 1; c.world_mode = 0;
    xw_items(&w, &c, it); assert(!strcmp(it[XW_MODE].value, "Immersive (3D world) (env.txt)"));
    assert(find(&w, &c, XW_ROW(XW_RESTART)) < 0 && xw_choose(&w, &c, XW_MODE) == 0 && c.world_mode == 0);
    /* live (Phase B): no Restart row, Mode without "(on restart)", the Mode click is live */
    {
        LgXrSettings lc = LG_XR_SETTINGS_DEFAULT;
        XwState l; memset(&l, 0, sizeof l);
        world_mode_resolve(&lc, 0, 1, 0, &l.eff, NULL);
        l.live = 1;
        fx = xw_choose(&l, &lc, XW_MODE);
        assert((fx & XP_SAVE) && (fx & XW_PAGE_CHANGED) && (fx & XW_LIVE_CHANGED) && lc.world_mode == 1);
        xw_items(&l, &lc, it); assert(!strcmp(it[XW_MODE].value, "Immersive (3D world)"));
        assert(find(&l, &lc, XW_ROW(XW_RESTART)) < 0 && find(&l, &lc, XW_ROW(XW_ADVANCED)) == 1);
        assert(!strcmp(it[XW_ADVANCED].label, "Advanced settings (Immersive) >"));
        fx = xw_choose(&l, &lc, XW_MODE);
        assert((fx & XW_LIVE_CHANGED) && lc.world_mode == 0);
        xw_items(&l, &lc, it); assert(!strcmp(it[XW_MODE].value, "Classic (2D screen)"));
        /* shadow quality still on restart; a pinned mode still pinned */
        l.page = XW_PAGE_CLASSIC; l.eff.shadow = 1024; lc.classic_shadow = 2048;
        xw_items(&l, &lc, it); assert(!strcmp(it[XW_SHADOW].value, "High 2048 (on restart)"));
        l.page = XW_PAGE_MAIN; l.pins = 1u << WM_MODE; l.running = 1; lc.world_mode = 0;
        xw_items(&l, &lc, it); assert(!strcmp(it[XW_MODE].value, "Immersive (3D world) (env.txt)"));
        assert(find(&l, &lc, XW_ROW(XW_RESTART)) < 0 && xw_choose(&l, &lc, XW_MODE) == 0 && lc.world_mode == 0);
    }
    /* every value fits beside its label: 42 characters across the panel */
    for (int p = 0; p < XW_PAGES; p++) {
        w.page = p; w.pins = 0x1ff; c.imm_shadow = 4096; c.classic_shadow = 4096; w.eff.shadow = 1024;
        xw_items(&w, &c, it);
        for (int i = 0; i < XW_COUNT; i++) assert(strlen(it[i].label) + strlen(it[i].value) + (it[i].value[0] ? 1 : 0) <= 41);
        assert(strlen(xw_hint(&w)) <= 42);
        assert(!xw_title(&w) || strlen(xw_title(&w)) <= 42);
    }
    /* rows under the pointer: each row's middle maps back to the row */
    for (int i = 0; i < XM_ROWS; i++) assert(xm_row_at(0.5f, (64 + 8 + 36 * i + 18) / (float)XM_H, XM_ROWS) == i);
    puts("ok");
    return 0;
}
''')
            b = Path(temp) / "wp"
            subprocess.run([cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(rt), str(src), str(rt / "xr_world_panel.c"),
                            str(rt / "world_mode.c"), str(rt / "xr_menu.c"), "-pthread", "-lm", "-o", str(b)], check=True)
            self.assertEqual(subprocess.run([str(b)], check=True, capture_output=True, text=True).stdout.strip(), "ok")


if __name__ == "__main__": unittest.main()
