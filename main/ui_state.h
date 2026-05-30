#pragma once
/*
 * ui_state.h — shared state for the split ui_*.c translation units.
 *
 * All state lives in ui.c and is declared extern here so ui_draw.c
 * and ui_screens.c can read it without circular dependencies.
 *
 * Only include this from ui.c, ui_draw.c, ui_screens.c.
 */

#include <stdint.h>
#include <stdbool.h>
#include "ui.h"
#include "lane.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Piano key record ────────────────────────────────────────────────────── */
#define MAX_KEYS       28
#define KEY_OFF_THRESH  3

typedef struct {
    int16_t x, y, w, h;
    int     semi;        /* 0–11 within octave */
    int     oct_offset;  /* 0 or 1 */
    bool    is_black;
    bool    pressed;
} piano_key_t;

/* ── Navigation stack (ui.c) ─────────────────────────────────────────────── */
extern screen_id_t  s_nav_stack[NAV_STACK_MAX];
extern int          s_nav_top;
extern int          s_active_tab;
extern int          s_ctx_lane;
extern int          s_ctx_drum_row;

screen_id_t current_screen(void);
void        push_screen(screen_id_t scr);
void        pop_screen(void);
bool        is_main_screen(screen_id_t scr);

/* ── Live / piano state (ui.c) ───────────────────────────────────────────── */
extern int      s_live_lane;
extern int      s_live_octave;
extern int      s_live_pad_page;   /* current page of LIVE drum pads (8/page) */

extern const int s_wk_semi[7];
extern const int s_bk_semi[5];

extern piano_key_t    s_piano_keys[MAX_KEYS];
extern int      s_key_cnt;
extern bool     s_key_held[MAX_KEYS];
extern uint8_t  s_key_off_cnt[MAX_KEYS];

/* ── Per-screen selection state (ui.c) ───────────────────────────────────── */
extern int  s_song_browser_sel;
extern int  s_drag_fader;
extern int  s_dg_long_row, s_dg_long_step, s_dg_hold_ms;
extern int  s_dg_row_offset;   /* first visible row in drum grid */
extern int  s_fx_sel_slot;
/* FX edit target for the FX_ADSR screen: a lane index, or one of these. */
#define FX_TGT_MASTER 255
#define FX_TGT_SEND   254
extern int  s_fx_target;               /* lane idx, FX_TGT_MASTER, or FX_TGT_SEND */
/* FX type picker overlay (FX_ADSR screen) */
extern bool s_fx_picker_open;
extern int  s_fx_picker_target_slot;   /* slot to insert into when a type is chosen */
extern bool s_fx_picker_replace;       /* true = replace existing slot, false = insert */
extern int  s_fx_picker_scroll;        /* scroll row offset in the picker grid */
/* FX param-slider drag (FX_ADSR screen) */
extern int  s_fx_param_drag;           /* param index 0–7 being dragged, -1 = none */
/* Lane ADSR knob drag (FX_ADSR screen) */
extern int  s_fx_adsr_drag;            /* 0=A 1=D 2=S 3=R being dragged, -1 = none */
extern float s_fx_adsr_drag_start;     /* value at drag start */
extern int  s_fx_adsr_drag_y;          /* finger y at drag start */
extern int  s_sb_kit_sel, s_sb_file_sel;

/* Resolve an FX target to its chain. *out_count receives the count pointer;
 * *out_adsr receives the lane ADSR (NULL for master/send); *out_notify the
 * WS_MSG_FX_UPDATE idx (lane / 0xFF / 0xFE). Returns the chain array. */
fx_node_t **fx_target_resolve(int target, int **out_count,
                              lane_adsr_t **out_adsr, int *out_notify);
const char *fx_target_title(int target);

/* ── Real sound browser (ui.c) ───────────────────────────────────────────── */
extern int   s_sb_kit_count, s_sb_kit_scroll;
extern int   s_sb_file_count, s_sb_file_scroll;
extern char  s_sb_kits[SB_KITS_MAX][SB_NAME_LEN];
extern char  s_sb_files[SB_FILES_MAX][SB_NAME_LEN];

/* ── Piano roll view state (ui.c) ────────────────────────────────────────── */
extern int      s_pr_view_semitone;
extern int      s_pr_drag_note;
extern int      s_pr_drag_orig_row;   /* semitone of note being dragged */
extern uint32_t s_pr_drag_orig_tick;  /* original tick_start when drag began */
extern int      s_pr_resize_note;     /* note index being right-edge resized */
extern uint32_t s_pr_tick_offset;     /* leftmost visible tick (scroll) */
extern uint32_t s_pr_ticks_wide;      /* ticks spanning the roll width (zoom) */
extern bool     s_pr_delete_mode;     /* DELETE tool active — tap a note to remove */

/* ── Context menu overlay (ui.c) ─────────────────────────────────────────── */
extern bool s_ctx_menu_open;   /* true while the lane ctx-menu overlay is showing */
extern int  s_ctx_menu_lane;   /* lane index the menu was opened for */

/* ── Song view scroll (ui.c) ─────────────────────────────────────────────── */
extern int  s_song_scroll;     /* first visible lane row index */

/* ── Drum double-tap / long-press state (ui.c) ───────────────────────────── */
extern int     s_dg_last_row;       /* row of last tap for double-tap accent */
extern int     s_dg_last_step;      /* step of last tap */
extern int64_t s_dg_last_tap_ms;    /* timestamp (ms) of last tap */
extern int     s_dg_vel_row;        /* row for velocity overlay (-1 = hidden) */
extern int     s_dg_vel_step;       /* step for velocity overlay */
extern int     s_dg_vel_value;      /* current slider value 1–127 */

/* ── Touch state (ui.c) ──────────────────────────────────────────────────── */
extern bool s_touch_down;
extern int  s_down_x, s_down_y;
extern int  s_drag_x, s_drag_y;

/* Touch highlight flash (ui.c) */
extern int  s_hl_x, s_hl_y, s_hl_w, s_hl_h;
extern bool s_hl_pending;
extern bool s_hl_visible;

/* ── Fader delta-drag state (ui.c) ───────────────────────────────────────── */
extern int   s_fader_drag_lane;    /* lane index being dragged, -1 = none */
extern float s_fader_drag_start_v; /* lane volume at drag start */
extern int   s_fader_last_tap_lane;
extern int   s_fader_last_tap_x;
extern int64_t s_fader_last_tap_ms;

/* ── Inline volume panel (ui.c) ──────────────────────────────────────────── */
#define LANE_VOL_H  88
extern int s_vol_open_lane;   /* absolute lane index with open panel, -1 = none */

/* ── Status banner (ui.c) ────────────────────────────────────────────────── */
/* Transient one-line message shown over the top bar (e.g. "Saved", error
 * reason from a failed save). Cleared automatically after ~3 s. */
extern char    s_status_msg[64];
extern int64_t s_status_until_ms;
void           ui_status_set(const char *fmt, ...);

/* ── Functions provided by ui_draw.c ─────────────────────────────────────── */
void draw_screen(void);
void draw_live_piano_key(int k);

/* ── Functions provided by ui_screens.c ─────────────────────────────────── */
void setup_live_piano_keys(void);
void song_browser_refresh(void);
void draw_wav_detail_screen(void);
void draw_ctx_menu_overlay(void);
void draw_dg_vel_overlay(void);
void draw_arp_screen(void);
void draw_groove_screen(void);
void draw_arrangement_screen(void);
void draw_note_repeat_screen(void);
void draw_osk_screen(void);
void draw_bluetooth_screen(void);

/* FX param descriptor lookup (defined in ui_screens.c).
 *   tid       — fx_type_t value
 *   pi        — 0..7 param index
 *   out_lbl/min/max/dec — receive descriptor; defaults if out of range */
int  fx_param_count(int tid);
void fx_param_descriptor(int tid, int pi,
                         const char **out_lbl, float *out_min,
                         float *out_max, int *out_dec);

/* ── Song browser file list (ui_screens.c) ───────────────────────────────── */
#define SONG_BROWSER_MAX 32
extern char s_song_browser_files[SONG_BROWSER_MAX][64];
extern int  s_song_browser_count;
extern bool s_song_browser_dirty;

/* ── Arrangement editor state (ui_screens.c) ─────────────────────────────── */
extern int s_arr_sel_step;    /* selected step index, -1=none */

/* ── Euclidean popup state (ui_screens.c) ────────────────────────────────── */
extern bool s_eucl_popup;
extern int  s_eucl_hits;
extern int  s_eucl_steps;

/* ── On-screen keyboard state (ui.c) ────────────────────────────────────── */
#define OSK_MAX_LEN 31
extern char   s_osk_buf[OSK_MAX_LEN + 1]; /* text being edited              */
extern int    s_osk_len;                   /* current length                 */
extern char   s_osk_title[40];             /* prompt shown above keyboard    */
/* Callback invoked when user taps DONE. Return value ignored. */
typedef void (*osk_done_cb_t)(const char *text);
extern osk_done_cb_t s_osk_done_cb;

/* ── Bluetooth state (ui_screens.c) ─────────────────────────────────────── */
#define BT_SCAN_MAX 8
extern char s_bt_devices[BT_SCAN_MAX][32];
extern int  s_bt_device_count;
extern int  s_bt_sel;
extern bool s_bt_scanning;
extern bool s_bt_connected;
extern char s_bt_connected_name[32];

/* ── Synth edit state (ui.c) ─────────────────────────────────────────────── */
/* Which tab is active: 0=ENV 1=OSC 2=FILTER 3=MOD */
extern int s_se_tab;
/* Synth edit screen is entered for this lane */
extern int s_se_lane;
/* Param values live in lane->synth_params[] — no separate UI cache needed */
void draw_synth_edit_screen(void);

#ifdef __cplusplus
}
#endif
