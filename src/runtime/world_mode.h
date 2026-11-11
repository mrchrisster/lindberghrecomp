/*
 * world_mode.h - Classic (the game on a 2D screen) or Immersive (Virtua
 * Tennis 3's match drawn as a world around the head), and the settings that
 * go with each mode. Every value is resolved env > vr.cfg > the mode's
 * default (world_mode.c). The mode itself is fixed for the run: hooks are
 * installed for it at start (hle_sega.c vt3_jumps); a changed mode applies on
 * the next start. Compiled in every build; the desktop reads env only.
 */
#pragma once
#include "xr_settings.h"
#ifdef __cplusplus
extern "C" {
#endif

/* the values, for their env variables, sources and pins */
enum { WM_MODE, WM_REPLAYS, WM_OVERLAYS, WM_CUTAWAYS, WM_SHADOW, WM_STAND, WM_DOLLY, WM_BACK, WM_SKY, WM_COUNT };
enum { WM_SRC_DEFAULT, WM_SRC_FILE, WM_SRC_ENV };

typedef struct {
    int immersive;              /* the running mode: 0 Classic, 1 Immersive */
    int replays, overlays, cutaways, sky;
    int shadow;                 /* shadow map size: 1024 (the game's own), 2048, 4096 */
    float shadow_scale;         /* what stereo.c scales shadow maps by: 0 = as the game makes them */
    float stand_y, dolly, back; /* stand stretch, viewpoint forward / back in game units (immersive) */
} WmValues;

/* the env variable that pins a value for a run (LINDBERGH_REPLAYS, ...) */
const char *world_mode_env_name(int key);
/* whether that variable is set (non-empty): the menu shows "(env.txt)" */
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
 * cutaways, stand, dolly, back, sky) for the running mode; the mode and the
 * shadow size stay as started. */
void world_mode_update(const LgXrSettings *stored);
const WmValues *world_mode_values(void);
int world_mode_capable(void);
int world_mode_immersive(void);
int world_mode_replays(void);
int world_mode_overlays(void);
int world_mode_cutaways(void);
int world_mode_sky(void);
float world_mode_shadow_scale(void);
float world_mode_stand_y(void);
float world_mode_dolly(void);
float world_mode_back(void);

#ifdef __cplusplus
}
#endif
