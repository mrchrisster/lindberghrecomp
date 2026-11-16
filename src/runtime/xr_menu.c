/*
 * xr_menu.c - the settings panel's picture: text in rows, painted into an
 * RGBA image the overlay shows on a quad in front of the player. Painted only
 * when something on it changes.
 */
#include <string.h>
#include <math.h>

#include "xr_font.h"
#include "xr_menu.h"

#define MARGIN 28
#define TITLE_H 64
#define ROW_H 36

static void fill(unsigned char *img, int x0, int y0, int x1, int y1, const unsigned char c[4])
{
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) memcpy(img + ((size_t)y * XM_W + x) * 4, c, 4);
}

static void text(unsigned char *img, int x, int y, const char *s, const unsigned char c[3])
{
    for (; *s && x + FONT_W <= XM_W; s++, x += FONT_W - 3) {
        int ch = (unsigned char)*s - FONT_FIRST;
        if (ch < 0 || ch >= FONT_COUNT) continue;
        for (int j = 0; j < FONT_H && y + j < XM_H; j++)
            for (int i = 0; i < FONT_W; i++) {
                unsigned a = FONT[ch][j * FONT_W + i];
                if (!a) continue;
                unsigned char *p = img + ((size_t)(y + j) * XM_W + x + i) * 4;
                for (int k = 0; k < 3; k++) p[k] = (unsigned char)((p[k] * (255 - a) + c[k] * a) / 255);
            }
    }
}

static int text_w(const char *s) { return (int)strlen(s) * (FONT_W - 3); }

void xm_paint_label(unsigned char *img, const char *s)
{
    const unsigned char bg[4] = { 16, 16, 20, 190 }, fg[3] = { 140, 255, 140 };
    for (int i = 0; i < XM_LABEL_W * XM_LABEL_H; i++) memcpy(img + (size_t)i * 4, bg, 4);
    int x = (XM_LABEL_W - text_w(s)) / 2, y = (XM_LABEL_H - FONT_H) / 2;
    for (; *s && x + FONT_W <= XM_LABEL_W; s++, x += FONT_W - 3) {
        int ch = (unsigned char)*s - FONT_FIRST;
        if (ch < 0 || ch >= FONT_COUNT) continue;
        for (int j = 0; j < FONT_H && y + j < XM_LABEL_H; j++)
            for (int i = 0; i < FONT_W; i++) {
                unsigned a = FONT[ch][j * FONT_W + i];
                if (!a) continue;
                unsigned char *p = img + ((size_t)(y + j) * XM_LABEL_W + x + i) * 4;
                for (int k = 0; k < 3; k++) p[k] = (unsigned char)((p[k] * (255 - a) + fg[k] * a) / 255);
                if (p[3] < a) p[3] = (unsigned char)a;
            }
    }
}

void xm_paint_lines(unsigned char *img, const char *const *lines, int n)
{
    const unsigned char bg[4] = { 16, 16, 20, 190 }, fg[3] = { 140, 255, 140 };
    const int pad = 12, line_h = 36;
    for (int i = 0; i < XM_STATS_W * XM_STATS_H; i++) memcpy(img + (size_t)i * 4, bg, 4);
    for (int l = 0; l < n; l++) {
        const int y = pad + l * line_h;
        int x = pad;
        for (const char *s = lines[l]; *s && x + FONT_W <= XM_STATS_W - pad / 2; s++, x += FONT_W - 3) {
            int ch = (unsigned char)*s - FONT_FIRST;
            if (ch < 0 || ch >= FONT_COUNT) continue;
            for (int j = 0; j < FONT_H && y + j < XM_STATS_H; j++)
                for (int i = 0; i < FONT_W; i++) {
                    unsigned a = FONT[ch][j * FONT_W + i];
                    if (!a) continue;
                    unsigned char *p = img + ((size_t)(y + j) * XM_STATS_W + x + i) * 4;
                    for (int k = 0; k < 3; k++) p[k] = (unsigned char)((p[k] * (255 - a) + fg[k] * a) / 255);
                    if (p[3] < a) p[3] = (unsigned char)a;
                }
        }
    }
}

void xm_paint_message(unsigned char *img, const char *title, const char *line)
{
    static const unsigned char clear[4] = { 0, 0, 0, 0 }, bg[4] = { 18, 22, 28, 235 }, edge[4] = { 70, 80, 90, 255 };
    static const unsigned char white[3] = { 235, 238, 240 }, green[3] = { 120, 230, 140 };
    const int top = (XM_H - XM_MESSAGE_H) / 2, bottom = top + XM_MESSAGE_H;
    fill(img, 0, 0, XM_W, XM_H, clear);
    fill(img, 0, top, XM_W, bottom, bg);
    fill(img, 0, top, XM_W, top + 3, edge);
    fill(img, 0, bottom - 3, XM_W, bottom, edge);
    text(img, (XM_W - text_w(title)) / 2, top + 36, title, white);
    text(img, (XM_W - text_w(line)) / 2, top + 36 + FONT_H + 28, line, green);
}

void xm_paint(unsigned char *img, const char *title, const XmItem *items, int n, int hover, const char *hint)
{
    static const unsigned char bg[4] = { 18, 22, 28, 235 }, bar[4] = { 40, 70, 52, 255 },
                               hi[4] = { 60, 110, 80, 255 }, line[4] = { 70, 80, 90, 255 };
    static const unsigned char white[3] = { 235, 238, 240 }, green[3] = { 120, 230, 140 },
                               grey[3] = { 150, 158, 166 }, gold[3] = { 250, 200, 90 };
    fill(img, 0, 0, XM_W, XM_H, bg);
    fill(img, 0, 0, XM_W, TITLE_H, bar);
    text(img, MARGIN, (TITLE_H - FONT_H) / 2, title, white);
    for (int i = 0; i < n && i < XM_ROWS; i++) {
        int y = TITLE_H + 8 + i * ROW_H;
        if (i == hover) fill(img, 8, y, XM_W - 8, y + ROW_H - 2, hi);
        else fill(img, MARGIN, y + ROW_H - 2, XM_W - MARGIN, y + ROW_H - 1, line);
        int action = !items[i].value[0];
        text(img, MARGIN, y + 1, items[i].label, action ? gold : white);
        if (!action) text(img, XM_W - MARGIN - text_w(items[i].value), y + 1, items[i].value, green);
    }
    if (hint) text(img, MARGIN, XM_H - FONT_H - 10, hint, grey);
}

int xm_row_at(float u, float v, int n)
{
    if (u < 0 || u > 1 || v < 0 || v > 1) return -1;
    int y = (int)(v * XM_H) - TITLE_H - 8;
    if (y < 0) return -1;
    int r = y / ROW_H;
    return r < n && r < XM_ROWS ? r : -1;
}

/* the Classic / VR buttons (xr_menu.h) */
#define BAR_TOP (TITLE_H + 10)
#define BTN_H 88
#define BTN_GAP 20
static void frame(unsigned char *img, int x0, int y0, int x1, int y1, int t, const unsigned char c[4])
{
    fill(img, x0, y0, x1, y0 + t, c); fill(img, x0, y1 - t, x1, y1, c);
    fill(img, x0, y0, x0 + t, y1, c); fill(img, x1 - t, y0, x1, y1, c);
}
static int g_wide;
void xm_modes_wide(int on) { g_wide = on != 0; }
#define WIDE_TOP (TITLE_H + XM_BAR_H + 4)
#define WIDE_BH (XM_WIDE_H - 12)
/* ---- Virtua Tennis 3's look (2026-11-16): the arcade's red banners with a
 * glossy top, white italic capitals, charcoal score boxes with a red stripe,
 * yellow labels ("GAME") ---- */
static void grad(unsigned char *img, int x0, int y0, int x1, int y1, const unsigned char a[3], const unsigned char b[3], int alpha)
{
    const int h = y1 - y0 > 1 ? y1 - y0 - 1 : 1;
    for (int y = y0; y < y1; y++) {
        const int t = y - y0;
        unsigned char c[4] = { (unsigned char)(a[0] + (b[0] - a[0]) * t / h), (unsigned char)(a[1] + (b[1] - a[1]) * t / h),
                               (unsigned char)(a[2] + (b[2] - a[2]) * t / h), (unsigned char)alpha };
        for (int x = x0; x < x1; x++) memcpy(img + ((size_t)y * XM_W + x) * 4, c, 4);
    }
}
/* a banner: a parallelogram leaning right (the arcade's slanted ends), a
 * gradient a -> b and a lighter gloss on its upper third */
static void banner(unsigned char *img, int x0, int y0, int x1, int y1, int lean, const unsigned char a[3], const unsigned char b[3])
{
    const int h = y1 - y0 > 1 ? y1 - y0 - 1 : 1;
    for (int y = y0; y < y1; y++) {
        const int t = y - y0, sh = lean * (h - t) / h;       /* the top further right */
        unsigned char c[4] = { (unsigned char)(a[0] + (b[0] - a[0]) * t / h), (unsigned char)(a[1] + (b[1] - a[1]) * t / h),
                               (unsigned char)(a[2] + (b[2] - a[2]) * t / h), 255 };
        if (t < h / 3) for (int k = 0; k < 3; k++) c[k] = (unsigned char)(c[k] + (255 - c[k]) * (h / 3 - t) / (h / 3) / 4);   /* gloss */
        int xa = x0 + sh, xb = x1 - lean + sh;
        if (xa < 0) xa = 0;
        if (xb > XM_W) xb = XM_W;
        for (int x = xa; x < xb; x++) memcpy(img + ((size_t)y * XM_W + x) * 4, c, 4);
    }
}
/* text at n times the font's size, leaning like the arcade's italic captions */
static void texti(unsigned char *img, int x, int y, const char *s, const unsigned char c[3], int n, int italic)
{
    for (; *s && x + n * FONT_W <= XM_W; s++, x += n * (FONT_W - 3)) {
        int ch = (unsigned char)*s - FONT_FIRST;
        if (ch < 0 || ch >= FONT_COUNT) continue;
        for (int j = 0; j < n * FONT_H && y + j < XM_H; j++) {
            const int dx = italic ? (n * FONT_H - j) / 5 : 0;
            for (int i = 0; i < n * FONT_W; i++) {
                unsigned a = FONT[ch][(j / n) * FONT_W + i / n];
                if (!a || x + i + dx >= XM_W) continue;
                unsigned char *p = img + ((size_t)(y + j) * XM_W + x + i + dx) * 4;
                for (int k = 0; k < 3; k++) p[k] = (unsigned char)((p[k] * (255 - a) + c[k] * a) / 255);
            }
        }
    }
}
static void upper(char *o, size_t cap, const char *s)
{
    size_t k = 0;
    for (; *s && k < cap - 1; s++) o[k++] = (char)(*s >= 'a' && *s <= 'z' ? *s - 32 : *s);
    o[k] = 0;
}
void xm_paint_modes(unsigned char *img, const char *title, const XmItem *items, int n, int hover, const char *note, const char *hint)
{
    static const unsigned char bg_top[3] = { 30, 32, 38 }, bg_bot[3] = { 12, 13, 16 },
                               red_a[3] = { 236, 58, 34 }, red_b[3] = { 150, 16, 10 },
                               org_a[3] = { 255, 150, 40 }, org_b[3] = { 214, 60, 16 },
                               box_a[3] = { 58, 62, 70 }, box_b[3] = { 30, 32, 38 },
                               hov_a[3] = { 120, 30, 22 }, hov_b[3] = { 70, 16, 12 };
    static const unsigned char line[4] = { 52, 56, 64, 255 }, stripe[4] = { 226, 40, 24, 255 }, yel4[4] = { 255, 206, 40, 255 },
                               pill[4] = { 22, 23, 27, 255 }, pill_edge[4] = { 70, 74, 82, 255 }, gold_ring[4] = { 255, 206, 40, 255 };
    static const unsigned char white[3] = { 245, 245, 245 }, grey[3] = { 150, 156, 164 }, dim[3] = { 150, 154, 160 },
                               yellow[3] = { 255, 206, 40 }, red[3] = { 255, 110, 96 };
    grad(img, 0, 0, XM_W, XM_H, bg_top, bg_bot, 240);
    /* the title: a red banner across the top, an orange line under it */
    banner(img, 0, 0, XM_W, TITLE_H - 6, 0, red_a, red_b);
    fill(img, 0, TITLE_H - 6, XM_W, TITLE_H - 2, yel4);
    { char t[64]; upper(t, sizeof t, title); texti(img, MARGIN, (TITLE_H - 6 - FONT_H) / 2, t, white, 1, 1); }
    /* Classic / VR: the chosen one a red banner ringed yellow */
    const int bw = (XM_W - 2 * MARGIN - BTN_GAP) / 2;
    for (int b = 0; b < 2 && b < n; b++) {
        const int x0 = MARGIN + b * (bw + BTN_GAP), x1 = x0 + bw, y0 = BAR_TOP, y1 = BAR_TOP + BTN_H;
        const int chosen = items[b].value[0] != 0;
        if (chosen) banner(img, x0, y0, x1, y1, 0, red_a, red_b); else grad(img, x0, y0, x1, y1, box_a, box_b, 255);
        if (chosen) frame(img, x0, y0, x1, y1, 3, gold_ring);
        if (b == hover) frame(img, x0 + 4, y0 + 4, x1 - 4, y1 - 4, 3, gold_ring);
        char l[32]; upper(l, sizeof l, items[b].label);
        texti(img, x0 + (bw - 2 * text_w(l)) / 2, y0 + (BTN_H - 2 * FONT_H) / 2, l, chosen ? white : dim, 2, 1);
    }
    if (note) texti(img, (XM_W - text_w(note)) / 2, BAR_TOP + BTN_H + 6, note, yellow, 1, 0);
    const int first = g_wide ? 3 : 2, top = TITLE_H + XM_BAR_H + (g_wide ? XM_WIDE_H : 0), cap = g_wide ? XM_WIDE_ROWS + 1 : XM_MODE_ROWS;
    if (g_wide && n > 2) {      /* Versus: the arcade's orange banner, slanted ends */
        const int x0 = MARGIN, x1 = XM_W - MARGIN, y0 = WIDE_TOP, y1 = WIDE_TOP + WIDE_BH;
        banner(img, x0, y0, x1, y1, 18, org_a, org_b);
        if (hover == 2) { fill(img, x0 + 18, y0, x1, y0 + 3, yel4); fill(img, x0, y1 - 3, x1 - 18, y1, yel4); }
        char l[32]; upper(l, sizeof l, items[2].label);
        if (items[2].value[0]) {
            texti(img, x0 + 34, y0 + (WIDE_BH - 2 * FONT_H) / 2, l, white, 2, 1);
            texti(img, x1 - 34 - text_w(items[2].value), y0 + (WIDE_BH - FONT_H) / 2, items[2].value, white, 1, 1);
        } else texti(img, x0 + ((x1 - x0) - 2 * text_w(l)) / 2, y0 + (WIDE_BH - 2 * FONT_H) / 2, l, white, 2, 1);
    }
    for (int i = first; i < n && i < cap; i++) {
        const int y = top + 8 + (i - first) * ROW_H, st = items[i].style;
        if (st == XM_ROW_HEADING) {         /* a section: yellow capitals after a red tick, like the arcade's GAME */
            char up[48]; upper(up, sizeof up, items[i].label);
            fill(img, MARGIN, y + 9, MARGIN + 6, y + ROW_H - 9, stripe);
            texti(img, MARGIN + 16, y + 1, up, yellow, 1, 1);
            continue;
        }
        if (st == XM_ROW_PRIMARY) {         /* the page's main action: a red banner */
            banner(img, MARGIN - 8, y + 1, XM_W - MARGIN + 8, y + ROW_H - 2, 12, i == hover ? org_a : red_a, i == hover ? org_b : red_b);
            char up[48]; upper(up, sizeof up, items[i].label);
            texti(img, (XM_W - text_w(up)) / 2, y + 1, up, white, 1, 1);
            continue;
        }
        if (i == hover) { grad(img, 8, y, XM_W - 8, y + ROW_H - 2, hov_a, hov_b, 255); fill(img, 8, y, 14, y + ROW_H - 2, yel4); }
        else fill(img, MARGIN, y + ROW_H - 2, XM_W - MARGIN, y + ROW_H - 1, line);
        texti(img, MARGIN, y + 1, items[i].label, st == XM_ROW_DANGER ? red : white, 1, 0);
        if (items[i].value[0]) {            /* the value in a score box: charcoal, a red stripe on its left */
            const int tw = text_w(items[i].value), x1 = XM_W - MARGIN, x0 = x1 - tw - 24;
            fill(img, x0, y + 3, x1, y + ROW_H - 5, pill); frame(img, x0, y + 3, x1, y + ROW_H - 5, 1, pill_edge);
            texti(img, x0 + 14, y + 1, items[i].value, st == XM_ROW_DANGER ? red : white, 1, 0);
        }
    }
    if (hint) texti(img, MARGIN, XM_H - FONT_H - 10, hint, grey, 1, 0);
}
int xm_row_at_modes(float u, float v, int n)
{
    if (u < 0 || u > 1 || v < 0 || v > 1) return -1;
    const int x = (int)(u * XM_W), y = (int)(v * XM_H);
    const int bw = (XM_W - 2 * MARGIN - BTN_GAP) / 2;
    if (y >= BAR_TOP && y < BAR_TOP + BTN_H) {
        if (x >= MARGIN && x < MARGIN + bw) return n > 0 ? 0 : -1;
        if (x >= MARGIN + bw + BTN_GAP && x < XM_W - MARGIN) return n > 1 ? 1 : -1;
        return -1;
    }
    if (g_wide && n > 2 && y >= WIDE_TOP && y < WIDE_TOP + WIDE_BH) return x >= MARGIN && x < XM_W - MARGIN ? 2 : -1;
    const int first = g_wide ? 3 : 2, cap = g_wide ? XM_WIDE_ROWS + 1 : XM_MODE_ROWS;
    int r = y - TITLE_H - XM_BAR_H - (g_wide ? XM_WIDE_H : 0) - 8;
    if (r < 0) return -1;
    r = first + r / ROW_H;
    return r < n && r < cap ? r : -1;
}

/* text n times as large, into an image w pixels wide (xm_paint_code) */
static void textn(unsigned char *img, int w, int h, int x, int y, const char *s, const unsigned char c[3], int n)
{
    for (; *s && x + n * FONT_W <= w; s++, x += n * (FONT_W - 3)) {
        int ch = (unsigned char)*s - FONT_FIRST;
        if (ch < 0 || ch >= FONT_COUNT) continue;
        for (int j = 0; j < n * FONT_H && y + j < h; j++)
            for (int i = 0; i < n * FONT_W; i++) {
                unsigned a = FONT[ch][(j / n) * FONT_W + i / n];
                if (!a) continue;
                unsigned char *p = img + ((size_t)(y + j) * w + x + i) * 4;
                for (int k = 0; k < 3; k++) p[k] = (unsigned char)((p[k] * (255 - a) + c[k] * a) / 255);
            }
    }
}
void xm_paint_code(unsigned char *img, const char *title, const char *code, const char *line)
{
    static const unsigned char bg[4] = { 18, 22, 28, 240 }, edge[4] = { 250, 200, 90, 255 };
    static const unsigned char white[3] = { 235, 238, 240 }, gold[3] = { 250, 200, 90 }, grey[3] = { 170, 176, 184 };
    const int W = XM_CODE_W, H = XM_CODE_H;
    for (int i = 0; i < W * H; i++) memcpy(img + (size_t)i * 4, bg, 4);
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++)
        if (x < 4 || y < 4 || x >= W - 4 || y >= H - 4) memcpy(img + ((size_t)y * W + x) * 4, edge, 4);
    const int big = 5, cw = big * (FONT_W - 3) * (int)strlen(code);
    textn(img, W, H, (W - text_w(title)) / 2, 22, title, white, 1);
    textn(img, W, H, (W - cw) / 2, 22 + FONT_H + 18, code, gold, big);
    textn(img, W, H, (W - text_w(line)) / 2, H - FONT_H - 24, line, grey, 1);
}

void xm_paint_speaker(unsigned char *img)
{
    const int N = XM_ICON; const float c = N / 2.0f;
    for (int y = 0; y < N; y++) for (int x = 0; x < N; x++) {
        /* 4x4 samples a pixel: the shape's coverage is its alpha */
        int in = 0, ring = 0;
        for (int sy = 0; sy < 4; sy++) for (int sx = 0; sx < 4; sx++) {
            float u = (x + (sx + 0.5f) / 4) / N, v = (y + (sy + 0.5f) / 4) / N;   /* 0..1, v down */
            float dv = v - 0.5f, ad = dv < 0 ? -dv : dv;
            int body = u >= 0.10f && u <= 0.28f && ad <= 0.13f;                     /* the box */
            int cone = u > 0.28f && u <= 0.50f && ad <= 0.13f + (u - 0.28f) * 1.15f;  /* the cone, flaring out */
            float du = u - 0.50f, r = sqrtf(du * du + dv * dv);
            int wave = du > 0 && (ad < du * 1.3f) &&
                       ((r > 0.13f && r < 0.20f) || (r > 0.27f && r < 0.34f) || (r > 0.41f && r < 0.48f));
            if (body || cone) in++;
            else if (wave) ring++;
        }
        unsigned char *p = img + ((size_t)y * N + x) * 4;
        int a = (in + ring) * 255 / 16;
        p[0] = 250; p[1] = 200; p[2] = 90; p[3] = (unsigned char)a;   /* the menu's gold */
        (void)c;
    }
    /* a dark outline under the gold, so it reads on a bright court */
    static unsigned char out[XM_ICON * XM_ICON * 4];
    memcpy(out, img, sizeof out);
    for (int y = 0; y < N; y++) for (int x = 0; x < N; x++) {
        unsigned char *p = out + ((size_t)y * N + x) * 4;
        if (p[3] > 200) continue;
        int m = 0;
        for (int j = -3; j <= 3; j++) for (int i = -3; i <= 3; i++) {
            int xx = x + i, yy = y + j;
            if (xx < 0 || yy < 0 || xx >= N || yy >= N) continue;
            int q = img[((size_t)yy * N + xx) * 4 + 3]; if (q > m) m = q;
        }
        if (m > p[3]) {   /* blend gold over a dark edge */
            float ga = p[3] / 255.0f;
            p[0] = (unsigned char)(250 * ga + 15 * (1 - ga)); p[1] = (unsigned char)(200 * ga + 18 * (1 - ga)); p[2] = (unsigned char)(90 * ga + 24 * (1 - ga));
            p[3] = (unsigned char)(m * 0.85f > p[3] ? m * 0.85f : p[3]);
        }
    }
    memcpy(img, out, sizeof out);
}

/* ---- the tabbed panel ---- */
#define XT_MAX 64
static struct { int x0, y0, x1, y1; } g_hit[XT_MAX];
static int g_nhit;
int xm_hit_tabs(float u, float v)
{
    const int x = (int)(u * XM_W), y = (int)(v * XM_H);
    if (u < 0 || u > 1 || v < 0 || v > 1) return -1;
    for (int i = 0; i < g_nhit; i++)
        if (x >= g_hit[i].x0 && x < g_hit[i].x1 && y >= g_hit[i].y0 && y < g_hit[i].y1) return i;
    return -1;
}
static void hit(int i, int x0, int y0, int x1, int y1)
{
    if (i < 0 || i >= XT_MAX) return;
    g_hit[i].x0 = x0; g_hit[i].y0 = y0; g_hit[i].x1 = x1; g_hit[i].y1 = y1;
    if (i >= g_nhit) g_nhit = i + 1;
}
static void bold(unsigned char *img, int x, int y, const char *s, const unsigned char c[3], int n)
{
    texti(img, x, y, s, c, n, 1); texti(img, x + 1, y, s, c, n, 1);
}
void xm_paint_tabs(unsigned char *img, const char *title, const XmItem *items, int n, int hover, const char *note, const char *hint)
{
    static const unsigned char bg_top[3] = { 30, 32, 38 }, bg_bot[3] = { 12, 13, 16 },
                               red_a[3] = { 236, 58, 34 }, red_b[3] = { 150, 16, 10 },
                               org_a[3] = { 255, 150, 40 }, org_b[3] = { 214, 60, 16 },
                               box_a[3] = { 58, 62, 70 }, box_b[3] = { 30, 32, 38 },
                               tab_a[3] = { 44, 47, 54 }, tab_b[3] = { 26, 28, 33 },
                               hov_a[3] = { 120, 30, 22 }, hov_b[3] = { 70, 16, 12 };
    static const unsigned char line[4] = { 52, 56, 64, 255 }, stripe[4] = { 226, 40, 24, 255 }, yel4[4] = { 255, 206, 40, 255 },
                               boxc[4] = { 22, 23, 27, 255 }, box_edge[4] = { 70, 74, 82, 255 }, dark[4] = { 16, 17, 20, 255 };
    static const unsigned char white[3] = { 245, 245, 245 }, grey[3] = { 150, 156, 164 },
                               yellow[3] = { 255, 206, 40 }, red[3] = { 255, 110, 96 };
    const int TH = 58, TAB_TOP = TH + 4, TAB_H = 56;
    g_nhit = 0;
    for (int i = 0; i < XT_MAX; i++) g_hit[i].x0 = g_hit[i].x1 = 0;
    grad(img, 0, 0, XM_W, XM_H, bg_top, bg_bot, 240);
    banner(img, 0, 0, XM_W, TH, 0, red_a, red_b);
    fill(img, 0, TH, XM_W, TH + 4, yel4);
    { char t[64]; upper(t, sizeof t, title); bold(img, MARGIN, (TH - FONT_H) / 2, t, white, 1); }
    int y = TAB_TOP;
    for (int i = 0; i < n; ) {
        const int st = items[i].style;
        int k = i; while (k < n && items[k].style == st && (st == XM_ROW_TAB || st == XM_ROW_MODE || st == XM_ROW_TILE || st == XM_ROW_DIGIT)) k++;
        if (k == i) k = i + 1;
        const int cnt = k - i;
        if (st == XM_ROW_TAB) {             /* the tab bar */
            fill(img, 0, y, XM_W, y + TAB_H, dark);
            const int tw = XM_W / cnt;
            for (int j = 0; j < cnt; j++) {
                const XmItem *it = &items[i + j]; const int x0 = j * tw, x1 = j == cnt - 1 ? XM_W : x0 + tw, on = it->value[0] != 0;
                if (on) grad(img, x0, y, x1, y + TAB_H, tab_a, tab_b, 255);
                if (on) fill(img, x0 + 10, y + TAB_H - 5, x1 - 10, y + TAB_H, yel4);
                char l[32]; upper(l, sizeof l, it->label);
                bold(img, x0 + (x1 - x0 - text_w(l)) / 2, y + (TAB_H - 5 - FONT_H) / 2, l, i + j == hover ? yellow : on ? white : grey, 1);
                hit(i + j, x0, y, x1, y + TAB_H);
            }
            y += TAB_H + 14;
        } else if (st == XM_ROW_MODE) {     /* Classic / VR */
            const int gap = 20, bw = (XM_W - 2 * MARGIN - gap * (cnt - 1)) / cnt, h = 80;
            for (int j = 0; j < cnt; j++) {
                const XmItem *it = &items[i + j]; const int x0 = MARGIN + j * (bw + gap), x1 = x0 + bw, on = it->value[0] != 0;
                if (on) banner(img, x0, y, x1, y + h, 0, red_a, red_b); else grad(img, x0, y, x1, y + h, box_a, box_b, 255);
                if (on) frame(img, x0, y, x1, y + h, 3, yel4);
                if (i + j == hover) frame(img, x0 + 4, y + 4, x1 - 4, y + h - 4, 3, yel4);
                char l[32]; upper(l, sizeof l, it->label);
                texti(img, x0 + (bw - 2 * text_w(l)) / 2, y + (h - 2 * FONT_H) / 2, l, on ? white : grey, 2, 1);
                hit(i + j, x0, y, x1, y + h);
            }
            y += h + 4;
            if (note) { texti(img, (XM_W - text_w(note)) / 2, y, note, yellow, 1, 0); y += FONT_H; }
            y += 12;
        } else if (st == XM_ROW_TILE) {     /* big tiles, easy to hit */
            const int gap = 12, tw = (XM_W - 2 * MARGIN - gap * (cnt - 1)) / cnt, h = 116;
            for (int j = 0; j < cnt; j++) {
                const XmItem *it = &items[i + j]; const int x0 = MARGIN + j * (tw + gap), x1 = x0 + tw;
                grad(img, x0, y, x1, y + h, box_a, box_b, 255);
                frame(img, x0, y, x1, y + h, i + j == hover ? 3 : 1, i + j == hover ? yel4 : box_edge);
                if (it->value[0]) {
                    char v[40]; upper(v, sizeof v, it->value);
                    const int big = 2 * text_w(v) <= tw - 16 ? 2 : 1;
                    texti(img, x0 + (tw - big * text_w(v)) / 2, y + 14 + (big == 1 ? FONT_H / 2 : 0), v, white, big, 1);
                    texti(img, x0 + (tw - text_w(it->label)) / 2, y + h - FONT_H - 8, it->label, grey, 1, 0);
                } else texti(img, x0 + (tw - text_w(it->label)) / 2, y + (h - FONT_H) / 2, it->label, white, 1, 0);
                hit(i + j, x0, y, x1, y + h);
            }
            y += h + 14;
        } else if (st == XM_ROW_DIGIT) {    /* one row of digit boxes, its label on the left */
            const int h = 56, bw = 56, gap = 10, x_end = XM_W - MARGIN;
            texti(img, MARGIN, y + (h - FONT_H) / 2, items[i].label, white, 1, 0);
            for (int j = 0; j < cnt; j++) {
                const int x1 = x_end - (cnt - 1 - j) * (bw + gap), x0 = x1 - bw;
                fill(img, x0, y, x1, y + h, boxc);
                frame(img, x0, y, x1, y + h, i + j == hover ? 3 : 1, i + j == hover ? yel4 : box_edge);
                const char *d = items[i + j].value;
                texti(img, x0 + (bw - 2 * text_w(d)) / 2 - 4, y + (h - 2 * FONT_H) / 2 + 4, d, white, 2, 0);
                hit(i + j, x0, y, x1, y + h);
            }
            y += h + 8;
        } else if (st == XM_ROW_HEADING) {
            char up[48]; upper(up, sizeof up, items[i].label);
            fill(img, MARGIN, y + 9, MARGIN + 6, y + 27, stripe);
            texti(img, MARGIN + 16, y + 1, up, yellow, 1, 1);
            y += 36;
        } else if (st == XM_ROW_PRIMARY) {
            const int h = 54;
            banner(img, MARGIN, y, XM_W - MARGIN, y + h, 16, i == hover ? org_a : red_a, i == hover ? org_b : red_b);
            char up[48]; upper(up, sizeof up, items[i].label);
            bold(img, (XM_W - text_w(up)) / 2, y + (h - FONT_H) / 2, up, white, 1);
            hit(i, MARGIN, y, XM_W - MARGIN, y + h);
            y += h + 10;
        } else {                            /* a plain row: label, value in a box */
            if (i == hover) { grad(img, 8, y, XM_W - 8, y + ROW_H - 2, hov_a, hov_b, 255); fill(img, 8, y, 14, y + ROW_H - 2, yel4); }
            else fill(img, MARGIN, y + ROW_H - 2, XM_W - MARGIN, y + ROW_H - 1, line);
            texti(img, MARGIN, y + 1, items[i].label, st == XM_ROW_DANGER ? red : white, 1, 0);
            if (items[i].value[0]) {
                const int tw = text_w(items[i].value), x1 = XM_W - MARGIN, x0 = x1 - tw - 20;
                fill(img, x0, y + 3, x1, y + ROW_H - 5, boxc); frame(img, x0, y + 3, x1, y + ROW_H - 5, 1, box_edge);
                texti(img, x0 + 10, y + 1, items[i].value, st == XM_ROW_DANGER ? red : white, 1, 0);
            }
            hit(i, 0, y, XM_W, y + ROW_H);
            y += ROW_H;
        }
        i = k;
        if (y > XM_H - FONT_H - 20) break;      /* no room: the rest is left out */
    }
    if (hint) texti(img, MARGIN, XM_H - FONT_H - 10, hint, grey, 1, 0);
}
