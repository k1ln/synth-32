/*
 * ui_screens.c — per-screen draw functions for the v2 device UI.
 *
 * Each draw_*_screen() / draw_*_view() function is called from
 * draw_screen() in ui_draw.c.  All shared state is accessed via
 * externs declared in ui_state.h.
 */

#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <math.h>
#include <dirent.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "gfx.h"
#include "lane.h"
#include "clock.h"
#include "drum_seq.h"
#include "piano_roll.h"
#include "arp.h"
#include "fx.h"
#include "settings.h"
#include "song.h"
#include "ui.h"
#include "ui_state.h"
#include "render_export.h"
#include "audio.h"
#include "ws_server.h"

int  s_arr_sel_step = -1;
bool s_eucl_popup   = false;
int  s_eucl_hits    = 4;
int  s_eucl_steps   = 16;

/* ── Bluetooth state ─────────────────────────────────────────────────────── */
char s_bt_devices[BT_SCAN_MAX][32] = {};
int  s_bt_device_count             = 0;
int  s_bt_sel                      = -1;
bool s_bt_scanning                 = false;
bool s_bt_connected                = false;
char s_bt_connected_name[32]       = {};

/* ── Forward declarations ────────────────────────────────────────────────── */
static void draw_scene_strip(void);

/* ── Draw helpers provided by ui_draw.c ─────────────────────────────────── */
void draw_text_centred(int x, int y, int w, int h, const char *str,
                       uint16_t fg, uint16_t bg, int scale);
void draw_seg(int x, int y, int w, int h, const char **opts, int n,
              int active, uint16_t active_fg, uint16_t active_bg);
void draw_fader(int x, int y, int w, int h, float frac, uint16_t col);
void draw_toggle(int x, int y, bool on);
void draw_type_chip(int x, int y, lane_type_t type);
uint16_t lane_accent(lane_type_t t);
void draw_minibar(const char *title);
void draw_minibar_ex(const char *title,
                     const char *ctrl1_lbl, const char *ctrl1_val,
                     const char *ctrl2_lbl, const char *ctrl2_val);

/* ══════════════════════════════════════════════════════════════════════════
 * SCREEN: SONG VIEW
 * ══════════════════════════════════════════════════════════════════════════ */

static void draw_song_lane_row(int yi, int li, const lane_t *lane)
{
    int y = yi;
    bool muted   = lane->mute;
    bool soloed  = lane->solo;
    bool vol_open = (li == s_vol_open_lane);

    uint16_t row_bg = C_BG1;
    uint16_t accent = lane_accent(lane->type);

    /* Row background */
    gfx_fill_rect(0, y, 1280, LANE_ROW_H, row_bg);
    if (muted)
        gfx_fill_rect(0, y, 1280, LANE_ROW_H, 0x0820u);

    /* White separator line at bottom of row */
    gfx_hline(y + LANE_ROW_H - 1, vol_open ? accent : C_LINE2);

    /* Left accent bar */
    gfx_fill_rect(0, y, LANE_COLOR_W, LANE_ROW_H, muted ? C_T3 : accent);

    /* Vertical centre for single-line layout */
    int cy = y + LANE_ROW_H / 2;  /* pixel centre = y + 50 */

    /* ── LEFT SIDE: number + name + type chip ─────────────────────────────── */
    char num[16];
    snprintf(num, sizeof(num), "%02d", li + 1);
    /* scale-2 text is 16px tall → top = cy - 8 */
    gfx_draw_text(LANE_COLOR_W + 10, cy - 8, num, C_T2, row_bg, 2);

    char name[48] = "\xe2\x80\x94";
    if (lane->name[0] != '\0') {
        strncpy(name, lane->name, 47); name[47] = '\0';
    } else if (lane->type == LANE_TYPE_WAV) {
        const char *slash = strrchr(lane->wav_path, '/');
        strncpy(name, slash ? slash + 1 : lane->wav_path, 47); name[47] = '\0';
        char *dot = strrchr(name, '.'); if (dot) *dot = '\0';
    } else if (lane->type == LANE_TYPE_DRUM) {
        strcpy(name, "Drum");
    } else if (lane->type == LANE_TYPE_SYNTH) {
        static const char *SYNTH_NAMES[] = {
            "Mono WT","Poly WT","SuperSaw","FM2","FM4","Subtractive",
            "KS","Bell","Pad","Noise","Bass","Lead","Chord",
            "BD","SD","HH","Organ","Morph","Vowel","Bitcrush",
        };
        if (lane->synth && lane->synth->type_id < 20)
            strncpy(name, SYNTH_NAMES[lane->synth->type_id], 47);
    }

    int name_x = LANE_COLOR_W + 52;
    /* scale-3 text is 24px tall → top = cy - 12 */
    gfx_draw_text(name_x, cy - 12, name, muted ? C_T3 : C_T0, row_bg, 3);

    int chip_x = name_x + gfx_text_width(name, 3) + 14;
    if (!muted) draw_type_chip(chip_x, cy - 10, lane->type);

    /* ── Bars label — fixed position ────────────────────────────────────── */
    uint32_t bar_t = CLOCK_BAR_TICKS(&g_song.clock);
    char loop_buf[16] = "\xe2\x80\x94";
    if (lane->loop_len_ticks > 0 && bar_t > 0) {
        int bars = (int)(lane->loop_len_ticks / bar_t);
        snprintf(loop_buf, sizeof(loop_buf), bars == 1 ? "1 bar" : "%d bars", bars);
    }
    gfx_draw_text(500, cy - 8, loop_buf, C_T2, row_bg, 2);

    /* ── VOL pill — fixed position ───────────────────────────────────────── */
    int vol_bx = 620;
    int vol_bw = 88, vol_bh = 36;
    int vol_by = cy - vol_bh / 2;
    uint16_t vol_bg2 = vol_open ? accent  : C_BG3;
    uint16_t vol_fg  = vol_open ? C_BG    : C_T0;
    uint16_t vol_bdr = vol_open ? accent  : C_LINE2;
    gfx_fill_round_rect(vol_bx, vol_by, vol_bw, vol_bh, 8, vol_bg2);
    gfx_draw_round_rect(vol_bx, vol_by, vol_bw, vol_bh, 8, vol_bdr);
    char vol_str[12];
    snprintf(vol_str, sizeof(vol_str), "%d%%", (int)(lane->volume * 100.0f + 0.5f));
    draw_text_centred(vol_bx, vol_by, vol_bw, vol_bh, vol_str, vol_fg, vol_bg2, 2);

    /* ── Vol panel (inline, rendered as continuation below this row) ──────── */
    if (vol_open) {
        int py = y + LANE_ROW_H;
        gfx_fill_rect(0, py, 1280, LANE_VOL_H, C_BG2);
        gfx_hline(py + LANE_VOL_H - 1, C_LINE2);
        gfx_fill_rect(0, py, LANE_COLOR_W, LANE_VOL_H, accent);

        int fx = LANE_COLOR_W + 16, fw = 720;
        int fy = py + (LANE_VOL_H - 24) / 2;
        gfx_fill_round_rect(fx, fy + 8, fw, 8, 4, C_BG3);
        int fill_w = (int)(lane->volume * fw);
        if (fill_w > fw) fill_w = fw;
        if (fill_w > 0) gfx_fill_round_rect(fx, fy + 8, fill_w, 8, 4, accent);
        int thumb_x = fx + fill_w - 12;
        if (thumb_x < fx) thumb_x = fx;
        gfx_fill_round_rect(thumb_x, fy, 24, 24, 12, accent);
        gfx_draw_round_rect(thumb_x, fy, 24, 24, 12, 0xFFFFu);
        gfx_draw_text(fx, py + 8, "VOL", C_T2, C_BG2, 1);

        int bx = fx + fw + 24;
        gfx_draw_text(bx, py + 8, "BARS", C_T2, C_BG2, 1);
        int bars2 = (bar_t > 0 && lane->loop_len_ticks > 0)
                    ? (int)(lane->loop_len_ticks / bar_t) : 0;
        char bstr[16];
        snprintf(bstr, sizeof(bstr), "%d", bars2 > 0 ? bars2 : 0);
        gfx_fill_round_rect(bx,       py + 30, 48, 48, 8, C_BG3);
        gfx_draw_round_rect(bx,       py + 30, 48, 48, 8, C_LINE2);
        draw_text_centred   (bx,       py + 30, 48, 48, "-", C_T0, C_BG3, 3);
        draw_text_centred   (bx + 52,  py + 30, 52, 48, bstr, C_LIME, C_BG2, 3);
        gfx_fill_round_rect(bx + 108, py + 30, 48, 48, 8, C_BG3);
        gfx_draw_round_rect(bx + 108, py + 30, 48, 48, 8, C_LINE2);
        draw_text_centred   (bx + 108, py + 30, 48, 48, "+", C_LIME, C_BG3, 3);
    }

    /* ── RIGHT SIDE (built right-to-left) ────────────────────────────────── */
    int rx = 1280;
    int btn_sz = 56;  /* square button size */
    int btn_by = cy - btn_sz / 2;

    /* Chevron > */
    rx -= LANE_CHEV_W;
    gfx_fill_rect(rx, y, LANE_CHEV_W, LANE_ROW_H, row_bg);
    gfx_vline(rx, y, LANE_ROW_H, C_LINE2);
    draw_text_centred(rx, y, LANE_CHEV_W, LANE_ROW_H, ">", C_T2, row_bg, 2);

    /* FX button — centred in row */
    rx -= LANE_FX_W;
    gfx_fill_rect(rx, y, LANE_FX_W, LANE_ROW_H, row_bg);
    gfx_vline(rx, y, LANE_ROW_H, C_LINE2);
    bool has_fx = lane->fx_count > 0;
    draw_text_centred(rx, y, LANE_FX_W, LANE_ROW_H, "FX",
                      has_fx ? C_LIME : C_T3, row_bg, 2);

    /* SYNTH EDIT button — synth lanes only */
    if (lane->type == LANE_TYPE_SYNTH) {
        rx -= LANE_EDIT_W;
        gfx_fill_rect(rx, y, LANE_EDIT_W, LANE_ROW_H, 0x0240u);
        gfx_vline(rx, y, LANE_ROW_H, C_LINE2);
        draw_text_centred(rx, y, LANE_EDIT_W, LANE_ROW_H, "EDIT", C_CYAN, 0x0240u, 2);
    }

    /* Solo button */
    rx -= (btn_sz + 16);
    gfx_fill_round_rect(rx, btn_by, btn_sz, btn_sz, 6,
                        soloed ? C_AMBER : C_BG3);
    gfx_draw_round_rect(rx, btn_by, btn_sz, btn_sz, 6,
                        soloed ? C_AMBER : C_LINE2);
    draw_text_centred(rx, btn_by, btn_sz, btn_sz, "S",
                      soloed ? C_BG : C_T2, soloed ? C_AMBER : C_BG3, 2);

    /* Mute button */
    rx -= (btn_sz + 8);
    gfx_fill_round_rect(rx, btn_by, btn_sz, btn_sz, 6,
                        muted ? C_RED : C_BG3);
    gfx_draw_round_rect(rx, btn_by, btn_sz, btn_sz, 6,
                        muted ? C_RED : C_LINE2);
    draw_text_centred(rx, btn_by, btn_sz, btn_sz, "M",
                      muted ? 0xFFFFu : C_T2, muted ? C_RED : C_BG3, 2);
}

static void draw_lane_add_row(int yi)
{
    int y = yi;  /* pixel y passed directly */
    gfx_fill_rect(0, y, 1280, LANE_ADD_H, C_BG2);
    gfx_hline(y + LANE_ADD_H - 1, C_LINE);

    /* Two type-choice buttons: SYNTH | Drum/Sample */
    int bh = LANE_ADD_H - 16, by = y + 8;
    int bx[2] = { 24, 652 };
    int bw[2] = { 604, 604 };
    const char *labels[2] = { "+ SYNTH", "+ Drum/Sample" };
    uint16_t cols[2] = { C_CYAN, C_AMBER };
    for (int i = 0; i < 2; i++) {
        gfx_fill_round_rect(bx[i], by, bw[i], bh, 6, C_BG3);
        gfx_draw_round_rect(bx[i], by, bw[i], bh, 6, cols[i]);
        draw_text_centred(bx[i], by, bw[i], bh, labels[i], cols[i], C_BG3, 2);
    }
}

static void draw_master_strip(void)
{
    int y = MASTER_Y;
    gfx_fill_rect(0, y, 1280, MASTER_H, C_BG2);
    gfx_hline(y, C_LINE2);
    gfx_draw_text(24, y + 20, "MASTER", C_T2, C_BG2, 1);

    /* Volume fader */
    int fader_x = 120, fader_w = 260;
    draw_fader(fader_x, y + 14, fader_w, 10, g_settings.master_volume, C_LIME);

    /* Pan fader */
    float pan_frac = (g_settings.master_pan + 1.0f) * 0.5f;
    draw_fader(fader_x, y + 34, fader_w, 10, pan_frac, C_CYAN);
    gfx_fill_rect(fader_x + fader_w / 2, y + 32, 2, 14, C_LINE2);

    char db_buf[12];
    float vol = g_settings.master_volume > 0.001f ? g_settings.master_volume : 0.001f;
    snprintf(db_buf, sizeof(db_buf), "%.1f dB", 20.0f * log10f(vol));
    gfx_draw_text(fader_x + fader_w + 12, y + 14, db_buf, C_T0, C_BG2, 2);

    /* Playback mode toggle */
    bool is_song_mode = (g_song.playback_mode == 1);
    int pb_x = fader_x + fader_w + 12;
    int pb_y = y + 32;
    gfx_fill_round_rect(pb_x,      pb_y, 56, 20, 4, is_song_mode ? C_BG3 : C_LIME_DIM);
    gfx_fill_round_rect(pb_x + 60, pb_y, 56, 20, 4, is_song_mode ? C_LIME_DIM : C_BG3);
    gfx_draw_text(pb_x + 10,      pb_y + 4, "LIVE", is_song_mode ? C_T3 : C_LIME, C_BG2, 1);
    gfx_draw_text(pb_x + 66,      pb_y + 4, "SONG", is_song_mode ? C_LIME : C_T3, C_BG2, 1);

    /* Meters */
    int mx = 760;
    for (int ch = 0; ch < 2; ch++) {
        int my = y + 10 + ch * 20;
        gfx_fill_round_rect(mx, my, 400, 10, 3, C_BG4);
        gfx_fill_round_rect(mx, my, 280, 10, 3, C_LIME);
    }
}

void draw_song_view(void)
{
    gfx_fill_rect(0, CONTENT_Y, 1280, SONG_BODY_H, C_BG1);

    /* count active lanes for scroll indicator */
    int total_active = 0;
    for (int i = 0; i < NUM_LANES; i++)
        if (g_song.lanes[i].active) total_active++;

    /* clamp scroll */
    int max_visible = SONG_BODY_H / LANE_ROW_H;
    if (s_song_scroll > total_active - max_visible)
        s_song_scroll = total_active - max_visible;
    if (s_song_scroll < 0) s_song_scroll = 0;

    int vis_row = 0;
    int abs_row = 0;
    int y_cursor = CONTENT_Y;   /* pixel y for next row — accounts for open panel */
    for (int i = 0; i < NUM_LANES; i++) {
        if (!g_song.lanes[i].active) continue;
        if (abs_row < s_song_scroll) { abs_row++; continue; }
        if (y_cursor + LANE_ROW_H > CONTENT_Y + SONG_BODY_H) break;
        draw_song_lane_row(y_cursor, i, &g_song.lanes[i]);
        y_cursor += LANE_ROW_H;
        if (i == s_vol_open_lane) y_cursor += LANE_VOL_H;  /* panel drawn inside row fn */
        vis_row++;
        abs_row++;
    }

    /* ADD row */
    if (y_cursor + LANE_ADD_H <= CONTENT_Y + SONG_BODY_H)
        draw_lane_add_row(y_cursor);

    /* Scroll indicator */
    if (total_active > max_visible) {
        int dot_h = SONG_BODY_H / total_active;
        int dot_y = CONTENT_Y + s_song_scroll * dot_h;
        gfx_fill_rect(1272, CONTENT_Y, 8, SONG_BODY_H, C_BG4);
        gfx_fill_rect(1272, dot_y, 8, dot_h * max_visible, C_T2);
    }

    draw_master_strip();
    draw_scene_strip();
}

static void draw_scene_strip(void)
{
    int y = MASTER_Y - 52;
    gfx_fill_rect(0, y, 1280, 52, C_BG2);
    gfx_hline(y, C_LINE2);
    gfx_draw_text(14, y + 16, "SCN", C_T2, C_BG2, 1);

    int bw = 108, gap = 8, x0 = 80;
    for (int i = 0; i < SCENE_MAX; i++) {
        int bx = x0 + i * (bw + gap);
        bool active = (g_song.active_scene == i);
        bool filled = g_song.scenes[i].active;
        uint16_t bg  = active ? C_LIME_DIM : (filled ? C_BG3 : C_BG2);
        uint16_t brd = active ? C_LIME     : (filled ? C_LINE2 : C_LINE);
        gfx_fill_round_rect(bx, y + 6, bw, 40, 6, bg);
        gfx_draw_round_rect(bx, y + 6, bw, 40, 6, brd);
        char lbl[4] = { (char)('A' + i), '\0' };
        draw_text_centred(bx, y + 6, bw, 40, lbl,
                          active ? C_LIME : (filled ? C_T0 : C_T3), bg, 2);
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 * SCREEN: LIVE PLAY
 * ══════════════════════════════════════════════════════════════════════════ */

static void draw_live_lane_tabs(void)
{
    int y = CONTENT_Y;
    gfx_fill_rect(0, y, 1280, LIVE_LANE_TAB_H, C_BG2);
    gfx_hline(y + LIVE_LANE_TAB_H - 1, C_LINE);

    int x = 0, active_cnt = 0;
    for (int i = 0; i < NUM_LANES; i++) {
        if (!g_song.lanes[i].active) continue;
        char label[16];
        snprintf(label, sizeof(label), "%02d", i + 1);
        bool active = (active_cnt == s_live_lane);
        int tw = gfx_text_width(label, 2) + 48;
        uint16_t bg = active ? C_LIME_DIM : C_BG2;
        uint16_t fg = active ? C_LIME     : C_T2;
        gfx_fill_rect(x, y, tw, LIVE_LANE_TAB_H, bg);
        gfx_vline(x + tw, y, LIVE_LANE_TAB_H, C_LINE);
        gfx_draw_text(x + 24, y + 14, label, fg, bg, 2);
        x += tw;
        active_cnt++;
    }
}

static void draw_live_pad_grid(void)
{
    int pad_y = CONTENT_Y + LIVE_LANE_TAB_H + LIVE_PAD_PAD;
    int pad_area_h = TABBAR_Y - pad_y - LIVE_PAD_PAD;
    int pad_w = (1280 - LIVE_PAD_PAD * 5) / 4;
    int pad_h = (pad_area_h - LIVE_PAD_GAP) / 2;

    static const char *PAD_NAMES[8] = {
        "KICK","SNARE","HH-CL","HH-OP","CLAP","TOM","RIDE","CRASH"
    };
    for (int i = 0; i < 8; i++) {
        int col = i % 4, row = i / 4;
        int px = LIVE_PAD_PAD + col * (pad_w + LIVE_PAD_GAP);
        int py = pad_y + row * (pad_h + LIVE_PAD_GAP);
        gfx_fill_round_rect(px, py, pad_w, pad_h, 12, C_BG3);
        gfx_draw_round_rect(px, py, pad_w, pad_h, 12, C_LINE2);
        draw_text_centred(px, py, pad_w, pad_h, PAD_NAMES[i], C_T0, C_BG3, 3);
    }
}

void setup_live_piano_keys(void)
{
    s_key_cnt = 0;
    int piano_y = CONTENT_Y + LIVE_LANE_TAB_H + 72 + 12;
    int piano_h = TABBAR_Y - piano_y - 72 - 12;
    int wkw = 1280 / 14;          /* white key width */
    int bkw = (int)(wkw * 0.72f); /* black key width — wide enough to tap */
    int bkh = (int)(piano_h * 0.58f);

    /* White keys — white key index k maps to these semitones:
       C=0 D=2 E=4 F=5 G=7 A=9 B=11 */
    for (int oct = 0; oct < 2; oct++) {
        for (int k = 0; k < 7; k++) {
            piano_key_t r = {0};
            r.x          = (int16_t)(oct * 7 * wkw + k * wkw);
            r.y          = (int16_t)piano_y;
            r.w          = (int16_t)(wkw - 2);
            r.h          = (int16_t)piano_h;
            r.semi       = s_wk_semi[k];
            r.oct_offset = oct;
            r.is_black   = false;
            s_piano_keys[s_key_cnt++] = r;
        }
    }

    /* Black keys centred at the boundary between adjacent white keys.
       White-key boundaries (in units of wkw) for a standard octave:
         C#  between C(0) and D(1)  → at x = 1*wkw
         D#  between D(1) and E(2)  → at x = 2*wkw
         F#  between F(3) and G(4)  → at x = 4*wkw
         G#  between G(4) and A(5)  → at x = 5*wkw
         A#  between A(5) and B(6)  → at x = 6*wkw              */
    static const int BK_BOUNDARY[5] = { 1, 2, 4, 5, 6 }; /* white-key index of right neighbour */

    for (int oct = 0; oct < 2; oct++) {
        for (int k = 0; k < 5; k++) {
            int cx = oct * 7 * wkw + BK_BOUNDARY[k] * wkw; /* centre x */
            piano_key_t r = {0};
            r.x          = (int16_t)(cx - bkw / 2);
            r.y          = (int16_t)piano_y;
            r.w          = (int16_t)bkw;
            r.h          = (int16_t)bkh;
            r.semi       = s_bk_semi[k];
            r.oct_offset = oct;
            r.is_black   = true;
            s_piano_keys[s_key_cnt++] = r;
        }
    }
}

static void draw_live_piano(void)
{
    int synth_info_y = CONTENT_Y + LIVE_LANE_TAB_H;
    int synth_info_h = 72;

    gfx_fill_rect(0, synth_info_y, 1280, synth_info_h, C_BG2);
    gfx_hline(synth_info_y + synth_info_h - 1, C_LINE);
    gfx_draw_text(24, synth_info_y + 22, "SYNTH", C_CYAN, C_BG2, 2);

    int piano_y = synth_info_y + synth_info_h + 12;
    int piano_h = TABBAR_Y - piano_y - 72 - 12;
    gfx_fill_rect(0, piano_y, 1280, piano_h, C_BG);

    for (int k = 0; k < s_key_cnt; k++)
        if (!s_piano_keys[k].is_black) draw_live_piano_key(k);
    for (int k = 0; k < s_key_cnt; k++)
        if (s_piano_keys[k].is_black)  draw_live_piano_key(k);

    int tb_y = TABBAR_Y - 72;
    gfx_fill_rect(0, tb_y, 1280, 72, C_BG2);
    gfx_hline(tb_y, C_LINE2);
    gfx_fill_round_rect(24, tb_y + 8, 120, 56, 10, C_BG3);
    gfx_draw_round_rect(24, tb_y + 8, 120, 56, 10, C_LINE2);
    gfx_draw_text(40, tb_y + 20, "< OCT", C_T0, C_BG3, 2);

    char oct_buf[32];
    snprintf(oct_buf, sizeof(oct_buf), "C%d - C%d", s_live_octave, s_live_octave + 1);
    int ow = gfx_text_width(oct_buf, 2);
    gfx_draw_text((1280 - ow) / 2, tb_y + 24, oct_buf, C_T1, C_BG2, 2);

    gfx_fill_round_rect(1280 - 144, tb_y + 8, 120, 56, 10, C_BG3);
    gfx_draw_round_rect(1280 - 144, tb_y + 8, 120, 56, 10, C_LINE2);
    gfx_draw_text(1280 - 128, tb_y + 20, "OCT >", C_T0, C_BG3, 2);
}

void draw_live_screen(void)
{
    int li = -1, cnt = 0;
    for (int i = 0; i < NUM_LANES; i++) {
        if (!g_song.lanes[i].active) continue;
        if (cnt == s_live_lane) { li = i; break; }
        cnt++;
    }

    draw_live_lane_tabs();

    if (li < 0) {
        gfx_fill_rect(0, CONTENT_Y + LIVE_LANE_TAB_H, 1280,
                      TABBAR_Y - CONTENT_Y - LIVE_LANE_TAB_H, C_BG1);
        draw_text_centred(0, CONTENT_Y + LIVE_LANE_TAB_H,
                          1280, TABBAR_Y - CONTENT_Y - LIVE_LANE_TAB_H,
                          "NO ACTIVE LANES", C_T3, C_BG1, 2);
        return;
    }

    if (g_song.lanes[li].type == LANE_TYPE_DRUM) {
        draw_live_pad_grid();
    } else if (g_song.lanes[li].type == LANE_TYPE_SYNTH) {
        setup_live_piano_keys();
        draw_live_piano();
    } else {
        gfx_fill_rect(0, CONTENT_Y + LIVE_LANE_TAB_H, 1280,
                      TABBAR_Y - CONTENT_Y - LIVE_LANE_TAB_H, C_BG1);
        gfx_draw_text(24, CONTENT_Y + LIVE_LANE_TAB_H + 40,
                      g_song.lanes[li].wav_path, C_T1, C_BG1, 2);
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 * SCREEN: MASTER
 * ══════════════════════════════════════════════════════════════════════════ */

void draw_master_screen(void)
{
    int y0 = CONTENT_Y;
    int ch = CONTENT_H;
    int panel_gap = 2;

    int col1_w = 340, col3_w = 320;
    int col2_x = col1_w + panel_gap;
    int col2_w = 1280 - col1_w - col3_w - 2 * panel_gap;
    int col3_x = col2_x + col2_w + panel_gap;

    gfx_fill_rect(0,      y0, col1_w, ch, C_BG1);
    gfx_fill_rect(col2_x, y0, col2_w, ch, C_BG1);
    gfx_fill_rect(col3_x, y0, col3_w, ch, C_BG1);
    gfx_fill_rect(col1_w, y0, panel_gap, ch, C_LINE);
    gfx_fill_rect(col3_x - panel_gap, y0, panel_gap, ch, C_LINE);

    /* Col 1: Volume + Pan + meters */
    gfx_draw_text(24, y0 + 16, "VOLUME / PAN", C_T2, C_BG1, 1);
    gfx_hline(y0 + 38, C_LINE);

    int fdr_h = 200, fdr_x = 40, fdr_y = y0 + 52;
    gfx_fill_round_rect(fdr_x, fdr_y, 40, fdr_h, 4, C_BG4);
    int fill_h = (int)(g_settings.master_volume * fdr_h);
    gfx_fill_round_rect(fdr_x, fdr_y + fdr_h - fill_h, 40, fill_h, 4, C_LIME);
    gfx_fill_rect(fdr_x - 8, fdr_y + fdr_h - fill_h - 5, 56, 10, C_T0);

    char db_buf[16];
    float vol = g_settings.master_volume > 0.001f ? g_settings.master_volume : 0.001f;
    snprintf(db_buf, sizeof(db_buf), "%.1f dB", 20.0f * log10f(vol));
    gfx_draw_text(24, fdr_y + fdr_h + 10, db_buf, C_T0, C_BG1, 2);
    gfx_draw_text(24, fdr_y + fdr_h + 34, "VOL", C_T2, C_BG1, 1);

    /* Pan fader (horizontal, below vol) */
    int pan_fy = fdr_y + fdr_h + 60;
    gfx_draw_text(24, pan_fy - 16, "PAN", C_T2, C_BG1, 1);
    float pan_frac = (g_settings.master_pan + 1.0f) * 0.5f;
    draw_fader(24, pan_fy, col1_w - 48, 14, pan_frac, C_CYAN);
    gfx_fill_rect(24 + (col1_w - 48) / 2, pan_fy - 4, 2, 22, C_LINE2);
    char pan_buf[12];
    int pan_pct = (int)(g_settings.master_pan * 100.0f);
    if (pan_pct == 0) snprintf(pan_buf, sizeof(pan_buf), "C");
    else snprintf(pan_buf, sizeof(pan_buf), pan_pct > 0 ? "R%d" : "L%d", pan_pct < 0 ? -pan_pct : pan_pct);
    gfx_draw_text(24, pan_fy + 20, pan_buf, C_T0, C_BG1, 2);

    /* Playback mode */
    int pb_y2 = pan_fy + 52;
    gfx_draw_text(24, pb_y2, "MODE", C_T2, C_BG1, 1);
    bool is_song_mode = (g_song.playback_mode == 1);
    static const char *pb_opts[] = { "LIVE", "SONG" };
    draw_seg(24, pb_y2 + 18, col1_w - 48, 44, pb_opts, 2,
             is_song_mode ? 1 : 0, C_BG, C_LIME);

    int mx = 200, my = fdr_y;
    for (int ch_i = 0; ch_i < 2; ch_i++) {
        gfx_fill_round_rect(mx + ch_i * 30, my, 20, fdr_h, 4, C_BG4);
        gfx_fill_round_rect(mx + ch_i * 30, my + fdr_h - (fdr_h * 70 / 100),
                            20, fdr_h * 70 / 100, 4, C_LIME);
        gfx_draw_text(mx + ch_i * 30 + 2, my + fdr_h + 10,
                      ch_i == 0 ? "L" : "R", C_T1, C_BG1, 2);
    }

    /* Col 2: Master FX chain */
    gfx_draw_text(col2_x + 24, y0 + 16, "MASTER FX", C_T2, C_BG1, 1);
    gfx_hline(y0 + 38, C_LINE);

    static const char *FX_SHORT_M[] = {
        "","FILT","EQ3","EQ5","COMP","LIM","GATE","TRAN",
        "DIST","ODRV","FOLD","BCSH","DLY","VERB","CHO","FLG",
        "PHS","TREM","VIB","RING","PTSH","PAN","WDTH",
        "NGSC","ENVF","DSSR","SIMR","TSAT","TUBE","EXCT",
        "HARM","FORM","COMB","TILT","PQNT","GFRZ","STUTR",
        "TSTOP","HAAS","RSON","FVRB","SFLT","SCMP","TGATE","ADLY",
    };
    int mfx_slots = FX_MAX_PER_LANE;
    int mfx_slot_w = (col2_w - 48 - (mfx_slots - 1) * 10) / mfx_slots;
    for (int i = 0; i < mfx_slots; i++) {
        int sx = col2_x + 24 + i * (mfx_slot_w + 10);
        int sy = y0 + 52;
        bool filled = (i < g_song.master_fx_count && g_song.master_fx[i] != NULL);
        bool enabled = filled && g_song.master_fx[i]->enabled;
        uint16_t bg  = filled ? C_BG3 : C_BG2;
        gfx_fill_round_rect(sx, sy, mfx_slot_w, 80, 8, bg);
        gfx_draw_round_rect(sx, sy, mfx_slot_w, 80, 8, C_LINE2);
        char slot_num[4];
        snprintf(slot_num, sizeof(slot_num), "%d", i + 1);
        gfx_draw_text(sx + 4, sy + 4, slot_num, C_T3, bg, 1);
        if (filled) {
            int tid = (int)g_song.master_fx[i]->type;
            const char *fn = (tid > 0 && tid < 45) ? FX_SHORT_M[tid] : "?";
            gfx_draw_text(sx + (mfx_slot_w - gfx_text_width(fn, 2)) / 2,
                          sy + 26, fn, C_T0, bg, 2);
            if (!enabled)
                gfx_draw_text(sx + 4, sy + 56, "OFF", C_T3, bg, 1);
        } else {
            gfx_draw_text(sx + mfx_slot_w / 2 - 8, sy + 26, "+", C_T3, C_BG2, 3);
        }
    }

    /* Col 3: Clock */
    gfx_draw_text(col3_x + 24, y0 + 16, "CLOCK", C_T2, C_BG1, 1);
    gfx_hline(y0 + 38, C_LINE);

    char bpm_big[16];
    snprintf(bpm_big, sizeof(bpm_big), "%.1f", (double)g_song.clock.bpm);
    gfx_draw_text(col3_x + 24, y0 + 52, bpm_big, C_LIME, C_BG1, 5);
    gfx_draw_text(col3_x + 24, y0 + 130, "BPM", C_T2, C_BG1, 2);

    gfx_fill_round_rect(col3_x + 24, y0 + 172, col3_w - 48, 72, 10, C_BG3);
    gfx_draw_round_rect(col3_x + 24, y0 + 172, col3_w - 48, 72, 10, C_LINE2);
    draw_text_centred(col3_x + 24, y0 + 172, col3_w - 48, 72, "TAP TEMPO", C_T0, C_BG3, 2);

    gfx_draw_text(col3_x + 24, y0 + 272, "PPQN", C_T2, C_BG1, 1);
    static const char *ppqn_opts[] = { "24", "48", "96", "192" };
    int ppqn_active = (g_song.clock.tick_rate == 24) ? 0 :
                      (g_song.clock.tick_rate == 48) ? 1 :
                      (g_song.clock.tick_rate == 96) ? 2 : 3;
    draw_seg(col3_x + 24, y0 + 300, col3_w - 48, 52,
             ppqn_opts, 4, ppqn_active, C_BG, C_LIME);

    gfx_draw_text(col3_x + 24, y0 + 380, "TIME SIG", C_T2, C_BG1, 1);
    static const char *tsig_opts[] = { "4/4", "3/4", "5/4", "7/8" };
    int tsig_active = (g_song.clock.beats_per_bar == 3) ? 1 :
                      (g_song.clock.beats_per_bar == 5) ? 2 :
                      (g_song.clock.beats_per_bar == 7) ? 3 : 0;
    draw_seg(col3_x + 24, y0 + 408, col3_w - 48, 52,
             tsig_opts, 4, tsig_active, C_BG, C_LIME);

    /* Export record/stop button */
    bool recording = render_export_active();
    uint16_t rec_bg = recording ? C_RED      : C_BG3;
    uint16_t rec_fg = recording ? C_BG       : C_T0;
    gfx_fill_round_rect(col3_x + 24, y0 + 480, col3_w - 48, 56, 10, rec_bg);
    gfx_draw_round_rect(col3_x + 24, y0 + 480, col3_w - 48, 56, 10, C_LINE2);
    draw_text_centred(col3_x + 24, y0 + 480, col3_w - 48, 56,
                      recording ? "STOP REC" : "REC MIX", rec_fg, rec_bg, 2);
    if (recording) {
        uint32_t secs = render_export_frames() / 48000;
        char elapsed[16];
        snprintf(elapsed, sizeof(elapsed), "%02lu:%02lu",
                 (unsigned long)(secs / 60), (unsigned long)(secs % 60));
        gfx_draw_text(col3_x + 24, y0 + 544, elapsed, C_RED, C_BG1, 2);
    }

    /* ── Stem export button (REC STEMS) ──────────────────────────────────── */
    bool stem_rec = g_song.stem_export_active;
    uint16_t stem_bg = stem_rec ? C_AMBER : C_BG3;
    gfx_fill_round_rect(col3_x + 24, y0 + 480 + 64, col3_w - 48, 56, 10, stem_bg);
    gfx_draw_round_rect(col3_x + 24, y0 + 480 + 64, col3_w - 48, 56, 10, C_LINE2);
    draw_text_centred(col3_x + 24, y0 + 480 + 64, col3_w - 48, 56,
                      stem_rec ? "STOP STEMS" : "REC STEMS",
                      stem_rec ? C_BG : C_T0, stem_bg, 2);

    /* ── Metronome toggle ────────────────────────────────────────────────── */
    bool metro = g_song.metronome_enabled;
    int mx_y = y0 + 480 + 128;
    gfx_draw_text(col3_x + 24, mx_y, "METRO", C_T2, C_BG1, 1);
    uint16_t mbg = metro ? C_LIME_DIM : C_BG3;
    gfx_fill_round_rect(col3_x + 24, mx_y + 16, (col3_w - 48) / 2 - 4, 40, 8, mbg);
    gfx_draw_round_rect(col3_x + 24, mx_y + 16, (col3_w - 48) / 2 - 4, 40, 8,
                        metro ? C_LIME : C_LINE2);
    draw_text_centred(col3_x + 24, mx_y + 16, (col3_w - 48) / 2 - 4, 40,
                      metro ? "ON" : "OFF", metro ? C_LIME : C_T2, mbg, 2);

    /* ── Send return level ───────────────────────────────────────────────── */
    int sr_y = mx_y + 68;
    gfx_draw_text(col3_x + 24, sr_y, "SEND RTN", C_T2, C_BG1, 1);
    draw_fader(col3_x + 24, sr_y + 20, col3_w - 48, 14,
               g_song.send_return_level, C_CYAN);
}

/* ══════════════════════════════════════════════════════════════════════════
 * SCREEN: MENU
 * ══════════════════════════════════════════════════════════════════════════ */

void draw_menu_screen(void)
{
    static const struct { const char *title; const char *sub; } ITEMS[12] = {
        { "NEW SONG",    "Blank project" },
        { "LOAD SONG",   "SD card"       },
        { "SAVE",        "Quick save"    },
        { "SAVE AS",     "Name + save"   },
        { "SOUNDS",      "Browse kits"  },
        { "SETTINGS",    "Audio / WiFi" },
        { "BLUETOOTH",   "BLE devices"  },
        { "SAMPLE CUT",  "Slice WAVs"   },
        { "ARP",         "Arpeggiator"  },
        { "GROOVE",      "Swing/Humanise"},
        { "ARRANGEMENT", "Scene chain"  },
        { "NOTE REPEAT", "Repeat/Send"  },
    };
    int n_items = 12;

    int cw = 1280 / 4;
    int ch = CONTENT_H / 3;
    gfx_fill_rect(0, CONTENT_Y, 1280, CONTENT_H, C_LINE);

    for (int i = 0; i < n_items; i++) {
        int col = i % 4, row = i / 4;
        int cx = col * cw, cy = CONTENT_Y + row * ch;
        bool primary = (i == 0);
        uint16_t bg = primary ? C_LIME_DIM : C_BG1;
        gfx_fill_rect(cx + (col > 0 ? 1 : 0), cy + (row > 0 ? 1 : 0),
                      cw - (col > 0 ? 1 : 0), ch - (row > 0 ? 1 : 0), bg);
        char num[8];
        snprintf(num, sizeof(num), "%02d", i + 1);
        gfx_draw_text(cx + 16, cy + 10, num, primary ? C_LIME : C_T3, bg, 1);
        gfx_draw_text(cx + 16, cy + 30, ITEMS[i].title,
                      primary ? C_LIME : C_T0, bg, 2);
        gfx_draw_text(cx + 16, cy + 60, ITEMS[i].sub, C_T2, bg, 1);
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 * SUB-SCREEN: DRUM GRID
 * ══════════════════════════════════════════════════════════════════════════ */

void draw_drum_grid_screen(void)
{
    lane_t     *lane = &g_song.lanes[s_ctx_lane];
    drum_seq_t *seq  = lane->drum_seq;

    char steps_buf[8], bars_buf[8];
    snprintf(steps_buf, sizeof(steps_buf), "%d", seq ? (int)seq->step_count : 16);
    snprintf(bars_buf, sizeof(bars_buf), "%d",
             (lane->loop_len_ticks && g_song.clock.tick_rate)
                 ? (int)(lane->loop_len_ticks / CLOCK_BAR_TICKS(&g_song.clock)) : 2);

    gfx_fill_rect(0, 0, 1280, MINIBAR_H, C_BG2);
    gfx_hline(MINIBAR_H - 1, C_LINE);
    gfx_fill_rect(0, 0, MINIBAR_BACK_W, MINIBAR_H, C_BG2);
    gfx_vline(MINIBAR_BACK_W, 0, MINIBAR_H, C_LINE);
    gfx_draw_text(30, 24, "<", C_T1, C_BG2, 3);

    char title[32];
    snprintf(title, sizeof(title), "DRUM %02d", s_ctx_lane + 1);
    gfx_draw_text(MINIBAR_BACK_W + 24, 22, title, C_T0, C_BG2, 3);

    /* ── STEPS +/- control (right side of minibar) ── */
    /* Layout: STEPS label + value + [−][+] at right */
    /* BARS: x=760..960  STEPS: x=960..1280 */
    gfx_vline(760, 0, MINIBAR_H, C_LINE);
    gfx_draw_text(776, 12, "BARS",  C_T2, C_BG2, 1);
    gfx_draw_text(776, 32, bars_buf, C_T0, C_BG2, 2);
    /* BARS − button */
    gfx_fill_round_rect(900, 10, 48, 52, 6, C_BG3);
    gfx_draw_round_rect(900, 10, 48, 52, 6, C_LINE2);
    draw_text_centred(900, 10, 48, 52, "-", C_T0, C_BG3, 3);
    /* BARS + button */
    gfx_fill_round_rect(956, 10, 48, 52, 6, C_BG3);
    gfx_draw_round_rect(956, 10, 48, 52, 6, C_LINE2);
    draw_text_centred(956, 10, 48, 52, "+", C_LIME, C_BG3, 3);

    gfx_vline(1012, 0, MINIBAR_H, C_LINE);
    gfx_draw_text(1028, 12, "STEPS", C_T2, C_BG2, 1);
    gfx_draw_text(1028, 32, steps_buf, C_T0, C_BG2, 2);
    /* STEPS − button */
    gfx_fill_round_rect(1152, 10, 48, 52, 6, C_BG3);
    gfx_draw_round_rect(1152, 10, 48, 52, 6, C_LINE2);
    draw_text_centred(1152, 10, 48, 52, "-", C_T0, C_BG3, 3);
    /* STEPS + button */
    gfx_fill_round_rect(1208, 10, 48, 52, 6, C_BG3);
    gfx_draw_round_rect(1208, 10, 48, 52, 6, C_LINE2);
    draw_text_centred(1208, 10, 48, 52, "+", C_LIME, C_BG3, 3);

    int steps = seq ? seq->step_count : 16;
    if (steps < 1) steps = 16;

    int hdr_y = MINIBAR_H;
    gfx_fill_rect(0, hdr_y, 1280, DG_BEAT_H, C_BG2);
    gfx_hline(hdr_y + DG_BEAT_H - 1, C_LINE);
    gfx_fill_rect(0, hdr_y, DG_LABEL_W, DG_BEAT_H, C_BG2);
    gfx_vline(DG_LABEL_W, hdr_y, DG_BEAT_H, C_LINE);

    int beats = steps / 4;
    if (beats < 1) beats = 1;
    int step_area_w = 1280 - DG_LABEL_W;
    int beat_w = step_area_w / beats;
    uint32_t cs_hdr = g_song.clock.running
                      ? (seq ? seq->current_step : 0xFFFFu) : 0u;
    int active_beat = (cs_hdr < (uint32_t)steps) ? (int)cs_hdr / 4 : -1;
    for (int b = 0; b < beats; b++) {
        int bx = DG_LABEL_W + b * beat_w;
        bool beat_active = (b == active_beat);
        /* Highlight active beat header cell */
        if (beat_active)
            gfx_fill_rect(bx + 1, hdr_y, beat_w - 1, DG_BEAT_H - 1, 0x0310u);
        char bnum[16];
        snprintf(bnum, sizeof(bnum), "%d", b + 1);
        gfx_draw_text(bx + 10, hdr_y + 10, bnum,
                      beat_active ? C_CYAN : C_T1, beat_active ? 0x0310u : C_BG2, 2);
        gfx_vline(bx + beat_w, hdr_y, DG_BEAT_H, C_LINE);
        /* Sub-step dots */
        for (int s = 0; s < 4; s++) {
            int si = b * 4 + s;
            bool is_play = (si == (int)cs_hdr);
            int dot_x = bx + beat_w / 2 - 30 + s * 20;
            gfx_fill_circle(dot_x, hdr_y + 34, is_play ? 6 : 4,
                            is_play ? C_CYAN : C_LINE2);
        }
    }

    int total_rows = seq ? seq->row_count : 0;
    int body_y = hdr_y + DG_BEAT_H;
    int body_h = SUB_CONTENT_H - DG_BEAT_H - DG_TOOLBAR_H;
    /* fit as many rows as possible at DG_STEP_H minimum; cap at total_rows */
    int rows_vis = body_h / (DG_STEP_H + 16);
    if (rows_vis < 1) rows_vis = 1;
    if (rows_vis > total_rows) rows_vis = total_rows;
    if (rows_vis == 0) rows_vis = 1;
    int row_h = body_h / rows_vis;

    /* clamp scroll offset */
    if (s_dg_row_offset > total_rows - rows_vis)
        s_dg_row_offset = total_rows - rows_vis;
    if (s_dg_row_offset < 0) s_dg_row_offset = 0;

    gfx_fill_rect(0, body_y, 1280, body_h, C_BG1);

    /* Playhead: vertical column highlight across the full body height */
    {
        /* Step pitch is sw+4 except for the final cell (no trailing gap), so
         * the right edge is sw*steps + 4*(steps-1). Solve for sw to keep all
         * cells on-screen at any step count. */
        int sw_ph = (1280 - DG_LABEL_W - 4 - 4 * (steps - 1)) / steps;
        if (sw_ph > DG_STEP_W) sw_ph = DG_STEP_W;
        if (sw_ph < 8) sw_ph = 8;
        /* When stopped show step 0 as "ready"; when running show current */
        uint32_t cs_ph = g_song.clock.running
                         ? (seq ? seq->current_step : 0xFFFFu)
                         : 0u;
        if (cs_ph < (uint32_t)steps) {
            int ph_x = DG_LABEL_W + 4 + (int)cs_ph * (sw_ph + 4);
            /* Semi-transparent cyan strip: draw as a thin bright rect */
            gfx_fill_rect(ph_x - 2, body_y, sw_ph + 4, body_h, 0x0310u); /* very dark cyan tint */
            gfx_vline(ph_x - 2,           body_y, body_h, C_CYAN);
            gfx_vline(ph_x + sw_ph + 1,   body_y, body_h, C_CYAN);
        }
    }

    for (int ri = 0; ri < rows_vis; ri++) {
        int abs_ri = ri + s_dg_row_offset;
        drum_row_t *row = (seq && abs_ri < total_rows) ? &seq->rows[abs_ri] : NULL;
        int ry = body_y + ri * row_h;

        gfx_hline(ry + row_h - 1, C_LINE);
        gfx_fill_rect(0, ry, DG_LABEL_W, row_h, C_BG2);
        gfx_vline(DG_LABEL_W, ry, row_h, C_LINE);

        /* Row mute button (right side of label area) */
        bool row_muted = row && row->mute;
        int mute_bx = DG_LABEL_W - 38;
        int mute_by = ry + (row_h - 28) / 2;
        gfx_fill_round_rect(mute_bx, mute_by, 28, 28, 4,
                            row_muted ? C_RED : C_BG3);
        gfx_draw_round_rect(mute_bx, mute_by, 28, 28, 4,
                            row_muted ? C_RED : C_LINE2);
        gfx_draw_text(mute_bx + 6, mute_by + 6,
                      "M", row_muted ? 0xFFFFu : C_T2,
                      row_muted ? C_RED : C_BG3, 2);

        /* Row name (filename without extension) — tap to open sound browser */
        char row_name[32] = "tap to assign";
        if (row && row->wav_path[0]) {
            const char *slash = strrchr(row->wav_path, '/');
            strncpy(row_name, slash ? slash + 1 : row->wav_path, 31);
            row_name[31] = '\0';
            char *dot = strrchr(row_name, '.');
            if (dot) *dot = '\0';
        }
        gfx_draw_text(8, ry + (row_h - 16) / 2,
                      row_name, row_muted ? C_T3 : (row && row->wav_path[0] ? C_T0 : C_T2),
                      C_BG2, 2);

        int sw = (1280 - DG_LABEL_W - 4 - 4 * (steps - 1)) / steps;
        if (sw > DG_STEP_W) sw = DG_STEP_W;
        if (sw < 8) sw = 8;
        int sh = row_h - 16;
        if (sh > DG_STEP_H) sh = DG_STEP_H;
        int sy = ry + (row_h - sh) / 2;

        uint32_t cs = g_song.clock.running
                      ? (seq ? seq->current_step : 0xFFFFu) : 0u;
        for (int si = 0; si < steps; si++) {
            int sx = DG_LABEL_W + 4 + si * (sw + 4);
            bool on   = row && row->steps[si].velocity > 0;
            bool acc  = row && row->steps[si].accent;
            bool play = ((int)cs == si);
            uint16_t bg = acc ? C_AMBER : on ? C_LIME : C_BG3;
            uint16_t br = play ? C_CYAN : C_LINE2;
            gfx_fill_round_rect(sx, sy, sw - 4, sh, 6, bg);
            gfx_draw_round_rect(sx, sy, sw - 4, sh, 6, br);
        }
    }

    int tb_y = SUB_CONTENT_H - DG_TOOLBAR_H + MINIBAR_H;
    gfx_fill_rect(0, tb_y, 1280, DG_TOOLBAR_H, C_BG2);
    gfx_hline(tb_y, C_LINE2);

    gfx_fill_round_rect( 24, tb_y + 8, 140, 56, 10, C_BG3);
    gfx_draw_round_rect( 24, tb_y + 8, 140, 56, 10, C_LINE2);
    gfx_draw_text(48, tb_y + 20, "COPY",  C_T0, C_BG3, 2);

    gfx_fill_round_rect(180, tb_y + 8, 160, 56, 10, C_BG3);
    gfx_draw_round_rect(180, tb_y + 8, 160, 56, 10, C_LINE2);
    gfx_draw_text(200, tb_y + 20, "PASTE", C_T0, C_BG3, 2);

    gfx_fill_round_rect(356, tb_y + 8, 152, 56, 10, C_BG3);
    gfx_draw_round_rect(356, tb_y + 8, 152, 56, 10, C_LINE2);
    gfx_draw_text(376, tb_y + 20, "CLEAR", C_T0, C_BG3, 2);

    /* ADD ROW button */
    gfx_fill_round_rect(524, tb_y + 8, 148, 56, 10, C_BG3);
    gfx_draw_round_rect(524, tb_y + 8, 148, 56, 10, C_LINE2);
    draw_text_centred(524, tb_y + 8, 148, 56, "+ROW", C_T0, C_BG3, 2);

    /* EUCL button */
    gfx_fill_round_rect(688, tb_y + 8, 148, 56, 10, C_BG3);
    gfx_draw_round_rect(688, tb_y + 8, 148, 56, 10, C_LINE2);
    draw_text_centred(688, tb_y + 8, 148, 56, "EUCL", C_CYAN, C_BG3, 2);

    /* row info text + scroll up/down chevrons */
    int end_row = s_dg_row_offset + rows_vis;
    if (end_row > total_rows) end_row = total_rows;
    char row_info[48];
    snprintf(row_info, sizeof(row_info), "%d\xe2\x80\x93%d / %d",
             s_dg_row_offset + 1, end_row, total_rows);
    draw_text_centred(852, tb_y, 168, 72, row_info, C_T2, C_BG2, 2);

    uint16_t up_col = (s_dg_row_offset > 0) ? C_T0 : C_T3;
    gfx_fill_round_rect(1036, tb_y + 8, 104, 56, 10, C_BG3);
    gfx_draw_round_rect(1036, tb_y + 8, 104, 56, 10, C_LINE2);
    draw_text_centred(1036, tb_y + 8, 104, 56, "^", up_col, C_BG3, 3);

    uint16_t dn_col = (end_row < total_rows) ? C_T0 : C_T3;
    gfx_fill_round_rect(1152, tb_y + 8, 104, 56, 10, C_BG3);
    gfx_draw_round_rect(1152, tb_y + 8, 104, 56, 10, C_LINE2);
    draw_text_centred(1152, tb_y + 8, 104, 56, "v", dn_col, C_BG3, 3);

    /* Euclidean popup overlay */
    if (s_eucl_popup) {
        int ep_x = 160, ep_y = 300, ep_w = 960, ep_h = 200;
        gfx_fill_round_rect(ep_x, ep_y, ep_w, ep_h, 12, C_BG2);
        gfx_draw_round_rect(ep_x, ep_y, ep_w, ep_h, 12, C_LINE2);
        draw_text_centred(ep_x, ep_y, ep_w, 44, "EUCLIDEAN GENERATOR", C_T0, C_BG2, 2);
        gfx_hline(ep_y + 44, C_LINE);

        /* Hits */
        gfx_draw_text(ep_x + 40, ep_y + 60, "HITS", C_T2, C_BG2, 2);
        gfx_fill_round_rect(ep_x + 280, ep_y + 80, 52, 52, 6, C_BG3);
        draw_text_centred(ep_x + 280, ep_y + 80, 52, 52, "-", C_T0, C_BG3, 3);
        char hits_buf[8];
        snprintf(hits_buf, sizeof(hits_buf), "%d", s_eucl_hits);
        draw_text_centred(ep_x + 340, ep_y + 80, 52, 52, hits_buf, C_LIME, C_BG2, 3);
        gfx_fill_round_rect(ep_x + 400, ep_y + 80, 52, 52, 6, C_BG3);
        draw_text_centred(ep_x + 400, ep_y + 80, 52, 52, "+", C_LIME, C_BG3, 3);

        /* Steps */
        gfx_draw_text(ep_x + 480, ep_y + 60, "STEPS", C_T2, C_BG2, 2);
        gfx_fill_round_rect(ep_x + 560, ep_y + 80, 52, 52, 6, C_BG3);
        draw_text_centred(ep_x + 560, ep_y + 80, 52, 52, "-", C_T0, C_BG3, 3);
        char steps_buf2[8];
        snprintf(steps_buf2, sizeof(steps_buf2), "%d", s_eucl_steps);
        draw_text_centred(ep_x + 620, ep_y + 80, 52, 52, steps_buf2, C_LIME, C_BG2, 3);
        gfx_fill_round_rect(ep_x + 680, ep_y + 80, 52, 52, 6, C_BG3);
        draw_text_centred(ep_x + 680, ep_y + 80, 52, 52, "+", C_LIME, C_BG3, 3);

        /* GEN button */
        gfx_fill_round_rect(ep_x + ep_w - 200, ep_y + ep_h - 60, 160, 44, 8, C_LIME);
        draw_text_centred(ep_x + ep_w - 200, ep_y + ep_h - 60, 160, 44, "GEN", C_BG, C_LIME, 3);
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 * SUB-SCREEN: PIANO ROLL
 * ══════════════════════════════════════════════════════════════════════════ */

void draw_piano_roll_screen(void)
{
    lane_t       *lane = &g_song.lanes[s_ctx_lane];
    piano_roll_t *pr   = lane->piano_roll;

    /* Minibar: < LANE_NAME  [SYNTH TYPE]  [EDIT]  SNAP 1/16 */
    gfx_fill_rect(0, 0, 1280, MINIBAR_H, C_BG2);
    gfx_hline(MINIBAR_H - 1, C_LINE);
    gfx_fill_rect(0, 0, MINIBAR_BACK_W, MINIBAR_H, C_BG2);
    gfx_vline(MINIBAR_BACK_W, 0, MINIBAR_H, C_LINE);
    gfx_draw_text(30, 24, "<", C_T1, C_BG2, 3);

    char title[48];
    if (lane->name[0])
        snprintf(title, sizeof(title), "%s", lane->name);
    else
        snprintf(title, sizeof(title), "LANE %02d", s_ctx_lane + 1);
    gfx_draw_text(MINIBAR_BACK_W + 24, 22, title, C_T0, C_BG2, 3);

    /* Synth type chip */
    if (lane->type == LANE_TYPE_SYNTH && lane->synth) {
        static const char *SNAMES[20] = {
            "MONO WT","POLY WT","SUPERSAW","FM2","FM4","SUBTRAC",
            "K-STRNG","BELL","PAD","NOISE","BASS","LEAD","CHORD",
            "BD","SD","HH","ORGAN","MORPH","VOWEL","BITCRSH",
        };
        const char *tname = (lane->synth->type_id < 20) ? SNAMES[lane->synth->type_id] : "?";
        int cx = MINIBAR_BACK_W + 24 + gfx_text_width(title, 3) + 20;
        int cw = gfx_text_width(tname, 1) + 16;
        gfx_fill_round_rect(cx, 20, cw, 32, 6, C_BG3);
        gfx_draw_text(cx + 8, 28, tname, C_CYAN, C_BG3, 1);
    }

    /* SNAP indicator */
    gfx_vline(1280 - 200, 0, MINIBAR_H, C_LINE);
    gfx_draw_text(1280 - 184, 10, "SNAP", C_T2, C_BG2, 1);
    gfx_draw_text(1280 - 184, 30, "1/16", C_T0, C_BG2, 2);

    /* SYNTH EDIT button */
    gfx_vline(1280 - 400, 0, MINIBAR_H, C_LINE);
    if (lane->type == LANE_TYPE_SYNTH) {
        gfx_fill_rect(1280 - 400, 0, 200, MINIBAR_H, 0x0240u);
        draw_text_centred(1280 - 400, 0, 200, MINIBAR_H, "EDIT", C_CYAN, 0x0240u, 2);
    }

    int bar_y  = MINIBAR_H;
    int roll_w = 1280 - PR_KEY_W;
    gfx_fill_rect(0, bar_y, 1280, PR_BAR_H, C_BG2);
    gfx_hline(bar_y + PR_BAR_H - 1, C_LINE);
    gfx_fill_rect(0, bar_y, PR_KEY_W, PR_BAR_H, C_BG2);
    gfx_vline(PR_KEY_W, bar_y, PR_BAR_H, C_LINE);

    uint32_t bar_t = CLOCK_BAR_TICKS(&g_song.clock);
    int total_bars = (lane->loop_len_ticks && bar_t) ?
                     (int)(lane->loop_len_ticks / bar_t) : 2;
    if (total_bars < 1) total_bars = 2;
    uint32_t total_ticks = (uint32_t)(total_bars * (int)bar_t);

    /* resolve scroll/zoom — default: show all ticks */
    uint32_t view_wide = s_pr_ticks_wide ? s_pr_ticks_wide : total_ticks;
    if (view_wide < bar_t / 4) view_wide = bar_t / 4;
    if (view_wide > total_ticks) view_wide = total_ticks;
    /* clamp scroll offset */
    if (s_pr_tick_offset + view_wide > total_ticks)
        s_pr_tick_offset = total_ticks > view_wide ? total_ticks - view_wide : 0;

    /* helper: tick → pixel x, returns -1 if outside view */
    #define PR_TICK_TO_X(tick) \
        ((int)PR_KEY_W + (int)(((int64_t)((tick) - (int64_t)s_pr_tick_offset) * roll_w) / (int64_t)view_wide))

    /* bar/beat header — draw labels for visible beats */
    int beats_total = total_bars * 4;
    int beat_ticks  = (int)bar_t / 4;
    for (int beat = 0; beat < beats_total; beat++) {
        uint32_t bt = (uint32_t)(beat * beat_ticks);
        int bx = PR_TICK_TO_X(bt);
        if (bx < PR_KEY_W || bx > 1280) continue;
        bool is_bar = (beat % 4 == 0);
        gfx_fill_rect(bx, bar_y, 2, PR_BAR_H, is_bar ? C_LINE2 : C_LINE);
        if (is_bar) {
            char lbl[16];
            snprintf(lbl, sizeof(lbl), "BAR %d", beat / 4 + 1);
            gfx_draw_text(bx + 8, bar_y + 10, lbl, C_T1, C_BG2, 1);
        }
    }

    int body_y  = bar_y + PR_BAR_H;
    int body_h  = 720 - body_y - PR_TOOLBAR_H;
    int rows_vis = body_h / PR_ROW_H;
    if (rows_vis > 24) rows_vis = 24;

    gfx_fill_rect(0, body_y, 1280, body_h, C_BG1);

    static const char *NOTE_NAMES[12] = {
        "C","C#","D","D#","E","F","F#","G","G#","A","A#","B"
    };
    static const bool IS_BLACK[12] = {
        false,true,false,true,false,false,true,false,true,false,true,false
    };
    for (int ri = 0; ri < rows_vis; ri++) {
        int note = s_pr_view_semitone - ri;
        if (note < 0 || note > 127) continue;
        int ky = body_y + ri * PR_ROW_H;
        bool blk = IS_BLACK[note % 12];
        uint16_t kbg = blk ? C_BG1 : C_BG2;
        gfx_fill_rect(0, ky, PR_KEY_W, PR_ROW_H, kbg);
        gfx_hline(ky + PR_ROW_H - 1, C_LINE);
        char note_label[8];
        snprintf(note_label, sizeof(note_label), "%s%d",
                 NOTE_NAMES[note % 12], note / 12 - 1);
        gfx_draw_text(14, ky + (PR_ROW_H - 8) / 2, note_label,
                      blk ? C_T2 : C_T0, kbg, 1);
    }
    gfx_vline(PR_KEY_W, body_y, body_h, C_LINE2);

    /* 16th-note grid lines (only those visible in view) */
    uint32_t sixteenth = bar_t / 4;
    for (int i = 0; i <= (int)(total_ticks / sixteenth); i++) {
        uint32_t gt = (uint32_t)i * sixteenth;
        int gx = PR_TICK_TO_X(gt);
        if (gx < PR_KEY_W || gx > 1280) continue;
        bool is_bar2 = (i % 16 == 0);
        int lw = is_bar2 ? 2 : 1;
        gfx_fill_rect(gx, body_y, lw, body_h, is_bar2 ? C_LINE2 : C_LINE);
    }

    for (int ri = 0; ri < rows_vis; ri++) {
        int note = s_pr_view_semitone - ri;
        if (note < 0) continue;
        gfx_hline(body_y + ri * PR_ROW_H + PR_ROW_H - 1, C_LINE);
    }

    if (pr) {
        for (int ni = 0; ni < pr->note_count; ni++) {
            const pr_note_t *n = &pr->notes[ni];
            int row_i = s_pr_view_semitone - (int)n->note;
            if (row_i < 0 || row_i >= rows_vis) continue;
            int nx = PR_TICK_TO_X(n->tick_start);
            int nx_end = PR_TICK_TO_X(n->tick_start + n->tick_len);
            if (nx_end < PR_KEY_W || nx > 1280) continue;
            if (nx < PR_KEY_W) nx = PR_KEY_W;
            int nw = nx_end - nx;
            if (nw < 4) nw = 4;
            int ny2 = body_y + row_i * PR_ROW_H + 3;
            int nh  = PR_ROW_H - 6;
            float br = 0.5f + (n->velocity / 127.0f) * 0.5f;
            uint16_t note_col = (br > 0.75f) ? C_LIME :
                                (br > 0.5f)  ? C_DKGREEN : 0x5360u;
            gfx_fill_round_rect(nx, ny2, nw - 2, nh, 4, note_col);
            gfx_fill_rect(nx, ny2, 3, nh, C_LIME);
            /* velocity bar at bottom of note (3px) */
            int vel_w = (int)((nw - 2) * n->velocity / 127);
            if (vel_w > 0)
                gfx_fill_rect(nx, ny2 + nh - 3, vel_w, 3, C_T0);
            /* note name label if wide enough */
            if (nw > 28) {
                char nlbl[8];
                snprintf(nlbl, sizeof(nlbl), "%s%d",
                         NOTE_NAMES[n->note % 12], (int)n->note / 12 - 1);
                gfx_draw_text(nx + 5, ny2 + 4, nlbl, C_BG, note_col, 1);
            }
            /* resize handle: right 24 px — wide enough for finger drag */
            if (nw > 32)
                gfx_fill_rect(nx + nw - 28, ny2, 26, nh, C_T1);
        }
    }

    if (total_ticks > 0) {
        uint32_t ph_tick = lane->lane_tick % total_ticks;
        int ph_x = PR_TICK_TO_X(ph_tick);
        if (ph_x >= PR_KEY_W && ph_x <= 1280)
            gfx_fill_rect(ph_x, body_y, 2, body_h, C_LIME);
    }

    #undef PR_TICK_TO_X

    int tb_y2 = 720 - PR_TOOLBAR_H;
    gfx_fill_rect(0, tb_y2, 1280, PR_TOOLBAR_H, C_BG2);
    gfx_hline(tb_y2, C_LINE2);

    /* OCT < > */
    gfx_fill_round_rect( 24, tb_y2 + 10, 120, 56, 10, C_BG3);
    gfx_draw_round_rect( 24, tb_y2 + 10, 120, 56, 10, C_LINE2);
    gfx_draw_text(36, tb_y2 + 22, "< OCT", C_T0, C_BG3, 2);

    char oct_lbl[16];
    snprintf(oct_lbl, sizeof(oct_lbl), "C%d", s_pr_view_semitone / 12 - 1);
    gfx_draw_text(170, tb_y2 + 28, oct_lbl, C_T1, C_BG2, 2);

    gfx_fill_round_rect(260, tb_y2 + 10, 120, 56, 10, C_BG3);
    gfx_draw_round_rect(260, tb_y2 + 10, 120, 56, 10, C_LINE2);
    gfx_draw_text(272, tb_y2 + 22, "OCT >", C_T0, C_BG3, 2);

    /* ZOOM - + */
    gfx_fill_round_rect(420, tb_y2 + 10, 80, 56, 10, C_BG3);
    gfx_draw_round_rect(420, tb_y2 + 10, 80, 56, 10, C_LINE2);
    gfx_draw_text(448, tb_y2 + 16, "-", C_T0, C_BG3, 3);

    gfx_fill_round_rect(516, tb_y2 + 10, 80, 56, 10, C_BG3);
    gfx_draw_round_rect(516, tb_y2 + 10, 80, 56, 10, C_LINE2);
    gfx_draw_text(538, tb_y2 + 16, "+", C_T0, C_BG3, 3);

    /* SCROLL < > */
    gfx_fill_round_rect(620, tb_y2 + 10, 80, 56, 10, C_BG3);
    gfx_draw_round_rect(620, tb_y2 + 10, 80, 56, 10, C_LINE2);
    gfx_draw_text(644, tb_y2 + 16, "<", C_T0, C_BG3, 3);

    gfx_fill_round_rect(716, tb_y2 + 10, 80, 56, 10, C_BG3);
    gfx_draw_round_rect(716, tb_y2 + 10, 80, 56, 10, C_LINE2);
    gfx_draw_text(738, tb_y2 + 16, ">", C_T0, C_BG3, 3);

    /* DELETE tool toggle button */
    {
        bool del = s_pr_delete_mode;
        uint16_t dbg = del ? C_RED : C_BG3;
        uint16_t dfg = del ? 0xFFFFu : C_T2;
        gfx_fill_round_rect(820, tb_y2 + 10, 100, 56, 10, dbg);
        gfx_draw_round_rect(820, tb_y2 + 10, 100, 56, 10, del ? C_RED : C_LINE2);
        draw_text_centred(820, tb_y2 + 10, 100, 56, "DEL", dfg, dbg, 2);
    }

    char nc_buf[32];
    snprintf(nc_buf, sizeof(nc_buf), "%d NOTES", pr ? pr->note_count : 0);
    gfx_draw_text(1280 - gfx_text_width(nc_buf, 2) - 24, tb_y2 + 28,
                  nc_buf, C_LIME, C_BG2, 2);
}

/* ══════════════════════════════════════════════════════════════════════════
 * SUB-SCREEN: FX + ADSR
 * ══════════════════════════════════════════════════════════════════════════ */

/* Long and short display names per FX type (index = fx_type_t). */
static const char *FX_LONG_NAMES[] = {
    "None","Filter","EQ3","EQ5","Compressor","Limiter","Gate","Transient",
    "Distortion","Overdrive","Wavefolder","Bitcrusher","Delay","Reverb",
    "Chorus","Flanger","Phaser","Tremolo","Vibrato","Ring Mod",
    "Pitch Shift","Auto Pan","Stereo Width",
    "Noise Gate SC","Env Follower","De-Esser","Stereo Imager",
    "Tape Sat","Tube Amp","Exciter","Harmonic Enh","Formant Filt",
    "Comb Filter","Tilt EQ","Pitch Quant","Granular Freeze","Stutter",
    "Tape Stop","Haas","Resonator","Freeze Reverb","Step Filter",
    "Sidechain Comp","Trance Gate","Arp Delay",
};
static const char *FX_SHORT_NAMES[] = {
    "","FILT","EQ3","EQ5","COMP","LIM","GATE","TRAN",
    "DIST","ODRV","FOLD","BCSH","DLY","VERB","CHO","FLG",
    "PHS","TREM","VIB","RING","PTSH","PAN","WDTH",
    "NGSC","ENVF","DSSR","SIMR","TSAT","TUBE","EXCT",
    "HARM","FORM","COMB","TILT","PQNT","GFRZ","STUTR",
    "TSTOP","HAAS","RSON","FVRB","SFLT","SCMP","TGATE","ADLY",
};

static const char *fx_long_name(int tid)
{
    if (tid > 0 && tid < (int)(sizeof FX_LONG_NAMES / sizeof FX_LONG_NAMES[0]))
        return FX_LONG_NAMES[tid];
    return "Unknown";
}
static const char *fx_short_name(int tid)
{
    if (tid > 0 && tid < (int)(sizeof FX_SHORT_NAMES / sizeof FX_SHORT_NAMES[0]))
        return FX_SHORT_NAMES[tid];
    return "?";
}

/* Per-type, per-param descriptor: {label, min, max, decimals}. */
typedef struct { const char *lbl; float mn, mx; int dec; } fx_pd_t;

static const fx_pd_t FX_PD_NONE[] = {{0}};
#define PDLIST(name, ...) static const fx_pd_t name[] = { __VA_ARGS__ }

PDLIST(PD_FILTER,
    {"CUTOFF Hz",   0.0f, 20000.0f, 0},
    {"RESONANCE",   0.0f, 4.0f,     2},
    {"MODE",        0.0f, 3.0f,     0});
PDLIST(PD_EQ3,
    {"LOW dB",     -18.0f, 18.0f, 1},
    {"MID dB",     -18.0f, 18.0f, 1},
    {"HI dB",      -18.0f, 18.0f, 1},
    {"MID Hz",      200.0f, 8000.0f, 0},
    {"MID Q",       0.1f, 8.0f, 1});
PDLIST(PD_EQ5,
    {"BAND",        0.0f, 4.0f, 0},
    {"GAIN dB",    -18.0f, 18.0f, 1},
    {"FREQ Hz",     20.0f, 20000.0f, 0},
    {"Q",           0.1f, 8.0f, 1},
    {"TYPE",        0.0f, 2.0f, 0},
    {"EN",          0.0f, 1.0f, 0});
PDLIST(PD_COMP,
    {"THRESH dB", -60.0f, 0.0f, 1},
    {"RATIO",     1.0f, 20.0f, 1},
    {"ATK ms",    1.0f, 300.0f, 0},
    {"REL ms",    10.0f, 2000.0f, 0},
    {"MAKEUP dB", 0.0f, 24.0f, 1});
PDLIST(PD_LIM,
    {"THRESH dB", -30.0f, 0.0f, 1},
    {"LOOKAHEAD ms", 0.0f, 10.0f, 1});
PDLIST(PD_GATE,
    {"THRESH dB", -80.0f, 0.0f, 1},
    {"HOLD ms",   1.0f, 500.0f, 0},
    {"ATK ms",    1.0f, 100.0f, 0},
    {"REL ms",    10.0f, 1000.0f, 0});
PDLIST(PD_TRANS,
    {"ATK GAIN",  0.0f, 4.0f, 2},
    {"SUS GAIN",  0.0f, 4.0f, 2});
PDLIST(PD_DIST,
    {"DRIVE",     0.0f, 16.0f, 2},
    {"TONE",      0.0f, 1.0f, 2},
    {"MODE",      0.0f, 3.0f, 0});
PDLIST(PD_OD,
    {"DRIVE",     0.0f, 16.0f, 2},
    {"TONE",      0.0f, 1.0f, 2});
PDLIST(PD_WF,
    {"FOLD",      0.0f, 1.0f, 2},
    {"GAIN",      0.0f, 4.0f, 2});
PDLIST(PD_BC,
    {"BITS",      1.0f, 16.0f, 0},
    {"SR DIV",    1.0f, 32.0f, 0});
PDLIST(PD_DELAY,
    {"TIME ms",   1.0f, 2000.0f, 0},
    {"FEEDBACK",  0.0f, 0.99f, 2},
    {"MIX",       0.0f, 1.0f, 2},
    {"PING PONG", 0.0f, 1.0f, 0},
    {"SYNC",      0.0f, 1.0f, 0});
PDLIST(PD_REVERB,
    {"ROOM",      0.0f, 1.0f, 2},
    {"DAMP",      0.0f, 1.0f, 2},
    {"WIDTH",     0.0f, 1.0f, 2},
    {"MIX",       0.0f, 1.0f, 2});
PDLIST(PD_CHO,
    {"RATE Hz",   0.1f, 10.0f, 1},
    {"DEPTH ms",  0.1f, 20.0f, 1},
    {"MIX",       0.0f, 1.0f, 2});
PDLIST(PD_FLG,
    {"RATE Hz",   0.1f, 10.0f, 1},
    {"DEPTH ms",  0.1f, 10.0f, 1},
    {"FEEDBACK",  0.0f, 0.99f, 2},
    {"MIX",       0.0f, 1.0f, 2});
PDLIST(PD_PHS,
    {"RATE Hz",   0.1f, 10.0f, 1},
    {"DEPTH",     0.0f, 1.0f, 2},
    {"FEEDBACK",  0.0f, 0.99f, 2},
    {"MIX",       0.0f, 1.0f, 2});
PDLIST(PD_TREM,
    {"RATE Hz",   0.1f, 20.0f, 1},
    {"DEPTH",     0.0f, 1.0f, 2});
PDLIST(PD_VIB,
    {"RATE Hz",   0.1f, 20.0f, 1},
    {"DEPTH ms",  0.1f, 10.0f, 1});
PDLIST(PD_RING,
    {"CARRIER Hz", 20.0f, 4000.0f, 0},
    {"MIX",       0.0f, 1.0f, 2});
PDLIST(PD_PTSH, {"SEMITONES", -24.0f, 24.0f, 0});
PDLIST(PD_PAN,
    {"RATE Hz",   0.1f, 10.0f, 1},
    {"DEPTH",     0.0f, 1.0f, 2});
PDLIST(PD_WDTH, {"WIDTH",     0.0f, 2.0f, 2});
PDLIST(PD_NGSC,
    {"THRESH dB", -80.0f, 0.0f, 1},
    {"HOLD ms",   1.0f, 500.0f, 0},
    {"ATK ms",    1.0f, 100.0f, 0},
    {"REL ms",    10.0f, 1000.0f, 0},
    {"SRC LANE",  0.0f, 15.0f, 0});
PDLIST(PD_ENVF,
    {"ATK ms",    1.0f, 500.0f, 0},
    {"REL ms",    10.0f, 2000.0f, 0});
PDLIST(PD_DSSR,
    {"FREQ Hz",   2000.0f, 12000.0f, 0},
    {"THRESH dB", -40.0f, 0.0f, 1},
    {"RATIO",     1.0f, 20.0f, 1});
PDLIST(PD_SIMR,
    {"X-OVER Hz", 100.0f, 2000.0f, 0},
    {"WIDTH",     0.0f, 2.0f, 2});
PDLIST(PD_TSAT,
    {"DRIVE",     0.0f, 4.0f, 2},
    {"BIAS",     -1.0f, 1.0f, 2},
    {"MIX",       0.0f, 1.0f, 2});
PDLIST(PD_TUBE,
    {"DRIVE",     0.0f, 4.0f, 2},
    {"BIAS",     -1.0f, 1.0f, 2},
    {"TONE",      0.0f, 1.0f, 2});
PDLIST(PD_EXCT,
    {"FREQ Hz",   2000.0f, 12000.0f, 0},
    {"DRIVE",     0.0f, 4.0f, 2},
    {"MIX",       0.0f, 1.0f, 2});
PDLIST(PD_HARM,
    {"H2 LEVEL",  0.0f, 1.0f, 2},
    {"H3 LEVEL",  0.0f, 1.0f, 2});
PDLIST(PD_FORM,
    {"VOWEL",     0.0f, 4.0f, 0},
    {"MORPH",     0.0f, 1.0f, 2},
    {"MIX",       0.0f, 1.0f, 2});
PDLIST(PD_COMB,
    {"FREQ Hz",   20.0f, 2000.0f, 0},
    {"FEEDBACK",  0.0f, 0.99f, 2},
    {"MIX",       0.0f, 1.0f, 2},
    {"LFO RATE",  0.0f, 10.0f, 1});
PDLIST(PD_TILT, {"TILT dB", -12.0f, 12.0f, 1});
PDLIST(PD_PQNT,
    {"ROOT",      0.0f, 11.0f, 0},
    {"SCALE",     0.0f, 7.0f, 0});
PDLIST(PD_GFRZ,
    {"FREEZE",    0.0f, 1.0f, 0},
    {"RATE",      0.25f, 4.0f, 2},
    {"SCATTER",   0.0f, 1.0f, 2},
    {"MIX",       0.0f, 1.0f, 2});
PDLIST(PD_STUTR,
    {"LENGTH ms", 10.0f, 500.0f, 0},
    {"TRIGGER",   0.0f, 1.0f, 0},
    {"SYNC",      0.0f, 1.0f, 0},
    {"SYNC DIV",  1.0f, 16.0f, 0});
PDLIST(PD_TSTOP,
    {"TRIGGER",   0.0f, 1.0f, 0},
    {"TIME ms",   50.0f, 4000.0f, 0});
PDLIST(PD_HAAS,
    {"DELAY ms",  1.0f, 35.0f, 1},
    {"SIDE",      0.0f, 1.0f, 0});
PDLIST(PD_RSON,
    {"ROOT Hz",   40.0f, 2000.0f, 0},
    {"DECAY",     0.0f, 1.0f, 2},
    {"COUNT",     2.0f, 8.0f, 0},
    {"MIX",       0.0f, 1.0f, 2});
PDLIST(PD_FVRB,
    {"ROOM",      0.0f, 1.0f, 2},
    {"DAMP",      0.0f, 1.0f, 2},
    {"MIX",       0.0f, 1.0f, 2},
    {"FREEZE",    0.0f, 1.0f, 0});
PDLIST(PD_SFLT,
    {"STEP CT",   4.0f, 16.0f, 0},
    {"SYNC",      0.0f, 1.0f, 0},
    {"STEP 1",    0.0f, 1.0f, 2},
    {"STEP 2",    0.0f, 1.0f, 2},
    {"STEP 3",    0.0f, 1.0f, 2},
    {"STEP 4",    0.0f, 1.0f, 2});
PDLIST(PD_SCMP,
    {"THRESH dB", -60.0f, 0.0f, 1},
    {"RATIO",     1.0f, 20.0f, 1},
    {"ATK ms",    1.0f, 300.0f, 0},
    {"REL ms",    10.0f, 2000.0f, 0},
    {"MAKEUP dB", 0.0f, 24.0f, 1},
    {"SRC LANE",  0.0f, 15.0f, 0});
PDLIST(PD_TGATE,
    {"PATTERN",   0.0f, 65535.0f, 0},
    {"STEP DIV",  1.0f, 16.0f, 0},
    {"ATK ms",    1.0f, 100.0f, 0},
    {"REL ms",    1.0f, 200.0f, 0});
PDLIST(PD_ADLY,
    {"TIME ms",   50.0f, 2000.0f, 0},
    {"FEEDBACK",  0.0f, 0.99f, 2},
    {"MIX",       0.0f, 1.0f, 2},
    {"SEMI STEP", -12.0f, 12.0f, 0},
    {"MAX REP",   1.0f, 16.0f, 0});

static const fx_pd_t *fx_pd_table(int tid, int *out_count)
{
#define R(t, arr)  case t: *out_count = (int)(sizeof arr / sizeof arr[0]); return arr
    switch (tid) {
    R(FX_TYPE_FILTER,        PD_FILTER);
    R(FX_TYPE_EQ3,           PD_EQ3);
    R(FX_TYPE_EQ5,           PD_EQ5);
    R(FX_TYPE_COMPRESSOR,    PD_COMP);
    R(FX_TYPE_LIMITER,       PD_LIM);
    R(FX_TYPE_GATE,          PD_GATE);
    R(FX_TYPE_TRANSIENT,     PD_TRANS);
    R(FX_TYPE_DISTORTION,    PD_DIST);
    R(FX_TYPE_OVERDRIVE,     PD_OD);
    R(FX_TYPE_WAVEFOLDER,    PD_WF);
    R(FX_TYPE_BITCRUSH,      PD_BC);
    R(FX_TYPE_DELAY,         PD_DELAY);
    R(FX_TYPE_REVERB,        PD_REVERB);
    R(FX_TYPE_CHORUS,        PD_CHO);
    R(FX_TYPE_FLANGER,       PD_FLG);
    R(FX_TYPE_PHASER,        PD_PHS);
    R(FX_TYPE_TREMOLO,       PD_TREM);
    R(FX_TYPE_VIBRATO,       PD_VIB);
    R(FX_TYPE_RING_MOD,      PD_RING);
    R(FX_TYPE_PITCH_SHIFT,   PD_PTSH);
    R(FX_TYPE_AUTO_PAN,      PD_PAN);
    R(FX_TYPE_STEREO_WIDTH,  PD_WDTH);
    R(FX_TYPE_NOISE_GATE_SC, PD_NGSC);
    R(FX_TYPE_ENV_FOLLOWER,  PD_ENVF);
    R(FX_TYPE_DEESSER,       PD_DSSR);
    R(FX_TYPE_STEREO_IMAGER, PD_SIMR);
    R(FX_TYPE_TAPE_SAT,      PD_TSAT);
    R(FX_TYPE_TUBE_AMP,      PD_TUBE);
    R(FX_TYPE_EXCITER,       PD_EXCT);
    R(FX_TYPE_HARMONIC_ENH,  PD_HARM);
    R(FX_TYPE_FORMANT_FILT,  PD_FORM);
    R(FX_TYPE_COMB_FILTER,   PD_COMB);
    R(FX_TYPE_TILT_EQ,       PD_TILT);
    R(FX_TYPE_PITCH_QUANT,   PD_PQNT);
    R(FX_TYPE_GRAN_FREEZE,   PD_GFRZ);
    R(FX_TYPE_STUTTER,       PD_STUTR);
    R(FX_TYPE_TAPE_STOP,     PD_TSTOP);
    R(FX_TYPE_HAAS,          PD_HAAS);
    R(FX_TYPE_RESONATOR,     PD_RSON);
    R(FX_TYPE_FREEZE_REVERB, PD_FVRB);
    R(FX_TYPE_STEP_FILTER,   PD_SFLT);
    R(FX_TYPE_SIDECHAIN_COMP, PD_SCMP);
    R(FX_TYPE_TRANCE_GATE,   PD_TGATE);
    R(FX_TYPE_ARP_DELAY,     PD_ADLY);
    default: *out_count = 0; return FX_PD_NONE;
    }
#undef R
}

int fx_param_count(int tid)
{
    int n; fx_pd_table(tid, &n); return n;
}
void fx_param_descriptor(int tid, int pi,
                         const char **out_lbl, float *out_min,
                         float *out_max, int *out_dec)
{
    int n; const fx_pd_t *t = fx_pd_table(tid, &n);
    if (pi < 0 || pi >= n) {
        *out_lbl = "-"; *out_min = 0.0f; *out_max = 1.0f; *out_dec = 2;
        return;
    }
    *out_lbl = t[pi].lbl;
    *out_min = t[pi].mn;
    *out_max = t[pi].mx;
    *out_dec = t[pi].dec;
}

void draw_fx_picker_overlay(void);

void draw_fx_adsr_screen(void)
{
    lane_t *lane = &g_song.lanes[s_ctx_lane];

    char title[32];
    snprintf(title, sizeof(title), "FX \xc2\xb7 LANE %02d", s_ctx_lane + 1);
    draw_minibar(title);

    int y = MINIBAR_H + 16;

    gfx_fill_round_rect(16, y, 1248, 140, 10, C_BG2);
    gfx_draw_round_rect(16, y, 1248, 140, 10, C_LINE2);
    gfx_draw_text(36, y + 12, "LANE ADSR", C_LIME, C_BG2, 1);

    const lane_adsr_t *a = &lane->adsr;
    static const char *adsr_labels[] = { "A", "D", "S", "R" };
    float adsr_vals[4] = { a->atk_ms, a->dcy_ms, a->sus * 100.0f, a->rel_ms };
    const char *adsr_units[4] = { "ms", "ms", "%", "ms" };
    for (int i = 0; i < 4; i++) {
        int kx = 420 + i * 200;
        int ky = y + 30;
        gfx_fill_circle(kx + 40, ky + 40, 38, C_BG3);
        gfx_draw_round_rect(kx + 2, ky + 2, 76, 76, 38, C_LINE2);
        gfx_draw_text(kx + 28, ky + 24, adsr_labels[i], C_T0, C_BG3, 3);
        char val_buf[16];
        snprintf(val_buf, sizeof(val_buf), "%.0f%s",
                 (double)adsr_vals[i], adsr_units[i]);
        gfx_draw_text(kx + (80 - gfx_text_width(val_buf, 1)) / 2, ky + 84,
                      val_buf, C_T1, C_BG2, 1);
        gfx_draw_text(kx + (80 - gfx_text_width(adsr_labels[i], 1)) / 2, ky + 96,
                      adsr_labels[i], C_T2, C_BG2, 1);
    }

    y += 156;

    char fx_hdr[32];
    snprintf(fx_hdr, sizeof(fx_hdr), "FX CHAIN \xc2\xb7 %d / %d",
             lane->fx_count, FX_MAX_PER_LANE);
    gfx_draw_text(16, y, fx_hdr, C_T2, C_BG1, 1);
    y += 22;

    int slot_total_w = 1248;
    int arrow_w      = 20;
    int slot_w       = (slot_total_w - (FX_MAX_PER_LANE - 1) * arrow_w) / FX_MAX_PER_LANE;
    for (int i = 0; i < FX_MAX_PER_LANE; i++) {
        int sx = 16 + i * (slot_w + arrow_w);
        bool filled   = (i < lane->fx_count && lane->fx[i] != NULL);
        bool selected = (i == s_fx_sel_slot);
        uint16_t bg  = selected ? C_LIME_DIM : (filled ? C_BG3 : C_BG2);
        uint16_t brd = selected ? C_LIME     : C_LINE2;
        gfx_fill_round_rect(sx, y, slot_w, 80, 8, bg);
        gfx_draw_round_rect(sx, y, slot_w, 80, 8, brd);
        char snum[4];
        snprintf(snum, sizeof(snum), "%d", i + 1);
        gfx_draw_text(sx + 8, y + 6, snum, C_T3, bg, 1);
        if (filled) {
            static const char *FX_SHORT[] = {
                "","FILT","EQ3","EQ5","COMP","LIM","GATE","TRAN",
                "DIST","ODRV","FOLD","BCSH","DLY","VERB","CHO","FLG",
                "PHS","TREM","VIB","RING","PTSH","PAN","WDTH",
                "NGSC","ENVF","DSSR","SIMR","TSAT","TUBE","EXCT",
                "HARM","FORM","COMB","TILT","PQNT","GFRZ","STUTR",
                "TSTOP","HAAS","RSON","FVRB","SFLT","SCMP","TGATE","ADLY",
            };
            int tid = (int)lane->fx[i]->type;
            const char *fname = (tid > 0 && tid < 45) ? FX_SHORT[tid] : "?";
            gfx_draw_text(sx + (slot_w - gfx_text_width(fname, 2)) / 2,
                          y + 26, fname, selected ? C_LIME : C_T0, bg, 2);
            if (!lane->fx[i]->enabled)
                gfx_draw_text(sx + 8, y + 56, "OFF", C_T3, bg, 1);
        } else {
            gfx_draw_text(sx + slot_w / 2 - 8, y + 26, "+", C_T3, bg, 3);
        }
        if (i < FX_MAX_PER_LANE - 1)
            gfx_draw_text(sx + slot_w + 4, y + 30, ">", C_T3, C_BG1, 2);
    }

    y += 96;
    if (s_fx_sel_slot < lane->fx_count && lane->fx[s_fx_sel_slot]) {
        fx_node_t *node = lane->fx[s_fx_sel_slot];
        int panel_h = 720 - y - 16;
        gfx_fill_round_rect(16, y, 1248, panel_h, 10, C_BG2);
        gfx_draw_round_rect(16, y, 1248, panel_h, 10, C_LINE2);
        int tid = (int)node->type;
        const char *fname = fx_long_name(tid);
        char fx_title[64];
        snprintf(fx_title, sizeof(fx_title), "%s \xc2\xb7 SLOT %d",
                 fname, s_fx_sel_slot + 1);
        gfx_draw_text(36, y + 12, fx_title, C_T0, C_BG2, 2);
        bool en = node->enabled;
        gfx_fill_round_rect(1170, y + 12, 76, 36, 6, en ? C_LIME_DIM : C_BG3);
        gfx_draw_round_rect(1170, y + 12, 76, 36, 6, en ? C_LIME : C_LINE2);
        gfx_draw_text(1186, y + 18, en ? "ON" : "OFF",
                      en ? C_LIME : C_T2, en ? C_LIME_DIM : C_BG3, 2);

        /* Param sliders: up to 8 horizontal rows. */
        int row_h    = 44;
        int rows_top = y + 60;
        int n_params = fx_param_count(tid);
        for (int pi = 0; pi < n_params && pi < 8; pi++) {
            int ry = rows_top + pi * row_h;
            if (ry + 34 > y + panel_h - 8) break;
            const char *lbl;
            float pmin, pmax;
            int decimals;
            fx_param_descriptor(tid, pi, &lbl, &pmin, &pmax, &decimals);
            float pv = node->params[pi];
            float t = (pmax > pmin) ? (pv - pmin) / (pmax - pmin) : 0.0f;
            if (t < 0.0f) t = 0.0f;
            if (t > 1.0f) t = 1.0f;
            /* Label */
            gfx_draw_text(36, ry + 6, lbl, C_T2, C_BG2, 1);
            /* Track */
            int track_x = 240, track_w = 880, track_y = ry + 14, track_h = 12;
            gfx_fill_round_rect(track_x, track_y, track_w, track_h, 6, C_BG3);
            int fill_w = (int)(t * (float)track_w);
            if (fill_w > 4) gfx_fill_round_rect(track_x, track_y, fill_w, track_h, 6, C_LIME);
            /* Thumb */
            int thumb_x = track_x + (int)(t * (float)track_w) - 8;
            gfx_fill_round_rect(thumb_x, track_y - 8, 16, track_h + 16, 4, C_T0);
            /* Value */
            char val_buf[24];
            if (decimals <= 0) snprintf(val_buf, sizeof(val_buf), "%.0f", (double)pv);
            else if (decimals == 1) snprintf(val_buf, sizeof(val_buf), "%.1f", (double)pv);
            else                 snprintf(val_buf, sizeof(val_buf), "%.2f", (double)pv);
            gfx_draw_text(1140, ry + 6, val_buf, C_T0, C_BG2, 1);
        }
        if (n_params == 0) {
            gfx_draw_text(36, y + 64, "No parameters", C_T3, C_BG2, 1);
        }
    }
    /* Picker overlay drawn last so it sits on top */
    if (s_fx_picker_open) draw_fx_picker_overlay();
}

/* FX type picker overlay (drawn on top of FX_ADSR screen) */
void draw_fx_picker_overlay(void)
{
    /* Dim backdrop */
    gfx_fill_rect(0, 0, 1280, 720, C_BG);
    /* Modal card */
    int mx = 80, my = 40, mw = 1120, mh = 640;
    gfx_fill_round_rect(mx, my, mw, mh, 12, C_BG2);
    gfx_draw_round_rect(mx, my, mw, mh, 12, C_LINE2);
    gfx_draw_text(mx + 24, my + 16, "CHOOSE EFFECT", C_T0, C_BG2, 2);
    /* Close X */
    gfx_fill_round_rect(mx + mw - 60, my + 14, 44, 36, 6, C_BG3);
    gfx_draw_round_rect(mx + mw - 60, my + 14, 44, 36, 6, C_LINE2);
    gfx_draw_text(mx + mw - 48, my + 20, "X", C_T1, C_BG3, 2);

    int cols = 6, item_w = (mw - 48) / cols, item_h = 72;
    int grid_top = my + 64;
    int grid_h   = mh - 80;
    int rows_vis = grid_h / item_h;
    int total = (int)FX_TYPE_COUNT - 1; /* skip FX_TYPE_NONE */
    int total_rows = (total + cols - 1) / cols;
    if (s_fx_picker_scroll < 0) s_fx_picker_scroll = 0;
    if (s_fx_picker_scroll > total_rows - rows_vis && total_rows > rows_vis)
        s_fx_picker_scroll = total_rows - rows_vis;

    for (int vr = 0; vr < rows_vis; vr++) {
        int row = vr + s_fx_picker_scroll;
        for (int c = 0; c < cols; c++) {
            int idx = row * cols + c;
            if (idx >= total) break;
            int tid = idx + 1;             /* skip NONE */
            int ix = mx + 24 + c * item_w;
            int iy = grid_top + vr * item_h;
            gfx_fill_round_rect(ix + 4, iy + 4, item_w - 8, item_h - 8, 8, C_BG3);
            gfx_draw_round_rect(ix + 4, iy + 4, item_w - 8, item_h - 8, 8, C_LINE2);
            const char *nm = fx_long_name(tid);
            int tw = gfx_text_width(nm, 1);
            if (tw > item_w - 20) {
                /* Fallback to short name if long doesn't fit. */
                nm = fx_short_name(tid);
                tw = gfx_text_width(nm, 2);
                gfx_draw_text(ix + (item_w - tw) / 2, iy + 24, nm, C_T0, C_BG3, 2);
            } else {
                gfx_draw_text(ix + (item_w - tw) / 2, iy + 28, nm, C_T0, C_BG3, 1);
            }
        }
    }
    /* Scroll hint */
    if (total_rows > rows_vis) {
        gfx_draw_text(mx + mw - 220, my + mh - 32,
                      "DRAG TO SCROLL", C_T3, C_BG2, 1);
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 * SUB-SCREEN: SOUND BROWSER
 * ══════════════════════════════════════════════════════════════════════════ */

void draw_sound_browser_screen(void)
{
    /* ── Minibar with breadcrumb ───────────────────────────────────────────── */
    gfx_fill_rect(0, 0, 1280, MINIBAR_H, C_BG2);
    gfx_hline(MINIBAR_H - 1, C_LINE);
    /* Back chevron */
    gfx_fill_rect(0, 0, MINIBAR_BACK_W, MINIBAR_H, C_BG2);
    gfx_vline(MINIBAR_BACK_W, 0, MINIBAR_H, C_LINE);
    gfx_draw_text(30, 24, "<", C_T1, C_BG2, 3);
    /* Breadcrumb: "Sounds" or "Sounds / KIT_NAME" */
    int bx = MINIBAR_BACK_W + 24;
    gfx_draw_text(bx, 22, "Sounds", C_T2, C_BG2, 3);
    if (s_sb_kit_sel >= 0 && s_sb_kit_sel < s_sb_kit_count) {
        int sw = gfx_text_width("Sounds", 3);
        gfx_draw_text(bx + sw + 8,  22, "/", C_T3, C_BG2, 3);
        gfx_draw_text(bx + sw + 32, 22, s_sb_kits[s_sb_kit_sel], C_T0, C_BG2, 3);
    }

    const int y0    = MINIBAR_H;
    const int row_h = 72;
    const int kit_w = 400;
    const int ab_h  = 80;
    const int list_h = 720 - y0 - ab_h;
    const int rows_vis = list_h / row_h;

    /* ── Backgrounds ───────────────────────────────────────────────────────── */
    gfx_fill_rect(0,         y0, kit_w,             list_h, C_BG1);
    gfx_fill_rect(kit_w,     y0, 2,                 720 - y0, C_LINE);
    gfx_fill_rect(kit_w + 2, y0, 1280 - kit_w - 2, list_h, C_BG1);

    /* ── Column header row ────────────────────────────────────────────────── */
    gfx_fill_rect(0,         y0, kit_w,             row_h, C_BG2);
    gfx_fill_rect(kit_w + 2, y0, 1280 - kit_w - 2, row_h, C_BG2);
    gfx_hline(y0 + row_h - 1, C_LINE);
    draw_text_centred(0,         y0, kit_w,             row_h, "KITS",  C_T2, C_BG2, 2);
    /* File column header shows kit name when one is selected */
    if (s_sb_kit_sel >= 0 && s_sb_kit_sel < s_sb_kit_count) {
        char hdr[SB_NAME_LEN + 8];
        snprintf(hdr, sizeof(hdr), "%.32s", s_sb_kits[s_sb_kit_sel]);
        draw_text_centred(kit_w + 2, y0, 1280 - kit_w - 2, row_h, hdr, C_LIME, C_BG2, 2);
    } else {
        draw_text_centred(kit_w + 2, y0, 1280 - kit_w - 2, row_h, "FILES", C_T2, C_BG2, 2);
    }

    /* ── Kit list ────────────────────────────────────────────────────────── */
    int kit_list_y   = y0 + row_h;
    int kit_list_h   = list_h - row_h;
    int kit_inner_w  = kit_w - SB_SCROLLBAR_W;
    if (s_sb_kit_count == 0) {
        gfx_draw_text(16, kit_list_y + 24, "No kits on SD card", C_T3, C_BG1, 2);
    }
    for (int vi = 0; vi < rows_vis; vi++) {
        int ki = vi + s_sb_kit_scroll;
        if (ki >= s_sb_kit_count) break;
        int ry  = kit_list_y + vi * row_h;
        bool sel = (ki == s_sb_kit_sel);
        uint16_t bg = sel ? C_LIME_DIM : C_BG1;
        gfx_fill_rect(0, ry, kit_inner_w, row_h, bg);
        gfx_hline(ry + row_h - 1, C_LINE);
        if (sel) gfx_fill_rect(0, ry, 4, row_h, C_LIME);
        char name[24];
        strncpy(name, s_sb_kits[ki], 23); name[23] = '\0';
        gfx_draw_text(16 + (sel ? 4 : 0), ry + 24,
                      name, sel ? C_LIME : C_T0, bg, 2);
    }
    /* Kit scrollbar */
    {
        int sbx = kit_inner_w;
        gfx_fill_rect(sbx, kit_list_y, SB_SCROLLBAR_W, kit_list_h, C_BG2);
        gfx_vline(sbx, kit_list_y, kit_list_h, C_LINE);
        if (s_sb_kit_count > rows_vis) {
            int track_h = kit_list_h - 8;
            int thumb_h = track_h * rows_vis / s_sb_kit_count;
            if (thumb_h < 24) thumb_h = 24;
            int thumb_y = kit_list_y + 4 +
                          (s_sb_kit_count > rows_vis ?
                           (track_h - thumb_h) * s_sb_kit_scroll / (s_sb_kit_count - rows_vis) : 0);
            gfx_fill_round_rect(sbx + 3, thumb_y, SB_SCROLLBAR_W - 6, thumb_h, 4, C_T2);
        }
    }

    /* ── File list ───────────────────────────────────────────────────────── */
    int file_x = kit_w + 2;
    int file_w = 1280 - kit_w - 2;
    int file_list_y = kit_list_y;
    int file_list_h = kit_list_h;
    int file_inner_w = file_w - SB_SCROLLBAR_W;
    if (s_sb_file_count == 0) {
        gfx_draw_text(file_x + 16, file_list_y + 24,
                      s_sb_kit_count ? "No WAV files" : "Select a kit", C_T3, C_BG1, 2);
    }
    for (int vi = 0; vi < rows_vis; vi++) {
        int fi = vi + s_sb_file_scroll;
        if (fi >= s_sb_file_count) break;
        int ry  = file_list_y + vi * row_h;
        bool sel = (fi == s_sb_file_sel);
        uint16_t bg = sel ? C_LIME_DIM : C_BG1;
        gfx_fill_rect(file_x, ry, file_inner_w, row_h, bg);
        gfx_hline(ry + row_h - 1, C_LINE);
        if (sel) gfx_fill_rect(file_x, ry, 4, row_h, C_LIME);
        char name[30];
        strncpy(name, s_sb_files[fi], 29); name[29] = '\0';
        char *dot = strrchr(name, '.');
        if (dot) *dot = '\0';
        gfx_draw_text(file_x + 16 + (sel ? 4 : 0), ry + 24,
                      name, sel ? C_LIME : C_T0, bg, 2);
    }
    /* File scrollbar */
    {
        int sbx = file_x + file_inner_w;
        gfx_fill_rect(sbx, file_list_y, SB_SCROLLBAR_W, file_list_h, C_BG2);
        gfx_vline(sbx, file_list_y, file_list_h, C_LINE);
        if (s_sb_file_count > rows_vis) {
            int track_h = file_list_h - 8;
            int thumb_h = track_h * rows_vis / s_sb_file_count;
            if (thumb_h < 24) thumb_h = 24;
            int thumb_y = file_list_y + 4 +
                          (s_sb_file_count > rows_vis ?
                           (track_h - thumb_h) * s_sb_file_scroll / (s_sb_file_count - rows_vis) : 0);
            gfx_fill_round_rect(sbx + 3, thumb_y, SB_SCROLLBAR_W - 6, thumb_h, 4, C_T2);
        }
    }

    /* ── Action bar: [PLAY] [ASSIGN] ─────────────────────────────────────── */
    int ab_y = 720 - ab_h;
    gfx_fill_rect(0, ab_y, 1280, ab_h, C_BG2);
    gfx_hline(ab_y, C_LINE);

    bool can_act = (s_sb_kit_sel >= 0 && s_sb_kit_sel < s_sb_kit_count &&
                    s_sb_file_sel >= 0 && s_sb_file_sel < s_sb_file_count);

    /* PLAY button */
    uint16_t play_bg = can_act ? C_CYAN : C_BG3;
    uint16_t play_fg = can_act ? C_BG   : C_T3;
    gfx_fill_round_rect(24, ab_y + 8, 240, 64, 10, play_bg);
    if (!can_act) gfx_draw_round_rect(24, ab_y + 8, 240, 64, 10, C_LINE2);
    draw_text_centred(24, ab_y + 8, 240, 64, "\xe2\x96\xb6 PLAY", play_fg, play_bg, 2);

    /* ASSIGN button */
    int asgn_x = 24 + 240 + 16;
    int asgn_w = 1280 - asgn_x - 24;
    uint16_t asgn_bg = can_act ? C_LIME : C_BG3;
    uint16_t asgn_fg = can_act ? C_BG   : C_T3;
    gfx_fill_round_rect(asgn_x, ab_y + 8, asgn_w, 64, 10, asgn_bg);
    if (!can_act) gfx_draw_round_rect(asgn_x, ab_y + 8, asgn_w, 64, 10, C_LINE2);
    draw_text_centred(asgn_x, ab_y + 8, asgn_w, 64, "ASSIGN", asgn_fg, asgn_bg, 3);
}

/* ══════════════════════════════════════════════════════════════════════════
 * SUB-SCREEN: SETTINGS
 * ══════════════════════════════════════════════════════════════════════════ */

void draw_settings_screen(void)
{
    draw_minibar("Settings");

    int y0 = MINIBAR_H;

    /* Row definitions: type 0=fader, 1=seg, 2=plain, 3=toggle, 4=pb_mode */
    struct {
        const char *label;
        int         type;
        float       frac;
        const char *val_str;
    } rows[] = {
        { "Master Volume",   0, g_settings.master_volume, NULL               },
        { "Default BPM",     2, 0,                        NULL               },
        { "PPQN",            1, 0,                        NULL               },
        { "Playback Mode",   4, 0,                        NULL               },
        { "WiFi AP SSID",    2, 0,                        g_settings.wifi_ssid },
        { "Hostname",        2, 0,                        "synth-32.local"   },
        { "Auto-save",       3, 0,                        NULL               },
        { "Autosave Bars",   2, 0,                        NULL               },
        { "Metro Volume",    0, g_song.metronome_volume,  NULL               },
    };
    int nrows = 9;

    for (int i = 0; i < nrows; i++) {
        int ry = y0 + i * 76;
        gfx_fill_rect(0, ry, 1280, 76, C_BG1);
        gfx_hline(ry + 75, C_LINE);
        gfx_draw_text(28, ry + 24, rows[i].label, C_T0, C_BG1, 2);

        int ctrl_x = 680;
        if (rows[i].type == 0) {
            draw_fader(ctrl_x, ry + 32, 400, 12, rows[i].frac, C_LIME);
            char pct[12];
            snprintf(pct, sizeof(pct), "%d%%", (int)(rows[i].frac * 100));
            gfx_draw_text(ctrl_x + 416, ry + 24, pct, C_T0, C_BG1, 2);
        } else if (rows[i].type == 1) {
            static const char *ppqn_o[] = { "24","48","96","192" };
            int ppqn_act = (g_settings.ppqn == 24) ? 0 :
                           (g_settings.ppqn == 48) ? 1 :
                           (g_settings.ppqn == 96) ? 2 : 3;
            draw_seg(ctrl_x, ry + 12, 440, 52, ppqn_o, 4, ppqn_act, C_BG, C_LIME);
        } else if (rows[i].type == 2) {
            char plain[64];
            if (rows[i].val_str)
                strncpy(plain, rows[i].val_str, 63);
            else
                snprintf(plain, sizeof(plain), "%.1f BPM", (double)g_settings.bpm);
            gfx_draw_text(ctrl_x, ry + 24, plain, C_T0, C_BG1, 2);
        } else if (rows[i].type == 3) {
            draw_toggle(ctrl_x, ry + 20, true);
        } else if (rows[i].type == 4) {
            static const char *pb_o[] = { "LIVE", "SONG" };
            draw_seg(ctrl_x, ry + 12, 220, 52, pb_o, 2,
                     g_song.playback_mode ? 1 : 0, C_BG, C_LIME);
        }
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 * SUB-SCREEN: SONG BROWSER
 * ══════════════════════════════════════════════════════════════════════════ */

char s_song_browser_files[SONG_BROWSER_MAX][64];
int  s_song_browser_count = 0;
bool s_song_browser_dirty = true;

void song_browser_refresh(void)
{
    s_song_browser_count = 0;
    /* Create songs dir if missing — opendir would otherwise silently return
     * an empty list on a fresh card, masking the real problem. */
    mkdir(SONG_DIR, 0755);
    DIR *d = opendir(SONG_DIR);
    if (!d) {
        ESP_LOGW("song_browser", "opendir(%s) failed", SONG_DIR);
        return;
    }
    int seen = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && s_song_browser_count < SONG_BROWSER_MAX) {
        seen++;
        const char *ext = strrchr(ent->d_name, '.');
        /* case-insensitive match — FATFS may return uppercase extensions */
        if (!ext || strcasecmp(ext, SONG_EXT) != 0) continue;
        strncpy(s_song_browser_files[s_song_browser_count], ent->d_name, 63);
        s_song_browser_files[s_song_browser_count][63] = '\0';
        s_song_browser_count++;
    }
    closedir(d);
    ESP_LOGI("song_browser", "%s: %d entries, %d match %s",
             SONG_DIR, seen, s_song_browser_count, SONG_EXT);
    s_song_browser_dirty = false;
}

void draw_song_browser_screen(void)
{
    if (s_song_browser_dirty) song_browser_refresh();

    draw_minibar("Songs");

    int y0 = MINIBAR_H;
    int list_h = 720 - y0 - 72;
    gfx_fill_rect(0, y0, 1280, list_h, C_BG1);
    gfx_draw_text(24, y0 + 14, "SD \xc2\xb7 /sdcard/songs/", C_T2, C_BG1, 1);
    gfx_hline(y0 + 38, C_LINE);

    int visible = list_h / 72 - 1;
    if (visible > s_song_browser_count) visible = s_song_browser_count;
    if (visible < 0) visible = 0;

    if (visible == 0) {
        gfx_draw_text(24, y0 + 60, "No songs found", C_T3, C_BG1, 2);
    }
    for (int i = 0; i < visible; i++) {
        int ry = y0 + 40 + i * 72;
        bool sel = (i == s_song_browser_sel);
        uint16_t bg = sel ? C_LIME_DIM : C_BG1;
        gfx_fill_rect(0, ry, 1280, 72, bg);
        gfx_hline(ry + 71, C_LINE);
        if (sel) gfx_fill_rect(0, ry, 4, 72, C_LIME);
        gfx_draw_text(20 + (sel ? 4 : 0), ry + 24,
                      s_song_browser_files[i], sel ? C_LIME : C_T0, bg, 2);
    }

    int ab_y = 720 - 72;
    gfx_fill_rect(0, ab_y, 1280, 72, C_BG2);
    gfx_hline(ab_y, C_LINE);
    gfx_fill_round_rect( 24, ab_y + 8, 480, 56, 10, C_LIME);
    draw_text_centred(24, ab_y + 8, 480, 56, "LOAD", C_BG, C_LIME, 3);
    gfx_fill_round_rect(524, ab_y + 8, 240, 56, 10, C_BG3);
    gfx_draw_round_rect(524, ab_y + 8, 240, 56, 10, C_LINE2);
    draw_text_centred(524, ab_y + 8, 240, 56, "DELETE", C_T0, C_BG3, 2);
    gfx_fill_round_rect(784, ab_y + 8, 240, 56, 10, C_BG3);
    gfx_draw_round_rect(784, ab_y + 8, 240, 56, 10, C_LINE2);
    draw_text_centred(784, ab_y + 8, 240, 56, "REFRESH", C_T2, C_BG3, 2);
}

/* ══════════════════════════════════════════════════════════════════════════
 * SCREEN: WAV DETAIL
 * ══════════════════════════════════════════════════════════════════════════ */
void draw_wav_detail_screen(void)
{
    lane_t *lane = &g_song.lanes[s_ctx_lane];

    char title[64];
    const char *slash = strrchr(lane->wav_path, '/');
    const char *fname = slash ? slash + 1 : (lane->wav_path[0] ? lane->wav_path : "WAV");
    snprintf(title, sizeof(title), "WAV — %.48s", fname);
    draw_minibar(title);

    int y = MINIBAR_H + 24;
    gfx_fill_rect(0, MINIBAR_H, 1280, 720 - MINIBAR_H, C_BG1);

    /* Action buttons row */
    gfx_fill_round_rect( 24, y, 400, 56, 10, C_BG3);
    gfx_draw_round_rect( 24, y, 400, 56, 10, C_LINE2);
    draw_text_centred(24, y, 400, 56, "CHANGE FILE", C_T0, C_BG3, 2);

    gfx_fill_round_rect(440, y, 280, 56, 10, C_BG3);
    gfx_draw_round_rect(440, y, 280, 56, 10, C_LINE2);
    bool has_fx = lane->fx_count > 0;
    draw_text_centred(440, y, 280, 56, has_fx ? "FX  ON" : "FX", has_fx ? C_LIME : C_T1, C_BG3, 2);

    y += 72;

    /* File path */
    gfx_draw_text(24, y + 8, "FILE", C_T2, C_BG1, 2);
    const char *disp = lane->wav_path[0] ? lane->wav_path : "\xe2\x80\x94 no file \xe2\x80\x94";
    gfx_draw_text(240, y + 8, disp, C_T1, C_BG1, 2);
    y += 40;

    /* Loop length */
    gfx_draw_text(24, y + 8, "LOOP", C_T2, C_BG1, 2);
    char loop_buf[24] = "\xe2\x80\x94";
    uint32_t bar_t = CLOCK_BAR_TICKS(&g_song.clock);
    if (lane->loop_len_ticks > 0 && bar_t > 0) {
        int bars = (int)(lane->loop_len_ticks / bar_t);
        snprintf(loop_buf, sizeof(loop_buf), bars == 1 ? "1 bar" : "%d bars", bars);
    }
    gfx_draw_text(240, y + 8, loop_buf, C_LIME, C_BG1, 2);
    y += 48;

    /* Volume fader */
    gfx_draw_text(24, y + 8, "VOL", C_T2, C_BG1, 2);
    draw_fader(240, y + 8, 800, 16, lane->volume, C_LIME);
    char vol_buf[12];
    snprintf(vol_buf, sizeof(vol_buf), "%d%%", (int)(lane->volume * 100));
    gfx_draw_text(1060, y + 8, vol_buf, C_T0, C_BG1, 2);
    y += 56;

    /* Pan fader */
    gfx_draw_text(24, y + 8, "PAN", C_T2, C_BG1, 2);
    float pan_frac = (lane->pan + 1.0f) * 0.5f;
    draw_fader(240, y + 8, 800, 16, pan_frac, C_CYAN);
    gfx_fill_rect(240 + 400, y + 6, 2, 20, C_LINE2);
    char pan_buf[12];
    int pan_pct = (int)(lane->pan * 100);
    snprintf(pan_buf, sizeof(pan_buf), pan_pct == 0 ? "C" : (pan_pct > 0 ? "R%d" : "L%d"), abs(pan_pct));
    gfx_draw_text(1060, y + 8, pan_buf, C_T0, C_BG1, 2);
}

/* ══════════════════════════════════════════════════════════════════════════
 * SCREEN: SYNTH EDIT — type picker + parameter tabs
 * ══════════════════════════════════════════════════════════════════════════ */

/* Parameter IDs matching synth_osc.c anonymous enum */
#define SE_P_WAVEFORM    0
#define SE_P_TUNE        1
#define SE_P_ATK         2
#define SE_P_DCY         3
#define SE_P_SUS         4
#define SE_P_REL         5
#define SE_P_DETUNE      6
#define SE_P_FM_RATIO    7
#define SE_P_FM_DEPTH    8
#define SE_P_FILTER_FREQ 9
#define SE_P_FILTER_RES  10
#define SE_P_DRIVE       11
#define SE_P_ALGO        12
/* Extra synths (synth_extra.c) use these offsets */
#define SE_P_EXT_ATK     20
#define SE_P_EXT_DCY     21
#define SE_P_EXT_SUS     22
#define SE_P_EXT_REL     23

static const char *SE_TYPE_NAMES[20] = {
    "MONO WT", "POLY WT", "SUPERSAW", "FM 2OP", "FM 4OP",
    "SUBTRAC", "K-STRNG", "BELL",    "PAD",    "NOISE",
    "BASS",    "LEAD",    "CHORD",   "BD",      "SNARE",
    "HI-HAT",  "ORGAN",   "MORPH",   "VOWEL",   "BITCRSH",
};

static void draw_se_slider(int x, int y, int w, const char *lbl, float val, float lo, float hi, uint16_t col)
{
    gfx_draw_text(x, y, lbl, C_T2, C_BG1, 2);
    int by = y + 22;
    int sh = 32;    /* tall enough for easy finger drag */
    gfx_fill_round_rect(x, by, w, sh, 6, C_BG3);
    gfx_draw_round_rect(x, by, w, sh, 6, C_LINE2);
    float frac = (hi > lo) ? (val - lo) / (hi - lo) : 0.0f;
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;
    int fw = (int)(frac * w);
    if (fw > 8) gfx_fill_round_rect(x, by, fw, sh, 6, col);
    char vbuf[16];
    if (hi - lo >= 10.0f)
        snprintf(vbuf, sizeof(vbuf), "%.0f", (double)val);
    else
        snprintf(vbuf, sizeof(vbuf), "%.2f", (double)val);
    gfx_draw_text(x + w + 12, by + 6, vbuf, C_T0, C_BG1, 2);
}

static void draw_se_wave_seg(int x, int y, int sel)
{
    static const char *WN[6] = {"SIN","TRI","SAW","RSAW","SQR","NOIS"};
    int sw = 160, sh = 40, gap = 4;
    for (int i = 0; i < 6; i++) {
        bool on = (i == sel);
        uint16_t bg = on ? C_CYAN : C_BG3;
        uint16_t fg = on ? C_BG   : C_T1;
        gfx_fill_round_rect(x + i*(sw+gap), y, sw, sh, 6, bg);
        if (!on) gfx_draw_round_rect(x + i*(sw+gap), y, sw, sh, 6, C_LINE2);
        draw_text_centred(x + i*(sw+gap), y, sw, sh, WN[i], fg, bg, 2);
    }
}

void draw_synth_edit_screen(void)
{
    int li = s_se_lane;
    lane_t *lane = (li >= 0 && li < NUM_LANES) ? &g_song.lanes[li] : NULL;
    synth_inst_t *synth = lane ? lane->synth : NULL;

    /* Mini-bar */
    char title[48];
    if (lane && lane->name[0])
        snprintf(title, sizeof(title), "SYNTH  %s", lane->name);
    else
        snprintf(title, sizeof(title), "SYNTH  Lane %d", li + 1);
    draw_minibar(title);

    int y0 = MINIBAR_H + 8;

    /* ── Type picker: 4 rows × 5 columns ────────────────────────────────── */
    int tc = 5, tr = 4;
    int tw = (1280 - 40) / tc;
    int th = 52;
    int tgap = 6;
    int cur_type = synth ? (int)synth->type_id : -1;

    gfx_draw_text(24, y0 + 2, "SYNTH TYPE", C_T2, C_BG1, 1);
    int ty0 = y0 + 18;

    for (int r = 0; r < tr; r++) {
        for (int c = 0; c < tc; c++) {
            int idx = r * tc + c;
            if (idx >= 20) break;
            bool sel = (idx == cur_type);
            int bx = 20 + c * tw;
            int by = ty0 + r * (th + tgap);
            uint16_t bg = sel ? C_CYAN : C_BG3;
            uint16_t fg = sel ? C_BG   : C_T1;
            gfx_fill_round_rect(bx, by, tw - 4, th, 6, bg);
            if (!sel) gfx_draw_round_rect(bx, by, tw - 4, th, 6, C_LINE2);
            draw_text_centred(bx, by, tw - 4, th, SE_TYPE_NAMES[idx], fg, bg, 1);
        }
    }

    int after_types = ty0 + tr * (th + tgap) + 8;

    /* ── Tab bar ─────────────────────────────────────────────────────────── */
    static const char *TAB_NAMES[4] = { "ENV", "OSC", "FILTER", "MOD" };
    int tab_h = 44, tab_w = 1280 / 4;
    gfx_fill_rect(0, after_types, 1280, tab_h, C_BG2);
    gfx_hline(after_types, C_LINE);
    gfx_hline(after_types + tab_h, C_LINE);
    for (int i = 0; i < 4; i++) {
        int tx = i * tab_w;
        bool on = (i == s_se_tab);
        uint16_t bg = on ? C_CYAN : C_BG2;
        uint16_t fg = on ? C_BG   : C_T2;
        gfx_fill_rect(tx, after_types, tab_w, tab_h, bg);
        if (i > 0) gfx_vline(tx, after_types, tab_h, C_LINE);
        draw_text_centred(tx, after_types, tab_w, tab_h, TAB_NAMES[i], fg, bg, 2);
    }

    int py = after_types + tab_h + 16;
    int pw = 1200;
    int px = 40;

    /* Read params from lane->synth_params (source of truth) */
    float *sp = lane ? lane->synth_params : NULL;
    bool is_extra = (cur_type >= 6);
    /* ENV params: extra synths use indices 20-23, standard use 2-5 */
    float atk = sp ? sp[is_extra ? 20 : 2]  : 10.0f;
    float dcy = sp ? sp[is_extra ? 21 : 3]  : 100.0f;
    float sus = sp ? sp[is_extra ? 22 : 4]  : 0.8f;
    float rel = sp ? sp[is_extra ? 23 : 5]  : 200.0f;

    /* Which tabs make sense for this synth type */
    bool has_filter = (cur_type == 5 || cur_type == 9 || cur_type == 10 ||
                       cur_type == 11 || cur_type == 19);  /* SUBTRAC,NOISE,BASS,LEAD,BITCRSH */
    bool has_fm     = (cur_type == 3 || cur_type == 4);    /* FM2, FM4 */
    bool has_osc    = (cur_type <= 5);                     /* types 0-5 have waveform/detune */

    switch (s_se_tab) {
    case 0: /* ENV */
        draw_se_slider(px, py,       pw, "ATTACK (ms)",  atk, 0.0f, 2000.0f, C_LIME);
        draw_se_slider(px, py + 64,  pw, "DECAY (ms)",   dcy, 0.0f, 2000.0f, C_AMBER);
        draw_se_slider(px, py + 128, pw, "SUSTAIN (0-1)", sus, 0.0f, 1.0f,   C_CYAN);
        draw_se_slider(px, py + 192, pw, "RELEASE (ms)", rel, 0.0f, 3000.0f, C_RED);
        break;
    case 1: /* OSC */
        if (has_osc) {
            gfx_draw_text(px, py, "WAVEFORM", C_T2, C_BG1, 2);
            draw_se_wave_seg(px, py + 22, sp ? (int)sp[SE_P_WAVEFORM] : 0);
            draw_se_slider(px, py + 78,  pw, "TUNE  (semitones)",
                           sp ? sp[SE_P_TUNE]   : 0.0f, -24.0f, 24.0f,  C_CYAN);
            draw_se_slider(px, py + 142, pw, "DETUNE / SPREAD (cents)",
                           sp ? sp[SE_P_DETUNE] : 0.0f,   0.0f, 100.0f, C_AMBER);
        } else {
            draw_se_slider(px, py,      pw, "TUNE (semitones)",
                           sp ? sp[SE_P_TUNE] : 0.0f, -24.0f, 24.0f, C_CYAN);
        }
        break;
    case 2: /* FILTER */
        if (has_filter) {
            draw_se_slider(px, py,       pw, "CUTOFF FREQ (Hz)",
                           sp ? sp[SE_P_FILTER_FREQ] : 8000.0f, 40.0f, 20000.0f, C_CYAN);
            draw_se_slider(px, py + 64,  pw, "RESONANCE (0-1)",
                           sp ? sp[SE_P_FILTER_RES] : 0.3f,      0.0f,     1.0f, C_AMBER);
            draw_se_slider(px, py + 128, pw, "DRIVE (0-1)",
                           sp ? sp[SE_P_DRIVE] : 0.0f,            0.0f,    1.0f, C_RED);
        } else {
            gfx_draw_text(px, py + 40, "No filter on this synth type.", C_T3, C_BG1, 2);
        }
        break;
    case 3: /* MOD / FM */
        if (has_fm) {
            draw_se_slider(px, py,      pw, "FM RATIO  (modulator : carrier)",
                           sp ? sp[SE_P_FM_RATIO] : 2.0f, 0.5f, 8.0f, C_CYAN);
            draw_se_slider(px, py + 64, pw, "FM DEPTH  (modulation index)",
                           sp ? sp[SE_P_FM_DEPTH] : 1.0f, 0.0f, 4.0f, C_AMBER);
            if (cur_type == 4) {
                /* FM4 algorithm picker with descriptive names */
                static const char *ALGO_NAMES[8] = {
                    "A\xE2\x86\x92""B\xE2\x86\x92""C\xE2\x86\x92""D",   /* 0: chain */
                    "AB\xE2\x86\x92""CD",                                   /* 1: 2+2   */
                    "A\xE2\x86\x92""B+C+D",                                 /* 2: 1:3   */
                    "A+B+C+D",                                               /* 3: all   */
                    "AB\xE2\x86\x92""C+D",                                  /* 4: 2+1+1 */
                    "A\xE2\x86\x92""B+C+D",                                 /* 5: brass */
                    "AB\xE2\x86\x92""C",                                    /* 6: stack */
                    "(A+B)\xE2\x86\x92""C\xE2\x86\x92""D",                 /* 7: split */
                };
                gfx_draw_text(px, py + 128, "FM ALGORITHM", C_T2, C_BG1, 2);
                int asel = sp ? (int)sp[SE_P_ALGO] : 0;
                int abw = (pw - 7 * 8) / 8;   /* button width */
                for (int i = 0; i < 8; i++) {
                    bool on = (i == asel);
                    int ax = px + i * (abw + 8);
                    uint16_t abg = on ? C_CYAN  : C_BG3;
                    uint16_t afg = on ? C_BG    : C_T1;
                    gfx_fill_round_rect(ax, py + 150, abw, 48, 6, abg);
                    if (!on) gfx_draw_round_rect(ax, py + 150, abw, 48, 6, C_LINE2);
                    draw_text_centred(ax, py + 150, abw, 48, ALGO_NAMES[i], afg, abg, 1);
                }
            }
        } else {
            gfx_draw_text(px, py + 40, "No FM modulation on this synth type.", C_T3, C_BG1, 2);
        }
        break;
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 * OVERLAY: Lane context menu (DUPLICATE / DELETE / CANCEL)
 * ══════════════════════════════════════════════════════════════════════════ */
void draw_ctx_menu_overlay(void)
{
    /* Semi-transparent dark scrim */
    gfx_fill_rect(0, 0, 1280, 720, 0x0000u);
    gfx_fill_rect(120, 148, 1040, 636, C_BG2);
    gfx_draw_round_rect(120, 148, 1040, 636, 12, C_LINE2);

    char hdr[64];
    int li = s_ctx_menu_lane;
    if (li >= 0 && li < NUM_LANES && g_song.lanes[li].name[0])
        snprintf(hdr, sizeof(hdr), "Lane %d  \xe2\x80\x94  %s", li + 1, g_song.lanes[li].name);
    else
        snprintf(hdr, sizeof(hdr), "Lane %d", li + 1);
    draw_text_centred(120, 148, 1040, 56, hdr, C_T2, C_BG2, 2);
    gfx_hline(204, C_LINE);

    /* RENAME */
    gfx_fill_round_rect(160, 216, 960, 56, 8, C_BG3);
    draw_text_centred(160, 216, 960, 56, "RENAME", C_T0, C_BG3, 2);

    /* CHANGE TYPE — show current type and cycle */
    const char *type_label = "SYNTH";
    if (li >= 0 && li < NUM_LANES) {
        lane_type_t t = g_song.lanes[li].type;
        if      (t == LANE_TYPE_DRUM) type_label = "DRUM";
        else if (t == LANE_TYPE_WAV)  type_label = "SAMPLE";
    }
    char type_buf[32];
    snprintf(type_buf, sizeof(type_buf), "TYPE: %s  \xbb", type_label);
    gfx_fill_round_rect(160, 288, 960, 56, 8, C_BG3);
    draw_text_centred(160, 288, 960, 56, type_buf, C_AMBER, C_BG3, 2);

    /* DUPLICATE */
    gfx_fill_round_rect(160, 360, 960, 56, 8, C_BG3);
    draw_text_centred(160, 360, 960, 56, "DUPLICATE", C_CYAN, C_BG3, 2);

    /* DELETE */
    gfx_fill_round_rect(160, 432, 960, 56, 8, C_BG3);
    gfx_draw_round_rect(160, 432, 960, 56, 8, C_RED);
    draw_text_centred(160, 432, 960, 56, "DELETE", C_RED, C_BG3, 2);

    /* SYNTH EDIT — only shown for synth lanes */
    if (li >= 0 && li < NUM_LANES && g_song.lanes[li].type == LANE_TYPE_SYNTH) {
        gfx_fill_round_rect(160, 504, 960, 56, 8, 0x0240u);
        gfx_draw_round_rect(160, 504, 960, 56, 8, C_CYAN);
        draw_text_centred(160, 504, 960, 56, "SYNTH EDIT", C_CYAN, 0x0240u, 2);
    }

    /* CANCEL */
    gfx_fill_round_rect(160, 576, 960, 56, 8, C_BG4);
    draw_text_centred(160, 576, 960, 56, "CANCEL", C_T2, C_BG4, 2);
}

/* ══════════════════════════════════════════════════════════════════════════
 * OVERLAY: Drum step velocity slider
 * ══════════════════════════════════════════════════════════════════════════ */
void draw_dg_vel_overlay(void)
{
    /* Overlay panel */
    gfx_fill_round_rect(120, 360, 1040, 240, 14, C_BG2);
    gfx_draw_round_rect(120, 360, 1040, 240, 14, C_LINE2);

    char title[48];
    snprintf(title, sizeof(title), "VELOCITY  R%d  Step %d",
             s_dg_vel_row + 1, s_dg_vel_step + 1);
    draw_text_centred(120, 360, 1040, 56, title, C_T2, C_BG2, 2);
    gfx_hline(416, C_LINE);

    /* Slider track */
    gfx_fill_round_rect(160, 444, 960, 18, 6, C_BG4);
    int fill_w = (int)((float)s_dg_vel_value / 127.0f * 960);
    gfx_fill_round_rect(160, 444, fill_w, 18, 6, C_AMBER);

    /* Value readout */
    char val_buf[8];
    snprintf(val_buf, sizeof(val_buf), "%d", s_dg_vel_value);
    draw_text_centred(120, 470, 1040, 40, val_buf, C_T0, C_BG2, 3);

    /* SET button */
    gfx_fill_round_rect(440, 530, 400, 52, 10, C_LIME);
    draw_text_centred(440, 530, 400, 52, "SET", C_BG, C_LIME, 3);
}

/* ══════════════════════════════════════════════════════════════════════════
 * SUB-SCREEN: ARPEGGIATOR
 * ══════════════════════════════════════════════════════════════════════════ */
void draw_arp_screen(void)
{
    lane_t *lane = &g_song.lanes[s_ctx_lane];
    arp_t  *arp  = &lane->arp;
    draw_minibar("Arpeggiator");

    int y = MINIBAR_H + 16;

    /* Enable toggle */
    gfx_draw_text(28, y + 12, "ENABLE", C_T2, C_BG1, 2);
    draw_toggle(680, y + 8, arp->enabled);
    y += 56;

    /* Mode seg */
    static const char *mode_lbl[] = { "UP","DOWN","U/D","D/U","ORD","RND","CHRD" };
    gfx_draw_text(28, y + 12, "MODE", C_T2, C_BG1, 2);
    draw_seg(280, y, 900, 48, mode_lbl, 7,
             (arp->mode < 7) ? (int)arp->mode : 0, C_BG, C_CYAN);
    y += 60;

    /* Oct range seg */
    static const char *oct_lbl[] = { "1","2","3","4" };
    gfx_draw_text(28, y + 12, "OCTAVE", C_T2, C_BG1, 2);
    draw_seg(280, y, 440, 48, oct_lbl, 4,
             (arp->octave_range > 0 ? arp->octave_range - 1 : 0), C_BG, C_LIME);
    y += 60;

    /* Step div seg */
    static const char *div_lbl[] = { "1/4","1/8","1/16","1/32" };
    int div_act = (arp->step_div == 4) ? 0 : (arp->step_div == 8) ? 1 :
                  (arp->step_div == 16) ? 2 : 3;
    gfx_draw_text(28, y + 12, "RATE", C_T2, C_BG1, 2);
    draw_seg(280, y, 540, 48, div_lbl, 4, div_act, C_BG, C_LIME);
    y += 60;

    /* Gate fader */
    gfx_draw_text(28, y + 12, "GATE", C_T2, C_BG1, 2);
    draw_fader(280, y + 18, 760, 12, arp->gate_pct / 100.0f, C_AMBER);
    char gate_buf[8];
    snprintf(gate_buf, sizeof(gate_buf), "%d%%", arp->gate_pct);
    gfx_draw_text(1060, y + 12, gate_buf, C_T0, C_BG1, 2);
    y += 56;

    /* Velocity mode seg */
    static const char *vel_lbl[] = { "ORIG","ACCENT","FIXED" };
    gfx_draw_text(28, y + 12, "VEL", C_T2, C_BG1, 2);
    draw_seg(280, y, 480, 48, vel_lbl, 3, (int)arp->velocity_mode, C_BG, C_LIME);
    y += 60;

    /* Latch + Retrig toggles */
    gfx_draw_text(28,  y + 12, "LATCH",   C_T2, C_BG1, 2);
    draw_toggle(280, y + 8, arp->latch);
    gfx_draw_text(440, y + 12, "RETRIG",  C_T2, C_BG1, 2);
    draw_toggle(680, y + 8, arp->retrigger);
    y += 56;

    /* Swing fader */
    gfx_draw_text(28, y + 12, "SWING", C_T2, C_BG1, 2);
    draw_fader(280, y + 18, 760, 12,
               (arp->swing_pct - 50) / 25.0f, C_CYAN);
    char sw_buf[8];
    snprintf(sw_buf, sizeof(sw_buf), "%d%%", arp->swing_pct);
    gfx_draw_text(1060, y + 12, sw_buf, C_T0, C_BG1, 2);
}

/* ══════════════════════════════════════════════════════════════════════════
 * SUB-SCREEN: GROOVE / SWING
 * ══════════════════════════════════════════════════════════════════════════ */
void draw_groove_screen(void)
{
    draw_minibar("Groove");

    int y = MINIBAR_H + 8;
    int row_h = 64;

    /* Header row */
    gfx_draw_text(28,  y + 8, "LANE",    C_T2, C_BG1, 2);
    gfx_draw_text(200, y + 8, "SWING %", C_T2, C_BG1, 2);
    gfx_draw_text(680, y + 8, "HUMANISE", C_T2, C_BG1, 2);
    gfx_draw_text(980, y + 8, "ON",      C_T2, C_BG1, 2);
    y += 36;
    gfx_hline(y, C_LINE);
    y += 4;

    for (int li = 0; li < NUM_LANES; li++) {
        lane_t *lane = &g_song.lanes[li];
        if (!lane->active) continue;
        if (y + row_h > 720) break;

        groove_t *g = &lane->groove;

        char lbl[8];
        snprintf(lbl, sizeof(lbl), "L%d", li + 1);
        gfx_draw_text(28, y + 20, lbl, C_T0, C_BG1, 2);

        /* Swing fader 50–75 */
        draw_fader(200, y + 24, 420, 12,
                   (g->swing_pct - 50) / 25.0f, C_AMBER);
        char sw[8];
        snprintf(sw, sizeof(sw), "%d", g->swing_pct);
        gfx_draw_text(636, y + 20, sw, C_T0, C_BG1, 2);

        /* Humanise fader 0–20 */
        draw_fader(680, y + 24, 280, 12,
                   g->humanise / 20.0f, C_CYAN);
        char hu[8];
        snprintf(hu, sizeof(hu), "%d", g->humanise);
        gfx_draw_text(974, y + 20, hu, C_T0, C_BG1, 2);

        /* Enable toggle */
        draw_toggle(1040, y + 16, g->enabled);

        gfx_hline(y + row_h - 1, C_LINE);
        y += row_h;
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 * SUB-SCREEN: ARRANGEMENT CHAIN
 * ══════════════════════════════════════════════════════════════════════════ */
void draw_arrangement_screen(void)
{
    draw_minibar("Arrangement");

    arrangement_t *arr = &g_song.arrangement;

    int y = MINIBAR_H + 16;

    /* Enabled toggle */
    gfx_draw_text(28, y + 12, "ENABLED", C_T2, C_BG1, 2);
    draw_toggle(680, y + 8, arr->enabled);
    char pos_buf[32];
    snprintf(pos_buf, sizeof(pos_buf), "Step %d / %d",
             arr->current_step + 1, arr->count > 0 ? arr->count : 1);
    gfx_draw_text(760, y + 12, pos_buf, C_T1, C_BG1, 2);
    y += 60;
    gfx_hline(y, C_LINE);
    y += 8;

    /* Step list */
    int row_h = 60;
    static const char *scene_names[] = { "A","B","C","D","E","F","G","H" };
    for (int i = 0; i < arr->count && y + row_h <= 690; i++) {
        arrangement_step_t *st = &arr->steps[i];
        bool sel = (i == s_arr_sel_step);
        uint16_t row_bg = sel ? C_BG3 : C_BG1;
        gfx_fill_rect(0, y, 1280, row_h, row_bg);
        if (sel) gfx_fill_rect(0, y, 4, row_h, C_LIME);

        char idx_buf[8];
        snprintf(idx_buf, sizeof(idx_buf), "%02d", i + 1);
        gfx_draw_text(20, y + 18, idx_buf, C_T2, row_bg, 2);

        /* Scene button */
        uint16_t sc_col = (st->scene_idx < SCENE_MAX &&
                           g_song.scenes[st->scene_idx].active) ? C_LIME : C_T1;
        const char *sn = (st->scene_idx < SCENE_MAX) ? scene_names[st->scene_idx] : "?";
        gfx_fill_round_rect(80, y + 10, 80, 40, 8, sel ? C_BG4 : C_BG2);
        draw_text_centred(80, y + 10, 80, 40, sn, sc_col, sel ? C_BG4 : C_BG2, 3);

        /* Repeat count */
        char rep_buf[12];
        snprintf(rep_buf, sizeof(rep_buf), "x%d", st->repeat);
        gfx_draw_text(200, y + 18, rep_buf, C_AMBER, row_bg, 2);

        /* +/- repeat buttons */
        gfx_fill_round_rect(320, y + 12, 52, 36, 6, C_BG3);
        draw_text_centred(320, y + 12, 52, 36, "-", C_T0, C_BG3, 3);
        gfx_fill_round_rect(384, y + 12, 52, 36, 6, C_BG3);
        draw_text_centred(384, y + 12, 52, 36, "+", C_LIME, C_BG3, 3);

        gfx_hline(y + row_h - 1, C_LINE);
        y += row_h;
    }

    /* ADD SCENE button */
    if (arr->count < ARRANGEMENT_MAX_STEPS) {
        gfx_fill_round_rect(24, y + 8, 320, 44, 8, C_BG3);
        gfx_draw_round_rect(24, y + 8, 320, 44, 8, C_LINE2);
        draw_text_centred(24, y + 8, 320, 44, "+ ADD STEP", C_LIME, C_BG3, 2);
    }
    /* CLEAR button */
    gfx_fill_round_rect(380, y + 8, 200, 44, 8, C_BG3);
    gfx_draw_round_rect(380, y + 8, 200, 44, 8, C_RED);
    draw_text_centred(380, y + 8, 200, 44, "CLEAR", C_RED, C_BG3, 2);
}

/* ══════════════════════════════════════════════════════════════════════════
 * SUB-SCREEN: NOTE REPEAT + SEND LEVELS
 * ══════════════════════════════════════════════════════════════════════════ */
void draw_note_repeat_screen(void)
{
    draw_minibar("Note Repeat / Send");

    int y = MINIBAR_H + 16;

    /* Per-lane: Note Repeat */
    gfx_draw_text(28,  y + 8, "LANE", C_T2, C_BG1, 2);
    gfx_draw_text(200, y + 8, "NOTE REPEAT", C_T2, C_BG1, 2);
    gfx_draw_text(520, y + 8, "RATE", C_T2, C_BG1, 2);
    gfx_draw_text(780, y + 8, "SEND LEVEL", C_T2, C_BG1, 2);
    y += 36;
    gfx_hline(y, C_LINE);
    y += 4;

    static const char *rate_lbl[] = { "1/4","1/8","1/16","1/32" };
    int row_h = 64;

    for (int li = 0; li < NUM_LANES; li++) {
        lane_t *lane = &g_song.lanes[li];
        if (!lane->active) continue;
        if (y + row_h > 720) break;

        char lbl[8];
        snprintf(lbl, sizeof(lbl), "L%d", li + 1);
        gfx_draw_text(28, y + 20, lbl, C_T0, C_BG1, 2);

        /* Note repeat toggle */
        draw_toggle(200, y + 18, lane->note_repeat);

        /* Rate seg */
        int rate_act = (lane->note_repeat_div == 4) ? 0 :
                       (lane->note_repeat_div == 8) ? 1 :
                       (lane->note_repeat_div == 16) ? 2 : 3;
        draw_seg(380, y + 8, 340, 46, rate_lbl, 4, rate_act, C_BG, C_AMBER);

        /* Send level fader */
        draw_fader(780, y + 24, 380, 12, lane->send_level, C_CYAN);
        char sl_buf[8];
        snprintf(sl_buf, sizeof(sl_buf), "%d%%", (int)(lane->send_level * 100));
        gfx_draw_text(1176, y + 20, sl_buf, C_T0, C_BG1, 2);

        gfx_hline(y + row_h - 1, C_LINE);
        y += row_h;
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 * SUB-SCREEN: ON-SCREEN KEYBOARD
 * Layout: text field at top, then a 10×4 QWERTY grid + backspace/space/done.
 * Row 0: Q W E R T Y U I O P
 * Row 1:  A S D F G H J K L
 * Row 2:    Z X C V B N M  ← (backspace)
 * Row 3:  [SPACE (wide)]  [DONE (wide)]
 * ══════════════════════════════════════════════════════════════════════════ */
void draw_osk_screen(void)
{
    gfx_fill_rect(0, 0, 1280, 720, C_BG);

    /* Mini-bar back */
    gfx_fill_rect(0, 0, 1280, MINIBAR_H, C_BG2);
    gfx_hline(MINIBAR_H - 1, C_LINE);
    gfx_fill_rect(0, 0, MINIBAR_BACK_W, MINIBAR_H, C_BG2);
    gfx_vline(MINIBAR_BACK_W, 0, MINIBAR_H, C_LINE);
    gfx_draw_text(30, 24, "<", C_T1, C_BG2, 3);
    draw_text_centred(MINIBAR_BACK_W, 0, 1280 - MINIBAR_BACK_W, MINIBAR_H,
                      s_osk_title, C_T1, C_BG2, 2);

    /* Text field */
    int tf_y = MINIBAR_H + 12;
    gfx_fill_round_rect(40, tf_y, 1200, 56, 8, C_BG3);
    gfx_draw_round_rect(40, tf_y, 1200, 56, 8, C_LIME);
    /* show buffer with blinking cursor char */
    char disp[OSK_MAX_LEN + 3];
    snprintf(disp, sizeof(disp), "%s_", s_osk_buf);
    gfx_draw_text(60, tf_y + 14, disp, C_LIME, C_BG3, 3);

    /* Keyboard rows */
    static const char *ROWS[3] = { "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM" };
    static const int   N_COLS[3] = { 10, 9, 7 };
    int key_w = 116, key_h = 86, gap = 4;
    int kb_y  = tf_y + 56 + 16;

    for (int r = 0; r < 3; r++) {
        int n   = N_COLS[r];
        int row_w = n * key_w + (n - 1) * gap;
        int x0  = (1280 - row_w) / 2;
        for (int c = 0; c < n; c++) {
            int kx = x0 + c * (key_w + gap);
            int ky = kb_y + r * (key_h + gap);
            gfx_fill_round_rect(kx, ky, key_w, key_h, 8, C_BG3);
            gfx_draw_round_rect(kx, ky, key_w, key_h, 8, C_LINE2);
            char ch[2] = { ROWS[r][c], 0 };
            draw_text_centred(kx, ky, key_w, key_h, ch, C_T0, C_BG3, 3);
        }
    }

    /* Row 3: SPACE + BACKSPACE + DONE */
    int r3y  = kb_y + 3 * (key_h + gap);
    /* Numbers row: 0-9 */
    int num_w = 116;
    for (int i = 0; i < 10; i++) {
        int kx = 40 + i * (num_w + gap);
        gfx_fill_round_rect(kx, r3y, num_w, key_h, 8, C_BG3);
        gfx_draw_round_rect(kx, r3y, num_w, key_h, 8, C_LINE2);
        char ch[2] = { (char)('0' + i), 0 };
        draw_text_centred(kx, r3y, num_w, key_h, ch, C_T0, C_BG3, 3);
    }
    /* Row 4: SPACE, BKSP, DONE */
    int r4y = r3y + key_h + gap;
    gfx_fill_round_rect(40,   r4y, 560, key_h, 8, C_BG3);
    gfx_draw_round_rect(40,   r4y, 560, key_h, 8, C_LINE2);
    draw_text_centred(40, r4y, 560, key_h, "SPACE", C_T1, C_BG3, 2);

    gfx_fill_round_rect(620,  r4y, 280, key_h, 8, C_BG3);
    gfx_draw_round_rect(620,  r4y, 280, key_h, 8, C_RED);
    draw_text_centred(620, r4y, 280, key_h, "DEL", C_RED, C_BG3, 2);

    gfx_fill_round_rect(920,  r4y, 320, key_h, 8, C_LIME_DIM);
    gfx_draw_round_rect(920,  r4y, 320, key_h, 8, C_LIME);
    draw_text_centred(920, r4y, 320, key_h, "DONE", C_LIME, C_LIME_DIM, 3);
}

/* ══════════════════════════════════════════════════════════════════════════
 * SUB-SCREEN: BLUETOOTH
 * ESP32-C6 (companion chip) supports BLE 5.0 only — no Classic BT / A2DP.
 * This screen exposes BLE advertisement / scan for custom MIDI-over-BLE
 * or future audio profiles.  Classic BT speaker pairing is not available.
 * ══════════════════════════════════════════════════════════════════════════ */
void draw_bluetooth_screen(void)
{
    gfx_fill_rect(0, 0, 1280, 720, C_BG);

    /* Mini-bar back */
    gfx_fill_rect(0, 0, 1280, MINIBAR_H, C_BG2);
    gfx_hline(MINIBAR_H - 1, C_LINE);
    gfx_fill_rect(0, 0, MINIBAR_BACK_W, MINIBAR_H, C_BG2);
    gfx_vline(MINIBAR_BACK_W, 0, MINIBAR_H, C_LINE);
    gfx_draw_text(30, 24, "<", C_T1, C_BG2, 3);
    draw_text_centred(MINIBAR_BACK_W, 0, 1280 - MINIBAR_BACK_W, MINIBAR_H,
                      "BLUETOOTH", C_T1, C_BG2, 2);

    int y = MINIBAR_H + 16;

    /* Info banner */
    gfx_fill_round_rect(40, y, 1200, 52, 8, C_BG3);
    gfx_draw_round_rect(40, y, 1200, 52, 8, C_AMBER);
    gfx_draw_text(64, y + 16,
        "BLE 5.0 only (ESP32-C6). Classic BT / A2DP speakers not supported.",
        C_AMBER, C_BG3, 1);
    y += 68;

    /* Connection status */
    if (s_bt_connected) {
        char buf[64];
        snprintf(buf, sizeof(buf), "Connected: %s", s_bt_connected_name);
        gfx_fill_round_rect(40, y, 1200, 52, 8, C_LIME_DIM);
        gfx_draw_round_rect(40, y, 1200, 52, 8, C_LIME);
        draw_text_centred(40, y, 1200, 52, buf, C_LIME, C_LIME_DIM, 2);
    } else {
        gfx_fill_round_rect(40, y, 1200, 52, 8, C_BG3);
        draw_text_centred(40, y, 1200, 52, "Not connected", C_T2, C_BG3, 2);
    }
    y += 68;

    /* Scan button */
    uint16_t scan_bg = s_bt_scanning ? C_CYAN : C_BG3;
    uint16_t scan_fg = s_bt_scanning ? C_BG  : C_CYAN;
    gfx_fill_round_rect(40, y, 400, 56, 10, s_bt_scanning ? C_CYAN : C_BG3);
    gfx_draw_round_rect(40, y, 400, 56, 10, C_CYAN);
    draw_text_centred(40, y, 400, 56,
        s_bt_scanning ? "SCANNING..." : "SCAN", scan_fg, scan_bg, 2);
    (void)scan_bg; (void)scan_fg;

    /* Device list */
    int list_y = y + 72;
    gfx_draw_text(40, list_y - 20, "BLE DEVICES", C_T2, C_BG, 1);
    if (s_bt_device_count == 0) {
        gfx_draw_text(40, list_y, s_bt_scanning ? "Scanning..." : "No devices found. Tap SCAN.",
                      C_T3, C_BG, 2);
    }
    for (int i = 0; i < s_bt_device_count && i < BT_SCAN_MAX; i++) {
        int dy = list_y + i * 56;
        bool sel = (i == s_bt_sel);
        gfx_fill_round_rect(40, dy, 1200, 48, 6, sel ? C_BG4 : C_BG2);
        if (sel) gfx_draw_round_rect(40, dy, 1200, 48, 6, C_CYAN);
        gfx_draw_text(64, dy + 12, s_bt_devices[i], sel ? C_CYAN : C_T0, sel ? C_BG4 : C_BG2, 2);
    }
}
