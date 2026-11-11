#include "xr_settings.h"
#include "xr_overlay.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static const char *const LASER_NAME[]={"laser","dot","off"};
static const char *const SIGHT_NAME[]={"none","reddot","holo"};
static const char *const FOVEATION_NAME[]={"off","low","medium","high"};
/* every stored setting as "key value" lines, in the file's fixed order */
static int render(const LgXrSettings *c, char *b, size_t n)
{
    return snprintf(b, n, "scale %g\nscreen %g\naim %g\nresolution %d\nwidth %d\nrefresh %d\nhand %s\nlaser %s\ncoins %s\n"
               "sight %s\ncrosshair %s\ngun %s\nhud_pitch %d\ndual %s\nstats %s\nfoveation %s\noverlays %s\nreplays %s\n"
               "mode %s\nclassic.shadow %d\nclassic.cutaways %s\nimmersive.replays %s\nimmersive.overlays %s\n"
               "immersive.cutaways %s\nimmersive.stand_y %g\nimmersive.dolly %d\nimmersive.back %d\nimmersive.sky %s\nimmersive.shadow %d\n",
            c->scale, c->screen, c->aim, c->resolution, c->width, c->refresh, c->left_handed ? "left" : "right",
            LASER_NAME[c->laser], c->auto_coins ? "auto" : "manual",
            SIGHT_NAME[c->sight], c->crosshair ? "on" : "off", c->show_gun ? "on" : "off", c->hud_pitch, c->dual ? "on" : "off", c->show_stats ? "on" : "off",
            FOVEATION_NAME[c->foveation < 0 || c->foveation > 3 ? 3 : c->foveation], c->arcade_overlays ? "on" : "off", c->replays ? "on" : "off",
            c->world_mode ? "immersive" : "classic", c->classic_shadow, c->classic_cutaways ? "on" : "off",
            c->imm_replays ? "on" : "off", c->imm_overlays ? "on" : "off", c->imm_cutaways ? "on" : "off",
            c->imm_stand_y, c->imm_dolly, c->imm_back, c->imm_sky ? "on" : "off", c->imm_shadow);
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
    while (f && fscanf(f, "%31s %31s", key, val) == 2) xr_settings_set(c,key,val);
    if (f) fclose(f);
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
    const LgXrSettings d = LG_XR_SETTINGS_DEFAULT;
    if (c->classic_shadow != 1024 && c->classic_shadow != 2048 && c->classic_shadow != 4096) c->classic_shadow = d.classic_shadow;
    if (c->imm_shadow != 1024 && c->imm_shadow != 2048 && c->imm_shadow != 4096) c->imm_shadow = d.imm_shadow;
    if (!(c->imm_stand_y >= 1.0f && c->imm_stand_y <= 3.0f)) c->imm_stand_y = d.imm_stand_y;
    if (c->imm_dolly < 0 || c->imm_dolly > 400) c->imm_dolly = d.imm_dolly;
    if (c->imm_back < 0 || c->imm_back > 300) c->imm_back = d.imm_back;
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
        else if (!strcmp(key, "immersive.replays")) c->imm_replays = !strcmp(val, "on");
        else if (!strcmp(key, "immersive.overlays")) c->imm_overlays = !strcmp(val, "on");
        else if (!strcmp(key, "immersive.cutaways")) c->imm_cutaways = !strcmp(val, "on");
        else if (!strcmp(key, "immersive.stand_y")) c->imm_stand_y = (float)atof(val);
        else if (!strcmp(key, "immersive.dolly")) c->imm_dolly = atoi(val);
        else if (!strcmp(key, "immersive.back")) c->imm_back = atoi(val);
        else if (!strcmp(key, "immersive.sky")) c->imm_sky = strcmp(val, "off") != 0;
        else if (!strcmp(key, "immersive.shadow")) c->imm_shadow = atoi(val);
        else if (!strcmp(key, "foveation")) {
            for (int i = 0; i < 4; i++) if (!strcmp(val, FOVEATION_NAME[i])) c->foveation = i;
        }
    }
}
