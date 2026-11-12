/*
 * world_mode.c - which mode the run is in (Classic / Immersive) and the
 * values that go with it, resolved once at start (world_mode.h).
 *
 * Switching live (Phase B) is groundwork only: the target / game / GL
 * latches, world_mode_frame and the test switch work, but nothing changes
 * mode until a switch callback is registered (world_mode_on_switch), and none
 * is yet - so world_mode_update keeps the started mode, as before.
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
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const ENV_NAME[WM_COUNT] = {
    "LINDBERGH_VT3_IMMERSIVE", "LINDBERGH_REPLAYS", "LINDBERGH_ARCADE_OVERLAYS", "LINDBERGH_VT3_CUTAWAYS",
    "LINDBERGH_SHADOW_SCALE", "LINDBERGH_VT3_DOLLY", "LINDBERGH_VT3_BACK", "LINDBERGH_VT3_SKY",
    "LINDBERGH_VT3_HEIGHT" };
static const char *const KEY_CLASSIC[WM_COUNT] = {
    "mode", "replays", "overlays", "classic.cutaways", "classic.shadow", NULL, NULL, NULL, NULL };
static const char *const KEY_IMMERSIVE[WM_COUNT] = {
    "mode", "immersive.replays", "immersive.overlays", "immersive.cutaways", "immersive.shadow",
    "immersive.dolly", "immersive.back", "immersive.sky", "immersive.height" };

const char *world_mode_env_name(int key) { return key >= 0 && key < WM_COUNT ? ENV_NAME[key] : NULL; }
const char *world_mode_key(int key, int immersive)
{
    if (key < 0 || key >= WM_COUNT) return NULL;
    return immersive ? KEY_IMMERSIVE[key] : KEY_CLASSIC[key];
}
static const char *env(int key) { const char *v = getenv(ENV_NAME[key]); return v && *v ? v : NULL; }
int world_mode_pinned(int key) { return key >= 0 && key < WM_COUNT && env(key) != NULL; }

int world_mode_sky_value(const char *v)
{
    if (!v || !*v) return LG_SKY_STADIUM;
    if (!strcmp(v, "0") || !strcmp(v, "off")) return LG_SKY_OFF;
    if (!strcmp(v, "2") || !strcmp(v, "painted")) return LG_SKY_PAINTED;
    return LG_SKY_STADIUM;      /* 1, on, stadium */
}
static const char *sky_name(int s) { return s == LG_SKY_OFF ? "off" : s == LG_SKY_PAINTED ? "painted" : "stadium"; }

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
    o->dolly = (float)s->imm_dolly; o->back = (float)s->imm_back; o->sky = s->imm_sky;
    o->height = (float)s->imm_height;
    if ((v = env(WM_REPLAYS))) { o->replays = *v != '0'; k[WM_REPLAYS] = WM_SRC_ENV; }
    if ((v = env(WM_OVERLAYS))) { o->overlays = *v != '0'; k[WM_OVERLAYS] = WM_SRC_ENV; }
    if ((v = env(WM_CUTAWAYS))) { o->cutaways = *v != '0'; k[WM_CUTAWAYS] = WM_SRC_ENV; }
    if ((v = env(WM_SHADOW))) {
        o->shadow_scale = (float)atof(v); k[WM_SHADOW] = WM_SRC_ENV;
        o->shadow = o->shadow_scale > 0 ? (int)(1024 * o->shadow_scale + 0.5f) : 1024;
    }
    if ((v = env(WM_DOLLY))) { o->dolly = (float)atof(v); k[WM_DOLLY] = WM_SRC_ENV; }
    if ((v = env(WM_BACK))) { o->back = (float)atof(v); k[WM_BACK] = WM_SRC_ENV; }
    if ((v = env(WM_HEIGHT))) { o->height = (float)atof(v); k[WM_HEIGHT] = WM_SRC_ENV; }
    if ((v = env(WM_SKY))) { o->sky = world_mode_sky_value(v); k[WM_SKY] = WM_SRC_ENV; }
    if (src) memcpy(src, k, sizeof k);
}

/* g_wm: the game's mode's values (the getters); g_by_mode: both modes' values
 * from the last stored settings, so a switch takes the new mode's values on
 * the frame it latches. The lock covers the two between the panel's thread
 * (world_mode_update) and the game thread (world_mode_frame); the getters
 * read single fields without it, as before. */
static WmValues g_wm, g_by_mode[2];
static int g_wm_done, g_wm_capable;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_start;                     /* the mode the run started in */
static atomic_int g_target;             /* the mode asked for: panel (world_mode_update) or test switch */
static atomic_int g_target_test;        /* the target was set by the test switch */
static int g_game;                      /* the game thread's mode (world_mode_frame) */
static atomic_int g_gl;                 /* the GL thread's mode (world_mode_gl_latch) */
static int g_panel_req;                 /* the mode the stored settings last asked for */
static _Atomic(WmSwitchFn) g_switch_fn;

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
    fprintf(stderr, "[world] mode %s (%s) dolly %.0f (%s) back %.0f (%s) height %.0f (%s) sky %s (%s) shadow %d (%s) "
                    "replays %s (%s) overlays %s (%s) cutaways %s (%s)%s\n",
            g_wm.immersive ? "immersive" : "classic", SRC[src[WM_MODE]],
            g_wm.dolly, SRC[src[WM_DOLLY]], g_wm.back, SRC[src[WM_BACK]], g_wm.height, SRC[src[WM_HEIGHT]],
            sky_name(g_wm.sky), SRC[src[WM_SKY]], g_wm.shadow, SRC[src[WM_SHADOW]],
            g_wm.replays ? "on" : "off", SRC[src[WM_REPLAYS]], g_wm.overlays ? "on" : "off", SRC[src[WM_OVERLAYS]],
            g_wm.cutaways ? "on" : "off", SRC[src[WM_CUTAWAYS]], capable ? "" : " - this game has no world mode");
    g_start = g_game = g_panel_req = g_wm.immersive;
    atomic_store(&g_target, g_start);
    atomic_store(&g_gl, g_start);
    for (int m = 0; m < 2; m++) {
        world_mode_resolve(&s, m, capable, 0, &g_by_mode[m], NULL);
        g_by_mode[m].shadow = g_wm.shadow; g_by_mode[m].shadow_scale = g_wm.shadow_scale;
    }
}

static int kill_switch(void) { const char *v = getenv("LINDBERGH_VT3_LIVE_SWITCH"); return v && *v == '0'; }
int world_mode_live(void) { return atomic_load(&g_switch_fn) != NULL && !kill_switch(); }
void world_mode_on_switch(WmSwitchFn fn) { atomic_store(&g_switch_fn, fn); }

void world_mode_update(const LgXrSettings *s)
{
    if (!g_wm_done) world_mode_init(0);
    WmValues v[2];
    for (int m = 0; m < 2; m++) {
        world_mode_resolve(s, m, g_wm_capable, 0, &v[m], NULL);
        v[m].shadow = g_wm.shadow; v[m].shadow_scale = g_wm.shadow_scale;  /* the shadow map is made at start */
    }
    pthread_mutex_lock(&g_lock);
    if (world_mode_live()) {
        /* the mode asked for: env's pin, else the stored mode (a game that
         * cannot draw a world keeps its start). Only a change of it moves the
         * target, so a click on another row keeps a test switch's mode. */
        const char *e = env(WM_MODE);
        const int req = e ? *e == '1' : g_wm_capable ? s && s->world_mode == 1 : g_start;
        if (req != g_panel_req) {
            g_panel_req = req;
            atomic_store(&g_target_test, 0);
            atomic_store(&g_target, req);
        }
    }
    g_by_mode[0] = v[0]; g_by_mode[1] = v[1];
    g_wm = v[g_game];       /* the running mode's values at once; another mode's when it latches */
    pthread_mutex_unlock(&g_lock);
}

/* LINDBERGH_VT3_MODE_AT=<frame>:<0|1>,... - test mode switches */
#define WM_TESTS_MAX 32
static struct { unsigned at; int mode, done; } g_tests[WM_TESTS_MAX];
static int g_ntests = -1;
static void tests_parse(void)
{
    g_ntests = 0;
    const char *v = getenv("LINDBERGH_VT3_MODE_AT");
    while (v && *v && g_ntests < WM_TESTS_MAX) {
        unsigned at; int mode, used = 0;
        if (sscanf(v, "%u:%d%n", &at, &mode, &used) == 2 && (mode == 0 || mode == 1)) {
            g_tests[g_ntests].at = at; g_tests[g_ntests].mode = mode; g_tests[g_ntests].done = 0; g_ntests++;
        } else fprintf(stderr, "[world] LINDBERGH_VT3_MODE_AT: cannot read \"%s\" (want <frame>:<0|1>,...)\n", v);
        v = strchr(v, ',');
        if (v) v++;
        (void)used;
    }
}

int world_mode_frame(unsigned frame)
{
    if (!g_wm_done) world_mode_init(0);
    WmSwitchFn fn = atomic_load(&g_switch_fn);
    if (!fn) return g_game;         /* nothing follows a switch yet: the start mode */
    if (g_ntests < 0) tests_parse();
    for (int i = 0; i < g_ntests; i++)
        if (!g_tests[i].done && frame >= g_tests[i].at) {
            g_tests[i].done = 1;
            atomic_store(&g_target_test, 1);
            atomic_store(&g_target, g_tests[i].mode);
        }
    const int t = atomic_load(&g_target);
    if (t == g_game) return g_game;
    pthread_mutex_lock(&g_lock);
    g_game = t;
    g_wm = g_by_mode[t];
    pthread_mutex_unlock(&g_lock);
    fprintf(stderr, "[world] frame %u: mode -> %s (%s)\n", frame, t ? "immersive" : "classic",
            atomic_load(&g_target_test) ? "test" : "panel");
    fn(t);
    return g_game;
}

static const WmValues *wm(void) { if (!g_wm_done) world_mode_init(0); return &g_wm; }
const WmValues *world_mode_values(void) { return wm(); }
int world_mode_capable(void) { wm(); return g_wm_capable; }
int world_mode_immersive(void) { wm(); return g_game; }
int world_mode_target(void) { wm(); return atomic_load(&g_target); }
int world_mode_drawing_immersive(void) { wm(); return atomic_load(&g_gl); }
void world_mode_gl_latch(int immersive) { atomic_store(&g_gl, immersive != 0); }
int world_mode_replays(void) { return wm()->replays; }
int world_mode_overlays(void) { return wm()->overlays; }
int world_mode_cutaways(void) { return wm()->cutaways; }
int world_mode_sky(void) { return wm()->sky; }
float world_mode_shadow_scale(void) { return wm()->shadow_scale; }
float world_mode_dolly(void) { return wm()->dolly; }
float world_mode_back(void) { return wm()->back; }
float world_mode_height(void) { return wm()->height; }
