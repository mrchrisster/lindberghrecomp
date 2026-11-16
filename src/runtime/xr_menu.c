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
/* text twice as large (each font pixel 2x2) */
static void text2(unsigned char *img, int x, int y, const char *s, const unsigned char c[3])
{
    for (; *s && x + 2 * FONT_W <= XM_W; s++, x += 2 * (FONT_W - 3)) {
        int ch = (unsigned char)*s - FONT_FIRST;
        if (ch < 0 || ch >= FONT_COUNT) continue;
        for (int j = 0; j < 2 * FONT_H && y + j < XM_H; j++)
            for (int i = 0; i < 2 * FONT_W; i++) {
                unsigned a = FONT[ch][(j / 2) * FONT_W + i / 2];
                if (!a) continue;
                unsigned char *p = img + ((size_t)(y + j) * XM_W + x + i) * 4;
                for (int k = 0; k < 3; k++) p[k] = (unsigned char)((p[k] * (255 - a) + c[k] * a) / 255);
            }
    }
}
static int g_wide;
void xm_modes_wide(int on) { g_wide = on != 0; }
#define WIDE_TOP (TITLE_H + XM_BAR_H + 4)
#define WIDE_BH (XM_WIDE_H - 12)
void xm_paint_modes(unsigned char *img, const char *title, const XmItem *items, int n, int hover, const char *note, const char *hint)
{
    static const unsigned char bg[4] = { 18, 22, 28, 235 }, bar[4] = { 40, 70, 52, 255 },
                               hi[4] = { 60, 110, 80, 255 }, line[4] = { 70, 80, 90, 255 },
                               on[4] = { 46, 150, 86, 255 }, off[4] = { 34, 40, 48, 255 },
                               ring[4] = { 150, 240, 170, 255 }, hover_ring[4] = { 250, 200, 90, 255 };
    static const unsigned char white[3] = { 235, 238, 240 }, green[3] = { 120, 230, 140 },
                               grey[3] = { 150, 158, 166 }, gold[3] = { 250, 200, 90 }, dim[3] = { 170, 176, 184 };
    fill(img, 0, 0, XM_W, XM_H, bg);
    fill(img, 0, 0, XM_W, TITLE_H, bar);
    text(img, MARGIN, (TITLE_H - FONT_H) / 2, title, white);
    const int bw = (XM_W - 2 * MARGIN - BTN_GAP) / 2;
    for (int b = 0; b < 2 && b < n; b++) {
        const int x0 = MARGIN + b * (bw + BTN_GAP), x1 = x0 + bw, y0 = BAR_TOP, y1 = BAR_TOP + BTN_H;
        const int chosen = items[b].value[0] != 0;
        fill(img, x0, y0, x1, y1, chosen ? on : off);
        if (chosen) frame(img, x0, y0, x1, y1, 4, ring);
        if (b == hover) frame(img, x0 + (chosen ? 4 : 0), y0 + (chosen ? 4 : 0), x1 - (chosen ? 4 : 0), y1 - (chosen ? 4 : 0), 4, hover_ring);
        const char *l = items[b].label;
        text2(img, x0 + (bw - 2 * text_w(l)) / 2, y0 + (BTN_H - 2 * FONT_H) / 2, l, chosen ? white : dim);
    }
    if (note) text(img, (XM_W - text_w(note)) / 2, BAR_TOP + BTN_H + 6, note, gold);
    const int first = g_wide ? 3 : 2, top = TITLE_H + XM_BAR_H + (g_wide ? XM_WIDE_H : 0), cap = g_wide ? XM_WIDE_ROWS + 1 : XM_MODE_ROWS;
    if (g_wide && n > 2) {      /* the wide button */
        const int x0 = MARGIN, x1 = XM_W - MARGIN, y0 = WIDE_TOP, y1 = WIDE_TOP + WIDE_BH;
        const int active = items[2].value[0] != 0;
        fill(img, x0, y0, x1, y1, active ? on : off);
        if (active) frame(img, x0, y0, x1, y1, 4, ring);
        if (hover == 2) frame(img, x0 + (active ? 4 : 0), y0 + (active ? 4 : 0), x1 - (active ? 4 : 0), y1 - (active ? 4 : 0), 4, hover_ring);
        const char *l = items[2].label;
        if (active) {
            text2(img, x0 + 24, y0 + (WIDE_BH - 2 * FONT_H) / 2, l, white);
            text(img, x1 - 24 - text_w(items[2].value), y0 + (WIDE_BH - FONT_H) / 2, items[2].value, white);
        } else text2(img, x0 + ((x1 - x0) - 2 * text_w(l)) / 2, y0 + (WIDE_BH - 2 * FONT_H) / 2, l, white);
    }
    for (int i = first; i < n && i < cap; i++) {
        int y = top + 8 + (i - first) * ROW_H;
        if (i == hover) fill(img, 8, y, XM_W - 8, y + ROW_H - 2, hi);
        else fill(img, MARGIN, y + ROW_H - 2, XM_W - MARGIN, y + ROW_H - 1, line);
        int action = !items[i].value[0];
        text(img, MARGIN, y + 1, items[i].label, action ? gold : white);
        if (!action) text(img, XM_W - MARGIN - text_w(items[i].value), y + 1, items[i].value, green);
    }
    if (hint) text(img, MARGIN, XM_H - FONT_H - 10, hint, grey);
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
