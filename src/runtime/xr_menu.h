/*
 * xr_menu.h - the headset's settings panel, painted on the CPU (xr_menu.c)
 * and shown by the overlay (xo_draw_panel).
 */
#ifndef XR_MENU_H
#define XR_MENU_H

#ifdef __cplusplus
extern "C" {
#endif

#define XM_W 768                /* the panel's picture, in pixels */
#define XM_H 860                /* title, XM_ROWS rows of 36 and the hint */
#define XM_ROWS 21

/* a row's look (xm_paint_modes): a plain row (a setting with its value in
 * a pill, or an action without one), a section heading (grey capitals, not
 * a button), a destructive action (red) or the page's main action (a gold
 * bar) */
enum { XM_ROW_PLAIN, XM_ROW_HEADING, XM_ROW_DANGER, XM_ROW_PRIMARY,
       XM_ROW_TAB,      /* xm_paint_tabs: a tab in the bar under the title (value not empty: the open one) */
       XM_ROW_MODE,     /* a big mode button, side by side with the next (value not empty: chosen) */
       XM_ROW_TILE,     /* a big tile in a row of them: value large, label under it */
       XM_ROW_DIGIT };  /* a digit box in one row of them (the first one's label on the left) */
typedef struct {
    const char *label;
    char value[40];             /* "" for an action */
    int style;                  /* XM_ROW_ (0: plain) */
} XmItem;

/* Paints the panel into rgba (XM_W x XM_H x 4): title, the items with their
 * values, hover highlighted (-1: none), and a hint at the bottom. */
void xm_paint(unsigned char *rgba, const char *title, const XmItem *items, int n, int hover, const char *hint);
/* A small label (the frame rate): light text on a dark box, rgba w x h x 4. */
#define XM_LABEL_W 224
#define XM_LABEL_H 48
void xm_paint_label(unsigned char *rgba, const char *s);
/* The stats panel: n left-aligned lines (up to 27 characters each) of light
 * text on a dark box, rgba XM_STATS_W x XM_STATS_H x 4. */
#define XM_STATS_W 480
#define XM_STATS_H 160
void xm_paint_lines(unsigned char *rgba, const char *const *lines, int n);
/* A startup message (XM_W x XM_H x 4): a centred box with a title and a
 * second line, transparent around it. */
#define XM_MESSAGE_H 170
void xm_paint_message(unsigned char *rgba, const char *title, const char *line);
/* The row under panel coordinates u, v (0..1 from the top left); -1 if none. */
int  xm_row_at(float u, float v, int n);
/* The same panel with a mode bar (a game with Classic / VR modes): items[0]
 * and items[1] are drawn as two big buttons side by side under the title -
 * the one whose value is not empty highlighted as the chosen one - with
 * `note` on a line under them; the other items as rows below. Hover 0 / 1 is
 * a button. xm_row_at_modes: 0 / 1 for the buttons, 2.. the rows below. */
#define XM_BAR_H 136            /* the buttons' band: 88 high and the note's line */
#define XM_MODE_ROWS 18         /* rows that fit below the band, the buttons counted */
void xm_paint_modes(unsigned char *rgba, const char *title, const XmItem *items, int n, int hover, const char *note, const char *hint);
int  xm_row_at_modes(float u, float v, int n);
/* With a wide button (xm_modes_wide(1)): items[2] is drawn as a third big
 * button across the panel, under the note (Virtua Tennis 3's Versus), its
 * value (if any) in small letters inside; the rows start at items[3], and
 * XM_WIDE_ROWS fit. xm_row_at_modes returns 2 for it. */
#define XM_WIDE_H 76            /* the wide button and its gap */
#define XM_WIDE_ROWS 16
void xm_modes_wide(int on);
/* A big code on a dark card (the netplay room code on the start-up screen):
 * title and line in normal letters, code five times as large; rgba
 * XM_CODE_W x XM_CODE_H x 4 */
#define XM_CODE_W 768
#define XM_CODE_H 320
void xm_paint_code(unsigned char *rgba, const char *title, const char *code, const char *line);
/* The tabbed panel (Virtua Tennis 3, 2026-11-16): items laid out by their
 * style - consecutive TAB, MODE, TILE and DIGIT items share a band - with
 * `note` under the mode buttons and `hint` at the bottom. xm_hit_tabs: the
 * item under u, v (0..1 from the top left) as last painted, -1 for none. */
void xm_paint_tabs(unsigned char *rgba, const char *title, const XmItem *items, int n, int hover, const char *note, const char *hint);
int  xm_hit_tabs(float u, float v);
/* voice chat: a small speaker with sound waves, XM_ICON x XM_ICON x 4, on a
 * transparent background (alpha) */
#define XM_ICON 96
void xm_paint_speaker(unsigned char *rgba);

#ifdef __cplusplus
}
#endif
#endif
