/*
 * ui.c — navigation stack, touch handling, runtime state, and ui_task().
 *
 * Draw primitives and chrome:  ui_draw.c
 * Per-screen draw functions:   ui_screens.c
 * Shared state declarations:   ui_state.h
 */

#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdarg.h>
#include <math.h>
#include <dirent.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "bsp/touch.h"
#include "esp_lcd_touch.h"
#include "gfx.h"
#include "audio.h"
#include "lane.h"
#include "clock.h"
#include "drum_seq.h"
#include "wav_player.h"
#include "piano_roll.h"
#include "arp.h"
#include "synth.h"
#include "fx.h"
#include "settings.h"
#include "song.h"
#include "ui.h"
#include "ui_state.h"
#include "ws_server.h"
#include "ws_cmd.h"
#include "render_export.h"

static const char *TAG = "ui";

/* ── Touch handle ────────────────────────────────────────────────────────── */
typedef struct { int16_t x, y; } tp_pt_t;
static esp_lcd_touch_handle_t s_touch = NULL;

static void touch_init(void)
{
    bsp_display_cfg_t cfg = {0};
    ESP_ERROR_CHECK(bsp_touch_new(&cfg, &s_touch));
    ESP_LOGI(TAG, "GT911 ready");
}

static int touch_get_points(tp_pt_t *pts, int max_pts)
{
    esp_lcd_touch_read_data(s_touch);
    esp_lcd_touch_point_data_t data[5];
    uint8_t cnt = 0;
    if (esp_lcd_touch_get_data(s_touch, data, &cnt, (uint8_t)max_pts) != ESP_OK || cnt == 0)
        return 0;
    for (int i = 0; i < cnt; i++) {
        pts[i].x = (int16_t)data[i].x;
        pts[i].y = (int16_t)data[i].y;
    }
    return (int)cnt;
}

/* GT911 portrait (0,0)=top-left → logical landscape (0,0)=top-left */
static inline void touch_transform(tp_pt_t *pt)
{
    int16_t px = pt->x, py = pt->y;
    pt->x = (int16_t)(1279 - py);
    pt->y = px;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Navigation stack
 * ══════════════════════════════════════════════════════════════════════════ */

screen_id_t s_nav_stack[NAV_STACK_MAX] = { SCREEN_SONG };
int         s_nav_top    = 0;
int         s_active_tab = 0;

int         s_ctx_lane    = 0;
int         s_ctx_drum_row = 0;

screen_id_t current_screen(void) { return s_nav_stack[s_nav_top]; }

void push_screen(screen_id_t scr)
{
    s_vol_open_lane = -1;   /* always close inline panel on navigation */
    if (s_nav_top < NAV_STACK_MAX - 1) s_nav_stack[++s_nav_top] = scr;
}

void pop_screen(void)
{
    if (s_nav_top > 0) s_nav_top--;
}

bool is_main_screen(screen_id_t scr)
{
    return scr == SCREEN_SONG || scr == SCREEN_LIVE ||
           scr == SCREEN_MASTER || scr == SCREEN_MENU;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Runtime UI state definitions (externs declared in ui_state.h)
 * ══════════════════════════════════════════════════════════════════════════ */

int   s_live_lane    = 0;
int   s_live_octave  = 4;
int   s_live_pad_page = 0;

/* ── LIVE drum pad layout (shared by draw_live_pad_grid + handle_live_tap) ── */
int live_drum_collect(const struct drum_seq_s *seq, int *rows_out, int max)
{
    if (!seq) return 0;
    int n = 0;
    for (int r = 0; r < seq->row_count && n < max; r++)
        if (seq->rows[r].wav_path[0]) rows_out[n++] = r;
    return n;
}

void live_drum_pad_rect(bool has_bar, int slot, int *px, int *py, int *pw, int *ph)
{
    int pad_y      = CONTENT_Y + LIVE_LANE_TAB_H + LIVE_PAD_PAD;
    int bottom     = TABBAR_Y - LIVE_PAD_PAD - (has_bar ? LIVE_PAGE_BAR_H : 0);
    int pad_area_h = bottom - pad_y;
    int pad_w      = (1280 - LIVE_PAD_PAD * (LIVE_PAD_COLS + 1)) / LIVE_PAD_COLS;
    int pad_h      = (pad_area_h - LIVE_PAD_GAP * (LIVE_PAD_ROWS - 1)) / LIVE_PAD_ROWS;
    int col        = slot % LIVE_PAD_COLS;
    int row        = slot / LIVE_PAD_COLS;
    *px = LIVE_PAD_PAD + col * (pad_w + LIVE_PAD_GAP);
    *py = pad_y       + row * (pad_h + LIVE_PAD_GAP);
    *pw = pad_w;
    *ph = pad_h;
}

/* Polyphonic gain normalisation: count of currently-held live keys.
 * Read by audio task (core 1) — volatile, no mutex needed (single writer). */
volatile int g_live_held_count = 0;
/* Lane index (g_song.lanes[]) of the currently active live lane, or -1. */
volatile int g_live_lane_idx = -1;

/* pan fader drag state (SONG view) */
static int   s_pan_drag_lane    = -1;
static float s_pan_drag_start_v = 0.0f;
/* master vol/pan drag state (MASTER screen) */
static bool  s_master_vol_drag  = false;
static bool  s_master_pan_drag  = false;

const int s_wk_semi[7] = { 0, 2, 4, 5, 7, 9, 11 };
const int s_bk_semi[5] = { 1, 3, 6, 8, 10 };

piano_key_t   s_piano_keys[MAX_KEYS]          = {};
int     s_key_cnt                 = 0;
bool    s_key_held[MAX_KEYS]      = {};
uint8_t s_key_off_cnt[MAX_KEYS]   = {};
/* Per-framebuffer record of the key pressed-state each buffer currently shows.
 * Because the panel is double-buffered, live-key feedback updates the back
 * buffer (tear-free) and presents it by swap; both buffers converge over two
 * frames. Indexed by gfx_back_index(). */
bool    s_key_shown[2][MAX_KEYS]  = {};
/* Set per buffer when the chrome (lane tabs, octave bar, …) needs a full repaint
 * — e.g. on entering the live piano or an octave/lane change. A dirty buffer
 * gets a full draw_screen() when it next becomes the back buffer. */
bool    s_live_chrome_dirty[2]    = {};

int s_song_browser_sel = 0;
int s_drag_fader       = -1;
int s_dg_long_row      = -1, s_dg_long_step = -1, s_dg_hold_ms = 0;
int s_dg_row_offset    = 0;
int s_fx_sel_slot      = 0;
int  s_fx_target            = 0;   /* lane idx / FX_TGT_MASTER / FX_TGT_SEND */
bool s_fx_picker_open       = false;
int  s_fx_picker_target_slot = 0;
bool s_fx_picker_replace    = false;
int  s_fx_picker_scroll     = 0;
int  s_fx_param_drag        = -1;
int  s_fx_adsr_drag         = -1;
float s_fx_adsr_drag_start  = 0.0f;
int  s_fx_adsr_drag_y       = 0;

/* Resolve an FX target (lane / master / send) to its chain. */
fx_node_t **fx_target_resolve(int target, int **out_count,
                              lane_adsr_t **out_adsr, int *out_notify)
{
    if (target == FX_TGT_MASTER) {
        *out_count = &g_song.master_fx_count;
        if (out_adsr)   *out_adsr   = NULL;
        if (out_notify) *out_notify = 0xFF;
        return g_song.master_fx;
    }
    if (target == FX_TGT_SEND) {
        *out_count = &g_song.send_fx_count;
        if (out_adsr)   *out_adsr   = NULL;
        if (out_notify) *out_notify = 0xFE;
        return g_song.send_fx;
    }
    if (target < 0 || target >= NUM_LANES) target = 0;
    lane_t *l = &g_song.lanes[target];
    *out_count = &l->fx_count;
    if (out_adsr)   *out_adsr   = &l->adsr;
    if (out_notify) *out_notify = target;
    return l->fx;
}

const char *fx_target_title(int target)
{
    static char buf[32];
    if (target == FX_TGT_MASTER)    snprintf(buf, sizeof(buf), "MASTER FX");
    else if (target == FX_TGT_SEND) snprintf(buf, sizeof(buf), "SEND FX");
    else snprintf(buf, sizeof(buf), "FX \xc2\xb7 LANE %02d", target + 1);
    return buf;
}

int s_sb_kit_sel       = 0, s_sb_file_sel = 0;

/* ── Real sound browser state ────────────────────────────────────────────── */
char    s_sb_kits[SB_KITS_MAX][SB_NAME_LEN];
int     s_sb_kit_count   = 0;
int     s_sb_kit_scroll  = 0;   /* derived: (int)(s_sb_kit_px / row_h) */
char    s_sb_files[SB_FILES_MAX][SB_NAME_LEN];
int     s_sb_file_count  = 0;
int     s_sb_file_scroll = 0;   /* derived: (int)(s_sb_file_px / row_h) */
/* Smooth-scroll: sub-row pixel accumulators + momentum velocity */
static float s_sb_kit_px       = 0.0f;   /* scroll offset in pixels */
static float s_sb_file_px      = 0.0f;
static float s_sb_kit_vel       = 0.0f;  /* momentum px/frame after finger lift */
static float s_sb_file_vel      = 0.0f;
static float s_sb_drag_vel_acc  = 0.0f;  /* EMA of per-frame drag deltas during drag */
static bool  s_sb_scrolling_kit = false; /* which pane the current drag is in */
static bool  s_sb_sb_drag        = false; /* true: dragging a scrollbar thumb */
static bool    s_sb_dirty        = true;  /* re-scan on next open */
static int     s_sb_preview_slot = -1;    /* wav pool slot reserved for preview */

static void sb_sync_scroll(void)
{
    const float row_h = 72.0f;
    /* rows_vis must match draw: (720 - MINIBAR_H - 80) / 72 - 1 (header row) */
    int rows_vis = (720 - MINIBAR_H - 80) / 72 - 1;
    if (rows_vis < 1) rows_vis = 1;
    int kit_max_rows  = s_sb_kit_count  - rows_vis;
    int file_max_rows = s_sb_file_count - rows_vis;
    if (kit_max_rows  < 0) kit_max_rows  = 0;
    if (file_max_rows < 0) file_max_rows = 0;
    float kit_max  = (float)kit_max_rows  * row_h;
    float file_max = (float)file_max_rows * row_h;
    if (s_sb_kit_px  < 0.0f)      s_sb_kit_px  = 0.0f;
    if (s_sb_kit_px  > kit_max)   s_sb_kit_px  = kit_max;
    if (s_sb_file_px < 0.0f)      s_sb_file_px = 0.0f;
    if (s_sb_file_px > file_max)  s_sb_file_px = file_max;
    s_sb_kit_scroll  = (int)(s_sb_kit_px  / row_h);
    s_sb_file_scroll = (int)(s_sb_file_px / row_h);
}

/* Play a one-shot preview of the currently selected file. */
static void sb_preview_current(void)
{
    if (s_sb_kit_sel < 0 || s_sb_kit_sel >= s_sb_kit_count) return;
    if (s_sb_file_sel < 0 || s_sb_file_sel >= s_sb_file_count) return;

    if (s_sb_preview_slot < 0) {
        s_sb_preview_slot = wav_lane_alloc_slot();
        if (s_sb_preview_slot < 0) return;
    }
    /* Detach from audio mix while we re-open the file (audio reads pcm_buf). */
    audio_set_preview_slot(-1);

    char full[280];
    snprintf(full, sizeof(full), "/sdcard/sounds/%s/%s",
             s_sb_kits[s_sb_kit_sel], s_sb_files[s_sb_file_sel]);
    if (wav_lane_open(s_sb_preview_slot, full)) {
        wav_lane_t *ln = wav_lane_get(s_sb_preview_slot);
        if (ln) {
            ln->play_mode = WAV_MODE_ONE_SHOT;
            ln->volume    = 200;
            ln->pcm_pos   = 0;
            ln->active    = true;
            audio_set_preview_slot(s_sb_preview_slot);
        }
    }
}

static int sb_cmp(const void *a, const void *b)
{
    return strcasecmp((const char *)a, (const char *)b);
}

static void sb_load_kits(void)
{
    s_sb_kit_count = 0;
    DIR *d = opendir("/sdcard/sounds");
    if (!d) return;
    struct dirent *e;
    char tmp[300];
    struct stat st;
    while ((e = readdir(d)) && s_sb_kit_count < SB_KITS_MAX) {
        if (e->d_name[0] == '.') continue;
        /* DT_DIR is unreliable on FAT — use stat() to confirm it's a directory */
        if (e->d_type == DT_REG) continue;  /* skip known regular files fast */
        if (e->d_type != DT_DIR) {
            snprintf(tmp, sizeof(tmp), "/sdcard/sounds/%s", e->d_name);
            if (stat(tmp, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        }
        strncpy(s_sb_kits[s_sb_kit_count], e->d_name, SB_NAME_LEN - 1);
        s_sb_kits[s_sb_kit_count][SB_NAME_LEN - 1] = '\0';
        s_sb_kit_count++;
    }
    closedir(d);
    qsort(s_sb_kits, s_sb_kit_count, SB_NAME_LEN, sb_cmp);
}

static void sb_load_files(int kit_idx)
{
    s_sb_file_count = 0;
    if (kit_idx < 0 || kit_idx >= s_sb_kit_count) return;
    char path[200];
    snprintf(path, sizeof(path), "/sdcard/sounds/%s", s_sb_kits[kit_idx]);
    DIR *d = opendir(path);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && s_sb_file_count < SB_FILES_MAX) {
        if (e->d_name[0] == '.') continue;
        const char *ext = strrchr(e->d_name, '.');
        if (!ext || (strcasecmp(ext, ".wav") != 0)) continue;
        strncpy(s_sb_files[s_sb_file_count], e->d_name, SB_NAME_LEN - 1);
        s_sb_files[s_sb_file_count][SB_NAME_LEN - 1] = '\0';
        s_sb_file_count++;
    }
    closedir(d);
    qsort(s_sb_files, s_sb_file_count, SB_NAME_LEN, sb_cmp);
}

bool    s_ctx_menu_open  = false;
int     s_ctx_menu_lane  = -1;
int     s_song_scroll    = 0;   /* first visible lane row index (scroll offset) */

/* ── On-screen keyboard ──────────────────────────────────────────────────── */
char          s_osk_buf[OSK_MAX_LEN + 1] = {};
int           s_osk_len                  = 0;
char          s_osk_title[40]            = "Enter name";
osk_done_cb_t s_osk_done_cb             = NULL;

/* ── Status banner ───────────────────────────────────────────────────────── */
char    s_status_msg[64]    = {0};
int64_t s_status_until_ms   = 0;

void ui_status_set(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_status_msg, sizeof(s_status_msg), fmt, ap);
    va_end(ap);
    s_status_until_ms = (esp_timer_get_time() / 1000) + 3000;
}

/* ── Synth editor ────────────────────────────────────────────────────────── */
int s_se_tab  = 0;
int s_se_lane = 0;

/* long-press detection for lane row ctx-menu */
static int     s_lp_lane     = -1;   /* lane being long-pressed */
static int64_t s_lp_start_ms = 0;    /* touch-down time in ms */

int     s_dg_last_row    = -1;
int     s_dg_last_step   = -1;
int64_t s_dg_last_tap_ms = 0;
int     s_dg_vel_row     = -1;
int     s_dg_vel_step    = -1;
int     s_dg_vel_value   = 100;

bool s_touch_down = false;
int  s_down_x = 0, s_down_y = 0;
int  s_drag_x = 0, s_drag_y = 0;

/* Touch highlight: drawn as a bright flash rect for one frame on finger-down */
int  s_hl_x = 0, s_hl_y = 0, s_hl_w = 0, s_hl_h = 0;
bool s_hl_pending = false;   /* highlight drawn, action not yet fired */
bool s_hl_visible = false;   /* currently visible (draw this frame) */

int     s_fader_drag_lane    = -1;
float   s_fader_drag_start_v = 0.0f;
int     s_fader_last_tap_lane = -1;
int     s_fader_last_tap_x    = 0;
int64_t s_fader_last_tap_ms   = 0;

/* ── Inline volume panel ─────────────────────────────────────────────────── */
/* s_vol_open_lane: absolute lane index whose vol panel is open, -1 = none.
 * When open, the panel is rendered as an extra LANE_VOL_H-pixel strip
 * immediately below that lane's row.  All rows after it are shifted down. */
#define LANE_VOL_H  88   /* height of the expanded vol/bars panel */
int s_vol_open_lane = -1;
static bool s_vol_fader_drag = false;   /* dragging the big fader */
/* Anchor for row-step scroll handlers (SONG view, FX picker). Unlike s_drag_y
 * — which is reset every move frame for per-frame-delta handlers — this is only
 * re-anchored when a scroll step actually fires, so drag distance accumulates. */
static int  s_scroll_anchor_y = 0;

int      s_pr_view_semitone  = 64;
int      s_pr_drag_note      = -1;
int      s_pr_drag_orig_row  = -1;
uint32_t s_pr_drag_orig_tick = 0;
int      s_pr_resize_note    = -1;
uint32_t s_pr_tick_offset    = 0;
uint32_t s_pr_ticks_wide     = 0;  /* 0 = fit all bars */
bool     s_pr_delete_mode    = false;
static int s_pr_lp_note      = -1;   /* note index being long-pressed for delete */
#define PR_AUD_MAX 5
static int8_t s_pr_audition[PR_AUD_MAX];  /* notes held via key strip, -1=empty */
static int    s_pr_aud_cnt = 0;

/* ══════════════════════════════════════════════════════════════════════════
 * Touch helpers
 * ══════════════════════════════════════════════════════════════════════════ */

static inline int clamp_i(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

static inline float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

static bool pt_in(int x, int y, int rx, int ry, int rw, int rh)
{
    return (x >= rx && x < rx + rw && y >= ry && y < ry + rh);
}

/* ── Forward declarations for OSK callbacks + synth editor ──────────────── */
static void osk_cb_lane_rename(const char *text);
static void osk_cb_song_save_as(const char *text);
static void osk_cb_new_song(const char *text);
static bool handle_synth_edit_tap(int x, int y);

/* ── Tab bar tap ─────────────────────────────────────────────────────────── */
static bool handle_tabbar_tap(int x, int y)
{
    if (!pt_in(x, y, 0, TABBAR_Y, 1280, TABBAR_H)) return false;
    int tab = x / TAB_W;
    if (tab < 0 || tab >= TAB_COUNT) return false;
    s_active_tab = tab;

    switch (tab) {
    case 0: /* SONG */
        s_nav_top      = 0;
        s_nav_stack[0] = SCREEN_SONG;
        break;
    case 1: /* DRUM — push drum grid for last active drum lane */
        s_nav_top      = 0;
        s_nav_stack[0] = SCREEN_SONG;
        for (int i = 0; i < NUM_LANES; i++) {
            if (g_song.lanes[i].active && g_song.lanes[i].type == LANE_TYPE_DRUM) {
                s_ctx_lane = i;
                push_screen(SCREEN_DRUM_GRID);
                break;
            }
        }
        break;
    case 2: /* ROLL — push piano roll for last active synth lane */
        s_nav_top      = 0;
        s_nav_stack[0] = SCREEN_SONG;
        for (int i = 0; i < NUM_LANES; i++) {
            if (g_song.lanes[i].active && g_song.lanes[i].type == LANE_TYPE_SYNTH) {
                s_ctx_lane = i;
                push_screen(SCREEN_PIANO_ROLL);
                break;
            }
        }
        break;
    case 3: /* MASTER */
        s_nav_top      = 0;
        s_nav_stack[0] = SCREEN_MASTER;
        break;
    case 4: /* FX — push FX screen for last context lane */
        s_nav_top      = 0;
        s_nav_stack[0] = SCREEN_SONG;
        s_fx_sel_slot  = 0;
        s_fx_target    = s_ctx_lane;
        push_screen(SCREEN_FX_ADSR);
        break;
    case 5: /* LIVE */
        s_nav_top      = 0;
        s_nav_stack[0] = SCREEN_LIVE;
        break;
    case 6: /* SONGS */
        s_nav_top      = 0;
        s_nav_stack[0] = SCREEN_SONG;
        s_song_browser_dirty = true;
        push_screen(SCREEN_SONG_BROWSER);
        break;
    case 7: /* MENU */
        s_nav_top      = 0;
        s_nav_stack[0] = SCREEN_MENU;
        break;
    default: break;
    }
    return true;
}

/* ── Mini bar back button ────────────────────────────────────────────────── */
static bool handle_minibar_back(int x, int y)
{
    if (!is_main_screen(current_screen()) &&
        pt_in(x, y, 0, 0, MINIBAR_BACK_W, MINIBAR_H)) {
        pop_screen();
        return true;
    }
    return false;
}

/* ── SONG VIEW taps ──────────────────────────────────────────────────────── */
static bool handle_song_view_tap(int x, int y)
{
    int rx_chev      = 1280 - LANE_CHEV_W;
    int rx_fx        = rx_chev - LANE_FX_W;
    int rx_solo_base = rx_fx - (LANE_MUTE_W + 16);

    int abs_row  = 0;
    int y_cursor = CONTENT_Y;
    for (int i = 0; i < NUM_LANES; i++) {
        if (!g_song.lanes[i].active) continue;
        if (abs_row < s_song_scroll) { abs_row++; continue; }
        int ry = y_cursor;
        if (ry + LANE_ROW_H > CONTENT_Y + SONG_BODY_H) break;

        /* ── Vol panel strip (below this row when open) ────────────────── */
        if (i == s_vol_open_lane && y >= ry + LANE_ROW_H &&
            y < ry + LANE_ROW_H + LANE_VOL_H) {
            int py = ry + LANE_ROW_H;
            uint32_t bar_t = CLOCK_BAR_TICKS(&g_song.clock);
            int  fx  = LANE_COLOR_W + 16, fw = 720;
            /* Fader scrub */
            if (pt_in(x, y, fx, py + 8, fw, 40)) {
                float v = (float)(x - fx) / fw;
                if (v < 0.0f) v = 0.0f;
                if (v > 1.0f) v = 1.0f;
                g_song.lanes[i].volume = v;
                g_song.dirty = true;
                ws_notify_change(WS_MSG_LANE_UPDATE, i);
                return true;
            }
            /* BARS buttons */
            int bx = fx + fw + 24;
            int cur_bars = (bar_t > 0 && g_song.lanes[i].loop_len_ticks > 0)
                           ? (int)(g_song.lanes[i].loop_len_ticks / bar_t) : 2;
            if (pt_in(x, y, bx,       py + 30, 48, 48) && cur_bars > 1) {
                g_song.lanes[i].loop_len_ticks = (uint32_t)((cur_bars - 1) * bar_t);
                g_song.dirty = true;
                ws_notify_change(WS_MSG_LANE_UPDATE, i);
                return true;
            }
            if (pt_in(x, y, bx + 108, py + 30, 48, 48) && cur_bars < 64) {
                g_song.lanes[i].loop_len_ticks = (uint32_t)((cur_bars + 1) * bar_t);
                g_song.dirty = true;
                ws_notify_change(WS_MSG_LANE_UPDATE, i);
                return true;
            }
            return true;  /* ate tap inside panel */
        }

        if (y >= ry && y < ry + LANE_ROW_H) {
            bool is_synth = (g_song.lanes[i].type == LANE_TYPE_SYNTH);
            int rx_edit = rx_fx - LANE_EDIT_W;
            int rx_solo = is_synth ? rx_edit - (LANE_MUTE_W + 16) : rx_solo_base;
            int rx_mute = rx_solo - (LANE_MUTE_W + 8);

            /* VOL button — fixed position, centred vertically */
            int vol_bx = 620;
            int vol_by = ry + LANE_ROW_H / 2 - 18;
            if (pt_in(x, y, vol_bx, vol_by, 88, 36)) {
                s_vol_open_lane = (s_vol_open_lane == i) ? -1 : i;
                return true;
            }
            /* Mute button */
            if (pt_in(x, y, rx_mute, ry, LANE_MUTE_W + 8, LANE_ROW_H)) {
                g_song.lanes[i].mute = !g_song.lanes[i].mute;
                g_song.dirty = true;
                ws_notify_change(WS_MSG_LANE_UPDATE, i);
                return true;
            }
            /* Solo button */
            if (pt_in(x, y, rx_solo, ry, LANE_MUTE_W + 16, LANE_ROW_H)) {
                bool new_solo = !g_song.lanes[i].solo;
                for (int j = 0; j < NUM_LANES; j++)
                    g_song.lanes[j].solo = false;
                g_song.lanes[i].solo = new_solo;
                g_song.dirty = true;
                for (int j = 0; j < NUM_LANES; j++)
                    if (g_song.lanes[j].active)
                        ws_notify_change(WS_MSG_LANE_UPDATE, j);
                return true;
            }
            /* SYNTH EDIT button */
            if (is_synth && pt_in(x, y, rx_edit, ry, LANE_EDIT_W, LANE_ROW_H)) {
                s_ctx_lane = i;
                s_se_lane  = i;
                s_se_tab   = 0;
                push_screen(SCREEN_SYNTH_EDIT);
                return true;
            }
            /* FX button */
            if (pt_in(x, y, rx_fx, ry, LANE_FX_W, LANE_ROW_H)) {
                s_ctx_lane    = i;
                s_fx_sel_slot = 0;
                s_fx_target   = i;
                push_screen(SCREEN_FX_ADSR);
                return true;
            }
            /* Chevron — open editor sub-screen */
            if (pt_in(x, y, rx_chev, ry, LANE_CHEV_W, LANE_ROW_H)) {
                s_ctx_lane = i;
                if (g_song.lanes[i].type == LANE_TYPE_DRUM)
                    push_screen(SCREEN_DRUM_GRID);
                else if (is_synth)
                    push_screen(SCREEN_PIANO_ROLL);
                else if (g_song.lanes[i].type == LANE_TYPE_WAV)
                    push_screen(SCREEN_WAV_DETAIL);
                return true;
            }
            /* Row body tap — close panel or open editor */
            if (s_vol_open_lane == i) {
                s_vol_open_lane = -1;
                return true;
            }
            s_ctx_lane = i;
            if (g_song.lanes[i].type == LANE_TYPE_DRUM)
                push_screen(SCREEN_DRUM_GRID);
            else if (is_synth)
                push_screen(SCREEN_PIANO_ROLL);
            else if (g_song.lanes[i].type == LANE_TYPE_WAV)
                push_screen(SCREEN_WAV_DETAIL);
            return true;
        }

        y_cursor += LANE_ROW_H;
        if (i == s_vol_open_lane) y_cursor += LANE_VOL_H;
        abs_row++;
    }

    /* "+ ADD LANE" row */
    {
        int add_ry = y_cursor;
        if (y >= add_ry && y < add_ry + LANE_ADD_H) {
            /* determine which button was tapped: SYNTH | Drum/Sample */
            lane_type_t new_type;
            if (x < 652) new_type = LANE_TYPE_SYNTH;
            else         new_type = LANE_TYPE_DRUM;

            for (int i = 0; i < NUM_LANES; i++) {
                if (!g_song.lanes[i].active) {
                    lane_t *nl         = &g_song.lanes[i];
                    nl->active         = true;
                    nl->type           = new_type;
                    nl->volume         = 1.0f;
                    nl->pan            = 0.0f;
                    nl->loop_len_ticks = CLOCK_BAR_TICKS(&g_song.clock) * 2;
                    if (new_type == LANE_TYPE_SYNTH) {
                        if (!nl->synth)
                            nl->synth = synth_new(SYNTH_TYPE_POLY_WT);
                        if (!nl->piano_roll)
                            nl->piano_roll = piano_roll_alloc();
                        arp_init(&nl->arp);
                        /* Auto-name "Synth N" — count existing synth lanes */
                        int sn = 0;
                        for (int j = 0; j < NUM_LANES; j++)
                            if (g_song.lanes[j].active && g_song.lanes[j].type == LANE_TYPE_SYNTH)
                                sn++;
                        snprintf(nl->name, sizeof(nl->name), "Synth %d", sn);
                        g_song.dirty = true;
                        ws_notify_change(WS_MSG_LANE_UPDATE, i);
                        /* Go straight to SYNTH EDIT so user can pick type immediately */
                        s_ctx_lane = i;
                        s_se_lane  = i;
                        s_se_tab   = 0;
                        push_screen(SCREEN_SYNTH_EDIT);
                    } else if (new_type == LANE_TYPE_DRUM) {
                        if (!nl->drum_seq) {
                            nl->drum_seq = drum_seq_alloc();
                            if (nl->drum_seq)
                                drum_seq_update_timing(nl->drum_seq, nl->loop_len_ticks);
                        }
                        /* Auto-name "Drum N" — count existing drum lanes */
                        int dn = 0;
                        for (int j = 0; j < NUM_LANES; j++)
                            if (g_song.lanes[j].active && g_song.lanes[j].type == LANE_TYPE_DRUM)
                                dn++;
                        snprintf(nl->name, sizeof(nl->name), "Drum %d", dn);
                        g_song.dirty = true;
                        ws_notify_change(WS_MSG_LANE_UPDATE, i);
                        s_ctx_lane = i;
                        push_screen(SCREEN_DRUM_GRID);
                    }
                    break;
                }
            }
            return true;
        }
    }

    /* Scene strip (sits between lane list and master strip) */
    {
        int scn_y = MASTER_Y - 52;
        int bw = 108, gap = 8, x0 = 80;
        if (pt_in(x, y, 0, scn_y, 1280, 52)) {
            for (int i = 0; i < SCENE_MAX; i++) {
                int bx = x0 + i * (bw + gap);
                if (pt_in(x, y, bx, scn_y + 6, bw, 40)) {
                    scene_recall(&g_song, i);
                    ws_notify_change(WS_MSG_LANE_UPDATE, -1);
                    return true;
                }
            }
            return false;
        }
    }

    /* Master strip pan fader */
    {
        int ms_y = MASTER_Y;
        int fader_x = 120, fader_w = 260;
        /* Pan fader is at y+34, height 10 */
        if (pt_in(x, y, fader_x, ms_y + 30, fader_w, 18)) {
            float v = (float)(x - fader_x) / fader_w * 2.0f - 1.0f;
            if (v < -1.0f) v = -1.0f;
            if (v >  1.0f) v =  1.0f;
            g_settings.master_pan = v;
            ws_notify_change(WS_MSG_MASTER_UPDATE, -1);
            return true;
        }
        /* Playback mode toggle in master strip */
        int pb_x = fader_x + fader_w + 12;
        if (pt_in(x, y, pb_x, ms_y + 30, 120, 22)) {
            g_song.playback_mode = (x < pb_x + 60) ? 0 : 1;
            g_song.dirty = true;
            ws_notify_change(WS_MSG_SETTINGS, -1);
            return true;
        }
    }

    return false;
}

/* ── LIVE VIEW taps ──────────────────────────────────────────────────────── */
static bool handle_live_tap(int x, int y)
{
    /* Lane tab bar */
    if (pt_in(x, y, 0, CONTENT_Y, 1280, LIVE_LANE_TAB_H)) {
        int tx = 0, cnt = 0;
        for (int i = 0; i < NUM_LANES; i++) {
            if (!g_song.lanes[i].active) continue;
            char label[16];
            snprintf(label, sizeof(label), "%02d", i + 1);
            int tw = gfx_text_width(label, 2) + 48;
            if (x >= tx && x < tx + tw) { s_live_lane = cnt; s_live_pad_page = 0; return true; }
            tx += tw; cnt++;
        }
    }
    int li = -1, cnt = 0;
    for (int i = 0; i < NUM_LANES; i++) {
        if (!g_song.lanes[i].active) continue;
        if (cnt == s_live_lane) { li = i; break; }
        cnt++;
    }
    if (li < 0) return false;

    /* Drum pad grid — one pad per assigned sample in the lane's song row,
     * paged when there are more than fit on screen. */
    if (g_song.lanes[li].type == LANE_TYPE_DRUM) {
        drum_seq_t *seq = g_song.lanes[li].drum_seq;
        int rows[DRUM_MAX_ROWS];
        int npads = live_drum_collect(seq, rows, DRUM_MAX_ROWS);
        if (npads == 0) return false;

        int pages    = (npads + LIVE_PAD_PER_PAGE - 1) / LIVE_PAD_PER_PAGE;
        bool has_bar = pages > 1;
        if (s_live_pad_page >= pages) s_live_pad_page = pages - 1;
        if (s_live_pad_page < 0)      s_live_pad_page = 0;

        /* Page strip: left third = prev, right third = next */
        if (has_bar) {
            int bar_y = TABBAR_Y - LIVE_PAGE_BAR_H;
            if (pt_in(x, y, 0, bar_y, 1280, LIVE_PAGE_BAR_H)) {
                if (x < 1280 / 3) {
                    if (s_live_pad_page > 0) s_live_pad_page--;
                } else if (x >= 1280 * 2 / 3) {
                    if (s_live_pad_page < pages - 1) s_live_pad_page++;
                }
                return true;
            }
        }

        int base = s_live_pad_page * LIVE_PAD_PER_PAGE;
        for (int slot = 0; slot < LIVE_PAD_PER_PAGE; slot++) {
            int idx = base + slot;
            if (idx >= npads) break;
            int px, py, pw, ph;
            live_drum_pad_rect(has_bar, slot, &px, &py, &pw, &ph);
            if (pt_in(x, y, px, py, pw, ph)) {
                int r = rows[idx];
                if (!seq->rows[r].mute) {
                    /* dispatch locally so the audio_task handles it uniformly.
                     * 4-byte frame: cmd, lane, row, velocity. */
                    uint8_t trig[4] = { WS_CMD_DRUM_TRIGGER, (uint8_t)li,
                                        (uint8_t)r, 110 };
                    ws_cmd_dispatch(trig, sizeof(trig), -1);
                }
                return true;
            }
        }
    }

    /* Synth piano oct buttons */
    if (g_song.lanes[li].type == LANE_TYPE_SYNTH) {
        int tb_y = TABBAR_Y - 72;
        if (pt_in(x, y, 24, tb_y + 8, 120, 56) && s_live_octave > 1) {
            s_live_octave--;
            setup_live_piano_keys();
            return true;
        }
        if (pt_in(x, y, 1280 - 144, tb_y + 8, 120, 56) && s_live_octave < 7) {
            s_live_octave++;
            setup_live_piano_keys();
            return true;
        }
    }
    return false;
}

/* ── MASTER VIEW taps ────────────────────────────────────────────────────── */
static bool handle_master_tap(int x, int y)
{
    int col1_w = 340, col3_w = 320;
    int col2_x = col1_w + 2;
    int col2_w = 1280 - col1_w - col3_w - 4;
    int col3_x = col2_x + col2_w + 2;

    /* Col 1: Playback mode seg */
    {
        int fdr_h = 200, fdr_y = CONTENT_Y + 52;
        int pan_fy = fdr_y + fdr_h + 60;
        int pb_y2  = pan_fy + 52;
        if (pt_in(x, y, 24, pb_y2 + 18, col1_w - 48, 44)) {
            int sel = (x - 24) / ((col1_w - 48) / 2);
            g_song.playback_mode = (sel == 1) ? 1 : 0;
            g_song.dirty = true;
            ws_notify_change(WS_MSG_SETTINGS, -1);
            return true;
        }
    }

    /* Col 2: Master FX slots — open the full FX editor (target = master). */
    {
        int mfx_slots   = FX_MAX_PER_LANE;
        int mfx_slot_w  = (col2_w - 48 - (mfx_slots - 1) * 10) / mfx_slots;
        int sy = CONTENT_Y + 52;
        for (int i = 0; i < mfx_slots; i++) {
            int sx2 = col2_x + 24 + i * (mfx_slot_w + 10);
            if (pt_in(x, y, sx2, sy, mfx_slot_w, 80)) {
                s_fx_target   = FX_TGT_MASTER;
                s_fx_sel_slot = (i < g_song.master_fx_count) ? i
                                                             : g_song.master_fx_count;
                push_screen(SCREEN_FX_ADSR);
                if (i >= g_song.master_fx_count) {
                    s_fx_picker_open        = true;
                    s_fx_picker_target_slot = g_song.master_fx_count;
                    s_fx_picker_replace     = false;
                    s_fx_picker_scroll      = 0;
                }
                return true;
            }
        }

        /* SEND FX button — sits below the master FX slot row in col 2. */
        if (pt_in(x, y, col2_x + 24, sy + 92, col2_w - 48, 48)) {
            s_fx_target   = FX_TGT_SEND;
            s_fx_sel_slot = 0;
            push_screen(SCREEN_FX_ADSR);
            return true;
        }
    }

    /* Col 3: Tap tempo */
    if (pt_in(x, y, col3_x + 24, CONTENT_Y + 172, col3_w - 48, 72)) {
        clock_tap(&g_song.clock);
        ws_notify_change(WS_MSG_CLOCK_UPDATE, -1);
        return true;
    }
    static const uint32_t ppqn_vals[] = { 24, 48, 96, 192 };
    if (pt_in(x, y, col3_x + 24, CONTENT_Y + 300, col3_w - 48, 52)) {
        int sel = (x - (col3_x + 24)) / ((col3_w - 48) / 4);
        if (sel >= 0 && sel < 4) {
            g_song.clock.tick_rate = ppqn_vals[sel];
            g_settings.ppqn       = ppqn_vals[sel];
            g_song.dirty          = true;
            ws_notify_change(WS_MSG_CLOCK_UPDATE, -1);
        }
        return true;
    }
    static const uint32_t tsig_beats[] = { 4, 3, 6 };
    if (pt_in(x, y, col3_x + 24, CONTENT_Y + 408, col3_w - 48, 52)) {
        int sel = (x - (col3_x + 24)) / ((col3_w - 48) / 3);
        if (sel >= 0 && sel < 3) {
            g_song.clock.beats_per_bar = tsig_beats[sel];
            g_settings.beats_per_bar   = tsig_beats[sel];
            g_song.dirty               = true;
            ws_notify_change(WS_MSG_CLOCK_UPDATE, -1);
        }
        return true;
    }
    /* Col 3: Export record/stop button */
    if (pt_in(x, y, col3_x + 24, CONTENT_Y + 480, col3_w - 48, 56)) {
        if (!render_export_active()) {
            char path[128];
            const char *base = g_song.name[0] ? g_song.name : "mix";
            snprintf(path, sizeof(path), SONG_DIR "/%s_mix.wav", base);
            render_export_start(path);
        } else {
            render_export_stop();
        }
        return true;
    }
    return false;
}

/* ── MENU taps ───────────────────────────────────────────────────────────── */
static bool handle_menu_tap(int x, int y)
{
    int cw = 1280 / 4;
    int ch = CONTENT_H / 3;
    if (!pt_in(x, y, 0, CONTENT_Y, 1280, CONTENT_H)) return false;
    int col  = x / cw;
    int row  = (y - CONTENT_Y) / ch;
    int item = row * 4 + col;

    switch (item) {
    case 0: {
        /* Ask for song name before creating it */
        snprintf(s_osk_title, sizeof(s_osk_title), "New song name");
        s_osk_buf[0] = '\0';
        s_osk_len    = 0;
        s_osk_done_cb = osk_cb_new_song;
        push_screen(SCREEN_OSK);
        return true;
    }
    case 1: push_screen(SCREEN_SONG_BROWSER); s_song_browser_dirty = true; return true;
    case 2: {
        /* Quick save — use current song name, fall back to SAVE AS if still "Untitled" */
        if (!g_song.name[0] || strcmp(g_song.name, "Untitled") == 0) {
            /* No name set — open OSK so user names the song first */
            snprintf(s_osk_title, sizeof(s_osk_title), "Song name");
            strncpy(s_osk_buf, g_song.name[0] ? g_song.name : "", OSK_MAX_LEN);
            s_osk_buf[OSK_MAX_LEN] = '\0';
            s_osk_len = (int)strlen(s_osk_buf);
            s_osk_done_cb = osk_cb_song_save_as;
            push_screen(SCREEN_OSK);
        } else {
            char path[160];
            snprintf(path, sizeof(path), "%s/%s%s", SONG_DIR, g_song.name, SONG_EXT);
            if (song_save(path)) {
                strncpy(g_settings.last_song, path, sizeof(g_settings.last_song) - 1);
                settings_save();
                s_song_browser_dirty = true;
                ui_status_set("Saved: %s%s", g_song.name, SONG_EXT);
            } else {
                ui_status_set("Save FAILED — see serial log");
            }
        }
        return true;
    }
    case 3: {
        /* Save As — open OSK to name the song */
        snprintf(s_osk_title, sizeof(s_osk_title), "Save song as...");
        strncpy(s_osk_buf, g_song.name[0] ? g_song.name : "", OSK_MAX_LEN);
        s_osk_buf[OSK_MAX_LEN] = '\0';
        s_osk_len = (int)strlen(s_osk_buf);
        s_osk_done_cb = osk_cb_song_save_as;
        push_screen(SCREEN_OSK);
        return true;
    }
    case 5: push_screen(SCREEN_SETTINGS); return true;
    case 6: push_screen(SCREEN_BLUETOOTH); return true;
    case 8: push_screen(SCREEN_ARP);         return true;
    case 9: push_screen(SCREEN_GROOVE);      return true;
    case 10: push_screen(SCREEN_ARRANGEMENT); return true;
    case 11: push_screen(SCREEN_NOTE_REPEAT); return true;
    default: break;
    }
    return false;
}

/* Append a blank drum row and immediately open the sound browser so the user
 * can assign a sample. Shared by the minibar +ROW button. */
static bool drum_grid_add_row(drum_seq_t *seq)
{
    if (!seq || seq->row_count >= DRUM_MAX_ROWS) return false;
    int ri = seq->row_count;
    memset(&seq->rows[ri], 0, sizeof(drum_row_t));
    seq->rows[ri].volume     = 1.0f;
    seq->rows[ri].step_count = (uint8_t)seq->step_count;
    seq->row_count++;
    g_song.dirty = true;
    ws_notify_change(WS_MSG_DRUM_STEP, (s_ctx_lane << 8) | ri);
    s_ctx_drum_row = ri;
    if (s_sb_dirty) {
        sb_load_kits();
        s_sb_kit_sel = 0; s_sb_file_sel = 0;
        s_sb_kit_scroll = 0; s_sb_file_scroll = 0; s_sb_kit_px = 0.0f; s_sb_file_px = 0.0f; s_sb_kit_vel = 0.0f; s_sb_file_vel = 0.0f;
        sb_load_files(0);
        s_sb_dirty = false;
    }
    push_screen(SCREEN_SOUND_BROWSER);
    return true;
}

/* ── DRUM GRID taps ──────────────────────────────────────────────────────── */
static bool handle_drum_grid_tap(int x, int y)
{
    if (handle_minibar_back(x, y)) return true;

    lane_t     *lane = &g_song.lanes[s_ctx_lane];
    drum_seq_t *seq  = lane->drum_seq;
    if (!seq) return false;

    /* ── Minibar STEPS/BARS +/- buttons ─────────────────────────────────── */
    if (y < MINIBAR_H) {
        uint32_t bar_t = CLOCK_BAR_TICKS(&g_song.clock);
        int cur_bars  = (lane->loop_len_ticks && bar_t)
                        ? (int)(lane->loop_len_ticks / bar_t) : 2;
        /* BARS − */
        if (pt_in(x, y, 900, 10, 48, 52) && cur_bars > 1) {
            cur_bars--;
            lane->loop_len_ticks = (uint32_t)(cur_bars * bar_t);
            drum_seq_update_timing(seq, lane->loop_len_ticks);
            g_song.dirty = true;
            ws_notify_change(WS_MSG_LANE_UPDATE, s_ctx_lane);
            return true;
        }
        /* BARS + */
        if (pt_in(x, y, 956, 10, 48, 52) && cur_bars < 8) {
            cur_bars++;
            lane->loop_len_ticks = (uint32_t)(cur_bars * bar_t);
            drum_seq_update_timing(seq, lane->loop_len_ticks);
            g_song.dirty = true;
            ws_notify_change(WS_MSG_LANE_UPDATE, s_ctx_lane);
            return true;
        }
        /* STEPS −  (rescale the pattern so existing hits keep their timing) */
        if (pt_in(x, y, 1152, 10, 48, 52) && seq->step_count > 4) {
            drum_seq_set_step_count(seq, seq->step_count - 4, lane->loop_len_ticks);
            g_song.dirty = true;
            ws_notify_change(WS_MSG_DRUM_STEP, s_ctx_lane << 8);
            return true;
        }
        /* STEPS +  (rescale the pattern so existing hits keep their timing) */
        if (pt_in(x, y, 1208, 10, 48, 52) && seq->step_count < DRUM_MAX_STEPS) {
            drum_seq_set_step_count(seq, seq->step_count + 4, lane->loop_len_ticks);
            g_song.dirty = true;
            ws_notify_change(WS_MSG_DRUM_STEP, s_ctx_lane << 8);
            return true;
        }
        /* +ROW (moved up from the toolbar to free space) */
        if (pt_in(x, y, 380, 10, 150, 52)) {
            drum_grid_add_row(seq);
            return true;
        }
        /* EUCL (moved up from the toolbar to free space) */
        if (pt_in(x, y, 540, 10, 150, 52)) {
            s_eucl_popup = !s_eucl_popup;
            return true;
        }
        return false;
    }

    int steps      = seq->step_count;
    int body_y     = MINIBAR_H + DG_BEAT_H;
    int body_h     = SUB_CONTENT_H - DG_BEAT_H - DG_TOOLBAR_H;
    int total_rows = seq->row_count;
    int rows_vis   = body_h / (DG_STEP_H + 16);
    if (rows_vis < 1) rows_vis = 1;
    if (rows_vis > total_rows) rows_vis = total_rows;
    if (rows_vis == 0) rows_vis = 1;
    int row_h = body_h / rows_vis;

    /* toolbar scroll chevrons + row-level buttons */
    int tb_y = SUB_CONTENT_H - DG_TOOLBAR_H + MINIBAR_H;
    if (pt_in(x, y, 1036, tb_y + 8, 104, 56)) {
        if (s_dg_row_offset > 0) { s_dg_row_offset--; return true; }
        return false;
    }
    if (pt_in(x, y, 1152, tb_y + 8, 104, 56)) {
        if (s_dg_row_offset + rows_vis < total_rows) { s_dg_row_offset++; return true; }
        return false;
    }

    /* CLEAR toolbar button — wipe every row's step pattern */
    if (pt_in(x, y, 24, tb_y + 8, 152, 56)) {
        for (int r = 0; r < seq->row_count; r++)
            memset(seq->rows[r].steps, 0, sizeof(seq->rows[r].steps));
        g_song.dirty = true;
        ws_notify_change(WS_MSG_DRUM_STEP, s_ctx_lane << 8);
        return true;
    }

    /* Euclidean popup: GEN button or +/- adjusters */
    if (s_eucl_popup) {
        int ep_x = 160, ep_y = 300, ep_w = 960, ep_h = 200;
        /* GEN button */
        if (pt_in(x, y, ep_x + ep_w - 200, ep_y + ep_h - 60, 160, 44)) {
            /* Generate over the visible (global) step count so the result lines
             * up with the grid; apply to the top visible row. */
            int ri = (s_dg_row_offset < seq->row_count) ? s_dg_row_offset : 0;
            if (seq->row_count > 0)
                drum_seq_euclidean(&seq->rows[ri], s_eucl_hits, seq->step_count, 100);
            s_eucl_popup = false;
            g_song.dirty = true;
            ws_notify_change(WS_MSG_DRUM_STEP, (s_ctx_lane << 8) | ri);
            return true;
        }
        /* Hits -/+ (clamped to the current global step count) */
        if (pt_in(x, y, ep_x + 280, ep_y + 80, 52, 52)) {
            if (s_eucl_hits > 1) s_eucl_hits--;
            return true;
        }
        if (pt_in(x, y, ep_x + 340, ep_y + 80, 52, 52)) {
            if (s_eucl_hits < seq->step_count) s_eucl_hits++;
            return true;
        }
        /* Tap outside to dismiss */
        if (!pt_in(x, y, ep_x, ep_y, ep_w, ep_h)) {
            s_eucl_popup = false;
            return true;
        }
        return true;
    }

    /* Label area — mute button or tap to assign WAV */
    if (x >= 0 && x < DG_LABEL_W && y > body_y && y < body_y + rows_vis * row_h) {
        int vis_ri = (y - body_y) / row_h;
        int abs_ri = vis_ri + s_dg_row_offset;
        if (abs_ri < total_rows) {
            /* mute button: right 38 px of label area */
            int mute_bx = DG_LABEL_W - 38;
            int ry2 = body_y + vis_ri * row_h;
            int mute_by = ry2 + (row_h - 28) / 2;
            if (pt_in(x, y, mute_bx, mute_by, 28, 28)) {
                seq->rows[abs_ri].mute = !seq->rows[abs_ri].mute;
                g_song.dirty = true;
                ws_notify_change(WS_MSG_DRUM_STEP, (s_ctx_lane << 8) | abs_ri);
                return true;
            }
            /* Tap label body → open sound browser for this row */
            s_ctx_drum_row = abs_ri;
            if (s_sb_dirty) { sb_load_kits(); s_sb_kit_sel = 0; s_sb_file_sel = 0;
                              s_sb_kit_scroll = 0; s_sb_file_scroll = 0; s_sb_kit_px = 0.0f; s_sb_file_px = 0.0f; s_sb_kit_vel = 0.0f; s_sb_file_vel = 0.0f;
                              sb_load_files(0); s_sb_dirty = false; }
            push_screen(SCREEN_SOUND_BROWSER);
            return true;
        }
    }

    /* Velocity overlay tap handling */
    if (s_dg_vel_row >= 0) {
        /* SET button: y=530..582 */
        if (pt_in(x, y, 440, 530, 400, 52)) {
            drum_step_t *vst = &seq->rows[s_dg_vel_row].steps[s_dg_vel_step];
            vst->velocity = (uint8_t)s_dg_vel_value;
            g_song.dirty = true;
            ws_notify_change(WS_MSG_DRUM_STEP, (s_ctx_lane << 8) | s_dg_vel_row);
            s_dg_vel_row = -1;
        } else if (!pt_in(x, y, 120, 360, 1040, 240)) {
            /* tapping outside the overlay cancels */
            s_dg_vel_row = -1;
        }
        /* Tapping inside the slider area updates value */
        if (s_dg_vel_row >= 0 && pt_in(x, y, 160, 436, 960, 34)) {
            int sv = (x - 160) * 126 / 960 + 1;
            if (sv < 1) sv = 1;
            if (sv > 127) sv = 127;
            s_dg_vel_value = sv;
        }
        return true;
    }

    /* step grid */
    if (x > DG_LABEL_W && y > body_y && y < body_y + rows_vis * row_h) {
        int vis_ri = (y - body_y) / row_h;
        int abs_ri = vis_ri + s_dg_row_offset;
        if (vis_ri >= 0 && vis_ri < rows_vis && abs_ri < total_rows) {
            int sw = (1280 - DG_LABEL_W - 4 - 4 * (steps - 1)) / steps;
            if (sw > DG_STEP_W) sw = DG_STEP_W;
            if (sw < 8) sw = 8;
            int si = (x - DG_LABEL_W - 4) / (sw + 4);
            if (si >= 0 && si < steps) {
                int64_t now_ms = esp_timer_get_time() / 1000;
                bool is_double = (s_dg_last_row == abs_ri && s_dg_last_step == si &&
                                  now_ms - s_dg_last_tap_ms < 400);
                drum_step_t *step = &seq->rows[abs_ri].steps[si];

                /* Arm long-press detection (main loop checks s_lp_lane==-100) */
                s_lp_lane     = -100;
                s_lp_start_ms = now_ms;

                if (is_double && step->velocity > 0) {
                    /* Double-tap: toggle accent */
                    step->accent = !step->accent;
                    s_dg_last_row = -1; /* reset so triple-tap doesn't re-trigger */
                } else {
                    s_dg_last_row    = abs_ri;
                    s_dg_last_step   = si;
                    s_dg_last_tap_ms = now_ms;
                    if (step->velocity == 0) step->velocity = 100;
                    else { step->velocity = 0; step->accent = false; }
                }
                g_song.dirty = true;
                ws_notify_change(WS_MSG_DRUM_STEP, (s_ctx_lane << 8) | abs_ri);
                return true;
            }
        }
    }
    return false;
}

/* ── PIANO ROLL helpers ──────────────────────────────────────────────────── */
/* Convert pixel x in the roll grid to a tick, clamped within [0, total_ticks) */
static uint32_t pr_x_to_tick(int px, int roll_w, uint32_t view_wide, uint32_t total_ticks)
{
    if (roll_w <= 0) return 0;
    int64_t t = (int64_t)(px - PR_KEY_W) * (int64_t)view_wide / roll_w
                + (int64_t)s_pr_tick_offset;
    if (t < 0) t = 0;
    if ((uint32_t)t >= total_ticks) t = (int64_t)(total_ticks - 1);
    return (uint32_t)t;
}

static int pr_tick_to_x(uint32_t tick, int roll_w, uint32_t view_wide)
{
    return PR_KEY_W + (int)((int64_t)(tick - s_pr_tick_offset) * roll_w / (int64_t)view_wide);
}

/* ── PIANO ROLL taps ─────────────────────────────────────────────────────── */
static bool handle_piano_roll_tap(int x, int y)
{
    if (handle_minibar_back(x, y)) return true;

    lane_t       *lane = &g_song.lanes[s_ctx_lane];
    piano_roll_t *pr   = lane->piano_roll;
    if (!pr) return false;

    /* EDIT button in minibar (x = 880..1080) for synth lanes */
    if (lane->type == LANE_TYPE_SYNTH && pt_in(x, y, 880, 0, 200, MINIBAR_H)) {
        s_se_lane = s_ctx_lane;
        s_se_tab  = 0;
        push_screen(SCREEN_SYNTH_EDIT);
        return true;
    }

    int body_y  = MINIBAR_H + PR_BAR_H;
    int body_h  = 720 - body_y - PR_TOOLBAR_H;
    int tb_y    = 720 - PR_TOOLBAR_H;
    int roll_w  = 1280 - PR_KEY_W;
    int rows_vis = body_h / PR_ROW_H;

    uint32_t bar_t      = CLOCK_BAR_TICKS(&g_song.clock);
    int      total_bars = (lane->loop_len_ticks && bar_t) ?
                          (int)(lane->loop_len_ticks / bar_t) : 2;
    if (total_bars < 1) total_bars = 2;
    uint32_t total_ticks = (uint32_t)(total_bars * (int)bar_t);
    uint32_t view_wide   = s_pr_ticks_wide ? s_pr_ticks_wide : total_ticks;
    if (view_wide < bar_t / 16) view_wide = bar_t / 16;
    if (view_wide > total_ticks) view_wide = total_ticks;
    uint32_t snap = bar_t / 16;   /* 1/16th note */

    /* ── Toolbar buttons ── */
    if (pt_in(x, y, 24, tb_y + 10, 120, 56)) {
        if (s_pr_view_semitone < 120) s_pr_view_semitone += 12;
        return true;
    }
    if (pt_in(x, y, 260, tb_y + 10, 120, 56)) {
        if (s_pr_view_semitone > 12)  s_pr_view_semitone -= 12;
        return true;
    }
    /* zoom out */
    if (pt_in(x, y, 420, tb_y + 10, 80, 56)) {
        uint32_t nw = view_wide * 2;
        if (nw > total_ticks) nw = total_ticks;
        s_pr_ticks_wide = nw;
        if (s_pr_tick_offset + nw > total_ticks)
            s_pr_tick_offset = total_ticks > nw ? total_ticks - nw : 0;
        return true;
    }
    /* zoom in */
    if (pt_in(x, y, 516, tb_y + 10, 80, 56)) {
        uint32_t nw = view_wide / 2;
        if (nw < bar_t / 4) nw = bar_t / 4;
        s_pr_ticks_wide = nw;
        return true;
    }
    /* scroll left */
    if (pt_in(x, y, 620, tb_y + 10, 80, 56)) {
        if (s_pr_tick_offset >= view_wide / 2)
            s_pr_tick_offset -= view_wide / 2;
        else
            s_pr_tick_offset = 0;
        return true;
    }
    /* scroll right */
    if (pt_in(x, y, 716, tb_y + 10, 80, 56)) {
        uint32_t noff = s_pr_tick_offset + view_wide / 2;
        uint32_t max_off = total_ticks > view_wide ? total_ticks - view_wide : 0;
        if (noff > max_off) noff = max_off;
        s_pr_tick_offset = noff;
        return true;
    }
    /* DELETE tool toggle — right side of toolbar */
    if (pt_in(x, y, 820, tb_y + 10, 100, 56)) {
        s_pr_delete_mode = !s_pr_delete_mode;
        return true;
    }

    /* ── Left key strip — handled per-frame for multi-touch (see main loop) ── */
    if (x < PR_KEY_W && y >= body_y && y < body_y + rows_vis * PR_ROW_H)
        return true;  /* consume touch so note grid doesn't fire */

    /* ── Note grid (>= so first column at PR_KEY_W is included) ── */
    if (x >= PR_KEY_W && y >= body_y && y < body_y + rows_vis * PR_ROW_H) {
        uint32_t tap_tick = pr_x_to_tick(x, roll_w, view_wide, total_ticks);
        if (snap > 0) tap_tick = (tap_tick / snap) * snap;
        int row_i = (y - body_y) / PR_ROW_H;
        int note  = s_pr_view_semitone - row_i;
        if (note < 0 || note > 127) return false;

        /* check if tapping an existing note — search all notes, not just matching row,
           so a finger slightly off-row can still hit a note */
        for (int ni = 0; ni < pr->note_count; ni++) {
            pr_note_t *n = &pr->notes[ni];
            /* allow ±1 row tolerance for easier finger targeting */
            int note_row = s_pr_view_semitone - (int)n->note;
            if (note_row < row_i - 1 || note_row > row_i + 1) continue;
            /* x hit-test: clamp drawn nx to PR_KEY_W (same as draw code) */
            int nx     = pr_tick_to_x(n->tick_start, roll_w, view_wide);
            int nx_end = pr_tick_to_x(n->tick_start + n->tick_len, roll_w, view_wide);
            if (nx < PR_KEY_W) nx = PR_KEY_W;  /* match draw clamp */
            if (nx_end < nx + 8) nx_end = nx + 8;
            if (x < nx || x > nx_end) continue;
            /* DEL mode: immediate delete on tap */
            if (s_pr_delete_mode) {
                piano_roll_remove_note(pr, n->tick_start, n->note);
                g_song.dirty = true;
                ws_notify_change(WS_MSG_NOTE_EVENT, s_ctx_lane);
                return true;
            }
            /* resize handle: rightmost 30 px */
            if (x >= nx_end - 30) {
                s_pr_resize_note = ni;
                s_pr_drag_note   = -1;
                s_pr_lp_note     = -1;
                return true;
            }
            /* body: arm long-press delete + drag move */
            s_pr_drag_note      = ni;
            s_pr_drag_orig_tick = n->tick_start;
            s_pr_drag_orig_row  = s_pr_view_semitone - (int)n->note;
            s_pr_resize_note    = -1;
            s_pr_lp_note        = ni;
            s_lp_start_ms       = esp_timer_get_time() / 1000;
            return true;
        }

        /* empty cell — in delete mode do nothing; otherwise place */
        if (!s_pr_delete_mode) {
            piano_roll_add_note(pr, tap_tick, snap > 0 ? snap : 96,
                                (uint8_t)note, 100);
            g_song.dirty = true;
            ws_notify_change(WS_MSG_NOTE_EVENT, s_ctx_lane);
        }
        return true;
    }
    return false;
}

/* Layout constants shared by handle_fx_adsr_tap and the picker overlay. */
#define FX_PICKER_MX  80
#define FX_PICKER_MY  40
#define FX_PICKER_MW  1120
#define FX_PICKER_MH  640
#define FX_PICKER_COLS 6

/* Slider geometry mirrors draw_fx_adsr_screen() exactly. */
#define FX_SLIDER_TRACK_X 240
#define FX_SLIDER_TRACK_W 880
#define FX_SLIDER_ROW_H   52

/* Picker tap: returns true if consumed (and possibly mutates picker state). */
static bool handle_fx_picker_tap(int x, int y)
{
    int mx = FX_PICKER_MX, my = FX_PICKER_MY, mw = FX_PICKER_MW, mh = FX_PICKER_MH;
    /* Tap outside modal closes. */
    if (!pt_in(x, y, mx, my, mw, mh)) {
        s_fx_picker_open    = false;
        s_fx_picker_replace = false;
        return true;
    }
    /* Close X */
    if (pt_in(x, y, mx + mw - 60, my + 14, 44, 36)) {
        s_fx_picker_open    = false;
        s_fx_picker_replace = false;
        return true;
    }
    int cols = FX_PICKER_COLS, item_w = (mw - 48) / cols, item_h = 72;
    int grid_top = my + 64, grid_h = mh - 80, rows_vis = grid_h / item_h;
    if (!pt_in(x, y, mx + 24, grid_top, mw - 48, rows_vis * item_h)) return true;
    int vr  = (y - grid_top) / item_h;
    int c   = (x - (mx + 24)) / item_w;
    if (vr < 0 || vr >= rows_vis || c < 0 || c >= cols) return true;
    int idx = (vr + s_fx_picker_scroll) * cols + c;
    int total = (int)FX_TYPE_COUNT - 1;
    if (idx < 0 || idx >= total) return true;
    int tid = fx_type_for_display(idx);  /* alphabetical order */
    int *fx_count; int notify;
    fx_node_t **chain = fx_target_resolve(s_fx_target, &fx_count, NULL, &notify);
    int slot = s_fx_picker_target_slot;
    fx_node_t *node = fx_new((fx_type_t)tid);
    if (node) {
        bool ok;
        if (s_fx_picker_replace && slot >= 0 && slot < *fx_count && chain[slot]) {
            /* Replace the effect occupying this slot, keeping its position. */
            fx_chain_remove(chain, fx_count, slot);
            ok = fx_chain_insert(chain, fx_count, slot, node);
        } else {
            if (slot < 0 || slot > *fx_count) slot = *fx_count;
            ok = fx_chain_insert(chain, fx_count, slot, node);
        }
        if (ok) {
            s_fx_sel_slot = slot;
            g_song.dirty  = true;
            ws_notify_change(WS_MSG_FX_UPDATE, notify);
        } else {
            node->free(node);
        }
    }
    s_fx_picker_open    = false;
    s_fx_picker_replace = false;
    return true;
}

/* Set the param value for current selected fx from a touch x within slider. */
static void fx_slider_set_from_x(int pi, int touch_x)
{
    int *fx_count;
    fx_node_t **chain = fx_target_resolve(s_fx_target, &fx_count, NULL, NULL);
    if (s_fx_sel_slot >= *fx_count || !chain[s_fx_sel_slot]) return;
    fx_node_t *node = chain[s_fx_sel_slot];
    const char *lbl; float pmin, pmax; int dec;
    fx_param_descriptor((int)node->type, pi, &lbl, &pmin, &pmax, &dec);
    float t = (float)(touch_x - FX_SLIDER_TRACK_X) / (float)FX_SLIDER_TRACK_W;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    float v = pmin + t * (pmax - pmin);
    if (dec == 0) v = (float)(int)(v + (v >= 0.0f ? 0.5f : -0.5f));
    node->params[pi] = v;
    if (node->set_param) node->set_param(node, (uint8_t)pi, v);
    g_song.dirty = true;
}

/* Apply a relative drag delta to one lane-ADSR knob (0=A 1=D 2=S 3=R). */
static void fx_adsr_drag_apply(lane_adsr_t *a, int knob, int dy)
{
    /* Upward drag (negative dy) increases value. */
    float delta = (float)(-dy);
    float atk = a->atk_ms, dcy = a->dcy_ms, sus = a->sus, rel = a->rel_ms;
    switch (knob) {
    case 0: atk = clampf(s_fx_adsr_drag_start + delta * 4.0f, 1.0f, 2000.0f); break;
    case 1: dcy = clampf(s_fx_adsr_drag_start + delta * 4.0f, 1.0f, 2000.0f); break;
    case 2: sus = clampf(s_fx_adsr_drag_start + delta * 0.004f, 0.0f, 1.0f);  break;
    case 3: rel = clampf(s_fx_adsr_drag_start + delta * 8.0f, 1.0f, 4000.0f); break;
    default: return;
    }
    lane_adsr_set_params(a, atk, dcy, sus, rel);
    g_song.dirty = true;
}

/* ── FX+ADSR taps ────────────────────────────────────────────────────────── */
static bool handle_fx_adsr_tap(int x, int y)
{
    /* Picker overlay swallows everything when open. */
    if (s_fx_picker_open) return handle_fx_picker_tap(x, y);

    if (handle_minibar_back(x, y)) return true;

    int        *fx_count;
    lane_adsr_t *adsr;
    int          notify;
    fx_node_t **chain = fx_target_resolve(s_fx_target, &fx_count, &adsr, &notify);

    int y_top = MINIBAR_H + 16;

    /* Lane ADSR knobs (lane targets only) — tap a knob to start a drag. */
    if (adsr) {
        for (int i = 0; i < 4; i++) {
            int kx = 420 + i * 200;
            int ky = y_top + 30;
            if (pt_in(x, y, kx, ky, 80, 80)) {
                s_fx_adsr_drag   = i;
                s_fx_adsr_drag_y = y;
                s_fx_adsr_drag_start = (i == 0) ? adsr->atk_ms :
                                       (i == 1) ? adsr->dcy_ms :
                                       (i == 2) ? adsr->sus    : adsr->rel_ms;
                return true;
            }
        }
    }

    /* FX chain row sits below the ADSR card (if present). */
    int slot_y  = adsr ? (y_top + 156 + 22) : (y_top + 22);
    int arrow_w = 20;
    int slot_w  = (1248 - (FX_MAX_PER_LANE - 1) * arrow_w) / FX_MAX_PER_LANE;

    /* Slot row tap */
    if (pt_in(x, y, 16, slot_y, 1248, 80)) {
        int slot_i = (x - 16) / (slot_w + arrow_w);
        slot_i = clamp_i(slot_i, 0, FX_MAX_PER_LANE - 1);
        if (slot_i >= *fx_count) {
            /* Empty slot — open type picker for this insertion point. */
            s_fx_picker_open        = true;
            s_fx_picker_target_slot = *fx_count;
            s_fx_picker_replace     = false;
            s_fx_picker_scroll      = 0;
            return true;
        }
        if (slot_i == s_fx_sel_slot) {
            /* Tapping the already-selected effect re-opens the picker so the
             * effect type can be swapped in place. */
            s_fx_picker_open        = true;
            s_fx_picker_target_slot = slot_i;
            s_fx_picker_replace     = true;
            s_fx_picker_scroll      = 0;
            return true;
        }
        s_fx_sel_slot = slot_i;
        return true;
    }

    /* Slot detail panel */
    if (s_fx_sel_slot < *fx_count && chain[s_fx_sel_slot]) {
        int detail_y = slot_y + 96;
        /* CHANGE button — re-open the picker in replace mode */
        if (pt_in(x, y, 852, detail_y + 8, 156, 44)) {
            s_fx_picker_open        = true;
            s_fx_picker_target_slot = s_fx_sel_slot;
            s_fx_picker_replace     = true;
            s_fx_picker_scroll      = 0;
            return true;
        }
        /* DELETE button — large, easy-to-hit target */
        if (pt_in(x, y, 1016, detail_y + 8, 140, 44)) {
            fx_chain_remove(chain, fx_count, s_fx_sel_slot);
            if (s_fx_sel_slot >= *fx_count && s_fx_sel_slot > 0) s_fx_sel_slot--;
            g_song.dirty = true;
            ws_notify_change(WS_MSG_FX_UPDATE, notify);
            return true;
        }
        /* Enable button */
        if (pt_in(x, y, 1168, detail_y + 8, 80, 44)) {
            chain[s_fx_sel_slot]->enabled = !chain[s_fx_sel_slot]->enabled;
            g_song.dirty = true;
            ws_notify_change(WS_MSG_FX_UPDATE, notify);
            return true;
        }
        /* Slider tap = start drag + set value */
        fx_node_t *node = chain[s_fx_sel_slot];
        int n_params = fx_param_count((int)node->type);
        int rows_top = detail_y + 60;
        for (int pi = 0; pi < n_params && pi < 8; pi++) {
            int ry = rows_top + pi * FX_SLIDER_ROW_H;
            if (pt_in(x, y, FX_SLIDER_TRACK_X - 20, ry - 4,
                      FX_SLIDER_TRACK_W + 40, FX_SLIDER_ROW_H)) {
                s_fx_param_drag = pi;
                fx_slider_set_from_x(pi, x);
                return true;
            }
        }
    }
    return false;
}

/* ── Settings taps ───────────────────────────────────────────────────────── */
static bool handle_settings_tap(int x, int y)
{
    if (handle_minibar_back(x, y)) return true;

    /* Row layout: 7 rows × 76px, ctrl_x=680 */
    /* Row 0: Master Volume fader */
    {
        int ry = MINIBAR_H + 0 * 76;
        if (pt_in(x, y, 680, ry + 32, 400, 12)) {
            float v = (float)(x - 680) / 400;
            g_settings.master_volume = v < 0 ? 0 : v > 1 ? 1 : v;
            settings_save();
            return true;
        }
    }
    /* Row 2: PPQN seg */
    {
        static const uint32_t ppqn_v[] = { 24, 48, 96, 192 };
        int ry = MINIBAR_H + 2 * 76;
        if (pt_in(x, y, 680, ry + 12, 440, 52)) {
            int sw  = 440 / 4;
            int sel = (x - 680) / sw;
            if (sel >= 0 && sel < 4) {
                g_settings.ppqn        = ppqn_v[sel];
                g_song.clock.tick_rate = ppqn_v[sel];
            }
            settings_save();
            return true;
        }
    }
    /* Row 3: Playback mode seg */
    {
        int ry = MINIBAR_H + 3 * 76;
        if (pt_in(x, y, 680, ry + 12, 220, 52)) {
            int sel = (x - 680) / (220 / 2);
            g_song.playback_mode = (sel == 1) ? 1 : 0;
            g_song.dirty = true;
            ws_notify_change(WS_MSG_SETTINGS, -1);
            return true;
        }
    }
    return false;
}

/* ── Sound browser taps ──────────────────────────────────────────────────── */
static bool handle_sound_browser_tap(int x, int y)
{
    if (handle_minibar_back(x, y)) return true;

    const int kit_w   = 400;
    const int y0      = MINIBAR_H;
    const int row_h   = 72;
    const int ab_h    = 80;
    const int list_h  = 720 - y0 - ab_h;
    const int rows_vis = list_h / row_h;
    const int hdr_bot = y0 + row_h;
    const int ab_y    = 720 - ab_h;
    /* ASSIGN bar: PLAY button (left 240px) + ASSIGN button (rest) */
    const int play_w  = 240;
    const int asgn_x  = 24 + play_w + 16;
    const int asgn_w  = 1280 - asgn_x - 24;

    /* PLAY button — preview selected file without assigning */
    if (pt_in(x, y, 24, ab_y, play_w + 24, ab_h)) {
        sb_preview_current();
        return true;
    }

    /* ASSIGN button */
    if (pt_in(x, y, asgn_x, ab_y, asgn_w + 24, ab_h)) {
        if (s_sb_kit_sel < 0 || s_sb_kit_sel >= s_sb_kit_count) return true;
        if (s_sb_file_sel < 0 || s_sb_file_sel >= s_sb_file_count) return true;
        char path[256];
        snprintf(path, sizeof(path), "sounds/%s/%s",
                 s_sb_kits[s_sb_kit_sel], s_sb_files[s_sb_file_sel]);

        if (s_ctx_drum_row >= 0) {
            lane_t *lane2 = &g_song.lanes[s_ctx_lane];
            if (lane2->drum_seq && s_ctx_drum_row < lane2->drum_seq->row_count) {
                drum_row_t *row = &lane2->drum_seq->rows[s_ctx_drum_row];
                strncpy(row->wav_path, path, sizeof(row->wav_path) - 1);
                row->wav_path[sizeof(row->wav_path) - 1] = '\0';
                /* Open the WAV file into the row's lane slot so it can play */
                char full_path[280];
                snprintf(full_path, sizeof(full_path), "/sdcard/%s", path);
                drum_row_load_wav(row, full_path);
                g_song.dirty = true;
                ws_notify_change(WS_MSG_DRUM_STEP, (s_ctx_lane << 8) | s_ctx_drum_row);
            }
        } else {
            lane_t *lane = &g_song.lanes[s_ctx_lane];
            strncpy(lane->wav_path, path, sizeof(lane->wav_path) - 1);
            lane->wav_path[sizeof(lane->wav_path) - 1] = '\0';
            /* Allocate a pool slot if needed, then open the WAV file */
            if (lane->wav_lane_slot < 0) {
                lane->wav_lane_slot = wav_lane_alloc_slot();
                if (lane->wav_lane_slot >= 0)
                    lane->wav_lane = wav_lane_get(lane->wav_lane_slot);
            }
            if (lane->wav_lane_slot >= 0) {
                char full_wav[280];
                snprintf(full_wav, sizeof(full_wav), "/sdcard/%s", path);
                wav_lane_open(lane->wav_lane_slot, full_wav);
                if (lane->wav_lane) lane->wav_lane->active = true;
            }
            g_song.dirty = true;
            ws_notify_change(WS_MSG_LANE_UPDATE, s_ctx_lane);
        }
        pop_screen();
        return true;
    }

    /* Kit list (left pane), below header row and above action bar */
    if (x < kit_w && y >= hdr_bot && y < ab_y) {
        int vi = (y - hdr_bot) / row_h;
        int ki = vi + s_sb_kit_scroll;
        if (vi >= 0 && vi < rows_vis && ki >= 0 && ki < s_sb_kit_count) {
            if (ki != s_sb_kit_sel) {
                s_sb_kit_sel     = ki;
                s_sb_file_sel    = 0;
                s_sb_file_scroll = 0;
                s_sb_file_px     = 0.0f;
                s_sb_file_vel    = 0.0f;
                sb_load_files(ki);
            }
        }
        return true;
    }

    /* File list (right pane), below header row and above action bar */
    if (x >= kit_w + 2 && y >= hdr_bot && y < ab_y) {
        int vi = (y - hdr_bot) / row_h;
        int fi = vi + s_sb_file_scroll;
        if (vi >= 0 && vi < rows_vis && fi >= 0 && fi < s_sb_file_count) {
            s_sb_file_sel = fi;
            sb_preview_current();
        }
        return true;
    }

    return false;
}

/* ── WAV detail taps ─────────────────────────────────────────────────────── */
static bool handle_wav_detail_tap(int x, int y)
{
    if (handle_minibar_back(x, y)) return true;

    lane_t *lane = &g_song.lanes[s_ctx_lane];

    /* "CHANGE FILE" button  → open sound browser */
    if (pt_in(x, y, 24, MINIBAR_H + 52, 400, 56)) {
        s_ctx_drum_row = -1;   /* -1 means assign to lane WAV path */
        if (s_sb_dirty) { sb_load_kits(); s_sb_kit_sel = 0; s_sb_file_sel = 0;
                          s_sb_kit_scroll = 0; s_sb_file_scroll = 0; s_sb_kit_px = 0.0f; s_sb_file_px = 0.0f; s_sb_kit_vel = 0.0f; s_sb_file_vel = 0.0f;
                          sb_load_files(0); s_sb_dirty = false; }
        push_screen(SCREEN_SOUND_BROWSER);
        return true;
    }
    /* Volume fader (row at MINIBAR_H + 152) */
    if (pt_in(x, y, 240, MINIBAR_H + 160, 800, 16)) {
        float v = (float)(x - 240) / 800;
        if (v < 0.0f) v = 0.0f;
        if (v > 1.0f) v = 1.0f;
        lane->volume = v;
        g_song.dirty = true;
        ws_notify_change(WS_MSG_LANE_UPDATE, s_ctx_lane);
        return true;
    }
    /* Pan fader (row at MINIBAR_H + 232) */
    if (pt_in(x, y, 240, MINIBAR_H + 240, 800, 16)) {
        float v = (float)(x - 240) / 800 * 2.0f - 1.0f;
        if (v < -1.0f) v = -1.0f;
        if (v >  1.0f) v =  1.0f;
        lane->pan = v;
        g_song.dirty = true;
        ws_notify_change(WS_MSG_LANE_UPDATE, s_ctx_lane);
        return true;
    }
    /* FX button */
    if (pt_in(x, y, 440, MINIBAR_H + 52, 280, 56)) {
        s_fx_sel_slot = 0;
        s_fx_target   = s_ctx_lane;
        push_screen(SCREEN_FX_ADSR);
        return true;
    }
    return false;
}

/* ── Song browser taps ───────────────────────────────────────────────────── */
static bool handle_song_browser_tap(int x, int y)
{
    if (handle_minibar_back(x, y)) return true;

    int y0 = MINIBAR_H + 40;
    int visible = (720 - 72 - y0) / 72;
    if (visible > s_song_browser_count) visible = s_song_browser_count;
    for (int i = 0; i < visible; i++) {
        if (pt_in(x, y, 0, y0 + i * 72, 1280, 72)) {
            s_song_browser_sel = i;
            return true;
        }
    }
    int ab_y = 720 - 72;
    /* LOAD */
    if (pt_in(x, y, 24, ab_y + 8, 480, 56)) {
        if (s_song_browser_sel < 0 || s_song_browser_sel >= s_song_browser_count) {
            ui_status_set("Select a song first");
            return true;
        }
        const char *fname = s_song_browser_files[s_song_browser_sel];
        char path[160];
        snprintf(path, sizeof(path), "%s/%s", SONG_DIR, fname);
        if (song_load(path)) {
            strncpy(g_settings.last_song, path, sizeof(g_settings.last_song) - 1);
            settings_save();
            ui_status_set("Loaded: %s", fname);
            /* Jump straight to the Song view so the user sees the loaded state */
            s_nav_top      = 0;
            s_nav_stack[0] = SCREEN_SONG;
            s_active_tab   = 0;
        } else {
            ui_status_set("Load FAILED: %s", fname);
        }
        return true;
    }
    /* DELETE */
    if (pt_in(x, y, 524, ab_y + 8, 240, 56)) {
        if (s_song_browser_sel >= 0 && s_song_browser_sel < s_song_browser_count) {
            char path[160];
            snprintf(path, sizeof(path), "%s/%s",
                     SONG_DIR, s_song_browser_files[s_song_browser_sel]);
            remove(path);
            s_song_browser_dirty = true;
            song_browser_refresh();
            if (s_song_browser_sel >= s_song_browser_count)
                s_song_browser_sel = s_song_browser_count - 1;
            if (s_song_browser_sel < 0) s_song_browser_sel = 0;
        }
        return true;
    }
    /* REFRESH */
    if (pt_in(x, y, 784, ab_y + 8, 240, 56)) {
        s_song_browser_dirty = true;
        song_browser_refresh();
        return true;
    }
    return false;
}

/* ── ARP screen taps ─────────────────────────────────────────────────────── */
static bool handle_arp_tap(int x, int y)
{
    if (handle_minibar_back(x, y)) return true;

    lane_t *lane = &g_song.lanes[s_ctx_lane];
    arp_t  *arp  = &lane->arp;
    int y0 = MINIBAR_H + 16;

    /* Enable toggle row (y0..y0+56) */
    if (pt_in(x, y, 680, y0 + 8, 48, 40)) {
        arp->enabled = !arp->enabled;
        g_song.dirty = true;
        ws_notify_change(WS_MSG_LANE_UPDATE, s_ctx_lane);
        return true;
    }
    y0 += 56;

    /* Mode seg (y0..y0+60), 7 opts over 900 px starting at x=280 */
    if (pt_in(x, y, 280, y0, 900, 48)) {
        int opt = (x - 280) * 7 / 900;
        if (opt >= 0 && opt < ARP_MODE_COUNT) {
            arp->mode = (arp_mode_t)opt;
            g_song.dirty = true;
            ws_notify_change(WS_MSG_LANE_UPDATE, s_ctx_lane);
        }
        return true;
    }
    y0 += 60;

    /* Octave seg */
    if (pt_in(x, y, 280, y0, 440, 48)) {
        int opt = (x - 280) * 4 / 440;
        if (opt >= 0 && opt < 4) {
            arp->octave_range = (uint8_t)(opt + 1);
            arp_build_seq(arp);
            g_song.dirty = true;
        }
        return true;
    }
    y0 += 60;

    /* Step div seg */
    if (pt_in(x, y, 280, y0, 540, 48)) {
        static const uint8_t divs[] = { 4, 8, 16, 32 };
        int opt = (x - 280) * 4 / 540;
        if (opt >= 0 && opt < 4) {
            arp->step_div = divs[opt];
            g_song.dirty  = true;
        }
        return true;
    }
    y0 += 60;

    /* Gate fader */
    if (pt_in(x, y, 280, y0, 760, 40)) {
        int pct = (x - 280) * 100 / 760;
        if (pct < 10) pct = 10;
        if (pct > 100) pct = 100;
        arp->gate_pct = (uint8_t)pct;
        g_song.dirty  = true;
        return true;
    }
    y0 += 56;

    /* Vel mode seg */
    if (pt_in(x, y, 280, y0, 480, 48)) {
        int opt = (x - 280) * 3 / 480;
        if (opt >= 0 && opt < 3) {
            arp->velocity_mode = (arp_vel_mode_t)opt;
            g_song.dirty = true;
        }
        return true;
    }
    y0 += 60;

    /* Latch toggle */
    if (pt_in(x, y, 280, y0 + 8, 48, 40)) {
        arp->latch = !arp->latch;
        g_song.dirty = true;
        return true;
    }
    /* Retrig toggle */
    if (pt_in(x, y, 680, y0 + 8, 48, 40)) {
        arp->retrigger = !arp->retrigger;
        g_song.dirty = true;
        return true;
    }
    y0 += 56;

    /* Swing fader */
    if (pt_in(x, y, 280, y0, 760, 40)) {
        int pct = 50 + (x - 280) * 25 / 760;
        if (pct < 50) pct = 50;
        if (pct > 75) pct = 75;
        arp->swing_pct = (uint8_t)pct;
        g_song.dirty   = true;
        return true;
    }
    return false;
}

/* ── GROOVE screen taps ──────────────────────────────────────────────────── */
static bool handle_groove_tap(int x, int y)
{
    if (handle_minibar_back(x, y)) return true;

    int y0 = MINIBAR_H + 8 + 36 + 4;
    int row_h = 64;
    for (int li = 0; li < NUM_LANES; li++) {
        lane_t *lane = &g_song.lanes[li];
        if (!lane->active) continue;
        if (y0 + row_h > 720) break;

        groove_t *g = &lane->groove;

        /* Swing fader (200..620) */
        if (pt_in(x, y, 200, y0 + 12, 420, 40)) {
            float f = (float)(x - 200) / 420.0f;
            if (f < 0.0f) f = 0.0f;
            if (f > 1.0f) f = 1.0f;
            g->swing_pct = (uint8_t)(50 + f * 25.0f);
            g_song.dirty = true;
            ws_notify_change(WS_MSG_LANE_UPDATE, li);
            return true;
        }
        /* Humanise fader (680..960) */
        if (pt_in(x, y, 680, y0 + 12, 280, 40)) {
            float f = (float)(x - 680) / 280.0f;
            if (f < 0.0f) f = 0.0f;
            if (f > 1.0f) f = 1.0f;
            g->humanise = (int8_t)(f * 20.0f);
            g_song.dirty = true;
            ws_notify_change(WS_MSG_LANE_UPDATE, li);
            return true;
        }
        /* Enable toggle */
        if (pt_in(x, y, 1040, y0 + 16, 48, 40)) {
            g->enabled = !g->enabled;
            g_song.dirty = true;
            ws_notify_change(WS_MSG_LANE_UPDATE, li);
            return true;
        }
        y0 += row_h;
    }
    return false;
}

/* ── ARRANGEMENT screen taps ─────────────────────────────────────────────── */
static bool handle_arrangement_tap(int x, int y)
{
    if (handle_minibar_back(x, y)) return true;

    arrangement_t *arr = &g_song.arrangement;
    int y0 = MINIBAR_H + 16;

    /* Enabled toggle */
    if (pt_in(x, y, 680, y0 + 8, 48, 40)) {
        arr->enabled = !arr->enabled;
        g_song.dirty = true;
        ws_notify_change(WS_MSG_SETTINGS, -1);
        return true;
    }
    y0 += 60 + 4 + 8;

    int row_h = 60;
    for (int i = 0; i < arr->count; i++) {
        if (y0 + row_h > 690) break;
        arrangement_step_t *st = &arr->steps[i];

        if (pt_in(x, y, 0, y0, 1280, row_h)) {
            s_arr_sel_step = (s_arr_sel_step == i) ? -1 : i;

            /* Repeat - button */
            if (pt_in(x, y, 320, y0 + 12, 52, 36)) {
                if (st->repeat > 1) { st->repeat--; g_song.dirty = true; }
                return true;
            }
            /* Repeat + button */
            if (pt_in(x, y, 384, y0 + 12, 52, 36)) {
                if (st->repeat < 16) { st->repeat++; g_song.dirty = true; }
                return true;
            }
            /* Scene button — cycle scene */
            if (pt_in(x, y, 80, y0 + 10, 80, 40)) {
                st->scene_idx = (st->scene_idx + 1) % SCENE_MAX;
                g_song.dirty  = true;
                return true;
            }
            ws_notify_change(WS_MSG_SETTINGS, -1);
            return true;
        }
        y0 += row_h;
    }

    /* ADD STEP */
    if (pt_in(x, y, 24, y0 + 8, 320, 44) && arr->count < ARRANGEMENT_MAX_STEPS) {
        arrangement_step_t *st2 = &arr->steps[arr->count];
        st2->scene_idx = 0;
        st2->repeat    = 1;
        arr->count++;
        g_song.dirty = true;
        ws_notify_change(WS_MSG_SETTINGS, -1);
        return true;
    }
    /* CLEAR */
    if (pt_in(x, y, 380, y0 + 8, 200, 44)) {
        arr->count = 0;
        arr->current_step = 0;
        g_song.dirty = true;
        ws_notify_change(WS_MSG_SETTINGS, -1);
        return true;
    }
    return false;
}

/* ── NOTE REPEAT / SEND LEVELS screen taps ───────────────────────────────── */
static bool handle_note_repeat_tap(int x, int y)
{
    if (handle_minibar_back(x, y)) return true;

    int y0 = MINIBAR_H + 16 + 36 + 4;
    int row_h = 64;
    static const uint8_t rate_divs[] = { 4, 8, 16, 32 };

    for (int li = 0; li < NUM_LANES; li++) {
        lane_t *lane = &g_song.lanes[li];
        if (!lane->active) continue;
        if (y0 + row_h > 720) break;

        /* Note repeat toggle */
        if (pt_in(x, y, 200, y0 + 18, 48, 40)) {
            lane->note_repeat = !lane->note_repeat;
            g_song.dirty = true;
            ws_notify_change(WS_MSG_LANE_UPDATE, li);
            return true;
        }
        /* Rate seg */
        if (pt_in(x, y, 380, y0 + 8, 340, 46)) {
            int opt = (x - 380) * 4 / 340;
            if (opt >= 0 && opt < 4) {
                lane->note_repeat_div = rate_divs[opt];
                g_song.dirty = true;
            }
            return true;
        }
        /* Send level fader */
        if (pt_in(x, y, 780, y0 + 12, 380, 40)) {
            float f = (float)(x - 780) / 380.0f;
            if (f < 0.0f) f = 0.0f;
            if (f > 1.0f) f = 1.0f;
            lane->send_level = f;
            g_song.dirty = true;
            ws_notify_change(WS_MSG_LANE_UPDATE, li);
            return true;
        }
        y0 += row_h;
    }
    return false;
}

/* ── Piano key touch (LIVE mode) ─────────────────────────────────────────── */
static bool handle_piano_touch(tp_pt_t *pts, int cnt)
{
    bool cur[MAX_KEYS] = {};
    for (int p = 0; p < cnt; p++) {
        int bk_hit = -1;
        for (int k = 0; k < s_key_cnt; k++) {
            if (!s_piano_keys[k].is_black) continue;
            if (pts[p].x >= s_piano_keys[k].x && pts[p].x < s_piano_keys[k].x + s_piano_keys[k].w &&
                pts[p].y >= s_piano_keys[k].y && pts[p].y < s_piano_keys[k].y + s_piano_keys[k].h)
            { cur[k] = true; bk_hit = k; }
        }
        if (bk_hit >= 0) continue;
        for (int k = 0; k < s_key_cnt; k++) {
            if (s_piano_keys[k].is_black) continue;
            if (pts[p].x >= s_piano_keys[k].x && pts[p].x < s_piano_keys[k].x + s_piano_keys[k].w &&
                pts[p].y >= s_piano_keys[k].y && pts[p].y < s_piano_keys[k].y + s_piano_keys[k].h)
                cur[k] = true;
        }
    }

    int li = -1, cnt2 = 0;
    for (int i = 0; i < NUM_LANES; i++) {
        if (!g_song.lanes[i].active) continue;
        if (cnt2 == s_live_lane) { li = i; break; }
        cnt2++;
    }
    synth_inst_t *synth = (li >= 0) ? g_song.lanes[li].synth : NULL;

    bool changed = false;
    for (int k = 0; k < s_key_cnt; k++) {
        uint8_t note = (uint8_t)(s_live_octave * 12 +
                                 s_piano_keys[k].oct_offset * 12 +
                                 s_piano_keys[k].semi);
        if (cur[k]) {
            s_key_off_cnt[k] = 0;
            if (!s_key_held[k]) {
                if (synth) audio_note_on(synth, note, 100);
                s_key_held[k]           = true;
                s_piano_keys[k].pressed = true;
                changed = true;
            }
        } else if (s_key_held[k]) {
            if (++s_key_off_cnt[k] >= KEY_OFF_THRESH) {
                if (synth) audio_note_off(synth, note);
                s_key_held[k]           = false;
                s_key_off_cnt[k]        = 0;
                s_piano_keys[k].pressed = false;
                changed = true;
            }
        }
    }

    /* Update polyphonic gain normalisation counters for the audio task.
     * Always keep lane_idx current; only recount held keys when something changed. */
    g_live_lane_idx = li;
    if (changed) {
        int held = 0;
        for (int k = 0; k < s_key_cnt; k++)
            if (s_key_held[k]) held++;
        g_live_held_count = held;
    }
    return changed;
}

/* Render the live piano tear-free without stalling the touch poll.
 *
 * A full draw_screen() clears and repaints all 1280×720 px (tens of ms of PSRAM
 * writes); doing that on every note edge starves the touch poll and swallows
 * fast re-taps. Writing key highlights straight into the scanned buffer instead
 * is cheap but tears. So: draw only what changed into the *back* buffer and
 * present it by swap (tear-free), and gate the whole thing on the non-blocking
 * gfx_present_ready() so the poll never waits on vsync.
 *
 * Updates target one buffer per frame, so a change propagates to both buffers
 * over two presents. Chrome (tabs/octave bar) is repainted via a full
 * draw_screen() only when the buffer's s_live_chrome_dirty flag is set. */
static void live_piano_render(void)
{
    int bi = gfx_back_index();

    bool keys_differ = false;
    for (int k = 0; k < s_key_cnt; k++)
        if (s_key_shown[bi][k] != s_piano_keys[k].pressed) { keys_differ = true; break; }

    if (!s_live_chrome_dirty[bi] && !keys_differ) return;  /* back buffer current */
    if (!gfx_present_ready()) return;                      /* still scanning; retry next frame */

    if (s_live_chrome_dirty[bi]) {
        draw_screen();                  /* full repaint of this buffer (chrome + keys); commits */
        s_live_chrome_dirty[bi] = false;
    } else {
        /* Keys are stored white-first then black; black keys overlap the top of
         * adjacent white keys, so repainting a changed white key erases the
         * black keys drawn over it.  Redraw changed keys in order (whites before
         * blacks), then repaint all black keys on top if any white changed. */
        bool white_changed = false;
        for (int k = 0; k < s_key_cnt; k++)
            if (s_key_shown[bi][k] != s_piano_keys[k].pressed) {
                draw_live_piano_key(k); /* into g_fb (back buffer) */
                if (!s_piano_keys[k].is_black) white_changed = true;
            }
        if (white_changed)
            for (int k = 0; k < s_key_cnt; k++)
                if (s_piano_keys[k].is_black) draw_live_piano_key(k);
        gfx_commit();                   /* present + swap */
    }
    for (int k = 0; k < s_key_cnt; k++)
        s_key_shown[bi][k] = s_piano_keys[k].pressed;
}

/* ── OSK callbacks ───────────────────────────────────────────────────────── */
static void osk_cb_lane_rename(const char *text)
{
    if (s_ctx_lane < 0 || s_ctx_lane >= NUM_LANES) return;
    strncpy(g_song.lanes[s_ctx_lane].name, text, 31);
    g_song.lanes[s_ctx_lane].name[31] = '\0';
    g_song.dirty = true;
    ws_notify_change(WS_MSG_LANE_UPDATE, s_ctx_lane);
}

static void osk_cb_song_save_as(const char *text)
{
    if (!text || !text[0]) { ui_status_set("Save cancelled: name empty"); return; }
    strncpy(g_song.name, text, sizeof(g_song.name) - 1);
    g_song.name[sizeof(g_song.name) - 1] = '\0';
    char path[160];
    snprintf(path, sizeof(path), "%s/%s%s", SONG_DIR, g_song.name, SONG_EXT);
    if (song_save(path)) {
        strncpy(g_settings.last_song, path, sizeof(g_settings.last_song) - 1);
        settings_save();
        s_song_browser_dirty = true;
        ui_status_set("Saved: %s%s", g_song.name, SONG_EXT);
    } else {
        ui_status_set("Save FAILED — see serial log");
    }
}

static void osk_cb_new_song(const char *text)
{
    song_new();
    if (text && text[0]) {
        strncpy(g_song.name, text, sizeof(g_song.name) - 1);
        g_song.name[sizeof(g_song.name) - 1] = '\0';

        /* Write the empty song to SD immediately so it shows up in the
         * browser and can be reloaded later. */
        char path[160];
        snprintf(path, sizeof(path), "%s/%s%s", SONG_DIR, g_song.name, SONG_EXT);
        if (song_save(path)) {
            strncpy(g_settings.last_song, path, sizeof(g_settings.last_song) - 1);
            settings_save();
            s_song_browser_dirty = true;
            ui_status_set("Created: %s%s", g_song.name, SONG_EXT);
        } else {
            ui_status_set("Save FAILED — see serial log");
        }
    }
    /* Return to SONG view */
    s_nav_top      = 0;
    s_nav_stack[0] = SCREEN_SONG;
    s_active_tab   = 0;
}

/* ── Synth edit tap handler ──────────────────────────────────────────────── */
static bool handle_synth_edit_tap(int x, int y)
{
    if (handle_minibar_back(x, y)) return true;

    int li = s_se_lane;
    lane_t *lane = (li >= 0 && li < NUM_LANES) ? &g_song.lanes[li] : NULL;
    synth_inst_t *synth = lane ? lane->synth : NULL;

    int y0   = MINIBAR_H + 8;
    int tc   = 5, tr = 4;
    int tw   = (1280 - 40) / tc;
    int th   = 52, tgap = 6;
    int ty0  = y0 + 18;

    /* ── Type picker ─────────────────────────────────────────────────────── */
    for (int r = 0; r < tr; r++) {
        for (int c = 0; c < tc; c++) {
            int idx = r * tc + c;
            if (idx >= 20) break;
            int bx = 20 + c * tw;
            int by = ty0 + r * (th + tgap);
            if (!pt_in(x, y, bx, by, tw - 4, th)) continue;
            if (synth && synth->type_id == (uint8_t)idx) return true;
            if (lane) {
                /* Null first so audio task stops using the old pointer,
                 * then free it, then assign the new one. */
                synth_inst_t *old = lane->synth;
                lane->synth = NULL;
                /* Small yield so audio task finishes any in-flight render
                 * before we free the memory (audio buf = ~1 ms). */
                vTaskDelay(pdMS_TO_TICKS(3));
                synth_free(old);
                lane->synth = synth_new((uint8_t)idx);
                synth = lane->synth;
                /* Re-apply saved params to new synth */
                if (synth && synth->set_param) {
                    float *sp = lane->synth_params;
                    bool extra = (idx >= 6);
                    synth->set_param(synth, 0,  sp[0]);
                    synth->set_param(synth, 1,  sp[1]);
                    synth->set_param(synth, extra ? 20 : 2, sp[extra ? 20 : 2]);
                    synth->set_param(synth, extra ? 21 : 3, sp[extra ? 21 : 3]);
                    synth->set_param(synth, extra ? 22 : 4, sp[extra ? 22 : 4]);
                    synth->set_param(synth, extra ? 23 : 5, sp[extra ? 23 : 5]);
                    synth->set_param(synth, 6,  sp[6]);
                    synth->set_param(synth, 7,  sp[7]);
                    synth->set_param(synth, 8,  sp[8]);
                    synth->set_param(synth, 9,  sp[9]);
                    synth->set_param(synth, 10, sp[10]);
                    synth->set_param(synth, 11, sp[11]);
                    synth->set_param(synth, 12, sp[12]);
                }
                g_song.dirty = true;
                ws_notify_change(WS_MSG_LANE_UPDATE, li);
            }
            return true;
        }
    }

    int after_types = ty0 + tr * (th + tgap) + 8;

    /* ── Tab bar ─────────────────────────────────────────────────────────── */
    int tab_h = 44, tab_w = 1280 / 4;
    if (pt_in(x, y, 0, after_types, 1280, tab_h)) {
        int new_tab = x / tab_w;
        if (new_tab >= 0 && new_tab < 4) s_se_tab = new_tab;
        return true;
    }

    /* ── Parameter panel ─────────────────────────────────────────────────── */
    int py = after_types + tab_h + 16;
    int pw = 1200, px = 40;

    if (!lane) return false;
    float *sp    = lane->synth_params;
    bool  is_ext = (synth && synth->type_id >= 6);

    /* Slider tap: label is 22px tall (scale 2), track starts at +22, height 32 */
#define SE_SLIDER_TAP(pid, lo, hi, sy) \
    if (pt_in(x, y, px, (sy) + 22, pw, 32)) { \
        float frac = (float)(x - px) / pw; \
        if (frac < 0.0f) { frac = 0.0f; } else if (frac > 1.0f) { frac = 1.0f; } \
        sp[(pid)] = (lo) + frac * ((hi) - (lo)); \
        if (synth && synth->set_param) synth->set_param(synth, (pid), sp[(pid)]); \
        g_song.dirty = true; ws_notify_change(WS_MSG_LANE_UPDATE, li); \
        return true; \
    }

    int cur_type2 = synth ? (int)synth->type_id : -1;
    bool has_fm     = (cur_type2 == 3 || cur_type2 == 4);

    switch (s_se_tab) {
    case 0: { /* ENV */
        int pa = is_ext ? 20 : 2, pd = is_ext ? 21 : 3;
        int ps = is_ext ? 22 : 4, pr = is_ext ? 23 : 5;
        SE_SLIDER_TAP(pa, 0.0f, 2000.0f, py);
        SE_SLIDER_TAP(pd, 0.0f, 2000.0f, py + 64);
        SE_SLIDER_TAP(ps, 0.0f, 1.0f,    py + 128);
        SE_SLIDER_TAP(pr, 0.0f, 3000.0f, py + 192);
        break;
    }
    case 1: { /* OSC */
        bool has_osc = (cur_type2 >= 0 && cur_type2 <= 5);
        if (has_osc) {
            /* Waveform seg: 6 buttons at py+22, h=40 */
            if (pt_in(x, y, px, py + 22, 6 * (160 + 4), 40)) {
                int wi = (x - px) / (160 + 4);
                if (wi >= 0 && wi < 6) {
                    sp[0] = (float)wi;
                    if (synth && synth->set_param) synth->set_param(synth, 0, sp[0]);
                    g_song.dirty = true;
                    ws_notify_change(WS_MSG_LANE_UPDATE, li);
                }
                return true;
            }
            SE_SLIDER_TAP(1, -24.0f, 24.0f,  py + 78);
            SE_SLIDER_TAP(6,   0.0f, 100.0f, py + 142);
        } else {
            SE_SLIDER_TAP(1, -24.0f, 24.0f, py);
        }
        break;
    }
    case 2: { /* FILTER */
        SE_SLIDER_TAP(9,  40.0f, 20000.0f, py);
        SE_SLIDER_TAP(10,  0.0f,     1.0f, py + 64);
        SE_SLIDER_TAP(11,  0.0f,     1.0f, py + 128);
        break;
    }
    case 3: { /* MOD / FM */
        if (has_fm) {
            SE_SLIDER_TAP(7, 0.5f, 8.0f, py);
            SE_SLIDER_TAP(8, 0.0f, 4.0f, py + 64);
            /* FM4 algo buttons */
            if (cur_type2 == 4) {
                int abw = (pw - 7 * 8) / 8;
                if (pt_in(x, y, px, py + 150, pw, 48)) {
                    int ai = (x - px) / (abw + 8);
                    if (ai >= 0 && ai < 8) {
                        sp[12] = (float)ai;
                        if (synth && synth->set_param) synth->set_param(synth, 12, sp[12]);
                        g_song.dirty = true;
                        ws_notify_change(WS_MSG_LANE_UPDATE, li);
                    }
                    return true;
                }
            }
        }
        break;
    }
    }
#undef SE_SLIDER_TAP
    return false;
}

/* ── OSK tap handler ─────────────────────────────────────────────────────── */
static bool handle_osk_tap(int x, int y)
{
    if (handle_minibar_back(x, y)) {
        /* Discard — don't invoke callback */
        return true;
    }

    /* Keyboard rows */
    static const char *ROWS[3] = { "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM" };
    static const int   N_COLS[3] = { 10, 9, 7 };
    int key_w = 116, key_h = 86, gap = 4;
    int tf_y  = MINIBAR_H + 12;
    int kb_y  = tf_y + 56 + 16;

    for (int r = 0; r < 3; r++) {
        int n     = N_COLS[r];
        int row_w = n * key_w + (n - 1) * gap;
        int x0    = (1280 - row_w) / 2;
        int ky    = kb_y + r * (key_h + gap);
        if (!pt_in(x, y, x0, ky, row_w, key_h)) continue;
        int c = (x - x0) / (key_w + gap);
        if (c < 0 || c >= n) return true;
        if (s_osk_len < OSK_MAX_LEN) {
            s_osk_buf[s_osk_len++] = ROWS[r][c];
            s_osk_buf[s_osk_len]   = '\0';
        }
        return true;
    }

    /* Number row */
    int r3y   = kb_y + 3 * (key_h + gap);
    int num_w = 116;
    if (pt_in(x, y, 40, r3y, 10 * (num_w + gap), key_h)) {
        int idx = (x - 40) / (num_w + gap);
        if (idx >= 0 && idx < 10 && s_osk_len < OSK_MAX_LEN) {
            s_osk_buf[s_osk_len++] = (char)('0' + idx);
            s_osk_buf[s_osk_len]   = '\0';
        }
        return true;
    }

    /* Bottom row: SPACE | DEL | DONE */
    int r4y = r3y + key_h + gap;
    if (pt_in(x, y, 40, r4y, 560, key_h)) {
        /* SPACE */
        if (s_osk_len < OSK_MAX_LEN) {
            s_osk_buf[s_osk_len++] = ' ';
            s_osk_buf[s_osk_len]   = '\0';
        }
        return true;
    }
    if (pt_in(x, y, 620, r4y, 280, key_h)) {
        /* DEL */
        if (s_osk_len > 0) s_osk_buf[--s_osk_len] = '\0';
        return true;
    }
    if (pt_in(x, y, 920, r4y, 320, key_h)) {
        /* DONE — invoke callback then pop screen */
        if (s_osk_done_cb) s_osk_done_cb(s_osk_buf);
        s_osk_done_cb = NULL;
        pop_screen();
        return true;
    }
    return false;
}

/* ── Bluetooth tap handler ───────────────────────────────────────────────── */
static bool handle_bluetooth_tap(int x, int y)
{
    if (handle_minibar_back(x, y)) return true;

    int y0 = MINIBAR_H + 16;
    /* SCAN button: y0+68+68=y0+136, w=400, h=56 */
    int scan_y = y0 + 68 + 68;
    if (pt_in(x, y, 40, scan_y, 400, 56)) {
        s_bt_scanning      = !s_bt_scanning;
        s_bt_device_count  = 0;
        /* Actual BLE scan would start here via esp_ble_gap_start_scanning().
         * Not yet wired — ESP32-C6 BLE init requires CONFIG_BT_ENABLED and
         * Bluedroid/NimBLE stack init, which would be added in bt_init.c. */
        return true;
    }

    /* Device list */
    int list_y = scan_y + 72;
    for (int i = 0; i < s_bt_device_count && i < BT_SCAN_MAX; i++) {
        int dy = list_y + i * 56;
        if (pt_in(x, y, 40, dy, 1200, 48)) {
            s_bt_sel = (s_bt_sel == i) ? -1 : i;
            return true;
        }
    }
    return false;
}

/* ── Top-level touch dispatcher ──────────────────────────────────────────── */
static bool handle_touch_down(int x, int y)
{
    screen_id_t scr     = current_screen();
    screen_id_t scr_pre = scr;
    bool        overlay_pre = s_ctx_menu_open;

    /* Context menu tap handling — intercepts all taps when open */
    if (s_ctx_menu_open) {
        /* RENAME: y=216..272 */
        if (pt_in(x, y, 160, 216, 960, 56)) {
            int li = s_ctx_menu_lane;
            s_ctx_menu_open = false;
            if (li >= 0 && li < NUM_LANES) {
                snprintf(s_osk_title, sizeof(s_osk_title), "Lane %d name", li + 1);
                strncpy(s_osk_buf, g_song.lanes[li].name, OSK_MAX_LEN);
                s_osk_buf[OSK_MAX_LEN] = '\0';
                s_osk_len = (int)strlen(s_osk_buf);
                s_ctx_lane    = li;
                s_osk_done_cb = osk_cb_lane_rename;
                push_screen(SCREEN_OSK);
            }
        /* CHANGE TYPE: y=288..344 — cycle SYNTH→DRUM→DRUMSYNTH→SAMPLE→SYNTH */
        } else if (pt_in(x, y, 160, 288, 960, 56)) {
            int li = s_ctx_menu_lane;
            if (li >= 0 && li < NUM_LANES) {
                lane_t *ln = &g_song.lanes[li];
                lane_type_t next;
                if      (ln->type == LANE_TYPE_SYNTH)     next = LANE_TYPE_DRUM;
                else if (ln->type == LANE_TYPE_DRUM)      next = LANE_TYPE_DRUMSYNTH;
                else if (ln->type == LANE_TYPE_DRUMSYNTH) next = LANE_TYPE_WAV;
                else                                       next = LANE_TYPE_SYNTH;
                ln->type = next;
                if (next == LANE_TYPE_SYNTH) {
                    if (!ln->synth)      ln->synth      = synth_new(SYNTH_TYPE_POLY_WT);
                    if (!ln->piano_roll) ln->piano_roll  = piano_roll_alloc();
                    arp_init(&ln->arp);
                } else if (next == LANE_TYPE_DRUM) {
                    if (!ln->drum_seq) {
                        ln->drum_seq = drum_seq_alloc();
                        if (ln->drum_seq)
                            drum_seq_update_timing(ln->drum_seq, ln->loop_len_ticks);
                    }
                } else if (next == LANE_TYPE_DRUMSYNTH) {
                    if (!ln->dsyn) {
                        ln->dsyn = dsyn_alloc();
                        if (ln->dsyn) {
                            dsyn_update_timing(ln->dsyn, ln->loop_len_ticks);
                            dsyn_reset(ln->dsyn, ln->lane_tick);
                        }
                    }
                }
                g_song.dirty = true;
                ws_notify_change(WS_MSG_LANE_UPDATE, li);
            }
            /* keep menu open so user can see new type label */
        /* DUPLICATE: y=360..416 */
        } else if (pt_in(x, y, 160, 360, 960, 56)) {
            s_ctx_menu_open = false;
            if (s_ctx_menu_lane >= 0) {
                uint8_t buf[2] = { WS_CMD_DUPLICATE_LANE, (uint8_t)s_ctx_menu_lane };
                ws_cmd_dispatch(buf, 2, -1);
            }
        /* DELETE: y=432..488 */
        } else if (pt_in(x, y, 160, 432, 960, 56)) {
            s_ctx_menu_open = false;
            if (s_ctx_menu_lane >= 0) {
                uint8_t buf[2] = { WS_CMD_DEL_LANE, (uint8_t)s_ctx_menu_lane };
                ws_cmd_dispatch(buf, 2, -1);
            }
        /* SYNTH EDIT: y=504..560 — only active for synth lanes */
        } else if (pt_in(x, y, 160, 504, 960, 56)) {
            int li = s_ctx_menu_lane;
            s_ctx_menu_open = false;
            if (li >= 0 && li < NUM_LANES && g_song.lanes[li].type == LANE_TYPE_SYNTH) {
                s_ctx_lane = li;
                s_se_lane  = li;
                s_se_tab   = 0;
                push_screen(SCREEN_SYNTH_EDIT);
            }
        /* CANCEL: y=576..632 or outside tap */
        } else {
            s_ctx_menu_open = false;
        }
        return true; /* overlay needs redraw */
    }

    /* Long-press tracking + vol panel fader drag in SONG VIEW */
    if (scr == SCREEN_SONG) {
        s_fader_drag_lane = -1;
        s_pan_drag_lane   = -1;
        s_lp_lane         = -1;
        s_vol_fader_drag  = false;
        int lp_abs  = 0;
        int yc      = CONTENT_Y;
        for (int i = 0; i < NUM_LANES; i++) {
            if (!g_song.lanes[i].active) continue;
            if (lp_abs < s_song_scroll) { lp_abs++; continue; }
            int ry = yc;
            if (ry + LANE_ROW_H > CONTENT_Y + SONG_BODY_H) break;
            /* Long-press tracking for context menu */
            if (pt_in(x, y, 0, ry, 1280, LANE_ROW_H)) {
                s_lp_lane     = i;
                s_lp_start_ms = esp_timer_get_time() / 1000;
            }
            /* Vol panel: drag-start on the fader track */
            if (i == s_vol_open_lane) {
                int py = ry + LANE_ROW_H;
                int fx = LANE_COLOR_W + 16, fw = 720;
                if (pt_in(x, y, fx, py + 8, fw, 40)) {
                    s_vol_fader_drag  = true;
                    s_fader_drag_lane = i;
                    /* immediate update */
                    float v = (float)(x - fx) / fw;
                    if (v < 0.0f) v = 0.0f;
                    if (v > 1.0f) v = 1.0f;
                    g_song.lanes[i].volume = v;
                    g_song.dirty = true;
                    return false;
                }
            }
            yc += LANE_ROW_H;
            if (i == s_vol_open_lane) yc += LANE_VOL_H;
            lp_abs++;
        }
        /* Master strip vol/pan drag */
        {
            int ms_y = MASTER_Y;
            int fx = 120, fw = 260;
            if (pt_in(x, y, fx, ms_y + 10, fw, 18)) {
                s_master_vol_drag = true;
                return false;
            }
            if (pt_in(x, y, fx, ms_y + 30, fw, 18)) {
                s_master_pan_drag = true;
                return false;
            }
        }
    }

    /* Transport toggle button in topbar (single play/stop button at 116,8) */
    if (is_main_screen(scr)) {
        if (pt_in(x, y, 116, 8, 56, 56)) {
            if (g_song.clock.running) {
                clock_stop(&g_song.clock);
                audio_panic();
                for (int k = 0; k < MAX_KEYS; k++) {
                    s_key_held[k]    = false;
                    s_key_off_cnt[k] = 0;
                    if (k < s_key_cnt) s_piano_keys[k].pressed = false;
                }
                g_live_held_count = 0;
            } else {
                clock_start(&g_song.clock);
            }
            ws_notify_change(WS_MSG_TRANSPORT, -1);
            return true;
        }
    }

    if (is_main_screen(scr) && handle_tabbar_tap(x, y))
        return true; /* tab changed — redraw */

    bool tapped = false;
    switch (scr) {
    case SCREEN_SONG:          tapped = handle_song_view_tap(x, y);   break;
    case SCREEN_LIVE:          tapped = handle_live_tap(x, y);        break;
    case SCREEN_MASTER:        tapped = handle_master_tap(x, y);      break;
    case SCREEN_MENU:          tapped = handle_menu_tap(x, y);        break;
    case SCREEN_DRUM_GRID:     tapped = handle_drum_grid_tap(x, y);   break;
    case SCREEN_PIANO_ROLL:    tapped = handle_piano_roll_tap(x, y);  break;
    case SCREEN_FX_ADSR:       tapped = handle_fx_adsr_tap(x, y);     break;
    case SCREEN_SOUND_BROWSER: tapped = handle_sound_browser_tap(x, y); break;
    case SCREEN_SETTINGS:      tapped = handle_settings_tap(x, y);    break;
    case SCREEN_SONG_BROWSER:  tapped = handle_song_browser_tap(x, y);break;
    case SCREEN_WAV_DETAIL:    tapped = handle_wav_detail_tap(x, y);  break;
    case SCREEN_ARP:           tapped = handle_arp_tap(x, y);         break;
    case SCREEN_GROOVE:        tapped = handle_groove_tap(x, y);      break;
    case SCREEN_ARRANGEMENT:   tapped = handle_arrangement_tap(x, y); break;
    case SCREEN_NOTE_REPEAT:   tapped = handle_note_repeat_tap(x, y); break;
    case SCREEN_OSK:           tapped = handle_osk_tap(x, y);          break;
    case SCREEN_BLUETOOTH:     tapped = handle_bluetooth_tap(x, y);    break;
    case SCREEN_SYNTH_EDIT:    tapped = handle_synth_edit_tap(x, y);   break;
    default: break;
    }

    /* Redraw if screen/overlay changed, or any handler consumed the tap. */
    return tapped || (current_screen() != scr_pre) || (s_ctx_menu_open != overlay_pre);
}

/* ══════════════════════════════════════════════════════════════════════════
 * UI task (core 0, priority 5)
 * ══════════════════════════════════════════════════════════════════════════ */

void ui_task(void *arg)
{
    (void)arg;
    lcd_init();
    lcd_register_refresh_cb();
    touch_init();

    s_nav_stack[0] = SCREEN_SONG;
    s_nav_top      = 0;
    s_active_tab   = 0;

    draw_screen();

    bool s_live_piano_was_shown = false;

    while (1) {
        tp_pt_t pts[5];
        int cnt = touch_get_points(pts, 5);
        for (int i = 0; i < cnt; i++) touch_transform(&pts[i]);

        bool need_redraw = false;

        screen_id_t scr = current_screen();
        /* True while a synth piano is showing on LIVE; used below to keep key
         * taps off the full-redraw path so fast re-taps aren't swallowed. */
        bool live_piano_shown = false;
        if (scr == SCREEN_LIVE) {
            int li = -1, cnt2 = 0;
            for (int i = 0; i < NUM_LANES; i++) {
                if (!g_song.lanes[i].active) continue;
                if (cnt2 == s_live_lane) { li = i; break; }
                cnt2++;
            }
            bool is_piano = (li >= 0 && g_song.lanes[li].type == LANE_TYPE_SYNTH);
            live_piano_shown = is_piano;
            if (is_piano) {
                if (s_key_cnt == 0) setup_live_piano_keys();
                /* On entering the live piano, neither buffer is guaranteed to
                 * hold its chrome — force a full repaint of both. */
                if (!s_live_piano_was_shown)
                    s_live_chrome_dirty[0] = s_live_chrome_dirty[1] = true;
                /* Only touches within the key band are note input; anything
                 * below the keys (OCT bar, tab/nav bar) must fall through to
                 * the generic dispatch path. */
                int key_top = s_key_cnt > 0 ? s_piano_keys[0].y : 0;
                int key_bot = s_key_cnt > 0 ? s_piano_keys[0].y + s_piano_keys[0].h : 0;
                tp_pt_t piano_pts[5];
                int piano_cnt = 0;
                for (int i = 0; i < cnt; i++) {
                    if (s_key_cnt > 0 && pts[i].y >= key_top && pts[i].y < key_bot)
                        piano_pts[piano_cnt++] = pts[i];
                }
                /* Note on/off is queued to audio immediately here; the visual
                 * key state is rendered tear-free below by live_piano_render(). */
                handle_piano_touch(piano_pts, piano_cnt);
            }
        }

        /* Piano roll key strip — multi-touch chord audition, polled every frame */
        if (scr == SCREEN_PIANO_ROLL) {
            lane_t *pr_lane3 = &g_song.lanes[s_ctx_lane];
            synth_inst_t *pr_synth = pr_lane3->synth;
            int pr_body_y = MINIBAR_H + PR_BAR_H;
            int pr_body_h = 720 - pr_body_y - PR_TOOLBAR_H;
            int pr_rows   = pr_body_h / PR_ROW_H;

            /* Build set of currently touched notes on key strip */
            int8_t cur_aud[PR_AUD_MAX];
            int    cur_cnt = 0;
            for (int i = 0; i < cnt && cur_cnt < PR_AUD_MAX; i++) {
                int px = pts[i].x, py = pts[i].y;
                if (px >= PR_KEY_W) continue;
                if (py < pr_body_y || py >= pr_body_y + pr_rows * PR_ROW_H) continue;
                int row_i = (py - pr_body_y) / PR_ROW_H;
                int note  = s_pr_view_semitone - row_i;
                if (note < 0 || note > 127) continue;
                /* deduplicate */
                bool dup = false;
                for (int j = 0; j < cur_cnt; j++) if (cur_aud[j] == (int8_t)note) { dup = true; break; }
                if (!dup) cur_aud[cur_cnt++] = (int8_t)note;
            }
            /* Release notes no longer touched */
            if (pr_synth) {
                for (int a = 0; a < s_pr_aud_cnt; a++) {
                    bool still = false;
                    for (int b = 0; b < cur_cnt; b++) if (cur_aud[b] == s_pr_audition[a]) { still = true; break; }
                    if (!still) { audio_note_off(pr_synth, (uint8_t)s_pr_audition[a]); need_redraw = true; }
                }
            }
            /* Fire note_on for newly touched notes */
            if (pr_synth) {
                for (int b = 0; b < cur_cnt; b++) {
                    bool already = false;
                    for (int a = 0; a < s_pr_aud_cnt; a++) if (s_pr_audition[a] == cur_aud[b]) { already = true; break; }
                    if (!already) { audio_note_on(pr_synth, (uint8_t)cur_aud[b], 100); need_redraw = true; }
                }
            }
            /* Update held set */
            for (int b = 0; b < cur_cnt; b++) s_pr_audition[b] = cur_aud[b];
            s_pr_aud_cnt = cur_cnt;
        }

        /* A finger in the live piano key strip is pure note input — already
         * handled and blitted above.  Routing it through the generic touch-down
         * path would request a full draw_screen() on every press and stall the
         * touch poll, which is exactly what swallows fast re-taps, so skip it. */
        bool live_key_press = (live_piano_shown && cnt > 0 && s_key_cnt > 0 &&
                               pts[0].y >= s_piano_keys[0].y &&
                               pts[0].y <  s_piano_keys[0].y + s_piano_keys[0].h);

        if (cnt > 0 && !live_key_press) {
            int sx = pts[0].x, sy = pts[0].y;
            if (!s_touch_down) {
                s_down_x = sx; s_down_y = sy;
                /* Initialise drag baseline so the first move delta is zero
                 * rather than the stale value from the previous gesture. */
                s_drag_x = sx; s_drag_y = sy;
                s_scroll_anchor_y = sy;
                /* Show touch-ring highlight this frame */
                s_hl_x = sx; s_hl_y = sy; s_hl_visible = true;
                need_redraw = true;
                /* Record which pane a sound-browser drag will scroll */
                if (scr == SCREEN_SOUND_BROWSER) {
                    s_sb_scrolling_kit = (sx < 400);
                    s_sb_kit_vel      = 0.0f;
                    s_sb_file_vel     = 0.0f;
                    s_sb_drag_vel_acc = 0.0f;
                    /* Scrollbar drag detection: kit-bar 382..400, file-bar 1262..1280 */
                    int list_top = MINIBAR_H + 72;
                    int list_bot = 720 - 80;
                    if (sy >= list_top && sy < list_bot &&
                        ((sx >= 382 && sx < 400) || (sx >= 1262 && sx < 1280))) {
                        s_sb_sb_drag = true;
                    } else {
                        s_sb_sb_drag = false;
                    }
                    /* Defer tap-to-select until lift so a drag-scroll doesn't
                     * accidentally select a row at the touch-down position. */
                } else {
                    if (handle_touch_down(sx, sy)) need_redraw = true;
                }
                s_touch_down = true;
            } else {
                /* Long-press detection in SONG view */
                if (scr == SCREEN_SONG && s_lp_lane >= 0 && !s_ctx_menu_open) {
                    int64_t now_ms = esp_timer_get_time() / 1000;
                    if (now_ms - s_lp_start_ms >= 500) {
                        s_ctx_menu_open = true;
                        s_ctx_menu_lane = s_lp_lane;
                        s_lp_lane = -1;
                        need_redraw = true;
                    }
                }

                /* Drum velocity overlay drag */
                if (scr == SCREEN_DRUM_GRID && s_dg_vel_row >= 0) {
                    /* Slider spans x=160..1120, value 1..127 */
                    int sv = (sx - 160) * 126 / 960 + 1;
                    if (sv < 1) sv = 1;
                    if (sv > 127) sv = 127;
                    s_dg_vel_value = sv;
                    need_redraw = true;
                }
                /* Piano roll note long-press → delete */
                if (scr == SCREEN_PIANO_ROLL && s_pr_lp_note >= 0) {
                    int dx = sx - s_down_x, dy = sy - s_down_y;
                    if (dx * dx + dy * dy > 20 * 20) {
                        s_pr_lp_note = -1;  /* moved too far — it's a drag, not a delete */
                    } else {
                        int64_t held = esp_timer_get_time() / 1000 - s_lp_start_ms;
                        if (held >= 400) {
                            lane_t *lp_lane = &g_song.lanes[s_ctx_lane];
                            piano_roll_t *lp_pr = lp_lane->piano_roll;
                            if (lp_pr && s_pr_lp_note < lp_pr->note_count) {
                                pr_note_t *lp_n = &lp_pr->notes[s_pr_lp_note];
                                piano_roll_remove_note(lp_pr, lp_n->tick_start, lp_n->note);
                                g_song.dirty = true;
                                ws_notify_change(WS_MSG_NOTE_EVENT, s_ctx_lane);
                            }
                            s_pr_lp_note   = -1;
                            s_pr_drag_note = -1;
                            need_redraw    = true;
                        }
                    }
                }
                /* Drum step long-press → velocity overlay */
                if (scr == SCREEN_DRUM_GRID && s_dg_vel_row < 0 && s_lp_lane == -100) {
                    int64_t held = esp_timer_get_time() / 1000 - s_lp_start_ms;
                    if (held >= 500) {
                        s_lp_lane = -1;
                        lane_t     *dg_lane = &g_song.lanes[s_ctx_lane];
                        drum_seq_t *dg_seq  = dg_lane->drum_seq;
                        if (dg_seq && s_dg_last_row >= 0 && s_dg_last_row < dg_seq->row_count) {
                            int cur_vel = dg_seq->rows[s_dg_last_row].steps[s_dg_last_step].velocity;
                            s_dg_vel_row   = s_dg_last_row;
                            s_dg_vel_step  = s_dg_last_step;
                            s_dg_vel_value = (cur_vel > 0) ? cur_vel : 100;
                            need_redraw = true;
                        }
                    }
                }

                /* Drag: vol panel fader + vertical scroll in SONG view */
                if (scr == SCREEN_SONG) {
                    /* Vertical scroll — only when not dragging the vol fader */
                    if (!s_vol_fader_drag) {
                        int dy = sy - s_scroll_anchor_y;
                        if (dy > LANE_ROW_H / 2) {
                            if (s_song_scroll > 0) { s_song_scroll--; need_redraw = true; }
                            s_scroll_anchor_y = sy;
                        } else if (dy < -(LANE_ROW_H / 2)) {
                            s_song_scroll++;
                            need_redraw = true;
                            s_scroll_anchor_y = sy;
                        }
                    }
                    if (s_vol_fader_drag && s_fader_drag_lane >= 0) {
                        int fx = LANE_COLOR_W + 16, fw = 720;
                        float v = (float)(sx - fx) / fw;
                        if (v < 0.0f) v = 0.0f;
                        if (v > 1.0f) v = 1.0f;
                        g_song.lanes[s_fader_drag_lane].volume = v;
                        g_song.dirty = true;
                        need_redraw  = true;
                    }
                    if (s_master_vol_drag) {
                        int fw = 260, fx2 = 120;
                        float v = (float)(sx - fx2) / fw;
                        if (v < 0.0f) v = 0.0f;
                        if (v > 1.0f) v = 1.0f;
                        g_settings.master_volume = v;
                        need_redraw = true;
                    }
                    if (s_master_pan_drag) {
                        int fw = 260, fx2 = 120;
                        float v = (float)(sx - fx2) / fw * 2.0f - 1.0f;
                        if (v < -1.0f) v = -1.0f;
                        if (v >  1.0f) v =  1.0f;
                        g_settings.master_pan = v;
                        need_redraw = true;
                    }
                }
                /* Drag: smooth-scroll kit and file lists in sound browser */
                if (scr == SCREEN_SOUND_BROWSER) {
                    if (s_sb_sb_drag) {
                        /* Scrollbar drag: absolute position from finger y */
                        const float row_h = 72.0f;
                        int list_top = MINIBAR_H + 72;
                        int list_bot = 720 - 80;
                        int track_h  = (list_bot - list_top) - 8;
                        float frac = (float)(sy - (list_top + 4)) / (float)track_h;
                        if (frac < 0.0f) frac = 0.0f;
                        if (frac > 1.0f) frac = 1.0f;
                        int rows_vis = (720 - MINIBAR_H - 80) / 72 - 1;
                        if (rows_vis < 1) rows_vis = 1;
                        if (s_sb_scrolling_kit) {
                            int max = s_sb_kit_count - rows_vis;
                            if (max < 0) max = 0;
                            s_sb_kit_px = frac * (float)max * row_h;
                        } else {
                            int max = s_sb_file_count - rows_vis;
                            if (max < 0) max = 0;
                            s_sb_file_px = frac * (float)max * row_h;
                        }
                        sb_sync_scroll();
                        need_redraw = true;
                    } else {
                        float delta = (float)(s_drag_y - sy);  /* up = positive = scroll forward */
                        /* EMA velocity accumulator: weight recent frames higher */
                        s_sb_drag_vel_acc = s_sb_drag_vel_acc * 0.6f + delta * 0.4f;
                        if (delta != 0.0f) {
                            if (s_sb_scrolling_kit)
                                s_sb_kit_px  += delta;
                            else
                                s_sb_file_px += delta;
                            sb_sync_scroll();
                            need_redraw = true;
                        }
                    }
                }
                /* Drag: master volume/pan in MASTER screen */
                if (scr == SCREEN_MASTER) {
                    int fdr_h = 200, fdr_x = 40, fdr_y = CONTENT_Y + 52;
                    if (pt_in(sx, sy, fdr_x - 8, fdr_y, 56, fdr_h)) {
                        float v = 1.0f - (float)(sy - fdr_y) / fdr_h;
                        g_settings.master_volume =
                            v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
                        need_redraw = true;
                    }
                    /* Master pan horizontal fader */
                    int col1_w = 340;
                    int pan_fy = fdr_y + fdr_h + 60;
                    int pan_fw = col1_w - 48;
                    if (pt_in(sx, sy, 24, pan_fy - 4, pan_fw, 22)) {
                        float v = (float)(sx - 24) / pan_fw * 2.0f - 1.0f;
                        if (v < -1.0f) v = -1.0f;
                        if (v >  1.0f) v =  1.0f;
                        g_settings.master_pan = v;
                        need_redraw = true;
                    }
                }
                /* Drag / resize in PIANO ROLL */
                if (scr == SCREEN_PIANO_ROLL &&
                    (s_pr_drag_note >= 0 || s_pr_resize_note >= 0)) {
                    lane_t       *pr_lane = &g_song.lanes[s_ctx_lane];
                    piano_roll_t *pr      = pr_lane->piano_roll;
                    if (pr) {
                        int roll_w = 1280 - PR_KEY_W;
                        uint32_t bar_t2    = CLOCK_BAR_TICKS(&g_song.clock);
                        int total_bars2    = (pr_lane->loop_len_ticks && bar_t2) ?
                                            (int)(pr_lane->loop_len_ticks / bar_t2) : 2;
                        if (total_bars2 < 1) total_bars2 = 2;
                        uint32_t total_t2  = (uint32_t)(total_bars2 * (int)bar_t2);
                        uint32_t vw2       = s_pr_ticks_wide ? s_pr_ticks_wide : total_t2;
                        if (vw2 < bar_t2 / 16) vw2 = bar_t2 / 16;
                        if (vw2 > total_t2)    vw2 = total_t2;
                        uint32_t snap2     = bar_t2 / 16;   /* 1/16th note */
                        (void)(MINIBAR_H + PR_BAR_H); /* body_y2 unused */

                        uint32_t cur_tick = pr_x_to_tick(sx, roll_w, vw2, total_t2);
                        if (snap2 > 0) cur_tick = (cur_tick / snap2) * snap2;

                        if (s_pr_drag_note >= 0 && s_pr_drag_note < pr->note_count) {
                            pr_note_t *n = &pr->notes[s_pr_drag_note];
                            /* horizontal: pixel delta → ticks, snap once at end */
                            int64_t px_delta = (int64_t)(sx - s_down_x);
                            int64_t tick_delta = px_delta * (int64_t)vw2 / roll_w;
                            int64_t new_t = (int64_t)s_pr_drag_orig_tick + tick_delta;
                            if (snap2 > 0) new_t = (new_t / (int64_t)snap2) * (int64_t)snap2;
                            if (new_t < 0) new_t = 0;
                            if ((uint32_t)new_t + n->tick_len > total_t2)
                                new_t = (int64_t)(total_t2 - n->tick_len);
                            n->tick_start = (uint32_t)new_t;
                            /* vertical: row offset from finger-down position */
                            int drow = (sy - s_down_y) / PR_ROW_H;
                            int new_note = (s_pr_view_semitone - s_pr_drag_orig_row) - drow;
                            if (new_note < 0)   new_note = 0;
                            if (new_note > 127) new_note = 127;
                            n->note = (uint8_t)new_note;
                            g_song.dirty = true;
                            need_redraw  = true;
                        }
                        if (s_pr_resize_note >= 0 && s_pr_resize_note < pr->note_count) {
                            pr_note_t *n = &pr->notes[s_pr_resize_note];
                            if (cur_tick > n->tick_start) {
                                uint32_t new_len = cur_tick - n->tick_start;
                                if (snap2 > 0 && new_len < snap2) new_len = snap2;
                                n->tick_len = new_len;
                                g_song.dirty = true;
                                need_redraw  = true;
                            }
                        }
                    }
                }
                /* FX slider drag */
                if (scr == SCREEN_FX_ADSR && s_fx_param_drag >= 0) {
                    fx_slider_set_from_x(s_fx_param_drag, sx);
                    need_redraw = true;
                }
                /* Lane ADSR knob drag (vertical) */
                if (scr == SCREEN_FX_ADSR && s_fx_adsr_drag >= 0) {
                    int *fxc; lane_adsr_t *adsr;
                    fx_target_resolve(s_fx_target, &fxc, &adsr, NULL);
                    if (adsr) {
                        fx_adsr_drag_apply(adsr, s_fx_adsr_drag, sy - s_fx_adsr_drag_y);
                        need_redraw = true;
                    }
                }
                /* FX picker scroll (vertical drag) */
                if (scr == SCREEN_FX_ADSR && s_fx_picker_open) {
                    int dy = sy - s_scroll_anchor_y;
                    if (dy > 36) {
                        if (s_fx_picker_scroll > 0) { s_fx_picker_scroll--; need_redraw = true; }
                        s_scroll_anchor_y = sy;
                    } else if (dy < -36) {
                        s_fx_picker_scroll++;
                        need_redraw = true;
                        s_scroll_anchor_y = sy;
                    }
                }
                s_drag_x = sx; s_drag_y = sy;
            }
        } else {
            if (s_touch_down) {
                /* lift: broadcast final values, clear drag state */
                if (s_fader_drag_lane >= 0)
                    ws_notify_change(WS_MSG_LANE_UPDATE, s_fader_drag_lane);
                if (s_pan_drag_lane >= 0)
                    ws_notify_change(WS_MSG_LANE_UPDATE, s_pan_drag_lane);
                if (s_master_vol_drag || s_master_pan_drag)
                    ws_notify_change(WS_MSG_MASTER_UPDATE, -1);
                if (s_pr_drag_note >= 0 || s_pr_resize_note >= 0)
                    ws_notify_change(WS_MSG_NOTE_EVENT, s_ctx_lane);
                /* Release all audition notes on finger lift */
                if (s_pr_aud_cnt > 0) {
                    lane_t *pr_lane2 = &g_song.lanes[s_ctx_lane];
                    if (pr_lane2->synth) {
                        for (int _a = 0; _a < s_pr_aud_cnt; _a++)
                            audio_note_off(pr_lane2->synth, (uint8_t)s_pr_audition[_a]);
                    }
                    s_pr_aud_cnt = 0;
                }
                /* Drum velocity: commit on lift */
                if (s_dg_vel_row >= 0) {
                    drum_seq_t *seq = g_song.lanes[s_ctx_lane].drum_seq;
                    if (seq && s_dg_vel_row < seq->row_count) {
                        drum_step_t *step = &seq->rows[s_dg_vel_row].steps[s_dg_vel_step];
                        step->velocity = (uint8_t)s_dg_vel_value;
                        g_song.dirty = true;
                        ws_notify_change(WS_MSG_DRUM_STEP, (s_ctx_lane << 8) | s_dg_vel_row);
                    }
                    s_dg_vel_row = -1;
                }
                /* Sound browser: tap-vs-drag discrimination on lift.
                 * If the finger moved < 12px, treat it as a tap and select
                 * the item at the touch-down position. Otherwise hand off the
                 * accumulated drag velocity to momentum. */
                if (scr == SCREEN_SOUND_BROWSER) {
                    if (s_sb_sb_drag) {
                        /* Scrollbar drag: no momentum, no tap */
                        s_sb_kit_vel  = 0.0f;
                        s_sb_file_vel = 0.0f;
                    } else {
                        int dx = s_drag_x - s_down_x;
                        int dy = s_drag_y - s_down_y;
                        int dist2 = dx*dx + dy*dy;
                        if (dist2 < 12 * 12) {
                            if (handle_touch_down(s_down_x, s_down_y))
                                need_redraw = true;
                        } else {
                            /* Scale up so a fast swipe feels natural (EMA is damped) */
                            float kick = s_sb_drag_vel_acc * 4.0f;
                            if (s_sb_scrolling_kit)
                                s_sb_kit_vel  = kick;
                            else
                                s_sb_file_vel = kick;
                        }
                    }
                    s_sb_drag_vel_acc = 0.0f;
                    s_sb_sb_drag = false;
                }
                if (s_fx_param_drag >= 0) {
                    int *fxc2; int notify2;
                    fx_target_resolve(s_fx_target, &fxc2, NULL, &notify2);
                    ws_notify_change(WS_MSG_FX_UPDATE, notify2);
                    s_fx_param_drag = -1;
                }
                if (s_fx_adsr_drag >= 0) {
                    if (s_fx_target >= 0 && s_fx_target < NUM_LANES)
                        ws_notify_change(WS_MSG_ADSR_UPDATE, s_fx_target);
                    s_fx_adsr_drag = -1;
                }
                s_pr_drag_note    = -1;
                s_pr_resize_note  = -1;
                s_pr_lp_note      = -1;
                s_fader_drag_lane = -1;
                s_pan_drag_lane   = -1;
                s_vol_fader_drag  = false;
                s_master_vol_drag = false;
                s_master_pan_drag = false;
                s_lp_lane         = -1;
                s_touch_down      = false;
                need_redraw       = true;
            }
        }

        /* Animate the playhead / key highlights while the clock runs.  The
         * repaint itself is rate-limited below, so this never throttles the
         * per-iteration touch poll / note dispatch. */
        if (g_song.clock.running &&
            (scr == SCREEN_PIANO_ROLL || scr == SCREEN_DRUM_GRID ||
             (scr == SCREEN_LIVE && !live_piano_shown)))
            need_redraw = true;

        /* Smooth-scroll momentum decay for sound browser.
         * Stop momentum immediately when the finger is down (new drag). */
        if (scr == SCREEN_SOUND_BROWSER && !s_touch_down) {
            if (fabsf(s_sb_kit_vel) > 0.5f) {
                s_sb_kit_px += s_sb_kit_vel;
                s_sb_kit_vel *= 0.82f;   /* ~0.5s stop at 60fps */
                sb_sync_scroll();
                need_redraw = true;
            } else {
                s_sb_kit_vel = 0.0f;
            }
            if (fabsf(s_sb_file_vel) > 0.5f) {
                s_sb_file_px += s_sb_file_vel;
                s_sb_file_vel *= 0.82f;
                sb_sync_scroll();
                need_redraw = true;
            } else {
                s_sb_file_vel = 0.0f;
            }
        }

        /* Repaint — rate-limited to ~30 fps on the playable screens.  Touch is
         * polled and note events are queued to the audio engine every loop
         * iteration regardless of drawing, so capping the (multi-ms) full-screen
         * repaint here stops a burst of key presses — each requesting a redraw —
         * from serialising repaints in front of the next touch poll and making
         * notes lag. */
        static int64_t s_last_draw_ms   = 0;
        static bool    s_redraw_pending = false;
        bool fast_poll = (scr == SCREEN_LIVE || scr == SCREEN_PIANO_ROLL);
        bool cap_draw  = fast_poll || (scr == SCREEN_DRUM_GRID);
        if (live_piano_shown) {
            /* Live piano renders itself tear-free without blocking the poll: a
             * chrome change repaints both buffers, key presses update only the
             * changed keys into the back buffer. See live_piano_render(). */
            if (need_redraw)
                s_live_chrome_dirty[0] = s_live_chrome_dirty[1] = true;
            live_piano_render();
            s_redraw_pending = false;
        } else {
            if (need_redraw) s_redraw_pending = true;
            if (s_redraw_pending) {
                int64_t now_ms = esp_timer_get_time() / 1000;
                if (!cap_draw || now_ms - s_last_draw_ms >= 33) {
                    draw_screen();
                    s_last_draw_ms   = now_ms;
                    s_redraw_pending = false;
                }
            }
        }

        s_live_piano_was_shown = live_piano_shown;
        vTaskDelay(pdMS_TO_TICKS(fast_poll ? 4 : 16));
    }
}
