/*
 * ui_draw.c — shared draw primitives, chrome (topbar/tabbar/minibar),
 *             and the main draw_screen() dispatcher.
 *
 * Per-screen draw functions live in ui_screens.c.
 * All mutable state lives in ui.c, accessed via ui_state.h externs.
 */

#include <string.h>
#include <stdio.h>
#include <math.h>
#include "esp_timer.h"
#include "gfx.h"
#include "lane.h"
#include "clock.h"
#include "settings.h"
#include "song.h"
#include "ui.h"
#include "ui_state.h"

/* ── Forward declarations for per-screen draw functions (ui_screens.c) ───── */
void draw_song_view(void);
void draw_live_screen(void);
void draw_master_screen(void);
void draw_menu_screen(void);
void draw_drum_grid_screen(void);
void draw_piano_roll_screen(void);
void draw_fx_adsr_screen(void);
void draw_sound_browser_screen(void);
void draw_settings_screen(void);
void draw_song_browser_screen(void);

/* ══════════════════════════════════════════════════════════════════════════
 * Low-level draw helpers
 * ══════════════════════════════════════════════════════════════════════════ */

static inline int clamp_i(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}


void draw_text_centred(int x, int y, int w, int h, const char *str,
                       uint16_t fg, uint16_t bg, int scale)
{
    int tw = gfx_text_width(str, scale);
    int tx = x + (w - tw) / 2;
    int ty = y + (h - 8 * scale) / 2;
    gfx_draw_text(tx, ty, str, fg, bg, scale);
}

void draw_seg(int x, int y, int w, int h, const char **opts, int n,
              int active, uint16_t active_fg, uint16_t active_bg)
{
    int sw = w / n;
    for (int i = 0; i < n; i++) {
        int sx = x + i * sw;
        bool on = (i == active);
        uint16_t bg = on ? active_bg : C_BG3;
        uint16_t fg = on ? active_fg : C_T1;
        gfx_fill_rect(sx, y, sw, h, bg);
        gfx_draw_rect(sx, y, sw, h, C_LINE2);
        draw_text_centred(sx, y, sw, h, opts[i], fg, bg, 2);
    }
}

void draw_fader(int x, int y, int w, int h, float frac, uint16_t col)
{
    gfx_fill_round_rect(x, y, w, h, 3, C_BG4);
    int fw = (int)(frac * w);
    if (fw > 4) gfx_fill_round_rect(x, y, fw, h, 3, col);
}

void draw_toggle(int x, int y, bool on)
{
    uint16_t bg = on ? C_LIME : C_BG4;
    gfx_fill_round_rect(x, y, 68, 38, 19, bg);
    gfx_draw_round_rect(x, y, 68, 38, 19, C_LINE2);
    int kx = on ? (x + 68 - 4 - 26) : (x + 4);
    gfx_fill_circle(kx + 13, y + 19, 13, on ? C_BG : C_T0);
}

void draw_type_chip(int x, int y, lane_type_t type)
{
    const char *label;
    uint16_t fg, bg;
    switch (type) {
    case LANE_TYPE_DRUM:      label = "DRUM";  fg = C_LIME;  bg = C_LIME_DIM; break;
    case LANE_TYPE_DRUMSYNTH: label = "808";   fg = C_RED; bg = 0x3000u;  break;
    case LANE_TYPE_SYNTH:     label = "SYNTH"; fg = C_CYAN;  bg = 0x0240u;    break;
    case LANE_TYPE_WAV:       label = "WAV";   fg = C_AMBER; bg = 0x2040u;    break;
    default:                  return;
    }
    int tw = gfx_text_width(label, 2);
    gfx_fill_round_rect(x, y, tw + 24, 36, 4, bg);
    gfx_draw_text(x + 12, y + 10, label, fg, bg, 2);
}

uint16_t lane_accent(lane_type_t t)
{
    switch (t) {
    case LANE_TYPE_DRUM:      return C_LIME;
    case LANE_TYPE_DRUMSYNTH: return C_RED;
    case LANE_TYPE_SYNTH:     return C_CYAN;
    case LANE_TYPE_WAV:       return C_AMBER;
    default:                  return C_T2;
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 * Chrome: top bar, tab bar, mini bar
 * ══════════════════════════════════════════════════════════════════════════ */

void draw_topbar(void)
{
    bool running = g_song.clock.running;

    gfx_fill_rect(0, 0, 1280, TOPBAR_H, C_BG2);
    gfx_hline(TOPBAR_H - 1, C_LINE);

    /* ── Transport: single toggle button ────────────────────────────────── */
    /* When stopped: lime bg + ▶ triangle (invite to play)                  */
    /* When running: red bg + ■ square   (invite to stop)                   */
    uint16_t tb_bg = running ? C_RED : C_LIME;
    gfx_fill_round_rect(116, 8, 56, 56, 10, tb_bg);
    gfx_draw_round_rect(116, 8, 56, 56, 10, C_LINE2);
    if (running) {
        /* ■ stop symbol centred in 56×56 at (116,8): 24×24 white square */
        gfx_fill_rect(130, 24, 24, 24, C_BG);
    } else {
        /* ▶ play triangle */
        int th = 28, td = 24;
        int ty = 8 + 28 - th / 2;
        int tx = 116 + 28 - td / 2;
        for (int r = 0; r < th; r++) {
            int dist = (r <= th / 2) ? r : (th - 1 - r);
            int len  = 1 + dist * td / (th / 2);
            gfx_fill_rect(tx, ty + r, len, 1, C_BG);
        }
    }

    /* ── BPM display ─────────────────────────────────────────────────────── */
    char bpm_buf[16];
    snprintf(bpm_buf, sizeof(bpm_buf), "%.1f", (double)g_song.clock.bpm);
    gfx_draw_text(188, 12, "BPM", C_T2, C_BG2, 1);
    gfx_draw_text(188, 28, bpm_buf, C_T0, C_BG2, 3);

    /* ── Song name (right) ────────────────────────────────────────────────── */
    char song_buf[80];
    snprintf(song_buf, sizeof(song_buf), "%s%s",
             g_song.name[0] ? g_song.name : "Untitled",
             g_song.dirty ? " *" : "");
    int nw = gfx_text_width(song_buf, 2);
    gfx_draw_text(1280 - nw - 24, 24, song_buf, C_T1, C_BG2, 2);

    /* Status banner is now drawn by draw_status_banner() so it appears over
     * every screen, not only main ones. */
}

void draw_status_banner(void)
{
    int64_t now_ms = esp_timer_get_time() / 1000;
    if (s_status_msg[0] && now_ms < s_status_until_ms) {
        int sw = gfx_text_width(s_status_msg, 2);
        int sx = 640 - sw / 2;
        if (sx < 280) sx = 280;
        gfx_fill_round_rect(sx - 14, 12, sw + 28, 48, 8, C_BG);
        gfx_draw_round_rect(sx - 14, 12, sw + 28, 48, 8, C_LIME);
        gfx_draw_text(sx, 24, s_status_msg, C_LIME, C_BG, 2);
    } else if (s_status_msg[0] && now_ms >= s_status_until_ms) {
        s_status_msg[0] = '\0';
    }
}

void draw_tabbar(void)
{
    /* 8 tabs matching the web UI primary navigation */
    static const char *LABELS[TAB_COUNT] = {
        "LANES", "DRUM", "ROLL", "MASTER", "FX", "LIVE", "FILES", "MENU"
    };
    /* Icons (single char, scale 2) shown above the label */
    static const char *ICONS[TAB_COUNT] = {
        "\xe2\x89\xa1",   /* ≡ */
        "\xe2\x8a\x9e",   /* ⊞ */
        "\xe2\x99\xa9",   /* ♩ */
        "\xe2\x8a\x99",   /* ⊙ */
        "\xe2\x8b\xae",   /* ⋮ */
        "\xe2\x96\xb6",   /* ▶ */
        "\xe2\x99\xab",   /* ♫ */
        "\xe2\x80\xa6",   /* … */
    };

    gfx_fill_rect(0, TABBAR_Y, 1280, TABBAR_H, C_BG2);
    gfx_hline(TABBAR_Y, C_LINE2);

    for (int i = 0; i < TAB_COUNT; i++) {
        int tx = i * TAB_W;
        bool active = (i == s_active_tab);
        uint16_t bg = active ? C_LIME_DIM : C_BG2;
        uint16_t fg = active ? C_LIME     : C_T2;
        gfx_fill_rect(tx, TABBAR_Y, TAB_W, TABBAR_H, bg);
        if (i > 0) gfx_vline(tx, TABBAR_Y, TABBAR_H, C_LINE2);
        /* Accent bar at top of active tab */
        if (active) gfx_fill_rect(tx, TABBAR_Y, TAB_W, 3, C_LIME);
        /* Icon */
        int iw = gfx_text_width(ICONS[i], 2);
        gfx_draw_text(tx + (TAB_W - iw) / 2, TABBAR_Y + 10, ICONS[i], fg, bg, 2);
        /* Label */
        int lw = gfx_text_width(LABELS[i], 1);
        gfx_draw_text(tx + (TAB_W - lw) / 2, TABBAR_Y + 58, LABELS[i], fg, bg, 1);
    }
}

void draw_minibar(const char *title)
{
    gfx_fill_rect(0, 0, 1280, MINIBAR_H, C_BG2);
    gfx_hline(MINIBAR_H - 1, C_LINE);
    gfx_fill_rect(0, 0, MINIBAR_BACK_W, MINIBAR_H, C_BG2);
    gfx_vline(MINIBAR_BACK_W, 0, MINIBAR_H, C_LINE);
    gfx_draw_text(30, 24, "<", C_T1, C_BG2, 3);
    gfx_draw_text(MINIBAR_BACK_W + 24, 22, title, C_T0, C_BG2, 3);
}

static int draw_minibar_ctrl(int rx, const char *lbl, const char *val)
{
    if (!lbl) return rx;
    const char *v = val ? val : "";
    int vw = gfx_text_width(v,   2);
    int lw = gfx_text_width(lbl, 1);
    int cw = (vw > lw ? vw : lw) + 48;
    rx -= cw;
    gfx_fill_rect(rx, 0, cw, MINIBAR_H, C_BG2);
    gfx_vline(rx, 0, MINIBAR_H, C_LINE);
    gfx_draw_text(rx + 16, 10, lbl, C_T2, C_BG2, 1);
    gfx_draw_text(rx + 16, 30, v,   C_T0, C_BG2, 2);
    return rx;
}

void draw_minibar_ex(const char *title,
                     const char *ctrl1_lbl, const char *ctrl1_val,
                     const char *ctrl2_lbl, const char *ctrl2_val)
{
    draw_minibar(title);
    int rx = 1280;
    rx = draw_minibar_ctrl(rx, ctrl2_lbl, ctrl2_val);
    draw_minibar_ctrl(rx, ctrl1_lbl, ctrl1_val);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Piano key (shared by ui_screens.c and ui.c piano touch handler)
 * ══════════════════════════════════════════════════════════════════════════ */

void draw_live_piano_key(int k)
{
    const piano_key_t *r = &s_piano_keys[k];
    uint16_t bg = r->pressed ? C_LIME : (r->is_black ? C_BLACK_K : C_WHITE_K);
    uint16_t br = r->pressed ? C_LIME : (r->is_black ? C_LINE    : C_LINE2);
    gfx_fill_round_rect(r->x, r->y, r->w, r->h, 4, bg);
    gfx_draw_round_rect(r->x, r->y, r->w, r->h, 4, br);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Main draw dispatcher
 * ══════════════════════════════════════════════════════════════════════════ */

void draw_screen(void)
{
    screen_id_t scr = current_screen();
    /* Wait for the previous frame to latch before overwriting the back buffer.
     * This block was moved out of gfx_commit() so the UI loop can keep polling
     * touch and firing notes during the panel-latch window instead of stalling
     * there — see gfx_wait_present(). */
    gfx_wait_present();
    gfx_fill_screen(C_BG1);

    if (is_main_screen(scr)) {
        draw_topbar();
        switch (scr) {
        case SCREEN_SONG:   draw_song_view();     break;
        case SCREEN_LIVE:   draw_live_screen();   break;
        case SCREEN_MASTER: draw_master_screen(); break;
        case SCREEN_MENU:   draw_menu_screen();   break;
        default: break;
        }
        draw_tabbar();
    } else {
        switch (scr) {
        case SCREEN_DRUM_GRID:     draw_drum_grid_screen();     break;
        case SCREEN_PIANO_ROLL:    draw_piano_roll_screen();    break;
        case SCREEN_FX_ADSR:       draw_fx_adsr_screen();       break;
        case SCREEN_SOUND_BROWSER: draw_sound_browser_screen(); break;
        case SCREEN_SETTINGS:      draw_settings_screen();      break;
        case SCREEN_SONG_BROWSER:  draw_song_browser_screen();  break;
        case SCREEN_WAV_DETAIL:    draw_wav_detail_screen();    break;
        case SCREEN_ARP:           draw_arp_screen();           break;
        case SCREEN_GROOVE:        draw_groove_screen();        break;
        case SCREEN_ARRANGEMENT:   draw_arrangement_screen();  break;
        case SCREEN_NOTE_REPEAT:   draw_note_repeat_screen();  break;
        case SCREEN_OSK:           draw_osk_screen();          break;
        case SCREEN_BLUETOOTH:     draw_bluetooth_screen();    break;
        case SCREEN_SYNTH_EDIT:    draw_synth_edit_screen();   break;
        default: break;
        }
    }

    /* Overlays drawn on top of any screen */
    if (s_ctx_menu_open)  draw_ctx_menu_overlay();
    if (s_dg_vel_row >= 0) draw_dg_vel_overlay();
    draw_status_banner();

    /* Touch highlight: white ring at finger-down position, one frame only */
    if (s_hl_visible) {
        int cx = s_hl_x - 24, cy = s_hl_y - 24;
        gfx_draw_round_rect(cx,     cy,     48, 48, 24, 0xFFFFu);
        gfx_draw_round_rect(cx + 1, cy + 1, 46, 46, 23, 0xFFFFu);
        gfx_draw_round_rect(cx + 2, cy + 2, 44, 44, 22, 0xC618u);
        s_hl_visible = false;
    }

    gfx_commit();
}
