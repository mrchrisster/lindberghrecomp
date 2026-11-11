#pragma once
#ifdef __cplusplus
extern "C" {
#endif
enum { LASER_ON, LASER_DOT, LASER_OFF };
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
    int imm_replays, imm_overlays, imm_cutaways;   /* "immersive.replays" / ".overlays" / ".cutaways" */
    float imm_stand_y;          /* "immersive.stand_y": the stands stretched up (1 = off) */
    int imm_dolly, imm_back;    /* "immersive.dolly" / ".back": the viewpoint forward / back, game units (0.1 m) */
    int imm_sky;                /* "immersive.sky" */
    int imm_shadow;             /* "immersive.shadow" */
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
 * Classic mode, shadows 1024, cutaways on; Immersive: replays, overlays and cutaways off, stands x1.5,
 * 200 units closer, 60 back, sky on, shadows 2048 */
#define LG_XR_SETTINGS_DEFAULT {1.0f,3.0f,15.0f,LG_XR_NATIVE_HEIGHT,LG_XR_NATIVE_WIDTH,120,0,LASER_ON,1,0,1,1,0,0,1,3,1,1, \
                                0,1024,1,0,0,0,1.5f,200,60,1,2048}
void xr_settings_load(LgXrSettings *settings);
void xr_settings_set(LgXrSettings *settings,const char *key,const char *value);
void xr_settings_save(const LgXrSettings *settings);
#ifdef __cplusplus
}
#endif
