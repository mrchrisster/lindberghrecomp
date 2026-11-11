/*
 * world_mode.c - which mode the run is in (Classic / Immersive) and the
 * values that go with it, resolved once at start (world_mode.h).
 *
 * Each value: the env variable when it is set (a run's own choice, env.txt on
 * the headset), else the stored setting (vr.cfg; a key missing there has the
 * mode's default), else the mode's default. Classic keeps the old keys
 * replays / overlays; Immersive has its own (immersive.replays, ...). The
 * stored settings are read here early, before the headset host loads them,
 * because which hooks to install is decided at registration
 * (hle_register_sega -> vt3_jumps), before xr_init.
 */
#include "world_mode.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const ENV_NAME[WM_COUNT] = {
    "LINDBERGH_VT3_IMMERSIVE", "LINDBERGH_REPLAYS", "LINDBERGH_ARCADE_OVERLAYS", "LINDBERGH_VT3_CUTAWAYS",
    "LINDBERGH_SHADOW_SCALE", "LINDBERGH_VT3_STAND_Y", "LINDBERGH_VT3_DOLLY", "LINDBERGH_VT3_BACK", "LINDBERGH_VT3_SKY" };
static const char *const KEY_CLASSIC[WM_COUNT] = {
    "mode", "replays", "overlays", "classic.cutaways", "classic.shadow", NULL, NULL, NULL, NULL };
static const char *const KEY_IMMERSIVE[WM_COUNT] = {
    "mode", "immersive.replays", "immersive.overlays", "immersive.cutaways", "immersive.shadow",
    "immersive.stand_y", "immersive.dolly", "immersive.back", "immersive.sky" };

const char *world_mode_env_name(int key) { return key >= 0 && key < WM_COUNT ? ENV_NAME[key] : NULL; }
const char *world_mode_key(int key, int immersive)
{
    if (key < 0 || key >= WM_COUNT) return NULL;
    return immersive ? KEY_IMMERSIVE[key] : KEY_CLASSIC[key];
}
static const char *env(int key) { const char *v = getenv(ENV_NAME[key]); return v && *v ? v : NULL; }
int world_mode_pinned(int key) { return key >= 0 && key < WM_COUNT && env(key) != NULL; }

static float scale_of(int size) { return size > 1024 ? size / 1024.0f : 0.0f; }

void world_mode_resolve(const LgXrSettings *s, int mode, int capable, unsigned present, WmValues *o, int src[WM_COUNT])
{
    const LgXrSettings def = LG_XR_SETTINGS_DEFAULT;
    if (!s) s = &def;
    int k[WM_COUNT];
    for (int i = 0; i < WM_COUNT; i++) k[i] = present >> i & 1 ? WM_SRC_FILE : WM_SRC_DEFAULT;
    const char *v;
    if (mode < 0) {
        if ((v = env(WM_MODE))) { o->immersive = *v == '1'; k[WM_MODE] = WM_SRC_ENV; }
        else o->immersive = capable && s->world_mode == 1;
        if (!capable && !env(WM_MODE)) k[WM_MODE] = WM_SRC_DEFAULT;
    } else o->immersive = mode != 0;
    const int im = o->immersive;
    o->replays = im ? s->imm_replays : s->replays;
    o->overlays = im ? s->imm_overlays : s->arcade_overlays;
    o->cutaways = im ? s->imm_cutaways : s->classic_cutaways;
    o->shadow = im ? s->imm_shadow : s->classic_shadow;
    o->shadow_scale = scale_of(o->shadow);
    o->stand_y = s->imm_stand_y; o->dolly = (float)s->imm_dolly; o->back = (float)s->imm_back; o->sky = s->imm_sky;
    if ((v = env(WM_REPLAYS))) { o->replays = *v != '0'; k[WM_REPLAYS] = WM_SRC_ENV; }
    if ((v = env(WM_OVERLAYS))) { o->overlays = *v != '0'; k[WM_OVERLAYS] = WM_SRC_ENV; }
    if ((v = env(WM_CUTAWAYS))) { o->cutaways = *v != '0'; k[WM_CUTAWAYS] = WM_SRC_ENV; }
    if ((v = env(WM_SHADOW))) {
        o->shadow_scale = (float)atof(v); k[WM_SHADOW] = WM_SRC_ENV;
        o->shadow = o->shadow_scale > 0 ? (int)(1024 * o->shadow_scale + 0.5f) : 1024;
    }
    if ((v = env(WM_STAND))) { o->stand_y = (float)atof(v); k[WM_STAND] = WM_SRC_ENV; }
    if ((v = env(WM_DOLLY))) { o->dolly = (float)atof(v); k[WM_DOLLY] = WM_SRC_ENV; }
    if ((v = env(WM_BACK))) { o->back = (float)atof(v); k[WM_BACK] = WM_SRC_ENV; }
    if ((v = env(WM_SKY))) { o->sky = *v != '0'; k[WM_SKY] = WM_SRC_ENV; }
    if (src) memcpy(src, k, sizeof k);
}

static WmValues g_wm;
static int g_wm_done, g_wm_capable;

#ifdef LINDBERGH_OPENXR
/* which of the running mode's keys vr.cfg holds (for the [world] line) */
static unsigned file_present(int immersive)
{
    FILE *f = fopen("vr.cfg", "r");
    if (!f) f = fopen("../vr.cfg", "r");
    if (!f) return 0;
    unsigned bits = 0;
    char key[32], val[32];
    while (fscanf(f, "%31s %31s", key, val) == 2)
        for (int i = 0; i < WM_COUNT; i++) {
            const char *a = KEY_CLASSIC[i], *b = KEY_IMMERSIVE[i];
            if (i == WM_MODE ? !strcmp(key, "mode") : (immersive ? b && !strcmp(key, b) : a && !strcmp(key, a))) bits |= 1u << i;
        }
    fclose(f);
    return bits;
}
#endif

void world_mode_init(int capable)
{
    if (g_wm_done) return;
    g_wm_done = 1;
    g_wm_capable = capable;
    LgXrSettings s = LG_XR_SETTINGS_DEFAULT;
    unsigned present = 0;
    int src[WM_COUNT];
#ifdef LINDBERGH_OPENXR
    xr_settings_load(&s);                   /* the headset's stored settings (a scratch copy) */
    world_mode_resolve(&s, -1, capable, 0, &g_wm, NULL);
    present = file_present(g_wm.immersive);
#endif
    world_mode_resolve(&s, -1, capable, present, &g_wm, src);
    static const char *const SRC[] = { "default", "vr.cfg", "env" };
    fprintf(stderr, "[world] mode %s (%s) dolly %.0f (%s) back %.0f (%s) stand %.2f (%s) sky %s (%s) shadow %d (%s) "
                    "replays %s (%s) overlays %s (%s) cutaways %s (%s)%s\n",
            g_wm.immersive ? "immersive" : "classic", SRC[src[WM_MODE]],
            g_wm.dolly, SRC[src[WM_DOLLY]], g_wm.back, SRC[src[WM_BACK]], g_wm.stand_y, SRC[src[WM_STAND]],
            g_wm.sky ? "on" : "off", SRC[src[WM_SKY]], g_wm.shadow, SRC[src[WM_SHADOW]],
            g_wm.replays ? "on" : "off", SRC[src[WM_REPLAYS]], g_wm.overlays ? "on" : "off", SRC[src[WM_OVERLAYS]],
            g_wm.cutaways ? "on" : "off", SRC[src[WM_CUTAWAYS]], capable ? "" : " - this game has no world mode");
}

void world_mode_update(const LgXrSettings *s)
{
    if (!g_wm_done) world_mode_init(0);
    WmValues v;
    world_mode_resolve(s, g_wm.immersive, g_wm_capable, 0, &v, NULL);
    v.shadow = g_wm.shadow; v.shadow_scale = g_wm.shadow_scale;     /* the shadow map is made at start */
    g_wm = v;
}

static const WmValues *wm(void) { if (!g_wm_done) world_mode_init(0); return &g_wm; }
const WmValues *world_mode_values(void) { return wm(); }
int world_mode_capable(void) { wm(); return g_wm_capable; }
int world_mode_immersive(void) { return wm()->immersive; }
int world_mode_replays(void) { return wm()->replays; }
int world_mode_overlays(void) { return wm()->overlays; }
int world_mode_cutaways(void) { return wm()->cutaways; }
int world_mode_sky(void) { return wm()->sky; }
float world_mode_shadow_scale(void) { return wm()->shadow_scale; }
float world_mode_stand_y(void) { return wm()->stand_y; }
float world_mode_dolly(void) { return wm()->dolly; }
float world_mode_back(void) { return wm()->back; }
