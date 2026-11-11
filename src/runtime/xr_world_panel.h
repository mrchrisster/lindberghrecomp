/*
 * xr_world_panel.h - the headset panel's pages for a game with a world mode
 * (Virtua Tennis 3): the main page's Mode / Restart now / Advanced rows and
 * the two Advanced settings pages, Classic and Immersive. Pure (no XR, no
 * game state) like xr_panel.c; host_xr.c shows these rows as XP_COUNT + XW_x
 * beside the shared XP_ rows (which the Vulkan host shows on their own).
 */
#pragma once
#include "xr_panel.h"
#include "world_mode.h"
#ifdef __cplusplus
extern "C" {
#endif

enum { XW_MODE, XW_RESTART, XW_ADVANCED, XW_BACK, XW_DOLLY, XW_STEPBACK, XW_REPLAYS, XW_CUTAWAYS,
       XW_OVERLAYS, XW_SKY, XW_SHADOW, XW_RESTORE, XW_COUNT };
enum { XW_PAGE_MAIN, XW_PAGE_CLASSIC, XW_PAGE_IMMERSIVE, XW_PAGES };
/* xw_choose's effects, beside XP_SAVE and XP_RESTART */
enum { XW_LIVE_CHANGED = 1 << 16,      /* a live value of the running mode (or, live, the mode) changed: world_mode_update */
       XW_PAGE_CHANGED = 1 << 17 };    /* the page or its rows changed */
/* a page's row, as host_xr.c's menu_ids hold them: an XP_ item, or XP_COUNT + an XW_ item */
#define XW_ROW(x) (XP_COUNT + (x))
#define XW_ROWS_MAX (XP_COUNT + XW_COUNT)

typedef struct {
    int page;                   /* XW_PAGE_ */
    int restore_armed;          /* "Restore ... defaults" pulled once */
    int running;                /* the mode this run started in: 0 Classic, 1 Immersive */
    unsigned pins;              /* WM_ bits whose env variable is set (for the running mode's rows) */
    WmValues eff;               /* the running values (world_mode_values) */
    int live;                   /* the mode switches live (world_mode_live): no Restart now, no
                                 * "(on restart)", Mode returns XW_LIVE_CHANGED. 0: on restart */
} XwState;

/* The rows of the current page, in order (XP_ ids or XW_ROW(x)); returns the
 * count. The main page carries every XP_ item but Frame rate counter, Arcade
 * overlays and Replays (on the Advanced pages here): the host still leaves
 * out rows its game has no use for. */
int xw_page_rows(const XwState *w, const LgXrSettings *cfg, int *rows);
/* Labels and values of all XW_COUNT items for the current page */
void xw_items(const XwState *w, const LgXrSettings *cfg, XmItem *items);
/* An XW_ item chosen: the settings changed, effects returned (0: nothing,
 * e.g. a row pinned by env). */
unsigned xw_choose(XwState *w, LgXrSettings *cfg, int item);
/* The page's title (the main page: NULL, the host's own) and the hint line */
const char *xw_title(const XwState *w);
const char *xw_hint(const XwState *w);
/* The stored defaults of a mode's Advanced settings */
void xw_restore_defaults(LgXrSettings *cfg, int immersive);

#ifdef __cplusplus
}
#endif
