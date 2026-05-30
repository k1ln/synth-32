#pragma once
/*
 * gfx.h — RGB565 framebuffer renderer + LCD init
 * Physical buffer: 720×1280 portrait (HX8394 native).
 * Logical coordinate space: 1280×720 landscape.
 */

#include <stdint.h>
#include <stdbool.h>
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_mipi_dsi.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Screen geometry ─────────────────────────────────────────────────────── */
#define LCD_PW  720
#define LCD_PH  1280
#define SCR_W   1280
#define SCR_H   720

/* ── v2 design-system colour tokens (RGB565, little-endian) ──────────────── */
/* Surfaces */
#define C_BG       ((uint16_t)0x0841u) /* #08080A  bg-0 */
#define C_BG1      ((uint16_t)0x0862u) /* #0E0E11  bg-1 */
#define C_BG2      ((uint16_t)0x10A3u) /* #15151A  bg-2 */
#define C_BG3      ((uint16_t)0x18E4u) /* #1C1C22  bg-3 */
#define C_BG4      ((uint16_t)0x2125u) /* #25252C  bg-4 */
/* Lines */
#define C_LINE     ((uint16_t)0x18E4u) /* #1E1E26 */
#define C_LINE2    ((uint16_t)0x2946u) /* #2A2A34 */
/* Text */
#define C_T0       ((uint16_t)0xF7BEu) /* #F4F4F6 */
#define C_T1       ((uint16_t)0xBDD8u) /* #B8B8C0 */
#define C_T2       ((uint16_t)0x73AFu) /* #74747D */
#define C_T3       ((uint16_t)0x4A4Au) /* #4B4B54 */
/* Accents */
#define C_LIME     ((uint16_t)0xC7A8u) /* #C7F546 */
#define C_LIME_DIM ((uint16_t)0x1921u) /* #1E240A lime dim */
#define C_AMBER    ((uint16_t)0xFDA8u) /* #FFB547 */
#define C_CYAN     ((uint16_t)0x5E3Fu) /* #5BC4FF */
#define C_RED      ((uint16_t)0xFA69u) /* #FF4F4F */
/* Piano keys */
#define C_WHITE_K  ((uint16_t)0xFFFFu) /* pure white */
#define C_BLACK_K  ((uint16_t)0x0000u) /* pure black */
/* Legacy aliases */
#define C_GREEN    C_LIME
#define C_DKGREEN  ((uint16_t)0x2D80u) /* #2E700x */

/* ── LCD / framebuffer ───────────────────────────────────────────────────── */
extern uint16_t              *g_fb;        /* current draw target */
extern esp_lcd_panel_handle_t g_dpi_panel;

/* Initialise MIPI-DSI display, allocate framebuffer, turn on backlight. */
void lcd_init(void);

/* Register the vsync callback used by gfx_commit() for pacing.
 * Must be called after lcd_init(). */
void lcd_register_refresh_cb(void);

/* Present the framebuffer to the display (non-blocking — the vsync wait is
 * deferred to gfx_wait_present() at the start of the next frame). */
void gfx_commit(void);

/* Block until the previously presented frame has been latched by the panel, so
 * the back buffer is safe to draw into. Call once at the top of each frame,
 * before drawing. */
void gfx_wait_present(void);

/* Index of the buffer drawing targets (the back buffer). */
int gfx_back_index(void);

/* Non-blocking check that the previous present has latched (back buffer free to
 * draw into). Clears the pending flag and returns true when ready. */
bool gfx_present_ready(void);

/* ── Draw primitives ─────────────────────────────────────────────────────── */
void     gfx_fill_rect      (int x, int y, int w, int h, uint16_t col);
void     gfx_draw_rect      (int x, int y, int w, int h, uint16_t col);  /* 1-px border */
void     gfx_fill_screen    (uint16_t col);
void     gfx_fill_circle    (int cx, int cy, int r, uint16_t col);
void     gfx_fill_round_rect(int x, int y, int w, int h, int r, uint16_t col);
void     gfx_draw_round_rect(int x, int y, int w, int h, int r, uint16_t col);
void     gfx_draw_text      (int x, int y, const char *str, uint16_t fg, uint16_t bg, int scale);
int      gfx_text_width     (const char *str, int scale);
/* Horizontal / vertical 1-px rules */
static inline void gfx_hline(int y, uint16_t col) { gfx_fill_rect(0, y, 1280, 1, col); }
static inline void gfx_vline(int x, int y, int h, uint16_t col) { gfx_fill_rect(x, y, 1, h, col); }

#ifdef __cplusplus
}
#endif
