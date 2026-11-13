#include "xr_settings.h"
#include "xr_overlay.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static const char *const LASER_NAME[]={"laser","dot","off"};
static const char *const SIGHT_NAME[]={"none","reddot","holo"};
static const char *const FOVEATION_NAME[]={"off","low","medium","high"};
static const char *const VIEW_NAME[]={"off","flat","world","camera"};
static const char *view_name(int v) { return VIEW_NAME[v < LG_VIEW_OFF || v > LG_VIEW_CAMERA ? LG_VIEW_OFF : v]; }
/* an Immersive replays / cutaways value: off|flat|camera, an old "on" = flat, "world" = camera */
static int view_value(const char *v)
{
    if (!strcmp(v, "world")) return LG_VIEW_CAMERA;    /* "In the world" is gone from the panel (T13): its old choice = Camera view */
    if (!strcmp(v, "vr")) return LG_VIEW_CAMERA;       /* the panel's label for it since 2026-11-13 (written as "camera") */
    for (int i = 0; i < 4; i++) if (!strcmp(v, VIEW_NAME[i])) return i;
    return !strcmp(v, "on") ? LG_VIEW_FLAT : LG_VIEW_OFF;
}
/* every stored setting as "key value" lines, in the file's fixed order */
static int render(const LgXrSettings *c, char *b, size_t n)
{
    return snprintf(b, n, "scale %g\nscreen %g\naim %g\nresolution %d\nwidth %d\nrefresh %d\nhand %s\nlaser %s\ncoins %s\n"
               "sight %s\ncrosshair %s\ngun %s\nhud_pitch %d\ndual %s\nstats %s\nfoveation %s\noverlays %s\nreplays %s\n"
               "mode %s\nclassic.shadow %d\nclassic.cutaways %s\nimmersive.replays %s\nimmersive.overlays %s\n"
               "immersive.cutaways %s\nimmersive.sky %s\nimmersive.shadow %d\n"
               "immersive.height %d\nclassic.card_notice %s\nlanguage %s\nmsaa %d\nsave %s\naniso %d\nsave_name %s\n",
            c->scale, c->screen, c->aim, c->resolution, c->width, c->refresh, c->left_handed ? "left" : "right",
            LASER_NAME[c->laser], c->auto_coins ? "auto" : "manual",
            SIGHT_NAME[c->sight], c->crosshair ? "on" : "off", c->show_gun ? "on" : "off", c->hud_pitch, c->dual ? "on" : "off", c->show_stats ? "on" : "off",
            FOVEATION_NAME[c->foveation < 0 || c->foveation > 3 ? 3 : c->foveation], c->arcade_overlays ? "on" : "off", c->replays ? "on" : "off",
            c->world_mode ? "immersive" : "classic", c->classic_shadow, c->classic_cutaways ? "on" : "off",
            view_name(c->imm_replays), c->imm_overlays ? "on" : "off", view_name(c->imm_cutaways),
            c->imm_sky == LG_SKY_OFF ? "off" : c->imm_sky == LG_SKY_PAINTED ? "painted" : "stadium", c->imm_shadow,
            c->imm_height, c->classic_card_notice ? "on" : "off", c->language ? "english" : "japanese", c->msaa, c->save ? "on" : "off",
            xr_settings_aniso(c->aniso), c->save_name[0] ? c->save_name : LG_XR_SAVE_NAME_DEFAULT);
}

int xr_settings_save_name(const char *in, char out[16])
{
    char b[16];
    size_t n = in ? strlen(in) : 0;
    int ok = n >= 1 && n <= LG_XR_SAVE_NAME_MAX;
    for (size_t i = 0; ok && i < n; i++) {
        char ch = in[i];
        if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9'))) ok = 0;
        b[i] = ch;
    }
    if (ok) { memcpy(out, b, n); out[n] = 0; }
    else snprintf(out, 16, "%s", LG_XR_SAVE_NAME_DEFAULT);
    return ok;
}

static void (*g_game_defaults)(struct LgXrSettings *);
void xr_settings_set_game_defaults(void (*fn)(struct LgXrSettings *)) { g_game_defaults = fn; }
void xr_settings_defaults(LgXrSettings *c)
{
    const LgXrSettings d = LG_XR_SETTINGS_DEFAULT;
    *c = d;
    if (g_game_defaults) g_game_defaults(c);
}
void xr_settings_vt3_defaults(struct LgXrSettings *c)
{
    c->world_mode = 1;
    c->imm_replays = c->imm_cutaways = LG_VIEW_FLAT; c->imm_overlays = 0; c->imm_sky = LG_SKY_STADIUM;
    c->imm_shadow = 4096; c->imm_height = 10;
    c->replays = c->classic_cutaways = 1; c->arcade_overlays = 0; c->classic_shadow = 1024; c->classic_card_notice = 0;
    c->msaa = 2; c->aniso = 4; c->save = 1; snprintf(c->save_name, sizeof c->save_name, "%s", LG_XR_SAVE_NAME_DEFAULT);
    c->language = 1; c->refresh = 120; c->foveation = 3; c->screen = 2.5f; c->hud_pitch = 10; c->show_stats = 0;
    c->auto_coins = 1;
}

int xr_settings_aniso(int v)
{
    return v >= 16 ? 16 : v >= 8 ? 8 : v >= 4 ? 4 : v >= 2 ? 2 : 0;
}

/* LINDBERGH_XR_SET's keys are for its run only: the save writes those keys as
 * the file had them (or the default), not the run's values */
static LgXrSettings g_file;         /* the settings as loaded, before LINDBERGH_XR_SET */
static char g_run_keys[24][32];
static int g_nrun;

void xr_settings_save(const LgXrSettings *c)
{
    char now[2048], file[2048];
    render(c, now, sizeof now);
    if (g_nrun) render(&g_file, file, sizeof file);
    FILE *f = fopen("vr.cfg", "w");
    if (!f) return;
    for (char *a = now, *b = g_nrun ? file : NULL; *a; ) {
        char *ea = strchr(a, '\n'), *eb = b ? strchr(b, '\n') : NULL;
        size_t kl = strcspn(a, " ");
        int run = 0;
        for (int i = 0; i < g_nrun; i++) if (strlen(g_run_keys[i]) == kl && !strncmp(a, g_run_keys[i], kl)) run = 1;
        const char *line = run && b ? b : a;
        fwrite(line, 1, (size_t)((run && b ? eb : ea) - line) + 1, f);
        a = ea + 1; if (b) b = eb + 1;
    }
    fclose(f);
}

void xr_settings_load(LgXrSettings *c)
{
    /* the game's own size of a metre, until vr.cfg says otherwise: set by
     * its android_main (Let's Go Jungle's world is in about decimetres) */
    { const char *v = getenv("LINDBERGH_XR_SCALE"); if (v && atof(v) > 0) c->scale = (float)atof(v); }
    FILE *f = fopen("vr.cfg", "r");
    if (!f) f = fopen("../vr.cfg", "r");    /* beside the game: where adb can put one */
    char key[32], val[32];
    int has_rep = 0, has_cut = 0, has_irep = 0, has_icut = 0;
    while (f && fscanf(f, "%31s %31s", key, val) == 2) {
        xr_settings_set(c,key,val);
        has_rep |= !strcmp(key, "replays"); has_cut |= !strcmp(key, "classic.cutaways");
        has_irep |= !strcmp(key, "immersive.replays"); has_icut |= !strcmp(key, "immersive.cutaways");
    }
    if (f) fclose(f);
    /* Replays and point cutaways are one row since 2026-11-13 ("Replays &
     * close-ups"): one value per mode, both old keys written with it. A file
     * with the two different: the replays key's value (an older build's file);
     * one key alone: that one's. */
    if (has_irep && has_icut && c->imm_replays != c->imm_cutaways)
        fprintf(stderr, "[xr] vr.cfg: immersive.replays %s and immersive.cutaways %s differ - one row now, the replays value taken\n",
                view_name(c->imm_replays), view_name(c->imm_cutaways));
    if (has_icut && !has_irep) c->imm_replays = c->imm_cutaways; else c->imm_cutaways = c->imm_replays;
    if (has_rep && has_cut && c->replays != c->classic_cutaways)
        fprintf(stderr, "[xr] vr.cfg: replays %s and classic.cutaways %s differ - one row now, the replays value taken\n",
                c->replays ? "on" : "off", c->classic_cutaways ? "on" : "off");
    if (has_cut && !has_rep) c->replays = c->classic_cutaways; else c->classic_cutaways = c->replays;
    g_file = *c; g_nrun = 0;
    /* LINDBERGH_XR_SET="key=value,...": for this run only, not saved (testing) */
    const char *e = getenv("LINDBERGH_XR_SET");
    while (e && *e) {
        if (sscanf(e, "%31[^=]=%31[^,]", key, val) == 2) {
            xr_settings_set(c,key,val);
            if (g_nrun < 24) memcpy(g_run_keys[g_nrun++], key, sizeof key);
        }
        e = strchr(e, ',');
        if (e) e++;
    }
    if (c->scale <= 0) c->scale = 1;
    if (c->screen <= 0.2f) c->screen = 3;
    if (c->aim < 1.0f || c->aim > 200.0f) c->aim = 15.0f;
    if (c->resolution < 512 || c->resolution > 4096 || c->width < 0 || c->width > 4096) {
        c->resolution = LG_XR_NATIVE_HEIGHT; c->width = LG_XR_NATIVE_WIDTH;
    }
    /* 2064x2280 was every build's default and is in every vr.cfg the panel
     * ever saved: it meant "the default", which is now the panel's own */
    if (c->width == 2064 && c->resolution == 2280) {
        c->resolution = LG_XR_NATIVE_HEIGHT; c->width = LG_XR_NATIVE_WIDTH;
    }
    LgXrSettings d; xr_settings_defaults(&d);
    if (c->classic_shadow != 1024 && c->classic_shadow != 2048 && c->classic_shadow != 4096) c->classic_shadow = d.classic_shadow;
    if (c->imm_shadow != 1024 && c->imm_shadow != 2048 && c->imm_shadow != 4096) c->imm_shadow = d.imm_shadow;
    if (c->imm_sky < LG_SKY_OFF || c->imm_sky > LG_SKY_PAINTED) c->imm_sky = d.imm_sky;
    c->imm_dolly = d.imm_dolly; c->imm_back = d.imm_back;
    if (c->msaa != 0 && c->msaa != 2 && c->msaa != 4) c->msaa = c->msaa > 4 ? 4 : c->msaa > 0 ? 2 : 0;
    c->aniso = xr_settings_aniso(c->aniso);
    { char nm[16]; xr_settings_save_name(c->save_name, nm); memcpy(c->save_name, nm, sizeof nm); }
    if (c->imm_replays < LG_VIEW_OFF || c->imm_replays > LG_VIEW_CAMERA) c->imm_replays = d.imm_replays;
    if (c->imm_cutaways < LG_VIEW_OFF || c->imm_cutaways > LG_VIEW_CAMERA) c->imm_cutaways = d.imm_cutaways;
    /* the panel's choices (xr_world_panel.c); any other stored value - an
     * older panel's -2 / -1 m, a hand edit - becomes the nearest one */
    c->imm_height = xr_settings_nearest(c->imm_height, LG_XR_HEIGHT_STEPS, LG_XR_HEIGHT_NSTEPS);
}

void xr_settings_set(LgXrSettings *c,const char *key,const char *val)
{
    {
        if (!strcmp(key, "scale")) c->scale = (float)atof(val);
        else if (!strcmp(key, "screen")) c->screen = (float)atof(val);
        else if (!strcmp(key, "hud_pitch")) c->hud_pitch = atoi(val);
        else if (!strcmp(key, "aim")) c->aim = (float)atof(val);
        else if (!strcmp(key, "resolution")) c->resolution = atoi(val);
        else if (!strcmp(key, "width")) c->width = atoi(val);
        else if (!strcmp(key, "refresh")) c->refresh = atoi(val);
        else if (!strcmp(key, "hand")) c->left_handed = !strcmp(val, "left");
        else if (!strcmp(key, "laser")) c->laser = !strcmp(val, "dot") ? LASER_DOT : !strcmp(val, "off") ? LASER_OFF : LASER_ON;
        else if (!strcmp(key, "coins")) c->auto_coins = strcmp(val, "manual") != 0;
        else if (!strcmp(key, "sight")) c->sight = !strcmp(val, "reddot") ? XO_SIGHT_RED_DOT : !strcmp(val, "holo") ? XO_SIGHT_HOLO : XO_SIGHT_NONE;
        else if (!strcmp(key, "crosshair")) c->crosshair = strcmp(val, "off") != 0;
        else if (!strcmp(key, "gun")) c->show_gun = strcmp(val, "off") != 0;
        else if (!strcmp(key, "dual")) c->dual = !strcmp(val, "on");
        else if (!strcmp(key, "stats") || !strcmp(key, "fps")) c->show_stats = !strcmp(val, "on");   /* "fps": before it was the stats panel */
        else if (!strcmp(key, "overlays")) c->arcade_overlays = !strcmp(val, "on");
        else if (!strcmp(key, "replays")) c->replays = !strcmp(val, "on");
        else if (!strcmp(key, "mode")) c->world_mode = !strcmp(val, "immersive");
        else if (!strcmp(key, "classic.shadow")) c->classic_shadow = atoi(val);
        else if (!strcmp(key, "classic.cutaways")) c->classic_cutaways = !strcmp(val, "on");
        else if (!strcmp(key, "immersive.replays")) c->imm_replays = view_value(val);
        else if (!strcmp(key, "immersive.overlays")) c->imm_overlays = !strcmp(val, "on");
        else if (!strcmp(key, "immersive.cutaways")) c->imm_cutaways = view_value(val);
        /* "immersive.stand_y" (the stands' old stretch) is ignored: it is not a key any more */
        /* "immersive.dolly" / ".back" (the old Camera closer / Step back rows) are ignored: fixed now */
        else if (!strcmp(key, "immersive.sky")) c->imm_sky = !strcmp(val, "off") ? LG_SKY_OFF : !strcmp(val, "painted") ? LG_SKY_PAINTED : LG_SKY_STADIUM;   /* "on" (before 2026-11-12): a sky, now the stadium's */
        else if (!strcmp(key, "immersive.shadow")) c->imm_shadow = atoi(val);
        else if (!strcmp(key, "immersive.height")) c->imm_height = atoi(val);
        else if (!strcmp(key, "classic.card_notice")) c->classic_card_notice = !strcmp(val, "on");
        else if (!strcmp(key, "language")) c->language = strcmp(val, "japanese") != 0;
        else if (!strcmp(key, "msaa")) c->msaa = atoi(val);
        else if (!strcmp(key, "save")) c->save = !strcmp(val, "on");
        else if (!strcmp(key, "aniso")) c->aniso = atoi(val);
        else if (!strcmp(key, "save_name")) snprintf(c->save_name, sizeof c->save_name, "%s", val);   /* checked at load */
        else if (!strcmp(key, "foveation")) {
            for (int i = 0; i < 4; i++) if (!strcmp(val, FOVEATION_NAME[i])) c->foveation = i;
        }
    }
}
