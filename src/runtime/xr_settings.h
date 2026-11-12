#pragma once
#ifdef __cplusplus
extern "C" {
#endif
enum { LASER_ON, LASER_DOT, LASER_OFF };
/* Immersive's sky: none, the stadium model's own sky dome (Sega's), or the
 * runtime's painted sky (stereo.c sky_draw) */
enum { LG_SKY_OFF, LG_SKY_STADIUM, LG_SKY_PAINTED };
/* Immersive's replays and point cutaways (imm_replays, imm_cutaways): off, or
 * shown and how - the flat picture on the screen, in the world from where you
 * are, or from the game camera's own place (vr.cfg off|flat|world|camera; an
 * old "on" is flat). Classic's are on / off (always flat). */
enum { LG_VIEW_OFF, LG_VIEW_FLAT, LG_VIEW_WORLD, LG_VIEW_CAMERA };
typedef struct {
    float scale,screen,aim;
    int resolution,width,refresh,left_handed,laser,auto_coins;
    int sight,crosshair,show_gun,hud_pitch,dual,show_stats;
    int foveation;              /* 0 off, 1 low, 2 medium, 3 high (Vulkan host) */
    int arcade_overlays;        /* the arcade's credit and card messages shown (games that can hide them) */
    int replays;                /* the game's replays of a point played (games that can skip them); Classic mode's */
    /* Classic / Immersive (a game with a world mode, Virtua Tennis 3: world_mode.c).
     * Appended: LG_XR_SETTINGS_DEFAULT is positional. Classic keeps replays and
     * arcade_overlays above; its own and Immersive's settings follow. */
    int world_mode;             /* vr.cfg "mode classic|immersive": 0 / 1, applies on the next start */
    int classic_shadow;         /* "classic.shadow": shadow map size 1024 (the game's) / 2048 / 4096 */
    int classic_cutaways;       /* "classic.cutaways": the close-up of the winner after each point */
    int imm_replays, imm_overlays, imm_cutaways;   /* "immersive.replays" / ".overlays" / ".cutaways";
                                 * replays and cutaways LG_VIEW_ (off|flat|world|camera), overlays on/off */
    int imm_dolly, imm_back;    /* "immersive.dolly" / ".back": the viewpoint forward / back, game units (0.1 m) */
    int imm_sky;                /* "immersive.sky stadium|painted|off" (old "on" = stadium): LG_SKY_ */
    int imm_shadow;             /* "immersive.shadow" */
    int imm_height;             /* "immersive.height": the viewpoint raised (+) / lowered (-) from the
                                 * camera's own, game units (0.1 m): -20 / -10 / 0 / 10 / 20 */
} LgXrSettings;
/* Each eye's image by default: the headset's own panel, pixel for pixel.
 * OpenXR reports only a runtime-scaled recommendation, so the panel is the
 * build's: Quest 3 (Android) 2064x2208 per eye, Steam Frame (native Linux)
 * 2160x2160 per eye. Other hosts keep the earlier 2064x2280. */
#if defined(__ANDROID__)
#define LG_XR_NATIVE_WIDTH  2064
#define LG_XR_NATIVE_HEIGHT 2208
#elif defined(__linux__)
#define LG_XR_NATIVE_WIDTH  2160
#define LG_XR_NATIVE_HEIGHT 2160
#else
#define LG_XR_NATIVE_WIDTH  2064
#define LG_XR_NATIVE_HEIGHT 2280
#endif
/* ... dual off, stats panel on (vr.cfg "stats off" hides it), medium foveation, arcade overlays shown, replays on;
 * Classic mode, shadows 1024, cutaways on; Immersive: replays, overlays and cutaways off,
 * 200 units closer, 60 back (5 / 6 / 7 / 8 m), the stadium's own sky, shadows 2048, the
 * camera's own height. (The stands' stretch,
 * "immersive.stand_y", is gone: an old file's key is ignored and not written back.) */
#define LG_XR_SETTINGS_DEFAULT {1.0f,3.0f,15.0f,LG_XR_NATIVE_HEIGHT,LG_XR_NATIVE_WIDTH,120,0,LASER_ON,1,0,1,1,0,0,1,3,1,1, \
                                0,1024,1,0,0,0,200,60,LG_SKY_STADIUM,2048,0}
/* Immersive's Step back (immersive.back) and Camera height (immersive.height)
 * choices, game units (0.1 m); a stored value off the list is read as the
 * nearest (xr_settings_nearest; the first of two equally near) */
#define LG_XR_BACK_NSTEPS 4
#define LG_XR_HEIGHT_NSTEPS 5
static const int LG_XR_BACK_STEPS[LG_XR_BACK_NSTEPS] = { 50, 60, 70, 80 };
static const int LG_XR_HEIGHT_STEPS[LG_XR_HEIGHT_NSTEPS] = { -20, -10, 0, 10, 20 };
static inline int xr_settings_nearest(int v, const int *steps, int n)
{
    int best = steps[0];
    for (int i = 1; i < n; i++) if ((v > steps[i] ? v - steps[i] : steps[i] - v) < (v > best ? v - best : best - v)) best = steps[i];
    return best;
}
void xr_settings_load(LgXrSettings *settings);
void xr_settings_set(LgXrSettings *settings,const char *key,const char *value);
void xr_settings_save(const LgXrSettings *settings);
#ifdef __cplusplus
}
#endif
