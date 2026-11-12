/*
 * xr_world_panel.c - the world-mode game's panel pages (xr_world_panel.h).
 *
 * Main page: Mode (Classic / Immersive, applies on the next start), Restart
 * now (only while the saved mode is not the running one), Advanced settings
 * (the saved mode's page), then the shared rows. Classic advanced: replays,
 * point cutaways, arcade overlays, shadow quality. Immersive advanced: camera
 * closer, step back (5 / 6 / 7 / 8 m), camera height (2 m lower .. 2 m higher),
 * the same three, sky (Stadium / Painted / Off), shadow quality. Replays,
 * cutaways, overlays, closer, step back, camera height and sky are live for
 * the running mode; shadow quality and the mode apply on restart. A row whose
 * env variable is set shows "(env.txt)" with the run's value and ignores clicks.
 * With XwState.live (switching live, Phase B; host_xr.c keeps it 0 for now)
 * the Mode row applies at once: no Restart now, no "(on restart)".
 */
#include "xr_world_panel.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static const int SHADOWS[] = { 1024, 2048, 4096 };
/* the choice after v's nearest, wrapping (Step back 5 -> 6 -> 7 -> 8 -> 5 m) */
static int next_step(int v, const int *steps, int n)
{
    v = xr_settings_nearest(v, steps, n);
    for (int i = 0; i < n; i++) if (steps[i] == v) return steps[(i + 1) % n];
    return steps[0];
}

/* the WM_ value behind an item, -1 for none */
static int wm_key(int item)
{
    switch (item) {
    case XW_MODE: return WM_MODE;
    case XW_REPLAYS: return WM_REPLAYS;
    case XW_CUTAWAYS: return WM_CUTAWAYS;
    case XW_OVERLAYS: return WM_OVERLAYS;
    case XW_SHADOW: return WM_SHADOW;
    case XW_DOLLY: return WM_DOLLY;
    case XW_STEPBACK: return WM_BACK;
    case XW_HEIGHT: return WM_HEIGHT;
    case XW_SKY: return WM_SKY;
    default: return -1;
    }
}
/* the page's mode: 0 Classic, 1 Immersive, -1 the main page */
static int page_mode(const XwState *w)
{
    return w->page == XW_PAGE_CLASSIC ? 0 : w->page == XW_PAGE_IMMERSIVE ? 1 : -1;
}
/* pinned for this row: its env variable is set and the row is the running mode's */
static int pinned(const XwState *w, int item)
{
    const int k = wm_key(item);
    if (k < 0 || !(w->pins >> k & 1)) return 0;
    return k == WM_MODE || page_mode(w) == w->running;
}

int xw_page_rows(const XwState *w, const LgXrSettings *cfg, int *rows)
{
    int n = 0;
    switch (w->page) {
    case XW_PAGE_CLASSIC: {
        static const int r[] = { XW_ROW(XW_BACK), XW_ROW(XW_REPLAYS), XW_ROW(XW_CUTAWAYS), XW_ROW(XW_OVERLAYS),
                                 XW_ROW(XW_SHADOW), XP_STATS, XW_ROW(XW_RESTORE), XP_CLOSE };
        for (size_t i = 0; i < sizeof r / sizeof *r; i++) rows[n++] = r[i];
        break; }
    case XW_PAGE_IMMERSIVE: {
        static const int r[] = { XW_ROW(XW_BACK), XW_ROW(XW_DOLLY), XW_ROW(XW_STEPBACK), XW_ROW(XW_HEIGHT),
                                 XW_ROW(XW_REPLAYS), XW_ROW(XW_CUTAWAYS), XW_ROW(XW_OVERLAYS), XW_ROW(XW_SKY),
                                 XW_ROW(XW_SHADOW), XP_STATS, XW_ROW(XW_RESTORE), XP_CLOSE };
        for (size_t i = 0; i < sizeof r / sizeof *r; i++) rows[n++] = r[i];
        break; }
    default:
        rows[n++] = XW_ROW(XW_MODE);
        if (!w->live && cfg->world_mode != w->running && !pinned(w, XW_MODE)) rows[n++] = XW_ROW(XW_RESTART);
        rows[n++] = XW_ROW(XW_ADVANCED);
        for (int i = 0; i < XP_COUNT; i++)
            if (i != XP_STATS && i != XP_OVERLAYS && i != XP_REPLAYS) rows[n++] = i;
        break;
    }
    return n;
}

static const char *on_off(int v) { return v ? "On" : "Off"; }
static void metres(char *s, float units) { if (fabsf(units - roundf(units / 10) * 10) < 0.01f) snprintf(s, 40, "%.0f m", units / 10); else snprintf(s, 40, "%.1f m", units / 10); }
static const char *sky_label(int v) { return v == LG_SKY_OFF ? "Off" : v == LG_SKY_PAINTED ? "Painted" : "Stadium"; }
static void shadow(char *s, int size)
{
    snprintf(s, 40, "%s %d", size <= 1024 ? "Standard" : size <= 2048 ? "High" : "Very high", size);
}

void xw_items(const XwState *w, const LgXrSettings *cfg, XmItem *it)
{
    for (int i = 0; i < XW_COUNT; i++) { it[i].label = ""; it[i].value[0] = 0; }
    const int im = page_mode(w) == 1;
    it[XW_MODE].label = "Mode";
    {
        const int m = pinned(w, XW_MODE) ? w->running : cfg->world_mode;
        snprintf(it[XW_MODE].value, 40, "%s%s", m ? "Immersive (3D world)" : "Classic (2D screen)",
                 pinned(w, XW_MODE) ? " (env.txt)" : !w->live && m != w->running ? " (on restart)" : "");
    }
    it[XW_RESTART].label = cfg->world_mode ? "Restart now in Immersive mode" : "Restart now in Classic mode";
    it[XW_ADVANCED].label = cfg->world_mode ? "Advanced settings (Immersive) >" : "Advanced settings (Classic) >";
    it[XW_BACK].label = "< Back";
    it[XW_DOLLY].label = "Camera closer";
    it[XW_STEPBACK].label = "Step back";
    it[XW_HEIGHT].label = "Camera height";
    it[XW_REPLAYS].label = "Replays";
    it[XW_CUTAWAYS].label = "Point cutaways";
    it[XW_OVERLAYS].label = "Arcade overlays";
    it[XW_SKY].label = "Sky";
    it[XW_SHADOW].label = "Shadow quality";
    it[XW_RESTORE].label = im ? "Restore Immersive defaults" : "Restore Classic defaults";
    /* the page's stored values; a pinned row the run's own */
    const WmValues *e = &w->eff;
    int replays = im ? cfg->imm_replays : cfg->replays, cut = im ? cfg->imm_cutaways : cfg->classic_cutaways;
    int over = im ? cfg->imm_overlays : cfg->arcade_overlays, sh = im ? cfg->imm_shadow : cfg->classic_shadow;
    float dl = (float)cfg->imm_dolly, bk = (float)cfg->imm_back, ht = (float)cfg->imm_height; int sky = cfg->imm_sky;
    if (pinned(w, XW_REPLAYS)) replays = e->replays;
    if (pinned(w, XW_CUTAWAYS)) cut = e->cutaways;
    if (pinned(w, XW_OVERLAYS)) over = e->overlays;
    if (pinned(w, XW_SHADOW)) sh = e->shadow;
    if (pinned(w, XW_DOLLY)) dl = e->dolly;
    if (pinned(w, XW_STEPBACK)) bk = e->back;
    if (pinned(w, XW_HEIGHT)) ht = e->height;
    if (pinned(w, XW_SKY)) sky = e->sky;
    snprintf(it[XW_REPLAYS].value, 40, "%s", on_off(replays));
    snprintf(it[XW_CUTAWAYS].value, 40, "%s", on_off(cut));
    snprintf(it[XW_OVERLAYS].value, 40, "%s", over ? "Shown" : "Hidden");
    snprintf(it[XW_SKY].value, 40, "%s", sky_label(sky));
    metres(it[XW_DOLLY].value, dl);
    metres(it[XW_STEPBACK].value, bk);
    if (fabsf(ht) < 0.5f) snprintf(it[XW_HEIGHT].value, 40, "Standard");
    else { metres(it[XW_HEIGHT].value, fabsf(ht)); size_t l = strlen(it[XW_HEIGHT].value);
           snprintf(it[XW_HEIGHT].value + l, 40 - l, ht > 0 ? " higher" : " lower"); }
    shadow(it[XW_SHADOW].value, sh);
    for (int i = 0; i < XW_COUNT; i++) {
        size_t l = strlen(it[i].value);
        if (i != XW_MODE && pinned(w, i)) snprintf(it[i].value + l, 40 - l, " (env.txt)");
    }
    if (!pinned(w, XW_SHADOW) && page_mode(w) == w->running && sh != e->shadow) {
        size_t l = strlen(it[XW_SHADOW].value);
        snprintf(it[XW_SHADOW].value + l, 40 - l, " (on restart)");
    }
    if (w->restore_armed) snprintf(it[XW_RESTORE].value, 40, "Pull again to restore");
}

void xw_restore_defaults(LgXrSettings *c, int immersive)
{
    const LgXrSettings d = LG_XR_SETTINGS_DEFAULT;
    if (immersive) {
        c->imm_replays = d.imm_replays; c->imm_overlays = d.imm_overlays; c->imm_cutaways = d.imm_cutaways;
        c->imm_dolly = d.imm_dolly; c->imm_back = d.imm_back; c->imm_height = d.imm_height;
        c->imm_sky = d.imm_sky; c->imm_shadow = d.imm_shadow;
    } else {
        c->replays = d.replays; c->arcade_overlays = d.arcade_overlays;
        c->classic_cutaways = d.classic_cutaways; c->classic_shadow = d.classic_shadow;
    }
}

unsigned xw_choose(XwState *w, LgXrSettings *c, int item)
{
    if (item < 0 || item >= XW_COUNT) return 0;
    const int armed = w->restore_armed;
    w->restore_armed = 0;
    if (pinned(w, item)) return armed ? XW_PAGE_CHANGED : 0;     /* (env.txt): the run's own value */
    const int im = page_mode(w) == 1;
    const unsigned live = page_mode(w) == w->running ? XW_LIVE_CHANGED : 0;   /* the other mode's: saved for later */
    unsigned fx = XP_SAVE;
    switch (item) {
    case XW_MODE: c->world_mode = !c->world_mode; fx |= XW_PAGE_CHANGED | (w->live ? XW_LIVE_CHANGED : 0); break;
    case XW_RESTART: return XP_RESTART;
    case XW_ADVANCED: w->page = c->world_mode ? XW_PAGE_IMMERSIVE : XW_PAGE_CLASSIC; return XW_PAGE_CHANGED;
    case XW_BACK: w->page = XW_PAGE_MAIN; return XW_PAGE_CHANGED;
    case XW_REPLAYS: if (im) c->imm_replays = !c->imm_replays; else c->replays = !c->replays; fx |= live; break;
    case XW_CUTAWAYS: if (im) c->imm_cutaways = !c->imm_cutaways; else c->classic_cutaways = !c->classic_cutaways; fx |= live; break;
    case XW_OVERLAYS: if (im) c->imm_overlays = !c->imm_overlays; else c->arcade_overlays = !c->arcade_overlays; fx |= live; break;
    case XW_SKY:        /* Stadium -> Painted -> Off -> Stadium */
        if (!im) return 0;
        c->imm_sky = c->imm_sky == LG_SKY_STADIUM ? LG_SKY_PAINTED : c->imm_sky == LG_SKY_PAINTED ? LG_SKY_OFF : LG_SKY_STADIUM;
        fx |= live; break;
    case XW_DOLLY: if (!im) return 0; c->imm_dolly = c->imm_dolly >= 300 ? 0 : (c->imm_dolly / 50 + 1) * 50; fx |= live; break;
    case XW_STEPBACK: if (!im) return 0; c->imm_back = next_step(c->imm_back, LG_XR_BACK_STEPS, LG_XR_BACK_NSTEPS); fx |= live; break;
    case XW_HEIGHT: if (!im) return 0; c->imm_height = next_step(c->imm_height, LG_XR_HEIGHT_STEPS, LG_XR_HEIGHT_NSTEPS); fx |= live; break;
    case XW_SHADOW: {
        int *s = im ? &c->imm_shadow : &c->classic_shadow, k = 0;
        while (k < 3 && SHADOWS[k] <= *s) k++;
        *s = SHADOWS[k % 3]; break; }     /* on restart */
    case XW_RESTORE:
        if (page_mode(w) < 0) return 0;
        if (!armed) { w->restore_armed = 1; return XW_PAGE_CHANGED; }
        xw_restore_defaults(c, im); fx |= live; break;
    default: return 0;
    }
    return fx;
}

const char *xw_title(const XwState *w)
{
    return w->page == XW_PAGE_CLASSIC ? "Classic mode - advanced settings"
         : w->page == XW_PAGE_IMMERSIVE ? "Immersive mode - advanced settings" : NULL;
}

const char *xw_hint(const XwState *w)
{
    const int m = page_mode(w);
    if (m >= 0 && m != w->running)
        return m ? "Applies when Immersive mode runs." : "Applies when Classic mode runs.";
    return "Point and pull the trigger. Menu: close.";    /* 42 characters fit */
}
