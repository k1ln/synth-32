/*
 * synth-32.c
 * Waveshare ESP32-P4 · BSP MIPI-DSI (HX8394) · GT911 touch · ES8311 audio
 * Renderer: hand-rolled RGB565 framebuffer + 8×8 bitmap font (no external GFX lib)
 */

#include <stdbool.h>
#include <string.h>
#include <inttypes.h>
#include <math.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_http_server.h"
#include "esp_mac.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "es8311_codec.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_cache.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "bsp/esp32_p4_platform.h"
#include "bsp/touch.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_gt911.h"

static const char *TAG = "synth32";

/* ── Board pin map ───────────────────────────────────────────────────────────── */
#define PIN_I2C_SDA    BSP_I2C_SDA
#define PIN_I2C_SCL    BSP_I2C_SCL
#define I2C_PORT       BSP_I2C_NUM

#define I2S_MCK_IO     BSP_I2S_MCLK
#define I2S_BCK_IO     BSP_I2S_SCLK
#define I2S_WS_IO      BSP_I2S_LCLK
#define I2S_DO_IO      BSP_I2S_DOUT
#define I2S_DI_IO      BSP_I2S_DSIN
#define GPIO_PA        BSP_POWER_AMP_IO
#define I2S_PORT       CONFIG_BSP_I2S_NUM
#define MCLK_MULTIPLE  256
#define BL_GPIO_FALLBACK GPIO_NUM_26

/* ── 8×8 bitmap font, ASCII 0x20–0x7F (96 glyphs × 8 bytes) ─────────────────── */
/* Each byte = one row; MSB = leftmost pixel. */
static const uint8_t FONT8[96][8] = {
    /* 20 ' ' */ {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 21 '!' */ {0x18,0x18,0x18,0x18,0x18,0x00,0x18,0x00},
    /* 22 '"' */ {0x66,0x66,0x44,0x00,0x00,0x00,0x00,0x00},
    /* 23 '#' */ {0x36,0x36,0x7F,0x36,0x7F,0x36,0x36,0x00},
    /* 24 '$' */ {0x0C,0x3E,0x03,0x1E,0x30,0x1F,0x06,0x00},
    /* 25 '%' */ {0x62,0x66,0x0C,0x18,0x30,0x66,0x46,0x00},
    /* 26 '&' */ {0x1C,0x36,0x1C,0x5B,0x73,0x66,0x5C,0x00},
    /* 27 '\''*/ {0x06,0x06,0x03,0x00,0x00,0x00,0x00,0x00},
    /* 28 '(' */ {0x18,0x0C,0x06,0x06,0x06,0x0C,0x18,0x00},
    /* 29 ')' */ {0x06,0x0C,0x18,0x18,0x18,0x0C,0x06,0x00},
    /* 2A '*' */ {0x00,0x36,0x1C,0x7F,0x1C,0x36,0x00,0x00},
    /* 2B '+' */ {0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x00},
    /* 2C ',' */ {0x00,0x00,0x00,0x00,0x00,0x1C,0x0C,0x06},
    /* 2D '-' */ {0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00},
    /* 2E '.' */ {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00},
    /* 2F '/' */ {0x60,0x30,0x18,0x0C,0x06,0x03,0x01,0x00},
    /* 30 '0' */ {0x3C,0x66,0x6E,0x76,0x66,0x66,0x3C,0x00},
    /* 31 '1' */ {0x0C,0x0E,0x0C,0x0C,0x0C,0x0C,0x3F,0x00},
    /* 32 '2' */ {0x3C,0x66,0x60,0x30,0x0C,0x06,0x7E,0x00},
    /* 33 '3' */ {0x3C,0x66,0x60,0x38,0x60,0x66,0x3C,0x00},
    /* 34 '4' */ {0x30,0x38,0x3C,0x36,0x7F,0x30,0x30,0x00},
    /* 35 '5' */ {0x7E,0x06,0x3E,0x60,0x60,0x66,0x3C,0x00},
    /* 36 '6' */ {0x38,0x06,0x3E,0x66,0x66,0x66,0x3C,0x00},
    /* 37 '7' */ {0x7E,0x60,0x30,0x18,0x0C,0x0C,0x0C,0x00},
    /* 38 '8' */ {0x3C,0x66,0x66,0x3C,0x66,0x66,0x3C,0x00},
    /* 39 '9' */ {0x3C,0x66,0x66,0x7C,0x60,0x30,0x1E,0x00},
    /* 3A ':' */ {0x00,0x18,0x18,0x00,0x18,0x18,0x00,0x00},
    /* 3B ';' */ {0x00,0x18,0x18,0x00,0x18,0x0C,0x06,0x00},
    /* 3C '<' */ {0x60,0x30,0x18,0x0C,0x18,0x30,0x60,0x00},
    /* 3D '=' */ {0x00,0x00,0x7E,0x00,0x7E,0x00,0x00,0x00},
    /* 3E '>' */ {0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0x00},
    /* 3F '?' */ {0x3C,0x66,0x60,0x30,0x18,0x00,0x18,0x00},
    /* 40 '@' */ {0x3E,0x63,0x6B,0x7B,0x6B,0x03,0x3E,0x00},
    /* 41 'A' */ {0x18,0x3C,0x66,0x66,0x7E,0x66,0x66,0x00},
    /* 42 'B' */ {0x3E,0x66,0x66,0x3E,0x66,0x66,0x3E,0x00},
    /* 43 'C' */ {0x3C,0x66,0x06,0x06,0x06,0x66,0x3C,0x00},
    /* 44 'D' */ {0x1E,0x36,0x66,0x66,0x66,0x36,0x1E,0x00},
    /* 45 'E' */ {0x7E,0x06,0x06,0x3E,0x06,0x06,0x7E,0x00},
    /* 46 'F' */ {0x7E,0x06,0x06,0x3E,0x06,0x06,0x06,0x00},
    /* 47 'G' */ {0x3C,0x66,0x06,0x76,0x66,0x66,0x3C,0x00},
    /* 48 'H' */ {0x66,0x66,0x66,0x7E,0x66,0x66,0x66,0x00},
    /* 49 'I' */ {0x3E,0x0C,0x0C,0x0C,0x0C,0x0C,0x3E,0x00},
    /* 4A 'J' */ {0x78,0x30,0x30,0x30,0x30,0x36,0x1C,0x00},
    /* 4B 'K' */ {0x66,0x36,0x1E,0x0E,0x1E,0x36,0x66,0x00},
    /* 4C 'L' */ {0x06,0x06,0x06,0x06,0x06,0x06,0x7E,0x00},
    /* 4D 'M' */ {0x63,0x77,0x7F,0x6B,0x63,0x63,0x63,0x00},
    /* 4E 'N' */ {0x66,0x6E,0x7E,0x76,0x66,0x66,0x66,0x00},
    /* 4F 'O' */ {0x3C,0x66,0x66,0x66,0x66,0x66,0x3C,0x00},
    /* 50 'P' */ {0x3E,0x66,0x66,0x3E,0x06,0x06,0x06,0x00},
    /* 51 'Q' */ {0x3C,0x66,0x66,0x66,0x6E,0x3C,0x70,0x00},
    /* 52 'R' */ {0x3E,0x66,0x66,0x3E,0x1E,0x36,0x66,0x00},
    /* 53 'S' */ {0x3C,0x66,0x06,0x1C,0x30,0x66,0x3C,0x00},
    /* 54 'T' */ {0x7E,0x18,0x18,0x18,0x18,0x18,0x18,0x00},
    /* 55 'U' */ {0x66,0x66,0x66,0x66,0x66,0x66,0x3C,0x00},
    /* 56 'V' */ {0x66,0x66,0x66,0x66,0x66,0x3C,0x18,0x00},
    /* 57 'W' */ {0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00},
    /* 58 'X' */ {0x66,0x66,0x3C,0x18,0x3C,0x66,0x66,0x00},
    /* 59 'Y' */ {0x66,0x66,0x66,0x3C,0x18,0x18,0x18,0x00},
    /* 5A 'Z' */ {0x7E,0x60,0x30,0x18,0x0C,0x06,0x7E,0x00},
    /* 5B '[' */ {0x1E,0x06,0x06,0x06,0x06,0x06,0x1E,0x00},
    /* 5C '\\'*/ {0x03,0x06,0x0C,0x18,0x30,0x60,0x40,0x00},
    /* 5D ']' */ {0x1E,0x18,0x18,0x18,0x18,0x18,0x1E,0x00},
    /* 5E '^' */ {0x18,0x3C,0x66,0x00,0x00,0x00,0x00,0x00},
    /* 5F '_' */ {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF},
    /* 60 '`' */ {0x0C,0x18,0x30,0x00,0x00,0x00,0x00,0x00},
    /* 61 'a' */ {0x00,0x00,0x3C,0x60,0x7C,0x66,0x7C,0x00},
    /* 62 'b' */ {0x06,0x06,0x3E,0x66,0x66,0x66,0x3E,0x00},
    /* 63 'c' */ {0x00,0x00,0x3C,0x06,0x06,0x06,0x3C,0x00},
    /* 64 'd' */ {0x60,0x60,0x7C,0x66,0x66,0x66,0x7C,0x00},
    /* 65 'e' */ {0x00,0x00,0x3C,0x66,0x7E,0x06,0x3C,0x00},
    /* 66 'f' */ {0x38,0x0C,0x0C,0x3E,0x0C,0x0C,0x0C,0x00},
    /* 67 'g' */ {0x00,0x00,0x7C,0x66,0x66,0x7C,0x60,0x3C},
    /* 68 'h' */ {0x06,0x06,0x3E,0x66,0x66,0x66,0x66,0x00},
    /* 69 'i' */ {0x18,0x00,0x1E,0x18,0x18,0x18,0x7E,0x00},
    /* 6A 'j' */ {0x30,0x00,0x30,0x30,0x30,0x30,0x36,0x1C},
    /* 6B 'k' */ {0x06,0x06,0x36,0x1E,0x0E,0x1E,0x36,0x00},
    /* 6C 'l' */ {0x0E,0x0C,0x0C,0x0C,0x0C,0x0C,0x3E,0x00},
    /* 6D 'm' */ {0x00,0x00,0x37,0x7F,0x6B,0x63,0x63,0x00},
    /* 6E 'n' */ {0x00,0x00,0x3E,0x66,0x66,0x66,0x66,0x00},
    /* 6F 'o' */ {0x00,0x00,0x3C,0x66,0x66,0x66,0x3C,0x00},
    /* 70 'p' */ {0x00,0x00,0x3E,0x66,0x66,0x3E,0x06,0x06},
    /* 71 'q' */ {0x00,0x00,0x7C,0x66,0x66,0x7C,0x60,0x60},
    /* 72 'r' */ {0x00,0x00,0x3E,0x06,0x06,0x06,0x06,0x00},
    /* 73 's' */ {0x00,0x00,0x3C,0x06,0x1C,0x30,0x1E,0x00},
    /* 74 't' */ {0x08,0x0C,0x3E,0x0C,0x0C,0x0C,0x38,0x00},
    /* 75 'u' */ {0x00,0x00,0x66,0x66,0x66,0x66,0x7C,0x00},
    /* 76 'v' */ {0x00,0x00,0x66,0x66,0x66,0x3C,0x18,0x00},
    /* 77 'w' */ {0x00,0x00,0x63,0x63,0x6B,0x7F,0x36,0x00},
    /* 78 'x' */ {0x00,0x00,0x66,0x3C,0x18,0x3C,0x66,0x00},
    /* 79 'y' */ {0x00,0x00,0x66,0x66,0x66,0x7C,0x60,0x3C},
    /* 7A 'z' */ {0x00,0x00,0x7E,0x30,0x18,0x0C,0x7E,0x00},
    /* 7B '{' */ {0x38,0x0C,0x0C,0x06,0x0C,0x0C,0x38,0x00},
    /* 7C '|' */ {0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x00},
    /* 7D '}' */ {0x0E,0x18,0x18,0x30,0x18,0x18,0x0E,0x00},
    /* 7E '~' */ {0x76,0xDC,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 7F DEL */ {0xFF,0x81,0x81,0x81,0x81,0x81,0xFF,0x00},
};

/* ── GFX — RGB565 framebuffer, logical landscape 1280×720 ────────────────────── */
/*
 * Physical buffer: portrait 720×1280 (HX8394 native).
 * Coordinate transform (rotation=1, mirror_x=1):
 *   physical (px, py) = (ly, 1279 - lx)
 * For a landscape rect (lx, ly, w, h), fixing dc = lx+0..lx+w-1:
 *   py = 1279 - dc  (constant per logical column)
 *   px = ly..ly+h-1 (contiguous in memory → fast fill)
 */
#define LCD_PW   720
#define LCD_PH   1280
#define SCR_W    1280
#define SCR_H    720

#define NUM_FRAME_BUFFERS CONFIG_BSP_LCD_DPI_BUFFER_NUMS

static void   *s_frame_buffers[3] = {NULL, NULL, NULL};
static uint16_t *s_fb = NULL;
static esp_lcd_panel_handle_t s_dpi_panel = NULL;
static SemaphoreHandle_t s_refresh_sem = NULL;

static inline uint16_t rgb(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((uint16_t)(r >> 3) << 11) |
                      ((uint16_t)(g >> 2) <<  5) |
                       (uint16_t)(b >> 3));
}

static const uint16_t C_BG      = /* rgb(0x00,0x00,0x00) */ 0x0000u;
static const uint16_t C_GREEN   = /* rgb(0x00,0xFF,0x00) */ 0x07E0u;
static const uint16_t C_DKGREEN = /* rgb(0x00,0xDD,0x00) */ 0x06C0u;
static const uint16_t C_DIMGRN  = /* rgb(0x00,0xFF,0x88) */ 0x07D1u;
static const uint16_t C_TRACK   = /* rgb(0x33,0x33,0x33) */ 0x1CE7u;
static const uint16_t C_BTN     = /* rgb(0x22,0x22,0x22) */ 0x1082u;
static const uint16_t C_WHITE_K = /* rgb(0xFF,0xFF,0xFF) */ 0xFFFFu;
static const uint16_t C_BLACK_K = /* rgb(0x11,0x11,0x11) */ 0x0842u;
static const uint16_t C_BORDER  = /* rgb(0x00,0xFF,0x00) */ 0x07E0u;
static const uint16_t C_GREY    = /* rgb(0xAA,0xAA,0xAA) */ 0xAD55u;
static const uint16_t C_DIM     = /* rgb(0x00,0xAA,0x00) */ 0x0540u;

static void gfx_fill_rect(int lx, int ly, int w, int h, uint16_t col)
{
    if (w <= 0 || h <= 0 || !s_fb) return;
    if (lx < 0)        { w += lx; lx = 0; }
    if (ly < 0)        { h += ly; ly = 0; }
    if (lx + w > SCR_W) w = SCR_W - lx;
    if (ly + h > SCR_H) h = SCR_H - ly;
    if (w <= 0 || h <= 0) return;

    /* The framebuffer is stored rotated: fb row = screen column.
       Iterate over screen columns (= fb rows) outermost so each inner
       loop is a contiguous run in PSRAM — sequential writes, no cache thrash. */
    for (int dc = 0; dc < w; dc++) {
        uint16_t *p = s_fb + (size_t)(LCD_PH - 1 - (lx + dc)) * LCD_PW + ly;
        for (int k = 0; k < h; k++) p[k] = col;
    }
}

static inline void gfx_pixel(int lx, int ly, uint16_t col)
{
    if ((unsigned)lx >= (unsigned)SCR_W || (unsigned)ly >= (unsigned)SCR_H || !s_fb) return;
    s_fb[(size_t)(LCD_PH - 1 - lx) * LCD_PW + (unsigned)ly] = col;
}

static void gfx_fill_screen(uint16_t col) { gfx_fill_rect(0, 0, SCR_W, SCR_H, col); }

static void gfx_fill_circle(int cx, int cy, int r, uint16_t col)
{
    for (int dy = -r; dy <= r; dy++) {
        int dx = (int)sqrtf((float)(r*r - dy*dy));
        gfx_fill_rect(cx - dx, cy + dy, 2*dx + 1, 1, col);
    }
}

static void gfx_fill_round_rect(int x, int y, int w, int h, int r, uint16_t col)
{
    if (r <= 0) { gfx_fill_rect(x, y, w, h, col); return; }
    gfx_fill_rect(x,     y + r, w,         h - 2*r, col);
    gfx_fill_rect(x + r, y,     w - 2*r,   r,       col);
    gfx_fill_rect(x + r, y + h - r, w - 2*r, r,     col);
    for (int dy = 0; dy <= r; dy++) {
        int dx = (int)sqrtf((float)(r*r - dy*dy));
        gfx_fill_rect(x + r - dx,         y + r - dy,         dx, 1, col); /* top-left  */
        gfx_fill_rect(x + w - r,          y + r - dy,         dx, 1, col); /* top-right */
        gfx_fill_rect(x + r - dx,         y + h - r + dy - 1, dx, 1, col); /* bot-left  */
        gfx_fill_rect(x + w - r,          y + h - r + dy - 1, dx, 1, col); /* bot-right */
    }
}

static void gfx_draw_round_rect(int x, int y, int w, int h, int r, uint16_t col)
{
    gfx_fill_rect(x + r, y,         w - 2*r, 1, col);
    gfx_fill_rect(x + r, y + h - 1, w - 2*r, 1, col);
    gfx_fill_rect(x,         y + r, 1, h - 2*r, col);
    gfx_fill_rect(x + w - 1, y + r, 1, h - 2*r, col);
    int bx = 0, by = r, d = 1 - r;
    while (bx <= by) {
        gfx_pixel(x + w-1-r+bx, y + r-by,     col);
        gfx_pixel(x + w-1-r+by, y + r-bx,     col);
        gfx_pixel(x + r-bx,     y + r-by,     col);
        gfx_pixel(x + r-by,     y + r-bx,     col);
        gfx_pixel(x + r-bx,     y + h-1-r+by, col);
        gfx_pixel(x + r-by,     y + h-1-r+bx, col);
        gfx_pixel(x + w-1-r+bx, y + h-1-r+by, col);
        gfx_pixel(x + w-1-r+by, y + h-1-r+bx, col);
        if (d < 0) d += 2*bx + 3;
        else { d += 2*(bx - by) + 5; by--; }
        bx++;
    }
}

/* scale: 1→8px tall, 2→16px, 3→24px per glyph row */
static void gfx_draw_char(int x, int y, char c, uint16_t fg, uint16_t bg, int scale)
{
    int idx = (uint8_t)c - 0x20;
    if (idx < 0 || idx >= 96) idx = 0;
    const uint8_t *g = FONT8[idx];
    for (int row = 0; row < 8; row++) {
        uint8_t bits = g[row];
        for (int col = 0; col < 8; col++) {
            uint16_t color = (bits & (1u << col)) ? fg : bg;
            gfx_fill_rect(x + col*scale, y + row*scale, scale, scale, color);
        }
    }
}

static void gfx_draw_text(int x, int y, const char *str, uint16_t fg, uint16_t bg, int scale)
{
    for (; *str; str++, x += 8*scale)
        gfx_draw_char(x, y, *str, fg, bg, scale);
}

static int gfx_text_width(const char *str, int scale)
{
    return (int)strlen(str) * 8 * scale;
}

/* Flush current write buffer and wait for transfer done, then advance to next buffer. */
static void gfx_commit(void)
{
    if (!s_fb) return;
    size_t fb_size = (size_t)LCD_PW * LCD_PH * sizeof(uint16_t);
    esp_cache_msync(s_fb, fb_size, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    esp_lcd_panel_draw_bitmap(s_dpi_panel, 0, 0, LCD_PW, LCD_PH, s_fb);
    /* Wait for the frame to finish scanning out before drawing the next one.
       Using a single buffer: no buffer swap, just wait then draw in place. */
    if (s_refresh_sem) xSemaphoreTake(s_refresh_sem, pdMS_TO_TICKS(100));
}

static bool refresh_done_cb(esp_lcd_panel_handle_t panel, esp_lcd_dpi_panel_event_data_t *edata, void *ctx)
{
    SemaphoreHandle_t sem = (SemaphoreHandle_t)ctx;
    BaseType_t need_yield = pdFALSE;
    xSemaphoreGiveFromISR(sem, &need_yield);
    return need_yield == pdTRUE;
}

static void backlight_try_gpio_fallback(void)
{
    gpio_config_t bk = {
        .pin_bit_mask = 1ULL << BL_GPIO_FALLBACK,
        .mode = GPIO_MODE_OUTPUT,
    };
    if (gpio_config(&bk) == ESP_OK) {
        gpio_set_level(BL_GPIO_FALLBACK, 1);
        ESP_LOGI(TAG, "backlight fallback: GPIO%d set HIGH", (int)BL_GPIO_FALLBACK);
    } else {
        ESP_LOGW(TAG, "backlight fallback: GPIO%d config failed", (int)BL_GPIO_FALLBACK);
    }
}

static void lcd_init(void)
{
    esp_lcd_panel_io_handle_t io;
    ESP_ERROR_CHECK(bsp_display_new(NULL, &s_dpi_panel, &io));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_dpi_panel, true));

    /* Some boards need a short delay/retry before the backlight controller responds. */
    esp_err_t bl_on_ret = bsp_display_backlight_on();
    if (bl_on_ret != ESP_OK) {
        ESP_LOGW(TAG, "backlight_on failed: %s", esp_err_to_name(bl_on_ret));
    }
    esp_err_t bl_ret = ESP_FAIL;
    for (int i = 0; i < 3; i++) {
        bl_ret = bsp_display_brightness_set(100);
        if (bl_ret == ESP_OK) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (bl_ret != ESP_OK) {
        ESP_LOGW(TAG, "brightness set failed: %s", esp_err_to_name(bl_ret));
    }

    /* Board-proven backlight path: GPIO26 */
    backlight_try_gpio_fallback();

    ESP_ERROR_CHECK(esp_lcd_dpi_panel_get_frame_buffer(s_dpi_panel, 1, &s_frame_buffers[0]));
    s_fb = (uint16_t *)s_frame_buffers[0];
    ESP_LOGI(TAG, "DPI fb[0]=%p (%d×%d RGB565)", s_frame_buffers[0], LCD_PW, LCD_PH);
}

/* ── SoftAP ──────────────────────────────────────────────────────────────────── */
#define AP_SSID     "synth-32"
#define AP_PASS     "synth1234"
#define AP_CHANNEL  1
#define AP_MAX_CONN 4

/* ── Wavetables ──────────────────────────────────────────────────────────────── */
#define WAVETABLE_SIZE   256
#define SAMPLE_RATE      48000
#define AUDIO_BUF_FRAMES 512
#define WT_AMP           28000

static int16_t wt_square  [WAVETABLE_SIZE];
static int16_t wt_sawtooth[WAVETABLE_SIZE];
static int16_t wt_sine    [WAVETABLE_SIZE];
static int16_t wt_triangle[WAVETABLE_SIZE];

static const int16_t * volatile s_wavetable = wt_square;

static void wavetable_init(void)
{
    for (int i = 0; i < WAVETABLE_SIZE; i++) {
        float t = (float)i / WAVETABLE_SIZE;
        wt_square[i]   = (i < WAVETABLE_SIZE / 2) ? WT_AMP : -WT_AMP;
        wt_sawtooth[i] = (int16_t)(-WT_AMP + (int)(2 * WT_AMP * t));
        wt_sine[i]     = (int16_t)(WT_AMP * sinf(2.0f * (float)M_PI * t));
        if      (t < 0.25f) wt_triangle[i] = (int16_t)( WT_AMP * 4.0f * t);
        else if (t < 0.75f) wt_triangle[i] = (int16_t)( WT_AMP * (2.0f - 4.0f * t));
        else                wt_triangle[i] = (int16_t)( WT_AMP * (4.0f * t - 4.0f));
    }
}

/* ── Voice pool ──────────────────────────────────────────────────────────────── */
#define NUM_VOICES 8

typedef enum { ST_IDLE, ST_ATTACK, ST_DECAY, ST_SUSTAIN, ST_RELEASE } adsr_st_t;

typedef struct {
    volatile uint32_t phase_step;
    volatile bool     note_on;
    volatile int      key_id;
    uint32_t  phase_acc;
    adsr_st_t st;
    float     env_amp;
    float     rel_amp;
    uint32_t  env_cnt;
    bool      prev_note;
} voice_t;

static voice_t s_voices[NUM_VOICES];

static uint32_t freq_to_step(float hz)
{
    return (uint32_t)(hz * (float)WAVETABLE_SIZE / (float)SAMPLE_RATE * (1u << 24));
}

static int alloc_voice(int key_id, uint32_t phase_step)
{
    int best = -1;
    float best_progress = -1.0f;
    for (int i = 0; i < NUM_VOICES; i++) {
        if (s_voices[i].st == ST_IDLE) { best = i; break; }
        if (s_voices[i].st == ST_RELEASE) {
            float p = (float)s_voices[i].env_cnt;
            if (p > best_progress) { best_progress = p; best = i; }
        }
    }
    if (best < 0) best = 0;
    s_voices[best].key_id     = key_id;
    s_voices[best].phase_step = phase_step;
    s_voices[best].note_on    = true;
    return best;
}

#define SUSTAIN_LEVEL 0.7f
static volatile uint32_t s_attack_samp  = 2400;
static volatile uint32_t s_decay_samp   = 4800;
static volatile uint32_t s_release_samp = 9600;

/* ── Audio task ──────────────────────────────────────────────────────────────── */
static i2s_chan_handle_t      s_tx  = NULL;
static esp_codec_dev_handle_t s_spk = NULL;
static int16_t s_audio_buf[AUDIO_BUF_FRAMES * 2];

static void audio_task(void *arg)
{
    while (1) {
        uint32_t atk = s_attack_samp, dcy = s_decay_samp, rel = s_release_samp;
        const int16_t *wt = s_wavetable;

        for (int v = 0; v < NUM_VOICES; v++) {
            voice_t *vp = &s_voices[v];
            bool note = vp->note_on;
            if (note && !vp->prev_note) {
                vp->rel_amp = vp->env_amp; vp->st = ST_ATTACK;  vp->env_cnt = 0;
            } else if (!note && vp->prev_note && vp->st != ST_IDLE) {
                vp->rel_amp = vp->env_amp; vp->st = ST_RELEASE; vp->env_cnt = 0;
            }
            vp->prev_note = note;
        }

        for (int i = 0; i < AUDIO_BUF_FRAMES; i++) {
            int32_t mix = 0;
            for (int v = 0; v < NUM_VOICES; v++) {
                voice_t *vp = &s_voices[v];
                switch (vp->st) {
                case ST_ATTACK:
                    vp->env_amp = atk ? (float)vp->env_cnt / (float)atk : 1.0f;
                    if (vp->env_cnt >= atk) { vp->env_amp = 1.0f; vp->st = ST_DECAY; vp->env_cnt = 0; }
                    break;
                case ST_DECAY:
                    vp->env_amp = 1.0f - (1.0f - SUSTAIN_LEVEL) * (float)vp->env_cnt / (float)(dcy ? dcy : 1);
                    if (vp->env_cnt >= dcy) { vp->env_amp = SUSTAIN_LEVEL; vp->st = ST_SUSTAIN; }
                    break;
                case ST_SUSTAIN:
                    vp->env_amp = SUSTAIN_LEVEL;
                    break;
                case ST_RELEASE:
                    vp->env_amp = vp->rel_amp * (1.0f - (float)vp->env_cnt / (float)(rel ? rel : 1));
                    if (vp->env_cnt >= rel) { vp->env_amp = 0.0f; vp->st = ST_IDLE; vp->key_id = -1; }
                    break;
                default:
                    vp->env_amp = 0.0f;
                    break;
                }
                vp->env_cnt++;
                if (vp->st != ST_IDLE)
                    mix += (int32_t)(wt[(vp->phase_acc >> 24) & 0xFF] * vp->env_amp);
                vp->phase_acc += vp->phase_step;
            }
            if (mix >  32767) mix =  32767;
            if (mix < -32768) mix = -32768;
            s_audio_buf[i * 2 + 0] = s_audio_buf[i * 2 + 1] = (int16_t)mix;
        }
        size_t written;
        i2s_channel_write(s_tx, s_audio_buf, sizeof(s_audio_buf), &written, portMAX_DELAY);
    }
}

/* ── I2C ─────────────────────────────────────────────────────────────────────── */
static void i2c_init(void)
{
    /* Recover a stuck bus before handing it to the BSP */
    gpio_config_t in = { .pin_bit_mask = (1ULL << PIN_I2C_SDA) | (1ULL << PIN_I2C_SCL),
                         .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE };
    gpio_config(&in);
    vTaskDelay(pdMS_TO_TICKS(2));
    if (!gpio_get_level(PIN_I2C_SDA) || !gpio_get_level(PIN_I2C_SCL)) {
        ESP_LOGW(TAG, "I2C bus stuck — recovering with 9 SCL pulses");
        gpio_set_direction(PIN_I2C_SCL, GPIO_MODE_OUTPUT_OD);
        gpio_set_direction(PIN_I2C_SDA, GPIO_MODE_OUTPUT_OD);
        gpio_set_level(PIN_I2C_SDA, 1);
        for (int i = 0; i < 9; i++) {
            gpio_set_level(PIN_I2C_SCL, 0); esp_rom_delay_us(5);
            gpio_set_level(PIN_I2C_SCL, 1); esp_rom_delay_us(5);
        }
        gpio_set_level(PIN_I2C_SDA, 0); esp_rom_delay_us(5);
        gpio_set_level(PIN_I2C_SCL, 1); esp_rom_delay_us(5);
        gpio_set_level(PIN_I2C_SDA, 1); esp_rom_delay_us(5);
    }
    ESP_ERROR_CHECK(bsp_i2c_init());
}

/* ── Audio init ──────────────────────────────────────────────────────────────── */
static void audio_init(void)
{
    gpio_config_t pa_cfg = {};
    pa_cfg.pin_bit_mask = 1ULL << GPIO_PA;
    pa_cfg.mode = GPIO_MODE_OUTPUT;
    gpio_config(&pa_cfg);
    gpio_set_level(GPIO_PA, 1);

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &s_tx, NULL));

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_MCK_IO, .bclk = I2S_BCK_IO, .ws = I2S_WS_IO,
            .dout = I2S_DO_IO,  .din  = I2S_DI_IO,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    std_cfg.clk_cfg.mclk_multiple = (i2s_mclk_multiple_t)MCLK_MULTIPLE;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx));

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port       = I2C_PORT,
        .addr       = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = bsp_i2c_get_handle(),
    };
    const audio_codec_ctrl_if_t *i2c_ctrl = audio_codec_new_i2c_ctrl(&i2c_cfg);
    assert(i2c_ctrl);

    audio_codec_i2s_cfg_t i2s_cfg = {};
    i2s_cfg.port      = I2S_PORT;
    i2s_cfg.tx_handle = s_tx;
    i2s_cfg.rx_handle = NULL;
    const audio_codec_data_if_t *i2s_data = audio_codec_new_i2s_data(&i2s_cfg);
    assert(i2s_data);

    es8311_codec_cfg_t es_cfg = {};
    es_cfg.ctrl_if     = i2c_ctrl;
    es_cfg.gpio_if     = audio_codec_new_gpio();
    es_cfg.codec_mode  = (esp_codec_dec_work_mode_t)ESP_CODEC_DEV_TYPE_OUT;
    es_cfg.pa_pin      = GPIO_PA;
    es_cfg.pa_reverted = false;
    es_cfg.master_mode = false;
    es_cfg.use_mclk    = true;
    es_cfg.hw_gain.pa_voltage        = 5.0f;
    es_cfg.hw_gain.codec_dac_voltage = 3.3f;
    const audio_codec_if_t *es_dev = es8311_codec_new(&es_cfg);
    assert(es_dev);

    esp_codec_dev_cfg_t dev_cfg = {};
    dev_cfg.dev_type = ESP_CODEC_DEV_TYPE_OUT;
    dev_cfg.codec_if = es_dev;
    dev_cfg.data_if  = i2s_data;
    s_spk = esp_codec_dev_new(&dev_cfg);
    assert(s_spk);
    esp_codec_dev_sample_info_t fs = {};
    fs.sample_rate     = SAMPLE_RATE;
    fs.channel         = 2;
    fs.bits_per_sample = 16;
    ESP_ERROR_CHECK(esp_codec_dev_open(s_spk, &fs));
    ESP_ERROR_CHECK(esp_codec_dev_set_out_vol(s_spk, 20));

    for (int i = 0; i < NUM_VOICES; i++) { s_voices[i].st = ST_IDLE; s_voices[i].key_id = -1; }
    xTaskCreatePinnedToCore(audio_task, "audio", 4096, NULL, 20, NULL, 1);
}

/* ── Touch (GT911 via esp_lcd_touch) ─────────────────────────────────────────── */
typedef struct { int16_t x, y; } tp_pt_t;

static esp_lcd_touch_handle_t s_touch = NULL;

static void touch_init(void)
{
    bsp_display_cfg_t cfg = {};
    ESP_ERROR_CHECK(bsp_touch_new(&cfg, &s_touch));
    ESP_LOGI(TAG, "GT911 ready via BSP");
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

/* panel-native (720×1280 portrait) → screen (1280×720 landscape) */
static inline void touch_transform(tp_pt_t *pt)
{
    int16_t px = pt->x, py = pt->y;
    pt->x = (int16_t)(1279 - py);
    pt->y = px;
}

/* ── Embedded web UI ─────────────────────────────────────────────────────────── */
static const char INDEX_HTML[] =
"<!DOCTYPE html><html lang='en'><head>"
"<meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>synth-32</title>"
"<style>"
"body{font-family:monospace;background:#111;color:#eee;margin:2rem;}"
"h1{color:#0f0;}"
"button{background:#222;color:#0f0;border:1px solid #0f0;padding:.5rem 1.2rem;margin:.3rem;cursor:pointer;font-size:1rem;}"
"button:hover{background:#0f0;color:#000;}"
"label{display:block;margin-top:1rem;}"
"input[type=range]{width:100%;max-width:300px;}"
"#log{background:#000;padding:1rem;height:200px;overflow-y:auto;border:1px solid #333;margin-top:1rem;white-space:pre;font-size:.85rem;}"
"</style></head><body>"
"<h1>synth-32</h1>"
"<label>Volume<input type='range' min='0' max='100' value='20' "
"oninput=\"cmd('vol/'+this.value)\"></label>"
"<div id='log'></div>"
"<script>"
"const log=document.getElementById('log');"
"function append(s){log.textContent+=s+'\\n';log.scrollTop=log.scrollHeight;}"
"async function cmd(path){"
"try{const r=await fetch('/cmd/'+path,{method:'POST'});append('< '+(await r.text()));}"
"catch(e){append('err: '+e);}}"
"</script></body></html>";

static esp_err_t index_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t cmd_post_handler(httpd_req_t *req)
{
    const char *name = req->uri + strlen("/cmd/");
    static char buf[32];
    if (strncmp(name, "vol/", 4) == 0) {
        int vol = atoi(name + 4);
        if (vol < 0) vol = 0;
        if (vol > 100) vol = 100;
        if (s_spk) esp_codec_dev_set_out_vol(s_spk, vol);
        snprintf(buf, sizeof(buf), "vol %d", vol);
    } else {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "unknown command");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

static void start_webserver(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    httpd_handle_t server = NULL;
    ESP_ERROR_CHECK(httpd_start(&server, &cfg));
    static const httpd_uri_t idx_uri = { .uri = "/",     .method = HTTP_GET,  .handler = index_get_handler };
    static const httpd_uri_t cmd_uri = { .uri = "/cmd/*",.method = HTTP_POST, .handler = cmd_post_handler  };
    httpd_register_uri_handler(server, &idx_uri);
    httpd_register_uri_handler(server, &cmd_uri);
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *e = (wifi_event_ap_staconnected_t *)data;
        ESP_LOGI(TAG, "station joined: " MACSTR, MAC2STR(e->mac));
    }
}

static void wifi_init_softap(void)
{
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL));
    wifi_config_t wifi_cfg = {};
    memcpy(wifi_cfg.ap.ssid, AP_SSID, strlen(AP_SSID));
    wifi_cfg.ap.ssid_len       = strlen(AP_SSID);
    wifi_cfg.ap.channel        = AP_CHANNEL;
    memcpy(wifi_cfg.ap.password, AP_PASS, strlen(AP_PASS));
    wifi_cfg.ap.max_connection = AP_MAX_CONN;
    wifi_cfg.ap.authmode       = WIFI_AUTH_WPA2_PSK;
    wifi_cfg.ap.pmf_cfg.required = false;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "SoftAP up — SSID:%s  http://192.168.4.1", AP_SSID);
}

/* ── UI layout constants ─────────────────────────────────────────────────────── */
/* Screen: 1280×720 landscape */

/* Sliders — 4 rows */
#define SL_X    90
#define SL_W    1000
#define SL_H    44
static const int   sl_y[4]    = { 10, 90, 170, 250 };
static const char *sl_name[4] = { "VOL", "ATK", "DEC", "REL" };
static int         sl_val[4]  = { 20, 12, 17, 24 };

/* Wave buttons */
#define WAVE_Y   305
#define WAVE_H    52
#define WAVE_W   285
static const int   wave_x[4]    = { 40, 340, 640, 940 };
static const char *wave_name[4] = { "SQR", "SAW", "SIN", "TRI" };
static int         s_wave_sel   = 0;

/* Octave row */
#define OCT_Y   368

/* Piano */
#define KY        395
#define KH        320
#define WKW        81
#define BKW        50
#define BKH       180
#define OCT_BTN_W  68
#define KX        (OCT_BTN_W + 5)

static const int wk_semi[7] = { 0, 2, 4, 5, 7,  9, 11 };
static const int bk_semi[5] = { 1, 3, 6, 8, 10 };
static const int bk_cx[5]   = { 54, 135, 297, 378, 459 };

#define MAX_KEYS 24
typedef struct { int16_t x, y, w, h; int key_data; bool is_black; bool pressed; } key_reg_t;
static key_reg_t s_keys[MAX_KEYS];
static int       s_key_cnt = 0;

typedef struct {
    int16_t y;
    int16_t white_h;
    int16_t oct_btn_w;
} piano_view_t;

static const piano_view_t s_main_piano = {
    .y = KY,
    .white_h = KH,
    .oct_btn_w = OCT_BTN_W,
};

static const float k_semi_c4[12] = {
    261.63f, 277.18f, 293.66f, 311.13f, 329.63f, 349.23f,
    369.99f, 392.00f, 415.30f, 440.00f, 466.16f, 493.88f
};

static float semitone_to_freq(int semi, int octave)
{
    float f = k_semi_c4[semi & 0x0F];
    for (int d = octave - 4; d > 0; d--) f *= 2.0f;
    for (int d = octave - 4; d < 0; d++) f *= 0.5f;
    return f;
}

static uint32_t slider_to_ms(int v) { return (uint32_t)(10 + (v * v) / 3); }

static int s_octave = 4;

/* ── Drawing ─────────────────────────────────────────────────────────────────── */
static void draw_slider_row(int idx)
{
    int y = sl_y[idx];

    gfx_draw_text(10, y + 14, sl_name[idx], C_DIMGRN, C_BG, 2);

    gfx_fill_rect(SL_X, y + 2, SL_W, SL_H - 4, C_TRACK);

    int iw = sl_val[idx] * SL_W / 100;
    if (iw > 0) gfx_fill_rect(SL_X, y + 2, iw, SL_H - 4, C_GREEN);

    gfx_fill_circle(SL_X + iw, y + SL_H / 2, SL_H / 2, C_DKGREEN);

    gfx_fill_rect(1100, y, 178, SL_H + 4, C_BG);
    char buf[16];
    if (idx == 0) {
        snprintf(buf, sizeof(buf), "%d%%", sl_val[0]);
    } else {
        uint32_t ms = slider_to_ms(sl_val[idx]);
        if (ms < 1000) snprintf(buf, sizeof(buf), "%ums", (unsigned)ms);
        else           snprintf(buf, sizeof(buf), "%.1fs", ms / 1000.0f);
    }
    gfx_draw_text(1104, y + 14, buf, C_GREEN, C_BG, 2);
}

static void draw_wave_buttons(void)
{
    for (int i = 0; i < 4; i++) {
        bool     sel = (i == s_wave_sel);
        uint16_t bg  = sel ? C_DKGREEN : C_BTN;
        uint16_t fg  = sel ? rgb(0,0,0) : C_GREEN;
        gfx_fill_round_rect(wave_x[i], WAVE_Y, WAVE_W, WAVE_H, 8, bg);
        gfx_draw_round_rect(wave_x[i], WAVE_Y, WAVE_W, WAVE_H, 8, C_GREEN);
        int tw = gfx_text_width(wave_name[i], 3);
        int tx = wave_x[i] + (WAVE_W - tw) / 2;
        int ty = WAVE_Y    + (WAVE_H - 24) / 2;
        gfx_draw_text(tx, ty, wave_name[i], fg, bg, 3);
    }
}

static void draw_oct_label(void)
{
    gfx_fill_rect(10, OCT_Y, 130, 20, C_BG);
    char buf[10];
    snprintf(buf, sizeof(buf), "OCT %d", s_octave);
    gfx_draw_text(10, OCT_Y, buf, C_GREEN, C_BG, 2);
}

static void draw_key_region(const key_reg_t *r)
{
    uint16_t col = r->pressed ? C_GREEN : (r->is_black ? C_BLACK_K : C_WHITE_K);
    gfx_fill_round_rect(r->x, r->y, r->w, r->h, 4, col);
    gfx_draw_round_rect(r->x, r->y, r->w, r->h, 4, r->is_black ? C_GREY : C_BORDER);
}

static void draw_key(int k)
{
    draw_key_region(&s_keys[k]);
}

static void draw_oct_btn(int x, int y, int w, int h, const char *txt)
{
    gfx_fill_round_rect(x, y, w, h, 6, C_BTN);
    gfx_draw_round_rect(x, y, w, h, 6, C_GREEN);
    int tw = gfx_text_width(txt, 2);
    int tx = x + (w - tw) / 2;
    int ty = y + (h - 16) / 2;
    gfx_draw_text(tx, ty, txt, C_GREEN, C_BTN, 2);
}

static int oct_dec_btn_x(const piano_view_t *p) { return 2; }
static int oct_inc_btn_x(const piano_view_t *p) { return KX + 14 * WKW + 3; }

static bool point_in_oct_btn(const piano_view_t *p, int x, int y)
{
    int dec_x = oct_dec_btn_x(p);
    int inc_x = oct_inc_btn_x(p);
    bool in_y = (y >= p->y && y < p->y + p->white_h);
    if (!in_y) return false;
    return (x >= dec_x && x < dec_x + p->oct_btn_w) ||
           (x >= inc_x && x < inc_x + p->oct_btn_w);
}

/* Reusable piano painter for any component that provides key regions. */
static void draw_piano_component(const piano_view_t *p, const key_reg_t *keys, int key_cnt, bool draw_oct_buttons)
{
    for (int k = 0; k < key_cnt; k++) {
        if (!keys[k].is_black) draw_key_region(&keys[k]);
    }
    for (int k = 0; k < key_cnt; k++) {
        if (keys[k].is_black) draw_key_region(&keys[k]);
    }

    if (draw_oct_buttons) {
        draw_oct_btn(oct_dec_btn_x(p), p->y, p->oct_btn_w, p->white_h, "<<");
        draw_oct_btn(oct_inc_btn_x(p), p->y, p->oct_btn_w, p->white_h, ">>");
    }
}

static bool rect_intersects(const key_reg_t *a, const key_reg_t *b)
{
    int ax2 = a->x + a->w;
    int ay2 = a->y + a->h;
    int bx2 = b->x + b->w;
    int by2 = b->y + b->h;
    return (a->x < bx2 && ax2 > b->x && a->y < by2 && ay2 > b->y);
}

static void redraw_overlapping_black_keys(int white_key_idx)
{
    key_reg_t *w = &s_keys[white_key_idx];
    if (w->is_black) return;
    for (int k = 0; k < s_key_cnt; k++) {
        if (!s_keys[k].is_black) continue;
        if (rect_intersects(w, &s_keys[k])) draw_key(k);
    }
}

static void draw_full(void)
{
    gfx_fill_screen(C_BG);
    for (int i = 0; i < 4; i++) draw_slider_row(i);
    draw_wave_buttons();
    draw_oct_label();

    gfx_draw_text(1150, OCT_Y + 3, "192.168.4.1", C_DIM, C_BG, 1);

    draw_piano_component(&s_main_piano, s_keys, s_key_cnt, true);

    gfx_commit();
}

/* ── Key region setup ────────────────────────────────────────────────────────── */
static void setup_key_regions(void)
{
    s_key_cnt = 0;
    for (int oct = 0; oct < 2; oct++) {
        int ox = KX + oct * 7 * WKW;
        for (int k = 0; k < 7; k++) {
            key_reg_t r = {};
            r.x = (int16_t)(ox + k * WKW); r.y = KY;
            r.w = WKW - 1;                 r.h = KH;
            r.key_data = (oct << 8) | wk_semi[k];
            r.is_black = false;
            s_keys[s_key_cnt++] = r;
        }
    }
    for (int oct = 0; oct < 2; oct++) {
        int ox = KX + oct * 7 * WKW;
        for (int k = 0; k < 5; k++) {
            key_reg_t r = {};
            r.x = (int16_t)(ox + bk_cx[k] - BKW/2); r.y = KY;
            r.w = BKW;                                r.h = BKH;
            r.key_data = (oct << 8) | bk_semi[k];
            r.is_black = true;
            s_keys[s_key_cnt++] = r;
        }
    }
}

/* ── ADSR sync ───────────────────────────────────────────────────────────────── */
static void sync_adsr(void)
{
    for (int p = 0; p < 3; p++) {
        uint32_t samp = slider_to_ms(sl_val[1 + p]) * (SAMPLE_RATE / 1000);
        if      (p == 0) s_attack_samp  = samp;
        else if (p == 1) s_decay_samp   = samp;
        else             s_release_samp = samp;
    }
}

/* ── Touch / UI event handling ───────────────────────────────────────────────── */
static int  s_drag_slider    = -1;
static bool s_touch_was_down = false;

static void handle_down(int sx, int sy)
{
    for (int i = 0; i < 4; i++) {
        int y = sl_y[i];
        if (sx >= SL_X && sx < SL_X + SL_W && sy >= y && sy < y + SL_H + 4) {
            sl_val[i] = (sx - SL_X) * 100 / SL_W;
            if (sl_val[i] < 0) sl_val[i] = 0;
            if (sl_val[i] > 100) sl_val[i] = 100;
            s_drag_slider = i;
            if (i == 0 && s_spk) esp_codec_dev_set_out_vol(s_spk, sl_val[0]);
            else sync_adsr();
            draw_slider_row(i);
            return;
        }
    }
    for (int i = 0; i < 4; i++) {
        if (sx >= wave_x[i] && sx < wave_x[i] + WAVE_W &&
            sy >= WAVE_Y    && sy < WAVE_Y + WAVE_H) {
            if (s_wave_sel != i) {
                s_wave_sel = i;
                static const int16_t *const wforms[4] = {
                    wt_square, wt_sawtooth, wt_sine, wt_triangle
                };
                s_wavetable = wforms[i];
                draw_wave_buttons();
            }
            return;
        }
    }
    int dec_x = oct_dec_btn_x(&s_main_piano);
    if (sx >= dec_x && sx < dec_x + s_main_piano.oct_btn_w &&
        sy >= s_main_piano.y && sy < s_main_piano.y + s_main_piano.white_h) {
        if (s_octave > 1) { s_octave--; draw_oct_label(); }
        return;
    }
    int inc_x = oct_inc_btn_x(&s_main_piano);
    if (sx >= inc_x && sx < inc_x + s_main_piano.oct_btn_w &&
        sy >= s_main_piano.y && sy < s_main_piano.y + s_main_piano.white_h) {
        if (s_octave < 7) { s_octave++; draw_oct_label(); }
        return;
    }
}

static void handle_drag(int sx)
{
    if (s_drag_slider < 0) return;
    int i = s_drag_slider;
    int v = (sx - SL_X) * 100 / SL_W;
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    if (v == sl_val[i]) return;
    sl_val[i] = v;
    if (i == 0 && s_spk) esp_codec_dev_set_out_vol(s_spk, sl_val[0]);
    else sync_adsr();
    draw_slider_row(i);
}

/* Piano multi-touch */
static bool    s_key_held[MAX_KEYS]    = {};
static uint8_t s_key_off_cnt[MAX_KEYS] = {};
#define OFF_THRESH 3

static void handle_piano(tp_pt_t *pts, int cnt)
{
    bool cur[MAX_KEYS] = {};
    for (int p = 0; p < cnt; p++) {
        int bk_hit = -1;
        for (int k = 0; k < s_key_cnt; k++) {
            key_reg_t *r = &s_keys[k];
            if (!r->is_black) continue;
            if (pts[p].x >= r->x && pts[p].x < r->x + r->w &&
                pts[p].y >= r->y && pts[p].y < r->y + r->h) {
                cur[k] = true; bk_hit = k;
            }
        }
        if (bk_hit >= 0) continue;
        for (int k = 0; k < s_key_cnt; k++) {
            key_reg_t *r = &s_keys[k];
            if (r->is_black) continue;
            if (pts[p].x >= r->x && pts[p].x < r->x + r->w &&
                pts[p].y >= r->y && pts[p].y < r->y + r->h)
                cur[k] = true;
        }
    }

    for (int k = 0; k < s_key_cnt; k++) {
        if (cur[k]) {
            s_key_off_cnt[k] = 0;
            if (!s_key_held[k]) {
                float hz = semitone_to_freq(s_keys[k].key_data & 0xFF,
                                            s_octave + (s_keys[k].key_data >> 8));
                alloc_voice(s_keys[k].key_data, freq_to_step(hz));
                s_key_held[k]     = true;
                s_keys[k].pressed = true;
                draw_key(k);
                redraw_overlapping_black_keys(k);
            }
        } else if (s_key_held[k]) {
            if (++s_key_off_cnt[k] >= OFF_THRESH) {
                for (int v = 0; v < NUM_VOICES; v++) {
                    if (s_voices[v].key_id == s_keys[k].key_data && s_voices[v].note_on) {
                        s_voices[v].note_on = false; break;
                    }
                }
                s_key_held[k]     = false;
                s_key_off_cnt[k]  = 0;
                s_keys[k].pressed = false;
                draw_key(k);
                redraw_overlapping_black_keys(k);
            }
        }
    }
}

/* ── UI task (core 0) ────────────────────────────────────────────────────────── */
static void ui_task(void *arg)
{
    lcd_init();

    /* Register refresh-done callback so gfx_commit can be paced to vsync */
    s_refresh_sem = xSemaphoreCreateBinary();
    esp_lcd_dpi_panel_event_callbacks_t dpi_cbs = {
        .on_refresh_done = refresh_done_cb,
    };
    ESP_ERROR_CHECK(esp_lcd_dpi_panel_register_event_callbacks(s_dpi_panel, &dpi_cbs, s_refresh_sem));

    setup_key_regions();
    sync_adsr();
    draw_full();

    while (1) {
        tp_pt_t pts[5];
        int cnt = touch_get_points(pts, 5);
        for (int i = 0; i < cnt; i++) touch_transform(&pts[i]);

        tp_pt_t ui_pts[5], pi_pts[5];
        int ui_cnt = 0, pi_cnt = 0;
        for (int i = 0; i < cnt; i++) {
            if (point_in_oct_btn(&s_main_piano, pts[i].x, pts[i].y) || pts[i].y < KY)
                ui_pts[ui_cnt++] = pts[i];
            else
                pi_pts[pi_cnt++] = pts[i];
        }

        handle_piano(pi_pts, pi_cnt);

        if (ui_cnt > 0) {
            int sx = ui_pts[0].x, sy = ui_pts[0].y;
            if (!s_touch_was_down) handle_down(sx, sy);
            else                   handle_drag(sx);
            s_touch_was_down = true;
        } else {
            if (s_touch_was_down) s_drag_slider = -1;
            s_touch_was_down = false;
        }

        gfx_commit();
    }
}

/* ── Entry point ─────────────────────────────────────────────────────────────── */
void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    i2c_init();
    touch_init();
    wavetable_init();
    audio_init();
    wifi_init_softap();
    start_webserver();

    xTaskCreatePinnedToCore(ui_task, "ui", 16384, NULL, 5, NULL, 0);

    ESP_LOGI(TAG, "ready");
}
