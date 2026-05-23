#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_http_server.h"
#include "esp_mac.h"
#include "esp_lcd_touch.h"
#include "driver/i2s_std.h"
#include "driver/ledc.h"
#include "esp_codec_dev_defaults.h"
#include "es8311_codec.h"

/* Waveshare P4 BSP — display (HX8394 MIPI-DSI), touch (GT911) */
#include "bsp/esp32_p4_platform.h"

/* LVGL */
#include "lvgl.h"
#include "esp_lv_adapter_display.h"

static const char *TAG = "synth32";

/* ── SoftAP config ────────────────────────────────────────────────────────── */
#define AP_SSID     "synth-32"
#define AP_PASS     "synth1234"
#define AP_CHANNEL  1
#define AP_MAX_CONN 4

/* ── Wavetables ───────────────────────────────────────────────────────────── */
#define WAVETABLE_SIZE   256
#define SAMPLE_RATE      48000
#define AUDIO_BUF_FRAMES 512
#define WT_AMP           28000

static int16_t wt_square  [WAVETABLE_SIZE];
static int16_t wt_sawtooth[WAVETABLE_SIZE];
static int16_t wt_sine    [WAVETABLE_SIZE];
static int16_t wt_triangle[WAVETABLE_SIZE];

/* audio task reads this pointer; UI writes it (32-bit aligned → atomic on P4) */
static const int16_t * volatile s_wavetable = wt_square;

/* ── Polyphonic voice pool ────────────────────────────────────────────────── */
#define NUM_VOICES 8

typedef enum { ST_IDLE, ST_ATTACK, ST_DECAY, ST_SUSTAIN, ST_RELEASE } adsr_st_t;

typedef struct {
    /* written by UI task; 32-bit aligned fields are atomic on P4 */
    volatile uint32_t phase_step;
    volatile bool     note_on;
    volatile int      key_id;   /* (oct_off<<8)|semi, -1 = free */
    /* owned exclusively by audio task */
    uint32_t  phase_acc;
    adsr_st_t st;
    float     env_amp;
    float     rel_amp;
    uint32_t  env_cnt;
    bool      prev_note;
} voice_t;

static voice_t s_voices[NUM_VOICES];

static void wavetable_init(void)
{
    for (int i = 0; i < WAVETABLE_SIZE; i++) {
        float t = (float)i / WAVETABLE_SIZE; /* 0..1 */

        /* square */
        wt_square[i] = (i < WAVETABLE_SIZE / 2) ? WT_AMP : -WT_AMP;

        /* sawtooth: -AMP → +AMP */
        wt_sawtooth[i] = (int16_t)(-WT_AMP + (int)(2 * WT_AMP * t));

        /* sine */
        wt_sine[i] = (int16_t)(WT_AMP * sinf(2.0f * (float)M_PI * t));

        /* triangle: /\ shape, peak at quarter-wave */
        if      (t < 0.25f) wt_triangle[i] = (int16_t)( WT_AMP * 4.0f *  t);
        else if (t < 0.75f) wt_triangle[i] = (int16_t)( WT_AMP * (2.0f - 4.0f * t));
        else                wt_triangle[i] = (int16_t)( WT_AMP * (4.0f * t - 4.0f));
    }
}

static uint32_t freq_to_step(float hz)
{
    float step = hz * (float)WAVETABLE_SIZE / (float)SAMPLE_RATE;
    return (uint32_t)(step * (1 << 24));
}

/* Allocate a voice for key_id; returns voice index.
   Priority: idle → furthest-in-release → voice 0 (steal). */
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
    if (best < 0) best = 0;  /* steal oldest */

    s_voices[best].key_id     = key_id;
    s_voices[best].phase_step = phase_step;
    s_voices[best].note_on    = true;
    return best;
}

/* ── ADSR envelope ────────────────────────────────────────────────────────── */
#define SUSTAIN_LEVEL 0.7f

/* Global ADSR timings (samples); shared across all voices. */
static volatile uint32_t s_attack_samp  = 2400;  /* 50 ms @ 48 kHz */
static volatile uint32_t s_decay_samp   = 4800;  /* 100 ms */
static volatile uint32_t s_release_samp = 9600;  /* 200 ms */

/* ── Audio (direct I2S + ES8311, Waveshare example style) ────────────────── */
#define I2S_MCK_IO      GPIO_NUM_13
#define I2S_BCK_IO      GPIO_NUM_12
#define I2S_WS_IO       GPIO_NUM_10
#define I2S_DO_IO       GPIO_NUM_9
#define I2S_DI_IO       GPIO_NUM_11
#define GPIO_PA         GPIO_NUM_53
#define MCLK_MULTIPLE   256   /* MCLK = SAMPLE_RATE * 256 = 12.288 MHz */

static i2s_chan_handle_t    s_tx  = NULL;
static esp_codec_dev_handle_t s_spk = NULL;

/* Interleaved stereo 16-bit buffer */
static int16_t s_audio_buf[AUDIO_BUF_FRAMES * 2];

static void backlight_init(void)
{
    ledc_timer_config_t timer = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .timer_num       = LEDC_TIMER_1,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .freq_hz         = 5000,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer));

    ledc_channel_config_t ch = {
        .gpio_num   = 26,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = LEDC_CHANNEL_1,
        .timer_sel  = LEDC_TIMER_1,
        .duty       = 1023,
        .hpoint     = 0,
        .intr_type  = LEDC_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ch));
    ESP_LOGI(TAG, "backlight on GPIO26 PWM at 100%%");
}

static void audio_task(void *arg)
{
    while (1) {
        uint32_t atk  = s_attack_samp;
        uint32_t dcy  = s_decay_samp;
        uint32_t rel  = s_release_samp;
        const int16_t *wt = s_wavetable;

        /* Detect note_on edges once per buffer (~10 ms latency, imperceptible) */
        for (int v = 0; v < NUM_VOICES; v++) {
            voice_t *vp = &s_voices[v];
            bool note = vp->note_on;
            if (note && !vp->prev_note) {
                vp->rel_amp = vp->env_amp;
                vp->st      = ST_ATTACK;
                vp->env_cnt = 0;
            } else if (!note && vp->prev_note && vp->st != ST_IDLE) {
                vp->rel_amp = vp->env_amp;
                vp->st      = ST_RELEASE;
                vp->env_cnt = 0;
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

                if (vp->st != ST_IDLE) {
                    uint8_t widx = (vp->phase_acc >> 24) & 0xFF;
                    mix += (int32_t)(wt[widx] * vp->env_amp);
                }
                vp->phase_acc += vp->phase_step;
            }

            /* mix down: clamp to int16 range */
            if (mix >  32767) mix =  32767;
            if (mix < -32768) mix = -32768;
            s_audio_buf[i * 2 + 0] = (int16_t)mix;
            s_audio_buf[i * 2 + 1] = (int16_t)mix;
        }

        size_t written;
        i2s_channel_write(s_tx, s_audio_buf, sizeof(s_audio_buf), &written, portMAX_DELAY);
    }
}

static void audio_init(void)
{
    /* Power amplifier enable */
    gpio_config_t pa_cfg = {
        .pin_bit_mask = (1ULL << GPIO_PA),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&pa_cfg);
    gpio_set_level(GPIO_PA, 1);

    /* I2S channel */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(CONFIG_BSP_I2S_NUM, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &s_tx, NULL));

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_MCK_IO,
            .bclk = I2S_BCK_IO,
            .ws   = I2S_WS_IO,
            .dout = I2S_DO_IO,
            .din  = I2S_DI_IO,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    std_cfg.clk_cfg.mclk_multiple = MCLK_MULTIPLE;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx));

    /* ES8311 codec — use esp_codec_dev with the BSP's i2c_master bus handle */
    audio_codec_i2c_cfg_t i2c_cfg = {
        .port       = BSP_I2C_NUM,
        .addr       = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = bsp_i2c_get_handle(),
    };
    const audio_codec_ctrl_if_t *i2c_ctrl = audio_codec_new_i2c_ctrl(&i2c_cfg);
    assert(i2c_ctrl);

    audio_codec_i2s_cfg_t i2s_cfg = {
        .port      = CONFIG_BSP_I2S_NUM,
        .tx_handle = s_tx,
        .rx_handle = NULL,
    };
    const audio_codec_data_if_t *i2s_data = audio_codec_new_i2s_data(&i2s_cfg);
    assert(i2s_data);

    es8311_codec_cfg_t es_cfg = {
        .ctrl_if     = i2c_ctrl,
        .gpio_if     = audio_codec_new_gpio(),
        .codec_mode  = ESP_CODEC_DEV_TYPE_OUT,
        .pa_pin      = BSP_POWER_AMP_IO,
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk    = true,
        .hw_gain     = { .pa_voltage = 5.0f, .codec_dac_voltage = 3.3f },
    };
    const audio_codec_if_t *es_dev = es8311_codec_new(&es_cfg);
    assert(es_dev);

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = es_dev,
        .data_if  = i2s_data,
    };
    s_spk = esp_codec_dev_new(&dev_cfg);
    assert(s_spk);
    ESP_ERROR_CHECK(esp_codec_dev_open(s_spk, &(esp_codec_dev_sample_info_t){
        .sample_rate     = SAMPLE_RATE,
        .channel         = 2,
        .bits_per_sample = 16,
    }));
    ESP_ERROR_CHECK(esp_codec_dev_set_out_vol(s_spk, 20));

    ESP_LOGI(TAG, "audio ready");

    for (int i = 0; i < NUM_VOICES; i++) {
        s_voices[i].st     = ST_IDLE;
        s_voices[i].key_id = -1;
    }
    xTaskCreatePinnedToCore(audio_task, "audio", 4096, NULL, 20, NULL, 1);
}

/* ── Embedded web UI ──────────────────────────────────────────────────────── */
static const char INDEX_HTML[] =
"<!DOCTYPE html><html lang='en'><head>"
"<meta charset='UTF-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>synth-32</title>"
"<style>"
"  body{font-family:monospace;background:#111;color:#eee;margin:2rem;}"
"  h1{color:#0f0;}"
"  button{background:#222;color:#0f0;border:1px solid #0f0;"
"         padding:.5rem 1.2rem;margin:.3rem;cursor:pointer;font-size:1rem;}"
"  button:hover{background:#0f0;color:#000;}"
"  label{display:block;margin-top:1rem;}"
"  input[type=range]{width:100%;max-width:300px;}"
"  #log{background:#000;padding:1rem;height:200px;overflow-y:auto;"
"       border:1px solid #333;margin-top:1rem;white-space:pre;font-size:.85rem;}"
"</style>"
"</head><body>"
"<h1>synth-32</h1>"
"<label>Frequency (Hz)"
"  <input type='range' min='20' max='2000' value='120' "
"         oninput=\"this.nextElementSibling.textContent=this.value+'Hz';cmd('freq/'+this.value)\">"
"  <span>120Hz</span>"
"</label>"
"<label>Volume"
"  <input type='range' min='0' max='100' value='70' "
"         oninput=\"cmd('vol/'+this.value)\">"
"</label>"
"<div id='log'></div>"
"<script>"
"const log=document.getElementById('log');"
"function append(s){log.textContent+=s+'\\n';log.scrollTop=log.scrollHeight;}"
"async function cmd(path){"
"  try{"
"    const r=await fetch('/cmd/'+path,{method:'POST'});"
"    const t=await r.text();"
"    append('< '+t);"
"  }catch(e){append('error: '+e);}"
"}"
"</script>"
"</body></html>";

/* ── HTTP handlers ────────────────────────────────────────────────────────── */
static esp_err_t index_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t cmd_post_handler(httpd_req_t *req)
{
    const char *name = req->uri + strlen("/cmd/");
    ESP_LOGI(TAG, "cmd: %s", name);

    const char *reply = "ok";
    static char buf[32];

    if (strncmp(name, "freq/", 5) == 0) {
        int hz = atoi(name + 5);
        if (hz < 20)   hz = 20;
        if (hz > 2000) hz = 2000;
        uint32_t step = freq_to_step((float)hz);
        for (int vi = 0; vi < NUM_VOICES; vi++) s_voices[vi].phase_step = step;
        snprintf(buf, sizeof(buf), "freq %d Hz", hz);
        reply = buf;
    } else if (strncmp(name, "vol/", 4) == 0) {
        int vol = atoi(name + 4);
        if (vol < 0)   vol = 0;
        if (vol > 100) vol = 100;
        if (s_spk) esp_codec_dev_set_out_vol(s_spk, vol);
        snprintf(buf, sizeof(buf), "vol %d", vol);
        reply = buf;
    } else {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "unknown command");
        return ESP_OK;
    }

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, reply);
    return ESP_OK;
}

static httpd_handle_t start_webserver(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.uri_match_fn   = httpd_uri_match_wildcard;

    httpd_handle_t server = NULL;
    ESP_ERROR_CHECK(httpd_start(&server, &cfg));

    static const httpd_uri_t index_uri = {
        .uri = "/", .method = HTTP_GET, .handler = index_get_handler,
    };
    httpd_register_uri_handler(server, &index_uri);

    static const httpd_uri_t cmd_uri = {
        .uri = "/cmd/*", .method = HTTP_POST, .handler = cmd_post_handler,
    };
    httpd_register_uri_handler(server, &cmd_uri);

    ESP_LOGI(TAG, "HTTP server started");
    return server;
}

/* ── WiFi SoftAP ──────────────────────────────────────────────────────────── */
static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *e = data;
        ESP_LOGI(TAG, "station joined: " MACSTR, MAC2STR(e->mac));
    } else if (id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *e = data;
        ESP_LOGI(TAG, "station left: " MACSTR, MAC2STR(e->mac));
    }
}

static void wifi_init_softap(void)
{
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_cfg = {
        .ap = {
            .ssid           = AP_SSID,
            .ssid_len       = strlen(AP_SSID),
            .channel        = AP_CHANNEL,
            .password       = AP_PASS,
            .max_connection = AP_MAX_CONN,
            .authmode       = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg        = { .required = false },
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "SoftAP up — SSID:%s  http://192.168.4.1", AP_SSID);
}

/* ── LVGL UI ──────────────────────────────────────────────────────────────── */
/*
 * Logical screen: 1280 × 720 landscape (ROTATE_90 of 720×1280 panel).
 *
 * Layout:
 *   y=  0..400  — 4 horizontal sliders: VOL, ATK, DEC, REL (100px per row)
 *   y=400..415  — octave label + WiFi hint
 *   y=415..715  — 2-octave piano keyboard with octave << / >> buttons
 */

/* C4..B4 semitone frequencies */
static const float k_semi_c4[12] = {
    261.63f, 277.18f, 293.66f, 311.13f, 329.63f, 349.23f,
    369.99f, 392.00f, 415.30f, 440.00f, 466.16f, 493.88f
};

static int        s_octave    = 4;
static lv_obj_t  *s_oct_label = NULL;

static float semitone_to_freq(int semi, int octave)
{
    float f = k_semi_c4[semi & 0x0F];
    for (int d = octave - 4; d > 0; d--) f *= 2.0f;
    for (int d = octave - 4; d < 0; d++) f *= 0.5f;
    return f;
}

/* ── Multitouch key regions ───────────────────────────────────────────────── */
typedef struct {
    int16_t   x, y, w, h;
    int       key_data;   /* (oct_off<<8)|semi */
    lv_obj_t *obj;        /* for visual pressed state */
} key_region_t;

#define MAX_KEYS 24
static key_region_t           s_key_regions[MAX_KEYS];
static int                    s_key_region_cnt = 0;
static esp_lcd_touch_handle_t s_touch_handle   = NULL;

typedef struct { lv_obj_t *obj; bool pressed; } key_vis_msg_t;
static QueueHandle_t s_key_vis_q;

static void key_vis_cb(lv_timer_t *t)
{
    key_vis_msg_t msg;
    while (xQueueReceive(s_key_vis_q, &msg, 0) == pdTRUE) {
        if (msg.pressed) lv_obj_add_state(msg.obj, LV_STATE_PRESSED);
        else             lv_obj_remove_state(msg.obj, LV_STATE_PRESSED);
    }
}

static void touch_task(void *arg)
{
    esp_lcd_touch_point_data_t pts[5];
    bool prev[MAX_KEYS] = {0};

    while (1) {
        /* Don't call read_data here — let the LVGL indev own the I2C bus.
           get_data reads the cached tp->data struct which LVGL keeps fresh. */
        uint8_t cnt = 0;
        esp_lcd_touch_get_data(s_touch_handle, pts, &cnt, 5);

        bool cur[MAX_KEYS] = {0};
        for (int p = 0; p < cnt; p++) {
            for (int k = 0; k < s_key_region_cnt; k++) {
                key_region_t *r = &s_key_regions[k];
                if (pts[p].x >= r->x && pts[p].x < r->x + r->w &&
                    pts[p].y >= r->y && pts[p].y < r->y + r->h)
                    cur[k] = true;
            }
        }

        for (int k = 0; k < s_key_region_cnt; k++) {
            if (cur[k] == prev[k]) continue;
            key_region_t *r = &s_key_regions[k];
            if (cur[k]) {
                float hz = semitone_to_freq(r->key_data & 0xFF, s_octave + (r->key_data >> 8));
                alloc_voice(r->key_data, freq_to_step(hz));
            } else {
                for (int v = 0; v < NUM_VOICES; v++) {
                    if (s_voices[v].key_id == r->key_data && s_voices[v].note_on) {
                        s_voices[v].note_on = false;
                        break;
                    }
                }
            }
            /* queue visual update for the LVGL task — never block here */
            key_vis_msg_t msg = { .obj = r->obj, .pressed = cur[k] };
            xQueueSend(s_key_vis_q, &msg, 0);
            prev[k] = cur[k];
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/* ms = 10 + v²/3  →  v=0: 10ms  v=100: 3343ms */
static uint32_t slider_to_ms(int v) { return (uint32_t)(10 + (v * v) / 3); }

/* ── Slider callbacks ── */
static lv_obj_t *s_slval[4]; /* value labels for VOL ATK DEC REL */

static void vol_slider_cb(lv_event_t *e)
{
    int v = lv_slider_get_value(lv_event_get_target(e));
    if (s_spk) esp_codec_dev_set_out_vol(s_spk, v);
    char b[12]; snprintf(b, sizeof(b), "%d%%", v);
    lv_label_set_text(s_slval[0], b);
}

static void adr_slider_cb(lv_event_t *e)
{
    int p    = (int)(intptr_t)lv_event_get_user_data(e); /* 0=A 1=D 2=R */
    int v    = lv_slider_get_value(lv_event_get_target(e));
    uint32_t ms   = slider_to_ms(v);
    uint32_t samp = ms * (SAMPLE_RATE / 1000);
    if      (p == 0) s_attack_samp  = samp;
    else if (p == 1) s_decay_samp   = samp;
    else             s_release_samp = samp;
    char b[16];
    if (ms < 1000) snprintf(b, sizeof(b), "%ums",  (unsigned)ms);
    else           snprintf(b, sizeof(b), "%.1fs", ms / 1000.0f);
    lv_label_set_text(s_slval[1 + p], b);
}

/* ── Octave controls ── */
static void update_oct_label(void)
{
    if (!s_oct_label) return;
    char b[10]; snprintf(b, sizeof(b), "OCT %d", s_octave);
    lv_label_set_text(s_oct_label, b);
}
static void oct_down_cb(lv_event_t *e) { if (s_octave > 1) { s_octave--; update_oct_label(); } }
static void oct_up_cb  (lv_event_t *e) { if (s_octave < 7) { s_octave++; update_oct_label(); } }

/* ── Waveform selector ── */
static const int16_t *const k_waveforms[4] = {
    wt_square, wt_sawtooth, wt_sine, wt_triangle
};
static lv_obj_t *s_wave_btns[4];
static int        s_wave_sel = 0;

static void wave_btn_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    s_wavetable = k_waveforms[idx];
    s_wave_sel  = idx;
    /* update button styles: selected = solid green, others = outline */
    for (int i = 0; i < 4; i++) {
        if (i == idx) {
            lv_obj_set_style_bg_color(s_wave_btns[i], lv_color_hex(0x00cc00), 0);
            lv_obj_set_style_bg_opa(s_wave_btns[i], LV_OPA_COVER, 0);
            lv_obj_set_style_text_color(lv_obj_get_child(s_wave_btns[i], 0),
                                        lv_color_hex(0x000000), 0);
        } else {
            lv_obj_set_style_bg_color(s_wave_btns[i], lv_color_hex(0x1a1a1a), 0);
            lv_obj_set_style_bg_opa(s_wave_btns[i], LV_OPA_COVER, 0);
            lv_obj_set_style_text_color(lv_obj_get_child(s_wave_btns[i], 0),
                                        lv_color_hex(0x00ff00), 0);
        }
    }
}

/* ── Widget factories ── */
static void make_slider_row(lv_obj_t *scr, int y, const char *name,
                             int rmin, int rmax, int init, const char *init_str,
                             int val_idx, lv_event_cb_t cb, void *udata)
{
    lv_obj_t *lbl = lv_label_create(scr);
    lv_label_set_text(lbl, name);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0x00bb00), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_pos(lbl, 10, y + 12);

    lv_obj_t *sl = lv_slider_create(scr);
    lv_obj_set_pos(sl, 88, y);
    lv_obj_set_size(sl, 1000, 44);
    lv_slider_set_range(sl, rmin, rmax);
    lv_slider_set_value(sl, init, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(sl, lv_color_hex(0x2a2a2a), LV_PART_MAIN);
    lv_obj_set_style_bg_color(sl, lv_color_hex(0x00ff00), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sl, lv_color_hex(0x00cc00), LV_PART_KNOB);
    lv_obj_set_style_pad_all(sl, 10, LV_PART_KNOB);
    if (cb) lv_obj_add_event_cb(sl, cb, LV_EVENT_VALUE_CHANGED, udata);

    lv_obj_t *vlbl = lv_label_create(scr);
    lv_label_set_text(vlbl, init_str);
    lv_obj_set_style_text_color(vlbl, lv_color_hex(0x00ff00), 0);
    lv_obj_set_style_text_font(vlbl, &lv_font_montserrat_20, 0);
    lv_obj_set_pos(vlbl, 1098, y + 12);
    s_slval[val_idx] = vlbl;
}

static void make_piano_key(lv_obj_t *scr, int x, int y, int w, int h,
                            bool black, int semi, int oct_off)
{
    lv_obj_t *k = lv_obj_create(scr);
    lv_obj_set_pos(k, x, y);
    lv_obj_set_size(k, w, h);
    lv_obj_remove_flag(k, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(k, 0, 0);
    lv_obj_set_style_radius(k, 4, 0);
    lv_obj_set_style_bg_opa(k, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(k, 1, 0);
    if (black) {
        lv_obj_set_style_bg_color(k, lv_color_hex(0x111111), 0);
        lv_obj_set_style_bg_color(k, lv_color_hex(0x00ff00), LV_STATE_PRESSED);
        lv_obj_set_style_border_color(k, lv_color_hex(0x555555), 0);
    } else {
        lv_obj_set_style_bg_color(k, lv_color_hex(0xe0e0e0), 0);
        lv_obj_set_style_bg_color(k, lv_color_hex(0x00ff00), LV_STATE_PRESSED);
        lv_obj_set_style_border_color(k, lv_color_hex(0x333333), 0);
    }
    /* keys are purely visual — touch handled by touch_task */

    if (s_key_region_cnt < MAX_KEYS) {
        s_key_regions[s_key_region_cnt++] = (key_region_t){
            .x = x, .y = y, .w = w, .h = h,
            .key_data = (oct_off << 8) | semi,
            .obj = k,
        };
    }
}

static void make_oct_btn(lv_obj_t *scr, int x, int y, int w, int h,
                          const char *txt, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_obj_create(scr);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, w, h);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x1a1a1a), 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x003300), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(btn, lv_color_hex(0x00ff00), 0);
    lv_obj_set_style_border_width(btn, 2, 0);
    lv_obj_set_style_radius(btn, 6, 0);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, txt);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0x00ff00), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
    lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);
}

static void ui_create(void)
{
    s_key_vis_q = xQueueCreate(MAX_KEYS * 2, sizeof(key_vis_msg_t));

    bsp_display_lock(0);
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x111111), 0);

    /* ── 4 sliders, 80px rows, y = 5 / 85 / 165 / 245 ── */
    make_slider_row(scr, 5,   "VOL", 0, 100, 20, "20%",   0, vol_slider_cb, NULL);
    make_slider_row(scr, 85,  "ATK", 0, 100, 12, "58ms",  1, adr_slider_cb, (void*)(intptr_t)0);
    make_slider_row(scr, 165, "DEC", 0, 100, 17, "106ms", 2, adr_slider_cb, (void*)(intptr_t)1);
    make_slider_row(scr, 245, "REL", 0, 100, 24, "202ms", 3, adr_slider_cb, (void*)(intptr_t)2);

    /* ── Waveform selector, y=300 ── */
    /* 4 buttons: SQR SAW SIN TRI, each 290px wide, 10px gap, centred */
    static const char *wnames[4] = { "SQR", "SAW", "SIN", "TRI" };
    for (int i = 0; i < 4; i++) {
        int bx = 40 + i * 300;
        lv_obj_t *btn = lv_obj_create(scr);
        lv_obj_set_pos(btn, bx, 300);
        lv_obj_set_size(btn, 285, 52);
        lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_radius(btn, 8, 0);
        lv_obj_set_style_border_color(btn, lv_color_hex(0x00ff00), 0);
        lv_obj_set_style_border_width(btn, 2, 0);
        /* first button starts selected */
        lv_obj_set_style_bg_color(btn, i == 0 ? lv_color_hex(0x00cc00)
                                               : lv_color_hex(0x1a1a1a), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(btn, wave_btn_cb, LV_EVENT_CLICKED, (void*)(intptr_t)i);

        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, wnames[i]);
        lv_obj_set_style_text_color(lbl, i == 0 ? lv_color_hex(0x000000)
                                                 : lv_color_hex(0x00ff00), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
        lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);
        s_wave_btns[i] = btn;
    }

    /* octave label */
    s_oct_label = lv_label_create(scr);
    lv_obj_set_style_text_color(s_oct_label, lv_color_hex(0x00ff00), 0);
    lv_obj_set_style_text_font(s_oct_label, &lv_font_montserrat_20, 0);
    lv_obj_set_pos(s_oct_label, 10, 362);
    update_oct_label();

    /* WiFi hint */
    lv_obj_t *wifi = lv_label_create(scr);
    lv_label_set_text(wifi, "192.168.4.1");
    lv_obj_set_style_text_color(wifi, lv_color_hex(0x333333), 0);
    lv_obj_set_style_text_font(wifi, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(wifi, 1150, 364);

    /* ── 2-octave piano keyboard, y=390..715 (325px) ── */
    const int KY  = 390;  /* piano top y */
    const int KH  = 325;  /* white key height */
    const int WKW = 81;   /* white key width */
    const int BKW = 50;   /* black key width */
    const int BKH = 183;  /* black key height ≈ 60% */
    const int OBW = 68;   /* octave button width */
    const int KX  = OBW + 5; /* left edge of first white key */

    /* white key semitones within an octave: C D E F G A B */
    static const int wk[7] = { 0, 2, 4, 5, 7, 9, 11 };
    /* black key semitones and their center-x within one 567px octave */
    static const int bk[5]   = { 1, 3, 6, 8, 10 };
    static const int bk_cx[5]= { 54, 135, 297, 378, 459 };

    /* white keys first (z-order below black) */
    for (int oct = 0; oct < 2; oct++) {
        int ox = KX + oct * 7 * WKW;
        for (int k = 0; k < 7; k++)
            make_piano_key(scr, ox + k * WKW, KY, WKW - 1, KH, false, wk[k], oct);
    }
    /* black keys on top */
    for (int oct = 0; oct < 2; oct++) {
        int ox = KX + oct * 7 * WKW;
        for (int k = 0; k < 5; k++)
            make_piano_key(scr, ox + bk_cx[k] - BKW / 2, KY, BKW, BKH, true, bk[k], oct);
    }

    /* octave << / >> buttons */
    make_oct_btn(scr, 2,              KY, OBW, KH, "<<", oct_down_cb);
    make_oct_btn(scr, KX + 14*WKW+3, KY, OBW, KH, ">>", oct_up_cb);

    /* apply defaults */
    if (s_spk) esp_codec_dev_set_out_vol(s_spk, 20);
    s_attack_samp  = slider_to_ms(12) * (SAMPLE_RATE / 1000);
    s_decay_samp   = slider_to_ms(17) * (SAMPLE_RATE / 1000);
    s_release_samp = slider_to_ms(24) * (SAMPLE_RATE / 1000);
    /* drain key visual update queue every 10 ms in the LVGL task */
    lv_timer_create(key_vis_cb, 10, NULL);

    /* voices start idle; first key press will set frequency */

    bsp_display_unlock();
}

/* ── Entry point ──────────────────────────────────────────────────────────── */
void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(bsp_i2c_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* Core 0: UI (LVGL + web server)   Core 1: audio only   C6: network (untouched) */
    esp_lv_adapter_config_t lv_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    lv_cfg.task_core_id = 0;

    bsp_display_cfg_t disp_cfg = {
        .lv_adapter_cfg   = lv_cfg,
        .rotation         = ESP_LV_ADAPTER_ROTATE_90,
        .tear_avoid_mode  = ESP_LV_ADAPTER_TEAR_AVOID_MODE_TRIPLE_PARTIAL,
        .touch_flags      = { .swap_xy = 1, .mirror_x = 1, .mirror_y = 0 },
    };
    bsp_display_start_with_config(&disp_cfg);

    wavetable_init();
    audio_init();

    wifi_init_softap();
    start_webserver();

    ui_create();

    /* Extract esp_lcd_touch_handle_t from LVGL indev driver_data.
     * The adapter's internal context struct starts with the handle as its first field. */
    lv_indev_t *touch_indev = bsp_display_get_input_dev();
    if (touch_indev) {
        s_touch_handle = *(esp_lcd_touch_handle_t *)lv_indev_get_driver_data(touch_indev);
        xTaskCreatePinnedToCore(touch_task, "touch_mt", 4096, NULL, 10, NULL, 0);
    }

    backlight_init();
    ESP_LOGI(TAG, "ready");
}
