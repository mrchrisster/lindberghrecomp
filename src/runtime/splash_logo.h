/* splash_logo.h - the game's own title logo on the headset's start-up screen,
 * read at start-up from the player's game files (never shipped).
 *
 * The port ships no game art (owner's release rule, 2026-11-12): the start-up
 * picture (games/<game>/src/splash_art.c, quest/tools/<game>_splash.py) is
 * our own drawing, with a free place for the title logo. At start-up the
 * logo is read from the player's copy of the game, decoded, and composited
 * into that place - with our "VR" badge beside it. When the file is missing
 * or not what is expected, our own text title is put there instead, so the
 * screen never breaks.
 *
 * Virtua Tennis 3's English title logo: resource/sprite/I_ADV_titlelogo_us_KB.abr
 * (AB resource 0x18, usrAdvTitleLoop::init; Japan's 0x16 is ..._jp_KB.abr):
 * an "ABRS" sprite file holding two DXT5 DDS textures - 512x512 (the right
 * part: "3", "Tennis") and 512x256 (the left: the swoosh, "Vi") - laid out by
 * its two quads as one 1024x512 picture: the 512x256 at x 0..512, y 128..384,
 * the 512x512 at x 512..1024 (top down). Plain C, no GL: the same code runs
 * in host_xr.c and in the macOS preview test (tests/test_splash_logo.py). */
#ifndef LINDBERGH_SPLASH_LOGO_H
#define LINDBERGH_SPLASH_LOGO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* DXT1 (dxt5 = 0) / DXT5 blocks, w x h (multiples of 4), into RGBA (w*h*4,
 * straight alpha). Returns 0, or -1 when n is short. */
int lg_dxt_decode(const uint8_t *src, size_t n, int w, int h, int dxt5, uint8_t *rgba);

/* Virtua Tennis 3's title logo from an .abr file: RGBA, cropped to its drawn
 * part (alpha), *w x *h; malloc'd, or NULL with the reason in why (64). */
uint8_t *lg_vt3_title_logo(const char *path, int *w, int *h, char why[64]);

/* src (sw x sh RGBA, straight alpha) scaled into dst's rectangle x, y, w, h
 * (dst dw x dh RGBA), blended over what is there (area-averaged, so a large
 * logo shrinks without shimmer) */
void lg_blit_over(uint8_t *dst, int dw, int dh, const uint8_t *src, int sw, int sh,
                  int x, int y, int w, int h);

/* The start-up picture's places (splash_art.c, all in atlas pixels, top down):
 * logo_at  where the logo goes (x, y, w, h; fitted, aspect kept); the group of
 *          the logo and the VR badge - their drawn pixels, measured - is centred
 *          on the place's centre line, and so is the fallback title
 * vr       our "VR" badge in the atlas (x, y, w, h); vr_at (dx, dy): its left
 *          edge dx right of the fitted logo's right edge, its bottom dy below
 *          the logo's bottom (negative: up) - it follows the logo's real size
 * title    our text title in the atlas (x, y, w, h), title_at where it goes
 *          (its y; x only when the title has no drawn pixels)
 * file     the logo's file, relative to the game's folder */
typedef struct {
    const int *logo_at, *vr, *vr_at, *title, *title_at;
    const char *file;
} LgSplashPlaces;

/* Composites the panel of the unpacked atlas px (aw x ah): the logo + VR, or
 * the fallback title. LINDBERGH_SPLASH_LOGO=0 forces the fallback (test).
 * Returns 1 with the logo, 0 with the fallback, -1 when places is incomplete
 * (nothing done). Logs one "[xr] splash: ..." line. */
int lg_splash_compose(uint8_t *px, int aw, int ah, const LgSplashPlaces *places);

#ifdef __cplusplus
}
#endif
#endif
