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
#include <stdlib.h>
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
    /* main page, Classic running: the Classic / VR buttons, Advanced, the shared rows less stats/overlays/replays; no Restart */
    n = xw_page_rows(&w, &c, r);
    assert(r[0] == XW_ROW(XW_MODE_CLASSIC) && r[1] == XW_ROW(XW_MODE_VR) && r[2] == XW_ROW(XW_ADVANCED) && !has(r, n, XW_ROW(XW_RESTART)));
    assert(!has(r, n, XW_ROW(XW_MODE)) && !has(r, n, XW_ROW(XW_DOLLY)) && !has(r, n, XW_ROW(XW_STEPBACK)));
    assert(!has(r, n, XP_STATS) && !has(r, n, XP_OVERLAYS) && !has(r, n, XP_REPLAYS) && has(r, n, XP_RESET) && r[n - 1] == XP_CLOSE);
    assert(n <= XW_ROWS_MAX && n - 6 + 1 <= XM_MODE_ROWS);     /* minus the gun rows a lever cabinet drops, plus Restart */
    xw_items(&w, &c, it);
    assert(!strcmp(it[XW_MODE_CLASSIC].label, "Classic") && !strcmp(it[XW_MODE_CLASSIC].value, "selected"));
    assert(!strcmp(it[XW_MODE_VR].label, "VR") && !it[XW_MODE_VR].value[0]);
    assert(!strcmp(xw_mode_note(&w, &c), "Classic runs now"));
    assert(!strcmp(it[XW_ADVANCED].label, "Advanced settings (Classic) >") && !it[XW_ADVANCED].value[0]);
    /* the chosen button again: nothing */
    assert(xw_choose(&w, &c, XW_MODE_CLASSIC) == 0 && c.world_mode == 0);
    /* VR: saved, applies on restart, Restart now shows right under the buttons */
    unsigned fx = xw_choose(&w, &c, XW_MODE_VR);
    assert((fx & XP_SAVE) && (fx & XW_PAGE_CHANGED) && !(fx & XW_LIVE_CHANGED) && c.world_mode == 1);   /* live = 0 */
    xw_items(&w, &c, it);
    assert(!strcmp(it[XW_MODE_VR].value, "selected") && !it[XW_MODE_CLASSIC].value[0]);
    assert(!strcmp(xw_mode_note(&w, &c), "Classic runs now - VR after a restart"));
    assert(find(&w, &c, XW_ROW(XW_RESTART)) == 2 && !strcmp(it[XW_RESTART].label, "Restart now in VR mode"));
    assert(xw_choose(&w, &c, XW_RESTART) == XP_RESTART);
    /* Advanced goes to the saved mode's page */
    assert(xw_choose(&w, &c, XW_ADVANCED) == XW_PAGE_CHANGED && w.page == XW_PAGE_IMMERSIVE);
    n = xw_page_rows(&w, &c, r);
    assert(n == 14 && !has(r, n, XW_ROW(XW_CUTAWAYS)) && r[0] == XW_ROW(XW_MODE_CLASSIC) && r[2] == XW_ROW(XW_BACK) && r[3] == XW_ROW(XW_HEIGHT) && has(r, n, XP_STATS) && r[n - 1] == XP_CLOSE);
    assert(!has(r, n, XW_ROW(XW_DOLLY)) && !has(r, n, XW_ROW(XW_STEPBACK)));
    xw_items(&w, &c, it);
    assert(!strcmp(xw_title(&w), "VR mode - advanced settings"));
    assert(!strcmp(it[XW_HEIGHT].label, "Camera height") && !strcmp(it[XW_HEIGHT].value, "Medium"));   /* default Medium (T13) */
    assert(!strcmp(it[XW_REPLAYS].value, "Off") && !strcmp(it[XW_CUTAWAYS].value, "Off") && !strcmp(it[XW_OVERLAYS].value, "Hidden"));
    assert(!strcmp(it[XW_SKY].value, "Stadium") && !strcmp(it[XW_SHADOW].value, "High 2048"));
    assert(!strcmp(it[XW_SKY].label, "Sky"));
    assert(!strcmp(xw_hint(&w), "Applies when VR mode runs."));
    /* the non-running mode's page: saved, not live */
    fx = xw_choose(&w, &c, XW_REPLAYS);
    assert(fx == XP_SAVE && c.imm_replays == 1 && c.replays == 1);
    /* VR's Replays & close-ups (one row for replays and point cutaways, 2026-11-13): Off > Flat screen > VR > Off */
    assert(c.imm_cutaways == LG_VIEW_FLAT);             /* the row sets both */
    xw_items(&w, &c, it); assert(!strcmp(it[XW_REPLAYS].value, "Flat screen") && !strcmp(it[XW_REPLAYS].label, "Replays & close-ups"));
    xw_choose(&w, &c, XW_REPLAYS); assert(c.imm_replays == LG_VIEW_CAMERA && c.imm_cutaways == LG_VIEW_CAMERA);
    xw_items(&w, &c, it); assert(!strcmp(it[XW_REPLAYS].value, "VR"));
    xw_choose(&w, &c, XW_REPLAYS); assert(c.imm_replays == LG_VIEW_OFF && c.imm_cutaways == LG_VIEW_OFF && c.replays == 1);
    c.imm_replays = LG_VIEW_WORLD; xw_choose(&w, &c, XW_REPLAYS); assert(c.imm_replays == LG_VIEW_OFF && c.imm_cutaways == LG_VIEW_OFF);   /* a stray world: on to Off */
    /* cycles */
    xw_choose(&w, &c, XW_SKY); assert(c.imm_sky == LG_SKY_PAINTED);
    xw_items(&w, &c, it); assert(!strcmp(it[XW_SKY].value, "Painted"));
    xw_choose(&w, &c, XW_SKY); assert(c.imm_sky == LG_SKY_OFF);
    xw_items(&w, &c, it); assert(!strcmp(it[XW_SKY].value, "Off"));
    xw_choose(&w, &c, XW_SKY); assert(c.imm_sky == LG_SKY_STADIUM);
    assert(xw_choose(&w, &c, XW_STEPBACK) == 0 && xw_choose(&w, &c, XW_DOLLY) == 0);    /* gone */
    xw_choose(&w, &c, XW_HEIGHT); assert(c.imm_height == 20);
    xw_items(&w, &c, it); assert(!strcmp(it[XW_HEIGHT].value, "High"));
    xw_choose(&w, &c, XW_HEIGHT); assert(c.imm_height == 0);
    xw_items(&w, &c, it); assert(!strcmp(it[XW_HEIGHT].value, "Low"));
    xw_choose(&w, &c, XW_HEIGHT); assert(c.imm_height == 10);
    c.imm_height = -20; xw_choose(&w, &c, XW_HEIGHT); assert(c.imm_height == 10);   /* an old -2 m: from Low */
    xw_choose(&w, &c, XW_SHADOW); assert(c.imm_shadow == 4096);
    xw_choose(&w, &c, XW_SHADOW); assert(c.imm_shadow == 1024);
    /* restore: two pulls */
    fx = xw_choose(&w, &c, XW_RESTORE); assert(w.restore_armed && !(fx & XP_SAVE));
    xw_items(&w, &c, it); assert(!strcmp(it[XW_RESTORE].value, "Pull again to restore"));
    fx = xw_choose(&w, &c, XW_RESTORE); assert((fx & XP_SAVE) && !w.restore_armed);
    assert(c.imm_sky == LG_SKY_STADIUM && c.imm_height == 10 && c.imm_shadow == 2048 && c.imm_replays == 0);
    xw_choose(&w, &c, XW_RESTORE); xw_choose(&w, &c, XW_SKY); assert(!w.restore_armed);    /* another row disarms */
    assert(xw_choose(&w, &c, XW_BACK) == XW_PAGE_CHANGED && w.page == XW_PAGE_MAIN);
    /* the running mode's page: live; a pinned row shows the run's value and ignores clicks */
    c.world_mode = 0; w.page = XW_PAGE_CLASSIC;
    fx = xw_choose(&w, &c, XW_REPLAYS); assert((fx & XW_LIVE_CHANGED) && c.classic_cutaways == 0 && c.replays == 0);
    xw_choose(&w, &c, XW_REPLAYS); assert(c.classic_cutaways == 1 && c.replays == 1);
    n = xw_page_rows(&w, &c, r);
    assert(n == 13 && !has(r, n, XW_ROW(XW_CUTAWAYS)) && has(r, n, XW_ROW(XW_CARD)) && !has(r, n, XW_ROW(XW_DOLLY)) && !has(r, n, XW_ROW(XW_SKY)));
    assert(xw_choose(&w, &c, XW_SKY) == 0);             /* not a Classic row */
    w.pins = 1u << WM_REPLAYS; w.eff.replays = 0;
    xw_items(&w, &c, it); assert(!strcmp(it[XW_REPLAYS].value, "Off (env.txt)"));
    assert(xw_choose(&w, &c, XW_REPLAYS) == 0 && c.replays == 1);
    w.page = XW_PAGE_IMMERSIVE;                          /* the pin is the running (Classic) mode's only */
    xw_items(&w, &c, it); assert(!strstr(it[XW_REPLAYS].value, "env"));
    /* Immersive running, its view pinned (LINDBERGH_VT3_REPLAY_VIEW): the run's view */
    w.running = 1; w.eff.replays = 1; w.eff.replay_view = LG_VIEW_WORLD;
    xw_items(&w, &c, it); assert(!strcmp(it[XW_REPLAYS].value, "In the world (env.txt)"));
    assert(xw_choose(&w, &c, XW_REPLAYS) == 0);
    w.running = 0; w.eff.replays = 0;
    /* Classic's Card reader notice: Off by default, on restart */
    w.page = XW_PAGE_CLASSIC;
    xw_items(&w, &c, it); assert(!strcmp(it[XW_CARD].label, "Card reader notice") && !strncmp(it[XW_CARD].value, "Off", 3));
    { int pins = w.pins; w.pins = 0; w.eff.card_notice = 0;
      fx = xw_choose(&w, &c, XW_CARD); assert(fx == XP_SAVE && c.classic_card_notice == 1);
      xw_items(&w, &c, it); assert(!strcmp(it[XW_CARD].value, "On (on restart)"));
      xw_choose(&w, &c, XW_CARD); assert(c.classic_card_notice == 0);
      w.page = XW_PAGE_IMMERSIVE; assert(xw_choose(&w, &c, XW_CARD) == 0); w.page = XW_PAGE_CLASSIC;   /* not VR's */
      w.pins = pins; }
    /* shadow on restart, on the running mode's page */
    w.page = XW_PAGE_CLASSIC; w.eff.shadow = 1024; c.classic_shadow = 2048;
    xw_items(&w, &c, it); assert(!strcmp(it[XW_SHADOW].value, "High 2048 (on restart)"));
    /* a pinned mode: no Restart row, clicks ignored */
    w.page = XW_PAGE_MAIN; w.pins = 1u << WM_MODE; w.running = 1; c.world_mode = 0;
    xw_items(&w, &c, it); assert(!strcmp(it[XW_MODE_VR].value, "selected") && !strcmp(xw_mode_note(&w, &c), "VR runs (set by env.txt)"));
    assert(find(&w, &c, XW_ROW(XW_RESTART)) < 0 && xw_choose(&w, &c, XW_MODE_CLASSIC) == 0 && c.world_mode == 0);
    /* Language on the main page (both modes), English by default, on restart */
    { XwState lw; memset(&lw, 0, sizeof lw); LgXrSettings lc = LG_XR_SETTINGS_DEFAULT;
      world_mode_resolve(&lc, 1, 1, 0, &lw.eff, NULL); lw.running = 1;
      int lr[XW_ROWS_MAX], ln = xw_page_rows(&lw, &lc, lr);
      assert(has(lr, ln, XW_ROW(XW_LANGUAGE)) && lc.language == 1 && lw.eff.language == 1);
      xw_items(&lw, &lc, it); assert(!strcmp(it[XW_LANGUAGE].value, "English"));
      assert(xw_choose(&lw, &lc, XW_LANGUAGE) == XP_SAVE && lc.language == 0);
      xw_items(&lw, &lc, it); assert(!strcmp(it[XW_LANGUAGE].value, "Japanese (on restart)"));
      lw.pins = 1u << WM_LANGUAGE; lw.eff.language = 1;
      xw_items(&lw, &lc, it); assert(!strcmp(it[XW_LANGUAGE].value, "English (env.txt)"));
      assert(xw_choose(&lw, &lc, XW_LANGUAGE) == 0); }
    /* Anti-aliasing on the main page (both modes), Off by default, Off > 2x > 4x > Off, on restart (T17) */
    { XwState aw; memset(&aw, 0, sizeof aw); LgXrSettings ac = LG_XR_SETTINGS_DEFAULT;
      world_mode_resolve(&ac, 0, 1, 0, &aw.eff, NULL);
      int ar[XW_ROWS_MAX], an = xw_page_rows(&aw, &ac, ar);
      assert(has(ar, an, XW_ROW(XW_MSAA)) && ac.msaa == 0 && aw.eff.msaa == 0);
      aw.page = XW_PAGE_CLASSIC; an = xw_page_rows(&aw, &ac, ar); assert(!has(ar, an, XW_ROW(XW_MSAA)));
      aw.page = XW_PAGE_IMMERSIVE; an = xw_page_rows(&aw, &ac, ar); assert(!has(ar, an, XW_ROW(XW_MSAA)));
      aw.page = XW_PAGE_MAIN;
      xw_items(&aw, &ac, it); assert(!strcmp(it[XW_MSAA].label, "Anti-aliasing") && !strcmp(it[XW_MSAA].value, "Off"));
      assert(xw_choose(&aw, &ac, XW_MSAA) == XP_SAVE && ac.msaa == 2);
      xw_items(&aw, &ac, it); assert(!strcmp(it[XW_MSAA].value, "2x (on restart)"));
      assert(xw_choose(&aw, &ac, XW_MSAA) == XP_SAVE && ac.msaa == 4);
      xw_items(&aw, &ac, it); assert(!strcmp(it[XW_MSAA].value, "4x (on restart)"));
      aw.eff.msaa = 4; xw_items(&aw, &ac, it); assert(!strcmp(it[XW_MSAA].value, "4x"));
      assert(xw_choose(&aw, &ac, XW_MSAA) == XP_SAVE && ac.msaa == 0);
      xw_items(&aw, &ac, it); assert(!strcmp(it[XW_MSAA].value, "Off (on restart)"));
      aw.pins = 1u << WM_MSAA; aw.eff.msaa = 2;
      xw_items(&aw, &ac, it); assert(!strcmp(it[XW_MSAA].value, "2x (env.txt)"));
      assert(xw_choose(&aw, &ac, XW_MSAA) == 0 && ac.msaa == 0); }
    /* Save progress on both Advanced pages (one value), On by default (T21), on restart; not on the main page (T16) */
    { XwState sw; memset(&sw, 0, sizeof sw); LgXrSettings sc = LG_XR_SETTINGS_DEFAULT;
      world_mode_resolve(&sc, 0, 1, 0, &sw.eff, NULL);
      int sr[XW_ROWS_MAX], sn = xw_page_rows(&sw, &sc, sr);
      assert(!has(sr, sn, XW_ROW(XW_SAVE)) && sc.save == 1 && sw.eff.save == 1);
      sw.page = XW_PAGE_CLASSIC; sn = xw_page_rows(&sw, &sc, sr); assert(has(sr, sn, XW_ROW(XW_SAVE)));
      xw_items(&sw, &sc, it); assert(!strcmp(it[XW_SAVE].label, "Save progress") && !strcmp(it[XW_SAVE].value, "On"));
      assert(xw_choose(&sw, &sc, XW_SAVE) == XP_SAVE && sc.save == 0);
      xw_items(&sw, &sc, it); assert(!strcmp(it[XW_SAVE].value, "Off (on restart)"));
      sw.page = XW_PAGE_IMMERSIVE; sn = xw_page_rows(&sw, &sc, sr); assert(has(sr, sn, XW_ROW(XW_SAVE)));
      xw_items(&sw, &sc, it); assert(!strcmp(it[XW_SAVE].value, "Off (on restart)"));         /* the same value on VR's page */
      xw_restore_defaults(&sc, 1); xw_restore_defaults(&sc, 0); assert(sc.save == 0);             /* Restore defaults keeps it */
      sw.pins = 1u << WM_SAVE; sw.eff.save = 0;
      xw_items(&sw, &sc, it); assert(!strcmp(it[XW_SAVE].value, "Off (env.txt)"));
      assert(xw_choose(&sw, &sc, XW_SAVE) == 0 && sc.save == 0); }
    /* Reset save below Save progress on both Advanced pages: pull twice (like Restore defaults), an action - nothing stored */
    { XwState rw; memset(&rw, 0, sizeof rw); LgXrSettings rc = LG_XR_SETTINGS_DEFAULT, before;
      world_mode_resolve(&rc, 0, 1, 0, &rw.eff, NULL);
      int rr[XW_ROWS_MAX], rn = xw_page_rows(&rw, &rc, rr);
      assert(!has(rr, rn, XW_ROW(XW_RESET_SAVE)));
      for (int pg = XW_PAGE_CLASSIC; pg <= XW_PAGE_IMMERSIVE; pg++) {
          rw.page = pg; rn = xw_page_rows(&rw, &rc, rr);
          assert(find(&rw, &rc, XW_ROW(XW_RESET_SAVE)) == find(&rw, &rc, XW_ROW(XW_SAVE)) + 1);
          assert(rn <= XM_MODE_ROWS);
          rw.reset_save_done = 0;
          xw_items(&rw, &rc, it); assert(!strcmp(it[XW_RESET_SAVE].label, "Reset save") && !it[XW_RESET_SAVE].value[0]);
          before = rc;
          assert(xw_choose(&rw, &rc, XW_RESET_SAVE) == XW_PAGE_CHANGED && rw.reset_save_armed);   /* first pull: armed */
          xw_items(&rw, &rc, it); assert(!strcmp(it[XW_RESET_SAVE].value, "Pull again to erase"));
          unsigned f2 = xw_choose(&rw, &rc, XW_RESET_SAVE);
          assert((f2 & XW_RESET_SAVE_FX) && !(f2 & XP_SAVE) && !rw.reset_save_armed && !memcmp(&before, &rc, sizeof rc));
          xw_items(&rw, &rc, it); assert(!strcmp(it[XW_RESET_SAVE].value, "Done (fresh at next game)"));
          /* armed, then another row: disarmed, no reset */
          xw_choose(&rw, &rc, XW_RESET_SAVE); assert(rw.reset_save_armed);
          assert(!(xw_choose(&rw, &rc, XW_SHADOW) & XW_RESET_SAVE_FX) && !rw.reset_save_armed);
          assert(!(xw_choose(&rw, &rc, XW_RESET_SAVE) & XW_RESET_SAVE_FX));                        /* one pull again: armed only */
          rw.reset_save_armed = 0;
      }
      rw.page = XW_PAGE_MAIN; assert(xw_choose(&rw, &rc, XW_RESET_SAVE) == 0); }
    /* Texture filtering on both Advanced pages (one value), Standard by default, Standard > Sharp 4x > Sharp 8x, on restart */
    { XwState tw; memset(&tw, 0, sizeof tw); LgXrSettings tc = LG_XR_SETTINGS_DEFAULT;
      world_mode_resolve(&tc, 0, 1, 0, &tw.eff, NULL);
      int tr[XW_ROWS_MAX], tn = xw_page_rows(&tw, &tc, tr);
      assert(!has(tr, tn, XW_ROW(XW_ANISO)) && tc.aniso == 0 && tw.eff.aniso == 0);
      tw.page = XW_PAGE_CLASSIC; tn = xw_page_rows(&tw, &tc, tr); assert(has(tr, tn, XW_ROW(XW_ANISO)));
      xw_items(&tw, &tc, it); assert(!strcmp(it[XW_ANISO].label, "Texture filtering") && !strcmp(it[XW_ANISO].value, "Standard"));
      assert(xw_choose(&tw, &tc, XW_ANISO) == XP_SAVE && tc.aniso == 4);
      xw_items(&tw, &tc, it); assert(!strcmp(it[XW_ANISO].value, "Sharp 4x (on restart)"));
      tw.page = XW_PAGE_IMMERSIVE; tn = xw_page_rows(&tw, &tc, tr); assert(has(tr, tn, XW_ROW(XW_ANISO)));
      assert(xw_choose(&tw, &tc, XW_ANISO) == XP_SAVE && tc.aniso == 8);
      tw.eff.aniso = 8; xw_items(&tw, &tc, it); assert(!strcmp(it[XW_ANISO].value, "Sharp 8x"));
      assert(xw_choose(&tw, &tc, XW_ANISO) == XP_SAVE && tc.aniso == 0);
      xw_items(&tw, &tc, it); assert(!strcmp(it[XW_ANISO].value, "Standard (on restart)"));
      tc.aniso = 16; assert(xw_choose(&tw, &tc, XW_ANISO) == XP_SAVE && tc.aniso == 0);        /* 16 (env / hand edit) -> 8 nearest -> Standard */
      tc.aniso = 2; assert(xw_choose(&tw, &tc, XW_ANISO) == XP_SAVE && tc.aniso == 4);
      xw_restore_defaults(&tc, 1); assert(tc.aniso == 0);                                      /* Restore defaults: the game's default (here every game's, Standard) */
      tc.aniso = 4;
      xr_settings_set_game_defaults(xr_settings_vt3_defaults);   /* Virtua Tennis 3's Restore defaults */
      { LgXrSettings v; memset(&v, 0, sizeof v); xw_restore_defaults(&v, 1);
        assert(v.imm_replays == LG_VIEW_FLAT && v.imm_cutaways == LG_VIEW_FLAT && v.imm_shadow == 4096 && v.imm_height == 10 && v.aniso == 4 && !v.imm_overlays);
        memset(&v, 0, sizeof v); xw_restore_defaults(&v, 0);
        assert(v.replays == 1 && v.classic_cutaways == 1 && v.arcade_overlays == 0 && v.classic_shadow == 1024 && v.aniso == 4); }
      xr_settings_set_game_defaults(NULL);
      tw.pins = 1u << WM_ANISO; tw.eff.aniso = 16;
      xw_items(&tw, &tc, it); assert(!strcmp(it[XW_ANISO].value, "Sharp 16x (env.txt)"));
      assert(xw_choose(&tw, &tc, XW_ANISO) == 0 && tc.aniso == 4); }
    /* a mode button on an Advanced page: that mode's page */
    w.pins = 0; w.running = 0; w.page = XW_PAGE_CLASSIC; c.world_mode = 0;
    fx = xw_choose(&w, &c, XW_MODE_VR); assert((fx & XP_SAVE) && c.world_mode == 1 && w.page == XW_PAGE_IMMERSIVE);
    xw_choose(&w, &c, XW_MODE_CLASSIC); assert(c.world_mode == 0 && w.page == XW_PAGE_CLASSIC);
    w.page = XW_PAGE_MAIN;
    /* live (Phase B): no Restart row, Mode without "(on restart)", the Mode click is live */
    {
        LgXrSettings lc = LG_XR_SETTINGS_DEFAULT;
        XwState l; memset(&l, 0, sizeof l);
        world_mode_resolve(&lc, 0, 1, 0, &l.eff, NULL);
        l.live = 1;
        fx = xw_choose(&l, &lc, XW_MODE);
        assert((fx & XP_SAVE) && (fx & XW_PAGE_CHANGED) && (fx & XW_LIVE_CHANGED) && lc.world_mode == 1);
        xw_items(&l, &lc, it); assert(!strcmp(it[XW_MODE].value, "VR (3D world)"));
        assert(find(&l, &lc, XW_ROW(XW_RESTART)) < 0 && find(&l, &lc, XW_ROW(XW_ADVANCED)) == 2);
        assert(!strcmp(it[XW_ADVANCED].label, "Advanced settings (VR) >"));
        fx = xw_choose(&l, &lc, XW_MODE);
        assert((fx & XW_LIVE_CHANGED) && lc.world_mode == 0);
        xw_items(&l, &lc, it); assert(!strcmp(it[XW_MODE].value, "Classic (2D screen)"));
        /* shadow quality still on restart; a pinned mode still pinned */
        l.page = XW_PAGE_CLASSIC; l.eff.shadow = 1024; lc.classic_shadow = 2048;
        xw_items(&l, &lc, it); assert(!strcmp(it[XW_SHADOW].value, "High 2048 (on restart)"));
        l.page = XW_PAGE_MAIN; l.pins = 1u << WM_MODE; l.running = 1; lc.world_mode = 0;
        xw_items(&l, &lc, it); assert(!strcmp(it[XW_MODE].value, "VR (3D world) (env.txt)"));
        assert(find(&l, &lc, XW_ROW(XW_RESTART)) < 0 && xw_choose(&l, &lc, XW_MODE) == 0 && lc.world_mode == 0);
    }
    /* every value fits beside its label: 42 characters across the panel */
    for (int p = 0; p < XW_PAGES; p++) {
        w.page = p; w.pins = 0x7ff; c.imm_shadow = 4096; c.classic_shadow = 4096; w.eff.shadow = 1024;
        xw_items(&w, &c, it);
        for (int i = 0; i < XW_COUNT; i++) assert(strlen(it[i].label) + strlen(it[i].value) + (it[i].value[0] ? 1 : 0) <= 41);
        assert(strlen(xw_hint(&w)) <= 42);
        assert(!xw_title(&w) || strlen(xw_title(&w)) <= 42);
    }
    /* rows under the pointer: each row's middle maps back to the row */
    for (int i = 0; i < XM_ROWS; i++) assert(xm_row_at(0.5f, (64 + 8 + 36 * i + 18) / (float)XM_H, XM_ROWS) == i);
    /* with the mode buttons: left 0, right 1, the gap and the margins none, rows from 2 below the band */
    assert(xm_row_at_modes(0.25f, (74 + 44) / (float)XM_H, XM_MODE_ROWS) == 0);
    assert(xm_row_at_modes(0.75f, (74 + 44) / (float)XM_H, XM_MODE_ROWS) == 1);
    assert(xm_row_at_modes(0.5f, (74 + 44) / (float)XM_H, XM_MODE_ROWS) < 0 && xm_row_at_modes(0.01f, (74 + 44) / (float)XM_H, XM_MODE_ROWS) < 0);
    for (int i = 2; i < XM_MODE_ROWS; i++) assert(xm_row_at_modes(0.5f, (64 + XM_BAR_H + 8 + 36 * (i - 2) + 18) / (float)XM_H, XM_MODE_ROWS) == i);
    assert(64 + XM_BAR_H + 8 + 36 * (XM_MODE_ROWS - 2) <= XM_H - 34 - 10);   /* above the hint */
    {   /* LINDBERGH_TEST_PANEL_PPM=<file>: the main page painted (the picture for the docs) */
        const char *pp = getenv("LINDBERGH_TEST_PANEL_PPM");
        if (pp) {
            static unsigned char img[XM_W * XM_H * 4];
            xr_settings_set_game_defaults(xr_settings_vt3_defaults);     /* Virtua Tennis 3's panel */
            LgXrSettings pc; xr_settings_defaults(&pc); XwState pw; memset(&pw, 0, sizeof pw);
            if (getenv("LINDBERGH_TEST_PANEL_VR")) pc.imm_replays = pc.imm_cutaways = LG_VIEW_CAMERA;
            world_mode_resolve(&pc, 1, 1, 0, &pw.eff, NULL);
            pw.running = 0; pc.world_mode = 1;
            int pr[XW_ROWS_MAX], pn = xw_page_rows(&pw, &pc, pr), m = 0;
            XmItem all[XP_COUNT], wall[XW_COUNT], rows[XW_ROWS_MAX];
            XpPanel panel; memset(&panel, 0, sizeof panel);
            xp_items(&panel, &pc, 0, 0, 2208, 2064, all);
            xw_items(&pw, &pc, wall);
            for (int i = 0; i < pn; i++) {
                const int id = pr[i];
                if (id == XP_SIGHT || id == XP_LASER || id == XP_CROSSHAIR || id == XP_GUN || id == XP_HAND || id == XP_DUAL) continue;
                rows[m++] = id < XP_COUNT ? all[id] : wall[id - XP_COUNT];
            }
            const char *page = getenv("LINDBERGH_TEST_PANEL_PAGE");
            if (page && (*page == '2' || *page == '1')) {      /* the VR (2) or Classic (1) page instead */
                pw.page = *page == '2' ? XW_PAGE_IMMERSIVE : XW_PAGE_CLASSIC; if (*page == '1') pc.world_mode = 0; pn = xw_page_rows(&pw, &pc, pr); m = 0; xw_items(&pw, &pc, wall);
                for (int i = 0; i < pn; i++) rows[m++] = pr[i] < XP_COUNT ? all[pr[i]] : wall[pr[i] - XP_COUNT];
            }
            xm_paint_modes(img, xw_title(&pw) ? xw_title(&pw) : "Virtua Tennis 3 - settings", rows, m, 1, xw_mode_note(&pw, &pc), xw_hint(&pw));
            FILE *f = fopen(pp, "wb");
            fprintf(f, "P6\n%d %d\n255\n", XM_W, XM_H);
            for (int i = 0; i < XM_W * XM_H; i++) fwrite(img + 4 * i, 1, 3, f);
            fclose(f);
        }
    }
    puts("ok");
    return 0;
}
''')
            b = Path(temp) / "wp"
            subprocess.run([cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(rt), str(src), str(rt / "xr_world_panel.c"),
                            str(rt / "world_mode.c"), str(rt / "xr_menu.c"), str(rt / "xr_panel.c"), str(rt / "stereo_projection.c"), str(rt / "xr_settings.c"), "-pthread", "-lm", "-o", str(b)], check=True)
            r = subprocess.run([str(b)], capture_output=True, text=True); self.assertEqual(r.stdout.strip(), "ok", r.stderr)


if __name__ == "__main__": unittest.main()
