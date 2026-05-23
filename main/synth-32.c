#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_http_server.h"
#include "esp_mac.h"
#include "driver/i2s_std.h"
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

/* ── Squarewave wavetable ─────────────────────────────────────────────────── */
#define WAVETABLE_SIZE  256
#define SAMPLE_RATE     48000
#define AUDIO_BUF_FRAMES 512   /* samples per write call */

/* 16-bit signed square wave: first half +30000, second half -30000 */
static int16_t s_wavetable[WAVETABLE_SIZE];

/* Phase accumulator — index into wavetable, fixed-point Q24 */
static volatile uint32_t s_phase_acc  = 0;
static volatile uint32_t s_phase_step = 0; /* updated by UI */

static void wavetable_init(void)
{
    for (int i = 0; i < WAVETABLE_SIZE; i++) {
        s_wavetable[i] = (i < WAVETABLE_SIZE / 2) ? 30000 : -30000;
    }
}

static void set_frequency(float hz)
{
    /* phase_step = freq * wavetable_size / sample_rate, scaled Q24 */
    float step = hz * (float)WAVETABLE_SIZE / (float)SAMPLE_RATE;
    s_phase_step = (uint32_t)(step * (1 << 24));
}

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

static void audio_task(void *arg)
{
    while (1) {
        uint32_t acc  = s_phase_acc;
        uint32_t step = s_phase_step;

        for (int i = 0; i < AUDIO_BUF_FRAMES; i++) {
            uint8_t idx = (acc >> 24) & 0xFF;
            int16_t sample = s_wavetable[idx];
            s_audio_buf[i * 2 + 0] = sample;
            s_audio_buf[i * 2 + 1] = sample;
            acc += step;
        }
        s_phase_acc = acc;

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
    ESP_ERROR_CHECK(esp_codec_dev_set_out_vol(s_spk, 70));

    ESP_LOGI(TAG, "audio ready");

    set_frequency(120.0f);
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
"  <input type='range' min='20' max='2000' value='440' "
"         oninput=\"this.nextElementSibling.textContent=this.value+'Hz';cmd('freq/'+this.value)\">"
"  <span>440Hz</span>"
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
        set_frequency((float)hz);
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

/* ── LVGL touch knob UI ───────────────────────────────────────────────────── */

/* Map arc value (0–100) → frequency (20–2000 Hz) exponentially.
   Uses a precomputed table to avoid libm dependency. */
/* 20 Hz → 2000 Hz log scale, 101 steps, precomputed: 20 * (2000/20)^(i/100) */
static const float s_freq_table[101] = {
     20.0f,  21.4f,  22.9f,  24.6f,  26.3f,  28.2f,  30.2f,  32.3f,  34.6f,  37.1f,
     39.8f,  42.6f,  45.6f,  48.9f,  52.4f,  56.1f,  60.1f,  64.4f,  68.9f,  73.8f,
     79.1f,  84.7f,  90.7f,  97.2f, 104.1f, 111.5f, 119.4f, 127.9f, 137.0f, 146.7f,
    157.1f, 168.3f, 180.3f, 193.1f, 206.9f, 221.6f, 237.4f, 254.3f, 272.4f, 291.8f,
    312.5f, 334.7f, 358.5f, 383.9f, 411.2f, 440.5f, 471.8f, 505.4f, 541.3f, 579.8f,
    621.0f, 665.2f, 712.5f, 763.2f, 817.4f, 875.5f, 937.7f,1004.4f,1075.7f,1152.3f,
   1234.3f,1322.2f,1416.4f,1517.4f,1625.8f,1742.1f,1866.9f,2000.0f,2000.0f,2000.0f,
   2000.0f,2000.0f,2000.0f,2000.0f,2000.0f,2000.0f,2000.0f,2000.0f,2000.0f,2000.0f,
   2000.0f,2000.0f,2000.0f,2000.0f,2000.0f,2000.0f,2000.0f,2000.0f,2000.0f,2000.0f,
   2000.0f,2000.0f,2000.0f,2000.0f,2000.0f,2000.0f,2000.0f,2000.0f,2000.0f,2000.0f,
   2000.0f,
};

static float knob_to_freq(int v)
{
    if (v < 0)   v = 0;
    if (v > 100) v = 100;
    return s_freq_table[v];
}

static lv_obj_t *s_freq_label = NULL;

static void knob_cb(lv_event_t *e)
{
    lv_obj_t *arc = lv_event_get_target(e);
    int val = lv_arc_get_value(arc);
    float hz = knob_to_freq(val);
    set_frequency(hz);

    char buf[32];
    snprintf(buf, sizeof(buf), "%.0f Hz", hz);
    lv_label_set_text(s_freq_label, buf);
}

static void ui_create(void)
{
    bsp_display_lock(0);

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x111111), 0);

    /* Title */
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "synth-32  squarewave");
    lv_obj_set_style_text_color(title, lv_color_hex(0x00ff00), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 30);

    /* Frequency arc (knob) — large, centered */
    lv_obj_t *arc = lv_arc_create(scr);
    lv_obj_set_size(arc, 400, 400);
    lv_arc_set_rotation(arc, 135);
    lv_arc_set_bg_angles(arc, 0, 270);
    lv_arc_set_range(arc, 0, 100);
    lv_arc_set_value(arc, 50); /* starts at ~440 Hz on log scale */
    lv_obj_align(arc, LV_ALIGN_CENTER, 0, 20);
    lv_obj_add_event_cb(arc, knob_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* Style the arc indicator */
    lv_obj_set_style_arc_color(arc, lv_color_hex(0x00ff00), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(arc, 12, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(arc, 12, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, lv_color_hex(0x333333), LV_PART_MAIN);

    /* Knob dot */
    lv_obj_set_style_bg_color(arc, lv_color_hex(0x00ff00), LV_PART_KNOB);
    lv_obj_set_style_pad_all(arc, 6, LV_PART_KNOB);

    /* Frequency label inside arc */
    s_freq_label = lv_label_create(scr);
    lv_label_set_text(s_freq_label, "440 Hz");
    lv_obj_set_style_text_color(s_freq_label, lv_color_hex(0x00ff00), 0);
    lv_obj_set_style_text_font(s_freq_label, &lv_font_montserrat_20, 0);
    lv_obj_align(s_freq_label, LV_ALIGN_CENTER, 0, 20);

    /* Hint labels */
    lv_obj_t *lo = lv_label_create(scr);
    lv_label_set_text(lo, "20 Hz");
    lv_obj_set_style_text_color(lo, lv_color_hex(0x888888), 0);
    lv_obj_align(lo, LV_ALIGN_CENTER, -200, 160);

    lv_obj_t *hi = lv_label_create(scr);
    lv_label_set_text(hi, "2000 Hz");
    lv_obj_set_style_text_color(hi, lv_color_hex(0x888888), 0);
    lv_obj_align(hi, LV_ALIGN_CENTER, 200, 160);

    /* WiFi hint at bottom */
    lv_obj_t *wifi = lv_label_create(scr);
    lv_label_set_text(wifi, "WiFi: " AP_SSID "  \xE2\x86\x92  http://192.168.4.1");
    lv_obj_set_style_text_color(wifi, lv_color_hex(0x444444), 0);
    lv_obj_align(wifi, LV_ALIGN_BOTTOM_MID, 0, -30);

    set_frequency(knob_to_freq(50));

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

    bsp_display_cfg_t disp_cfg = {
        .lv_adapter_cfg   = ESP_LV_ADAPTER_DEFAULT_CONFIG(),
        .rotation         = ESP_LV_ADAPTER_ROTATE_90,
        .tear_avoid_mode  = ESP_LV_ADAPTER_TEAR_AVOID_MODE_TRIPLE_PARTIAL,
        .touch_flags      = { .swap_xy = 1, .mirror_x = 0, .mirror_y = 1 },
    };
    bsp_display_start_with_config(&disp_cfg);
    vTaskDelay(pdMS_TO_TICKS(50));
    esp_err_t bl_err = bsp_display_brightness_set(100);
    if (bl_err != ESP_OK)
        ESP_LOGW(TAG, "backlight set failed: %s", esp_err_to_name(bl_err));

    wavetable_init();
    audio_init();

    wifi_init_softap();
    start_webserver();

    ui_create();

    ESP_LOGI(TAG, "ready");
}
