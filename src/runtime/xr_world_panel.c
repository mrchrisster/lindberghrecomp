/*
 * xr_world_panel.c - the world-mode game's panel pages (xr_world_panel.h).
 *
 * Every page starts with two big buttons, Classic and VR (= Immersive; the
 * vr.cfg keys and env names keep "immersive"), the chosen mode highlighted;
 * it applies on the next start, and the line under them says so. Main page:
 * Restart now (only while the saved mode is not the running one), Advanced
 * settings (the saved mode's page), then the shared rows; choosing a mode on
 * an Advanced page shows that mode's page. Classic advanced: replays, point
 * cutaways, arcade overlays, shadow quality (replays and cutaways On / Off).
 * VR advanced: camera height (Low / Medium / High; your player at the far
 * end is always High - stereo.c), the same three (replays and cutaways Off /
 * Flat screen / Camera view), sky (Stadium / Painted / Off),
 * shadow quality. Camera closer and Step back are gone (fixed 20 m / 5 m,
 * T12). Replays, cutaways, overlays, camera height and sky are live for the
 * running mode; shadow quality and the mode apply on restart. A row whose env
 * variable is set shows "(env.txt)" with the run's value and ignores clicks.
 * With XwState.live (switching live, Phase B; host_xr.c keeps it 0 for now)
 * the mode applies at once: no Restart now.
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
    case XW_MODE: case XW_MODE_CLASSIC: case XW_MODE_VR: return WM_MODE;
    case XW_REPLAYS: return WM_REPLAYS;
    case XW_CUTAWAYS: return WM_CUTAWAYS;
    case XW_OVERLAYS: return WM_OVERLAYS;
    case XW_SHADOW: return WM_SHADOW;
    case XW_DOLLY: return WM_DOLLY;
    case XW_STEPBACK: return WM_BACK;
    case XW_HEIGHT: return WM_HEIGHT;
    case XW_CARD: return WM_CARD;
    case XW_LANGUAGE: return WM_LANGUAGE;
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
    return k == WM_MODE || k == WM_LANGUAGE || page_mode(w) == w->running;
}

int xw_page_rows(const XwState *w, const LgXrSettings *cfg, int *rows)
{
    int n = 0;
    rows[n++] = XW_ROW(XW_MODE_CLASSIC);        /* the mode buttons, first on every page */
    rows[n++] = XW_ROW(XW_MODE_VR);
    switch (w->page) {
    case XW_PAGE_CLASSIC: {
        static const int r[] = { XW_ROW(XW_BACK), XW_ROW(XW_REPLAYS), XW_ROW(XW_CUTAWAYS), XW_ROW(XW_OVERLAYS),
                                 XW_ROW(XW_CARD), XW_ROW(XW_SHADOW), XP_STATS, XW_ROW(XW_RESTORE), XP_CLOSE };
        for (size_t i = 0; i < sizeof r / sizeof *r; i++) rows[n++] = r[i];
        break; }
    case XW_PAGE_IMMERSIVE: {
        static const int r[] = { XW_ROW(XW_BACK), XW_ROW(XW_HEIGHT),
                                 XW_ROW(XW_REPLAYS), XW_ROW(XW_CUTAWAYS), XW_ROW(XW_OVERLAYS), XW_ROW(XW_SKY),
                                 XW_ROW(XW_SHADOW), XP_STATS, XW_ROW(XW_RESTORE), XP_CLOSE };
        for (size_t i = 0; i < sizeof r / sizeof *r; i++) rows[n++] = r[i];
        break; }
    default:
        if (!w->live && cfg->world_mode != w->running && !pinned(w, XW_MODE)) rows[n++] = XW_ROW(XW_RESTART);
        rows[n++] = XW_ROW(XW_ADVANCED);
        rows[n++] = XW_ROW(XW_LANGUAGE);
        for (int i = 0; i < XP_COUNT; i++)
            if (i != XP_STATS && i != XP_OVERLAYS && i != XP_REPLAYS) rows[n++] = i;
        break;
    }
    return n;
}

static const char *on_off(int v) { return v ? "On" : "Off"; }
/* Immersive's Replays / Point cutaways: LG_VIEW_ */
static const char *view_label(int v)
{
    return v == LG_VIEW_FLAT ? "Flat screen" : v == LG_VIEW_WORLD ? "In the world" : v == LG_VIEW_CAMERA ? "Camera view" : "Off";
}
/* Off > Flat screen > Camera view > Off ("In the world" left out, T13: confusing when the angle does not change) */
static int view_next(int v) { return v == LG_VIEW_OFF ? LG_VIEW_FLAT : v == LG_VIEW_FLAT ? LG_VIEW_CAMERA : LG_VIEW_OFF; }
static const char *height_label(float units)
{
    const int v = (int)lroundf(units);
    return v == 0 ? "Low" : v == 10 ? "Medium" : v == 20 ? "High" : NULL;
}
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
        snprintf(it[XW_MODE].value, 40, "%s%s", m ? "VR (3D world)" : "Classic (2D screen)",
                 pinned(w, XW_MODE) ? " (env.txt)" : !w->live && m != w->running ? " (on restart)" : "");
        it[XW_MODE_CLASSIC].label = "Classic";
        it[XW_MODE_VR].label = "VR";
        snprintf(it[m ? XW_MODE_VR : XW_MODE_CLASSIC].value, 40, "selected");
    }
    it[XW_RESTART].label = cfg->world_mode ? "Restart now in VR mode" : "Restart now in Classic mode";
    it[XW_ADVANCED].label = cfg->world_mode ? "Advanced settings (VR) >" : "Advanced settings (Classic) >";
    it[XW_BACK].label = "< Back";
    it[XW_DOLLY].label = "Camera closer";
    it[XW_STEPBACK].label = "Step back";
    it[XW_HEIGHT].label = "Camera height";
    it[XW_REPLAYS].label = "Replays";
    it[XW_CUTAWAYS].label = "Point cutaways";
    it[XW_OVERLAYS].label = "Arcade overlays";
    it[XW_SKY].label = "Sky";
    it[XW_SHADOW].label = "Shadow quality";
    it[XW_CARD].label = "Card reader notice";
    it[XW_LANGUAGE].label = "Language";
    {   /* both apply on the next start: "(on restart)" while the stored one is not the running one */
        int card = cfg->classic_card_notice, lang = cfg->language;
        if (pinned(w, XW_CARD)) card = w->eff.card_notice;
        if (pinned(w, XW_LANGUAGE)) lang = w->eff.language;
        snprintf(it[XW_CARD].value, 40, "%s%s", on_off(card),
                 !pinned(w, XW_CARD) && w->running == 0 && card != w->eff.card_notice ? " (on restart)" : "");
        snprintf(it[XW_LANGUAGE].value, 40, "%s%s", lang ? "English" : "Japanese",
                 !pinned(w, XW_LANGUAGE) && lang != w->eff.language ? " (on restart)" : "");
    }
    it[XW_RESTORE].label = im ? "Restore VR defaults" : "Restore Classic defaults";
    /* the page's stored values; a pinned row the run's own */
    const WmValues *e = &w->eff;
    int replays = im ? cfg->imm_replays : cfg->replays, cut = im ? cfg->imm_cutaways : cfg->classic_cutaways;
    int over = im ? cfg->imm_overlays : cfg->arcade_overlays, sh = im ? cfg->imm_shadow : cfg->classic_shadow;
    float dl = (float)cfg->imm_dolly, bk = (float)cfg->imm_back, ht = (float)cfg->imm_height; int sky = cfg->imm_sky;
    if (pinned(w, XW_REPLAYS)) replays = !e->replays ? LG_VIEW_OFF : im ? e->replay_view : LG_VIEW_FLAT;
    if (pinned(w, XW_CUTAWAYS)) cut = !e->cutaways ? LG_VIEW_OFF : im ? e->cutaway_view : LG_VIEW_FLAT;
    if (pinned(w, XW_OVERLAYS)) over = e->overlays;
    if (pinned(w, XW_SHADOW)) sh = e->shadow;
    if (pinned(w, XW_DOLLY)) dl = e->dolly;
    if (pinned(w, XW_STEPBACK)) bk = e->back;
    if (pinned(w, XW_HEIGHT)) ht = e->height;
    if (pinned(w, XW_SKY)) sky = e->sky;
    snprintf(it[XW_REPLAYS].value, 40, "%s", im ? view_label(replays) : on_off(replays));
    snprintf(it[XW_CUTAWAYS].value, 40, "%s", im ? view_label(cut) : on_off(cut));
    snprintf(it[XW_OVERLAYS].value, 40, "%s", over ? "Shown" : "Hidden");
    snprintf(it[XW_SKY].value, 40, "%s", sky_label(sky));
    metres(it[XW_DOLLY].value, dl);
    metres(it[XW_STEPBACK].value, bk);
    if (height_label(ht)) snprintf(it[XW_HEIGHT].value, 40, "%s", height_label(ht));
    else { metres(it[XW_HEIGHT].value, fabsf(ht)); size_t l = strlen(it[XW_HEIGHT].value);    /* an env value off the list */
           snprintf(it[XW_HEIGHT].value + l, 40 - l, ht >= 0 ? " higher" : " lower"); }
    shadow(it[XW_SHADOW].value, sh);
    for (int i = 0; i < XW_COUNT; i++) {
        size_t l = strlen(it[i].value);
        if (i != XW_MODE && i != XW_MODE_CLASSIC && i != XW_MODE_VR && (i != XW_CARD || page_mode(w) == 0) && pinned(w, i)) snprintf(it[i].value + l, 40 - l, " (env.txt)");
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
        c->imm_height = d.imm_height;
        c->imm_sky = d.imm_sky; c->imm_shadow = d.imm_shadow;
    } else {
        c->replays = d.replays; c->arcade_overlays = d.arcade_overlays; c->classic_card_notice = d.classic_card_notice;
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
    case XW_MODE_CLASSIC: case XW_MODE_VR: {
        const int m = item == XW_MODE_VR;
        if (c->world_mode == m) return armed ? XW_PAGE_CHANGED : 0;
        c->world_mode = m;
        if (page_mode(w) >= 0) w->page = m ? XW_PAGE_IMMERSIVE : XW_PAGE_CLASSIC;   /* an Advanced page: the chosen mode's */
        fx |= XW_PAGE_CHANGED | (w->live ? XW_LIVE_CHANGED : 0); break; }
    case XW_RESTART: return XP_RESTART;
    case XW_ADVANCED: w->page = c->world_mode ? XW_PAGE_IMMERSIVE : XW_PAGE_CLASSIC; return XW_PAGE_CHANGED;
    case XW_BACK: w->page = XW_PAGE_MAIN; return XW_PAGE_CHANGED;
    case XW_REPLAYS: if (im) c->imm_replays = view_next(c->imm_replays); else c->replays = !c->replays; fx |= live; break;
    case XW_CUTAWAYS: if (im) c->imm_cutaways = view_next(c->imm_cutaways); else c->classic_cutaways = !c->classic_cutaways; fx |= live; break;
    case XW_OVERLAYS: if (im) c->imm_overlays = !c->imm_overlays; else c->arcade_overlays = !c->arcade_overlays; fx |= live; break;
    case XW_SKY:        /* Stadium -> Painted -> Off -> Stadium */
        if (!im) return 0;
        c->imm_sky = c->imm_sky == LG_SKY_STADIUM ? LG_SKY_PAINTED : c->imm_sky == LG_SKY_PAINTED ? LG_SKY_OFF : LG_SKY_STADIUM;
        fx |= live; break;
    case XW_HEIGHT: if (!im) return 0; c->imm_height = next_step(c->imm_height, LG_XR_HEIGHT_STEPS, LG_XR_HEIGHT_NSTEPS); fx |= live; break;
    case XW_CARD: if (im || page_mode(w) < 0) return 0; c->classic_card_notice = !c->classic_card_notice; break;   /* on restart */
    case XW_LANGUAGE: c->language = !c->language; break;                                                     /* on restart */
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
         : w->page == XW_PAGE_IMMERSIVE ? "VR mode - advanced settings" : NULL;
}

const char *xw_mode_note(const XwState *w, const LgXrSettings *cfg)
{
    if (pinned(w, XW_MODE)) return w->running ? "VR runs (set by env.txt)" : "Classic runs (set by env.txt)";
    if (!w->live && cfg->world_mode != w->running)
        return cfg->world_mode ? "Classic runs now - VR after a restart" : "VR runs now - Classic after a restart";
    return w->running ? "VR runs now" : "Classic runs now";
}

const char *xw_hint(const XwState *w)
{
    const int m = page_mode(w);
    if (m >= 0 && m != w->running)
        return m ? "Applies when VR mode runs." : "Applies when Classic mode runs.";
    return "Point and pull the trigger. Menu: close.";    /* 42 characters fit */
}
