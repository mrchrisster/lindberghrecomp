/*
 * world_mode.h - Classic (the game on a 2D screen) or Immersive (Virtua
 * Tennis 3's match drawn as a world around the head), and the settings that
 * go with each mode. Every value is resolved env > vr.cfg > the mode's
 * default (world_mode.c). The mode is fixed for the run for now: hooks are
 * installed for it at start (hle_sega.c vt3_jumps); a changed mode applies on
 * the next start. The groundwork for switching it live (Phase B) is here but
 * off: three latches - the target (panel / test switch), the game thread's
 * mode (world_mode_immersive, latched by world_mode_frame) and the GL
 * thread's (world_mode_drawing_immersive) - and the switch only happens once
 * the game registers a switch callback (world_mode_on_switch), which nothing
 * does yet. Compiled in every build; the desktop reads env only.
 */
#pragma once
#include "xr_settings.h"
#ifdef __cplusplus
extern "C" {
#endif

/* the values, for their env variables, sources and pins */
enum { WM_MODE, WM_REPLAYS, WM_OVERLAYS, WM_CUTAWAYS, WM_SHADOW, WM_DOLLY, WM_BACK, WM_SKY, WM_HEIGHT,
       WM_CARD, WM_LANGUAGE, WM_MSAA, WM_SAVE, WM_COUNT };
enum { WM_SRC_DEFAULT, WM_SRC_FILE, WM_SRC_ENV };

typedef struct {
    int immersive;              /* the running mode: 0 Classic, 1 Immersive */
    int replays, overlays, cutaways;
    /* how a replay / point cutaway is shown while on: LG_VIEW_FLAT (the flat
     * picture; always in Classic), _WORLD (in the world from where you are),
     * _CAMERA (from the game camera's place, 1:1) - Immersive's
     * immersive.replays / .cutaways, LINDBERGH_VT3_REPLAY_VIEW / _CUTAWAY_VIEW */
    int replay_view, cutaway_view;
    int sky;                    /* LG_SKY_OFF / _STADIUM (the stadium's own dome) / _PAINTED (stereo.c's) */
    int shadow;                 /* shadow map size: 1024 (the game's own), 2048, 4096 */
    float shadow_scale;         /* what stereo.c scales shadow maps by: 0 = as the game makes them */
    float dolly, back;          /* viewpoint forward / back in game units (immersive) */
    float height;               /* viewpoint raised (+) / lowered (-) in game units (immersive) */
    int card_notice;            /* the card reader's notice and boot wait (Classic's classic.card_notice;
                                 * VR: 0); LINDBERGH_VT3_CARD_RW=1|0; at registration, so on restart */
    int language;               /* 1 English, 0 Japanese (vr.cfg language, LINDBERGH_VT3_LANGUAGE); on restart */
    int msaa;                   /* anti-aliasing samples asked for: 0, 2, 4 (vr.cfg msaa, LINDBERGH_MSAA); on restart */
    int save;                   /* Save progress: 1P's card record kept in vt3_save.bin (vr.cfg save, LINDBERGH_VT3_SAVE); on restart */
} WmValues;

/* the env variable that pins a value for a run (LINDBERGH_REPLAYS, ...) */
const char *world_mode_env_name(int key);
/* whether that variable is set (non-empty): the menu shows "(env.txt)" -
 * for replays / cutaways also their view's (LINDBERGH_VT3_REPLAY_VIEW, _CUTAWAY_VIEW) */
int world_mode_pinned(int key);
/* Resolves every value: env > stored (vr.cfg, or the defaults) for the mode.
 * mode < 0: the mode is resolved too (env, then stored->mode; only when
 * capable - a game that cannot draw a world is always Classic); else fixed.
 * present: WM_ bits of values read from vr.cfg (for the source names). */
void world_mode_resolve(const LgXrSettings *stored, int mode, int capable, unsigned present, WmValues *out, int src[WM_COUNT]);
/* The vr.cfg key holding a value in a mode ("replays", "immersive.replays"...) */
const char *world_mode_key(int key, int immersive);

/* Once, before any hook is chosen (hle_sega.c vt3_jumps): resolves from env and,
 * on headset builds, vr.cfg (cwd, else ../vr.cfg), and logs one [world] line.
 * capable: the game can draw its match as a world (Virtua Tennis 3). A getter
 * called before it resolves from env alone, as an incapable game. */
void world_mode_init(int capable);
/* The menu changed a stored setting: the live values again (replays, overlays,
 * cutaways, dolly, back, height, sky) for the running mode; the shadow size
 * stays as started. The mode stays as started too unless switching is live
 * (world_mode_live): then the stored mode (an env-pinned one wins; a game
 * that cannot draw a world keeps its start) becomes the target, which the
 * next world_mode_frame latches. */
void world_mode_update(const LgXrSettings *stored);
const WmValues *world_mode_values(void);
int world_mode_capable(void);
/* the game thread's mode: as started until world_mode_frame latches a new target */
int world_mode_immersive(void);
/* the mode asked for (panel or test switch), not yet latched maybe */
int world_mode_target(void);
/* the GL thread's mode, latched by world_mode_gl_latch (stereo_frame_begin,
 * a later step; until then the start mode) */
int world_mode_drawing_immersive(void);
void world_mode_gl_latch(int immersive);

/* Live switching: on when a switch callback is registered and
 * LINDBERGH_VT3_LIVE_SWITCH is not 0 (the kill switch: the Mode row then
 * applies on restart as before). No callback is registered yet. */
int world_mode_live(void);
/* The game's part of a switch, called on the game thread by world_mode_frame
 * right after the mode is latched (hle_sega.c, a later step: host_sdl.c is
 * also built for the Vulkan host, which has no stereo.c). NULL unregisters. */
typedef void (*WmSwitchFn)(int immersive);
void world_mode_on_switch(WmSwitchFn fn);
/* Once per frame on the game thread, before the frame's GL job is queued
 * (host_sdl.c host_swap): latches the target as the game's mode, logs
 * "[world] frame N: mode -> immersive (panel)" and calls the switch callback.
 * Also runs LINDBERGH_VT3_MODE_AT=<frame>:<0|1>,... (a test: sets the target
 * at those frames, over an env pin too, never saved; only while a switch
 * callback is registered). Returns the game's mode for the next frame. */
int world_mode_frame(unsigned frame);
int world_mode_replays(void);
int world_mode_overlays(void);
int world_mode_cutaways(void);
int world_mode_card_notice(void);
int world_mode_language(void);
int world_mode_msaa(void);
int world_mode_save(void);
/* LG_VIEW_FLAT / _WORLD / _CAMERA (live) */
int world_mode_replay_view(void);
int world_mode_cutaway_view(void);
/* parses LINDBERGH_VT3_REPLAY_VIEW / _CUTAWAY_VIEW: flat|world|follow|camera
 * (follow = camera); -1 for anything else */
int world_mode_view_value(const char *v);
/* Immersive's sky: LG_SKY_OFF, LG_SKY_STADIUM, LG_SKY_PAINTED (LINDBERGH_VT3_SKY
 * 0|off, 1|on|stadium, 2|painted; vr.cfg immersive.sky) */
int world_mode_sky(void);
/* parses an env / vr.cfg sky value (as above) */
int world_mode_sky_value(const char *v);
float world_mode_shadow_scale(void);
float world_mode_dolly(void);
float world_mode_back(void);
float world_mode_height(void);

#ifdef __cplusplus
}
#endif
