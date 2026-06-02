#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Screen geometry (1280×720 logical landscape) ────────────────────────── */
#define TOPBAR_H     72    /* main top bar */
#define MINIBAR_H    72    /* sub-screen mini bar */
#define TABBAR_H     88    /* bottom 4-tab nav */
#define MASTER_H     60    /* master strip above tabs on SONG view */
#define CONTENT_Y    TOPBAR_H
#define CONTENT_H    (720 - TOPBAR_H - TABBAR_H)         /* 560 px */
#define TABBAR_Y     (720 - TABBAR_H)                     /* 632 */
#define TAB_COUNT    8
#define TAB_W        (1280 / TAB_COUNT)                   /* 160 */

/* Sub-screen content area (mini bar, no tab bar) */
#define SUB_CONTENT_Y  MINIBAR_H
#define SUB_CONTENT_H  (720 - MINIBAR_H)                  /* 648 */

/* Song view: lane body shrinks to fit master strip above tab bar */
#define SONG_BODY_H  (720 - TOPBAR_H - MASTER_H - TABBAR_H)  /* 500 */
#define MASTER_Y     (720 - MASTER_H - TABBAR_H)              /* 572 */

/* ── Layout constants from design spec ───────────────────────────────────── */
#define LANE_ROW_H   100   /* song view lane row */
#define LANE_ADD_H    70   /* "+ ADD LANE" row */
#define LANE_COLOR_W   6   /* left accent bar */
#define LANE_MUTE_W   56   /* mute button zone */
#define LANE_FX_W     64   /* FX icon zone */
#define LANE_EDIT_W  120   /* synth-edit shortcut (synth lanes only) */
#define LANE_CHEV_W   64   /* chevron zone */

/* Sound browser */
#define SB_KITS_MAX   256
#define SB_FILES_MAX  512
#define SB_NAME_LEN   64
#define SB_SCROLLBAR_W 18

/* Drum grid */
#define DG_LABEL_W   164
#define DG_STEP_W     68
#define DG_STEP_H     68
#define DG_BEAT_H     48
#define DG_TOOLBAR_H  72

/* Piano roll */
#define PR_KEY_W     100
#define PR_ROW_H      42
#define PR_BAR_H      46
#define PR_TOOLBAR_H  76

/* Live play */
#define LIVE_LANE_TAB_H  52
#define LIVE_PAD_GAP     12
#define LIVE_PAD_PAD     16
#define LIVE_PAD_COLS     4
#define LIVE_PAD_ROWS     2
#define LIVE_PAD_PER_PAGE (LIVE_PAD_COLS * LIVE_PAD_ROWS)   /* 8 pads / page */
#define LIVE_PAGE_BAR_H  56    /* prev/next page strip when > 1 page of pads */

/* LIVE drum pad layout helpers (shared by draw + touch; defined in ui.c).
 * live_drum_collect: fills rows_out with the absolute drum_seq row indices
 * that have an assigned sample, returns the count (the LIVE pads).
 * live_drum_pad_rect: pixel rect of visible pad `slot` (0..LIVE_PAD_PER_PAGE-1);
 * `has_bar` shrinks the grid to leave room for the page strip. */
struct drum_seq_s;
int  live_drum_collect(const struct drum_seq_s *seq, int *rows_out, int max);
void live_drum_pad_rect(bool has_bar, int slot, int *px, int *py, int *pw, int *ph);

/* FX picker helpers (defined in ui_screens.c, shared with ui.c tap handling).
 * fx_type_for_display maps an alphabetically-ordered picker slot to a type id;
 * fx_param_enum_label returns a text label for discrete params (e.g. filter
 * mode) or NULL for plain numeric sliders. */
int         fx_type_for_display(int idx);
const char *fx_param_enum_label(int tid, int pi, float v);

/* Back zone in mini bar */
#define MINIBAR_BACK_W   88

/* ── Screen IDs ──────────────────────────────────────────────────────────── */
typedef enum {
    SCREEN_SONG = 0,      /* SONG VIEW — 4-tab main screen */
    SCREEN_LIVE,          /* LIVE PLAY — pad or piano variant */
    SCREEN_MASTER,        /* MASTER SECTION — 3-column */
    SCREEN_MENU,          /* MENU — 8-card grid */
    /* Sub-screens (no tab bar, mini bar) */
    SCREEN_DRUM_GRID,     /* drum step editor */
    SCREEN_PIANO_ROLL,    /* piano roll editor */
    SCREEN_FX_ADSR,       /* FX chain + lane ADSR */
    SCREEN_SOUND_BROWSER, /* kit / file browser */
    SCREEN_SETTINGS,      /* device settings */
    SCREEN_SONG_BROWSER,  /* .s32 file list */
    SCREEN_WAV_DETAIL,   /* WAV lane detail editor */
    SCREEN_ARP,          /* arpeggiator editor */
    SCREEN_GROOVE,       /* groove / swing per lane */
    SCREEN_ARRANGEMENT,  /* arrangement chain editor */
    SCREEN_NOTE_REPEAT,  /* note repeat + send level */
    SCREEN_OSK,          /* on-screen keyboard for text input */
    SCREEN_BLUETOOTH,    /* Bluetooth speaker scan + pair */
    SCREEN_SYNTH_EDIT,   /* synth type + all parameter editor */
    SCREEN_COUNT
} screen_id_t;

/* ── Nav stack ───────────────────────────────────────────────────────────── */
#define NAV_STACK_MAX 6

/* ── Touch types ─────────────────────────────────────────────────────────── */
typedef enum {
    TOUCH_NONE = 0,
    TOUCH_DOWN,
    TOUCH_DRAG,
    TOUCH_UP,
} touch_event_t;

/* ── Entry point ─────────────────────────────────────────────────────────── */
void ui_task(void *arg);

#ifdef __cplusplus
}
#endif
