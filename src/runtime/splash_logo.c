/* splash_logo.c - see splash_logo.h */
#include "splash_logo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void c565(unsigned v, int c[3])
{
    c[0] = (int)((v >> 11 & 31) * 255 + 15) / 31;
    c[1] = (int)((v >> 5 & 63) * 255 + 31) / 63;
    c[2] = (int)((v & 31) * 255 + 15) / 31;
}

int lg_dxt_decode(const uint8_t *src, size_t n, int w, int h, int dxt5, uint8_t *rgba)
{
    const size_t bs = dxt5 ? 16 : 8;
    if (w <= 0 || h <= 0 || n < (size_t)((w + 3) / 4) * (size_t)((h + 3) / 4) * bs) return -1;
    for (int by = 0; by < h; by += 4)
        for (int bx = 0; bx < w; bx += 4, src += bs) {
            int alpha[16];
            const uint8_t *cb = src;
            if (dxt5) {
                int a[8]; a[0] = src[0]; a[1] = src[1];
                if (a[0] > a[1]) for (int i = 0; i < 6; i++) a[2 + i] = ((6 - i) * a[0] + (1 + i) * a[1]) / 7;
                else { for (int i = 0; i < 4; i++) a[2 + i] = ((4 - i) * a[0] + (1 + i) * a[1]) / 5; a[6] = 0; a[7] = 255; }
                uint64_t bits = 0;
                for (int i = 0; i < 6; i++) bits |= (uint64_t)src[2 + i] << (8 * i);
                for (int i = 0; i < 16; i++) alpha[i] = a[bits >> (3 * i) & 7];
                cb = src + 8;
            }
            const unsigned c0 = (unsigned)cb[0] | (unsigned)cb[1] << 8, c1 = (unsigned)cb[2] | (unsigned)cb[3] << 8;
            const uint32_t idx = (uint32_t)cb[4] | (uint32_t)cb[5] << 8 | (uint32_t)cb[6] << 16 | (uint32_t)cb[7] << 24;
            int p[4][4];
            c565(c0, p[0]); c565(c1, p[1]); p[0][3] = p[1][3] = 255;
            if (c0 > c1 || dxt5) {
                for (int k = 0; k < 3; k++) { p[2][k] = (2 * p[0][k] + p[1][k]) / 3; p[3][k] = (p[0][k] + 2 * p[1][k]) / 3; }
                p[2][3] = p[3][3] = 255;
            } else {
                for (int k = 0; k < 3; k++) { p[2][k] = (p[0][k] + p[1][k]) / 2; p[3][k] = 0; }
                p[2][3] = 255; p[3][3] = 0;
            }
            for (int i = 0; i < 16; i++) {
                const int x = bx + i % 4, y = by + i / 4;
                if (x >= w || y >= h) continue;
                const int *c = p[idx >> (2 * i) & 3];
                uint8_t *o = rgba + ((size_t)y * w + x) * 4;
                o[0] = (uint8_t)c[0]; o[1] = (uint8_t)c[1]; o[2] = (uint8_t)c[2];
                o[3] = (uint8_t)(dxt5 ? alpha[i] : c[3]);
            }
        }
    return 0;
}

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

uint8_t *lg_vt3_title_logo(const char *path, int *w, int *h, char why[64])
{
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(why, 64, "no %s", path); return NULL; }
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = n > 0 && n < (64L << 20) ? malloc((size_t)n) : NULL;
    if (!d || fread(d, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(d); snprintf(why, 64, "unreadable"); return NULL; }
    fclose(f);
    if (n < 4 || memcmp(d, "ABRS", 4)) { free(d); snprintf(why, 64, "not an ABRS sprite file"); return NULL; }
    /* the two DXT5 textures, by size: the right part 512x512, the left 512x256 */
    const uint8_t *right = NULL, *left = NULL;
    for (long i = 4; i + 128 <= n; i++) {
        if (memcmp(d + i, "DDS ", 4)) continue;
        const uint32_t th = le32(d + i + 12), tw = le32(d + i + 16);
        if (memcmp(d + i + 84, "DXT5", 4)) continue;
        if (tw == 512 && th == 512 && !right) right = d + i + 128;
        else if (tw == 512 && th == 256 && !left) left = d + i + 128;
    }
    const uint8_t *end = d + n;
    if (!right || !left || end - right < 512 * 512 || end - left < 512 * 256) {
        free(d); snprintf(why, 64, "the logo's two textures are not there"); return NULL;
    }
    const int W = 1024, H = 512;
    uint8_t *pic = calloc((size_t)W * H, 4), *t = malloc(512 * 512 * 4);
    if (!pic || !t) { free(d); free(pic); free(t); snprintf(why, 64, "out of memory"); return NULL; }
    lg_dxt_decode(right, (size_t)(end - right), 512, 512, 1, t);
    for (int y = 0; y < 512; y++) memcpy(pic + ((size_t)y * W + 512) * 4, t + (size_t)y * 512 * 4, 512 * 4);
    lg_dxt_decode(left, (size_t)(end - left), 512, 256, 1, t);
    for (int y = 0; y < 256; y++) memcpy(pic + ((size_t)(y + 128) * W) * 4, t + (size_t)y * 512 * 4, 512 * 4);
    free(t); free(d);
    /* crop to what is drawn */
    int x0 = W, y0 = H, x1 = -1, y1 = -1;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            if (pic[((size_t)y * W + x) * 4 + 3] > 8) {
                if (x < x0) x0 = x;
                if (x > x1) x1 = x;
                if (y < y0) y0 = y;
                if (y > y1) y1 = y;
            }
    if (x1 < x0 || (x1 - x0) < 64 || (y1 - y0) < 32) { free(pic); snprintf(why, 64, "the logo decodes empty"); return NULL; }
    const int cw = x1 - x0 + 1, ch = y1 - y0 + 1;
    for (int y = 0; y < ch; y++) memmove(pic + (size_t)y * cw * 4, pic + ((size_t)(y + y0) * W + x0) * 4, (size_t)cw * 4);
    *w = cw; *h = ch;
    return pic;
}

void lg_blit_over(uint8_t *dst, int dw, int dh, const uint8_t *src, int sw, int sh, int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0 || sw <= 0 || sh <= 0) return;
    for (int j = 0; j < h; j++) {
        const int ty = y + j;
        if (ty < 0 || ty >= dh) continue;
        int sy0 = (int)((long)j * sh / h), sy1 = (int)((long)(j + 1) * sh / h);
        if (sy1 <= sy0) sy1 = sy0 + 1;
        for (int i = 0; i < w; i++) {
            const int tx = x + i;
            if (tx < 0 || tx >= dw) continue;
            int sx0 = (int)((long)i * sw / w), sx1 = (int)((long)(i + 1) * sw / w);
            if (sx1 <= sx0) sx1 = sx0 + 1;
            /* the box's average, premultiplied */
            unsigned long r = 0, g = 0, b = 0, a = 0, cnt = 0;
            for (int v = sy0; v < sy1 && v < sh; v++)
                for (int u = sx0; u < sx1 && u < sw; u++) {
                    const uint8_t *s = src + ((size_t)v * sw + u) * 4;
                    r += (unsigned long)s[0] * s[3]; g += (unsigned long)s[1] * s[3]; b += (unsigned long)s[2] * s[3];
                    a += s[3]; cnt++;
                }
            if (!cnt || !a) continue;
            uint8_t *o = dst + ((size_t)ty * dw + tx) * 4;
            const unsigned long sa = a / cnt;                   /* 0..255 */
            /* premultiplied source over: out = src*a + dst*(1-a) */
            o[0] = (uint8_t)((r / cnt + (unsigned long)o[0] * (255 - sa)) / 255);
            o[1] = (uint8_t)((g / cnt + (unsigned long)o[1] * (255 - sa)) / 255);
            o[2] = (uint8_t)((b / cnt + (unsigned long)o[2] * (255 - sa)) / 255);
            o[3] = (uint8_t)(sa + (unsigned long)o[3] * (255 - sa) / 255);
        }
    }
}

/* a part of the atlas, copied out (blit sources must not overlap their target) */
static uint8_t *atlas_part(const uint8_t *px, int aw, int ah, const int r[4])
{
    if (r[0] < 0 || r[1] < 0 || r[2] <= 0 || r[3] <= 0 || r[0] + r[2] > aw || r[1] + r[3] > ah) return NULL;
    uint8_t *p = malloc((size_t)r[2] * r[3] * 4);
    if (p) for (int y = 0; y < r[3]; y++) memcpy(p + (size_t)y * r[2] * 4, px + ((size_t)(r[1] + y) * aw + r[0]) * 4, (size_t)r[2] * 4);
    return p;
}

int lg_splash_compose(uint8_t *px, int aw, int ah, const LgSplashPlaces *pl)
{
    if (!px || !pl || !pl->logo_at || !pl->vr || !pl->vr_at || !pl->title || !pl->title_at || !pl->file) return -1;
    const char *e = getenv("LINDBERGH_SPLASH_LOGO");
    char why[64] = "";
    int lw = 0, lh = 0;
    uint8_t *logo = NULL;
    if (e && *e == '0') snprintf(why, sizeof why, "LINDBERGH_SPLASH_LOGO=0");
    else logo = lg_vt3_title_logo(pl->file, &lw, &lh, why);
    if (logo) {
        const int *at = pl->logo_at;
        int fw = at[2], fh = (int)((long)at[2] * lh / lw);
        if (fh > at[3]) { fh = at[3]; fw = (int)((long)at[3] * lw / lh); }
        const int fx = at[0] + (at[2] - fw) / 2, fy = at[1] + (at[3] - fh) / 2;
        uint8_t *vr = atlas_part(px, aw, ah, pl->vr);
        lg_blit_over(px, aw, ah, logo, lw, lh, fx, fy, fw, fh);
        if (vr) lg_blit_over(px, aw, ah, vr, pl->vr[2], pl->vr[3], fx + fw + pl->vr_at[0], fy + fh + pl->vr_at[1] - pl->vr[3],
                             pl->vr[2], pl->vr[3]);
        free(vr); free(logo);
        fprintf(stderr, "[xr] splash: the game's title logo from %s (%dx%d, shown %dx%d) with VR\n", pl->file, lw, lh, fw, fh);
        return 1;
    }
    uint8_t *t = atlas_part(px, aw, ah, pl->title);
    if (t) lg_blit_over(px, aw, ah, t, pl->title[2], pl->title[3], pl->title_at[0], pl->title_at[1], pl->title[2], pl->title[3]);
    free(t);
    fprintf(stderr, "[xr] splash: our own title (the game's logo: %s)\n", why);
    return 0;
}
