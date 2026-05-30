/*
 * wifi_http.c — SoftAP, NVS credential storage, HTTP server.
 */

#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/stat.h>
#include <dirent.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_http_server.h"
#include "esp_codec_dev.h"
#include "sounds_http.h"
#include "audio.h"
#include "wifi_http.h"
#include "ws_server.h"
#include "render_export.h"
#include "lane.h"
#include "song.h"
#include "synth_voice.h"

static const char *TAG = "wifi_http";

/* ── NVS keys ────────────────────────────────────────────────────────────── */
#define AP_SSID_DEFAULT  "synth-32"
#define AP_PASS_DEFAULT  "synth1234"
#define AP_CHANNEL       1
#define AP_MAX_CONN      4
#define NVS_NS           "synth32"
#define NVS_KEY_SSID     "ap_ssid"
#define NVS_KEY_PASS     "ap_pass"

char s_ap_ssid[33] = AP_SSID_DEFAULT;
static char s_ap_pass[65] = AP_PASS_DEFAULT;

/* ── Embedded web UI ─────────────────────────────────────────────────────── */
extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[]   asm("_binary_index_html_end");
extern const char app_css_start[]    asm("_binary_app_css_start");
extern const char app_css_end[]      asm("_binary_app_css_end");
extern const char app_js_start[]     asm("_binary_app_js_start");
extern const char app_js_end[]       asm("_binary_app_js_end");

/* ── NVS helpers ─────────────────────────────────────────────────────────── */
void wifi_config_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t len = sizeof(s_ap_ssid);
    if (nvs_get_str(h, NVS_KEY_SSID, s_ap_ssid, &len) != ESP_OK)
        strncpy(s_ap_ssid, AP_SSID_DEFAULT, sizeof(s_ap_ssid) - 1);
    len = sizeof(s_ap_pass);
    if (nvs_get_str(h, NVS_KEY_PASS, s_ap_pass, &len) != ESP_OK)
        strncpy(s_ap_pass, AP_PASS_DEFAULT, sizeof(s_ap_pass) - 1);
    nvs_close(h);
}

static esp_err_t wifi_config_save(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, NVS_KEY_SSID, ssid);
    if (err == ESP_OK) err = nvs_set_str(h, NVS_KEY_PASS, pass);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

/* ── HTTP handlers ───────────────────────────────────────────────────────── */
static esp_err_t index_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, index_html_start,
                    (ssize_t)(index_html_end - index_html_start));
    return ESP_OK;
}

static esp_err_t app_css_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/css");
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=3600");
    httpd_resp_send(req, app_css_start, (ssize_t)(app_css_end - app_css_start));
    return ESP_OK;
}

static esp_err_t app_js_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/javascript");
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=3600");
    httpd_resp_send(req, app_js_start, (ssize_t)(app_js_end - app_js_start));
    return ESP_OK;
}

static esp_err_t cmd_post_handler(httpd_req_t *req)
{
    const char *name = req->uri + strlen("/cmd/");
    static char buf[32];
    if (strncmp(name, "vol/", 4) == 0) {
        int vol = atoi(name + 4);
        if (vol < 0)   vol = 0;
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

/* GET /settings/wifi — returns current SSID (password omitted) */
static esp_err_t settings_wifi_get_handler(httpd_req_t *req)
{
    char buf[96];
    snprintf(buf, sizeof(buf), "{\"ssid\":\"%s\"}", s_ap_ssid);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

/* POST /settings/wifi — body: ssid=NAME&pass=PASS — saves and reboots */
static esp_err_t settings_wifi_post_handler(httpd_req_t *req)
{
    char body[256] = "";
    int len = req->content_len;
    if (len <= 0 || len >= (int)sizeof(body)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_OK;
    }
    if (httpd_req_recv(req, body, len) <= 0) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv failed");
        return ESP_OK;
    }
    body[len] = '\0';

    char new_ssid[33] = "";
    char new_pass[65] = "";
    httpd_query_key_value(body, "ssid", new_ssid, sizeof(new_ssid));
    httpd_query_key_value(body, "pass", new_pass, sizeof(new_pass));

    if (strnlen(new_ssid, sizeof(new_ssid)) < 1) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid required");
        return ESP_OK;
    }
    if (strnlen(new_pass, sizeof(new_pass)) < 8) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "password min 8 chars");
        return ESP_OK;
    }

    esp_err_t err = wifi_config_save(new_ssid, new_pass);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "nvs write failed");
        return ESP_OK;
    }

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "ok — rebooting");
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
    return ESP_OK;
}

/* ── Export handlers ─────────────────────────────────────────────────────── */

static char s_export_path[128] = "";

/* POST /export/start — begins WAV capture, responds with file path */
static esp_err_t export_start_handler(httpd_req_t *req)
{
    if (render_export_active()) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "already recording");
        return ESP_OK;
    }
    const char *base = g_song.name[0] ? g_song.name : "mix";
    snprintf(s_export_path, sizeof(s_export_path), SONG_DIR "/%s_mix.wav", base);
    if (!render_export_start(s_export_path)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "open failed");
        return ESP_OK;
    }
    char buf[160];
    snprintf(buf, sizeof(buf), "{\"ok\":true,\"path\":\"%s\"}", s_export_path);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

/* POST /export/stop — finalises WAV header, responds with frame count */
static esp_err_t export_stop_handler(httpd_req_t *req)
{
    if (!render_export_active()) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "not recording");
        return ESP_OK;
    }
    uint32_t frames = render_export_frames();
    render_export_stop();
    char buf[128];
    snprintf(buf, sizeof(buf), "{\"ok\":true,\"frames\":%lu,\"seconds\":%.2f}",
             (unsigned long)frames, (double)frames / 48000.0);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

/* GET /export/download — streams the recorded WAV file */
static esp_err_t export_download_handler(httpd_req_t *req)
{
    if (s_export_path[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no export");
        return ESP_OK;
    }
    FILE *fp = fopen(s_export_path, "rb");
    if (!fp) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "file not found");
        return ESP_OK;
    }

    /* Content-Disposition: attachment with filename */
    char disp[160];
    const char *fname = strrchr(s_export_path, '/');
    fname = fname ? fname + 1 : s_export_path;
    snprintf(disp, sizeof(disp), "attachment; filename=\"%s\"", fname);

    httpd_resp_set_type(req, "audio/wav");
    httpd_resp_set_hdr(req, "Content-Disposition", disp);

    static char chunk[4096];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), fp)) > 0) {
        if (httpd_resp_send_chunk(req, chunk, (ssize_t)n) != ESP_OK) break;
    }
    fclose(fp);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* GET /export/status — {active, frames, seconds, path} */
static esp_err_t export_status_handler(httpd_req_t *req)
{
    uint32_t fr = render_export_frames();
    char buf[200];
    snprintf(buf, sizeof(buf),
             "{\"active\":%s,\"frames\":%lu,\"seconds\":%.2f,\"path\":\"%s\"}",
             render_export_active() ? "true" : "false",
             (unsigned long)fr, (double)fr / 48000.0,
             s_export_path);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

/* Captive-portal catch-all: redirects unknown hosts to the mDNS UI */
static esp_err_t catchall_get_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://synth-32.local/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* ── Stem export endpoints ───────────────────────────────────────────────── */
#define STEM_DIR  "/sdcard/stems"

static bool s_stem_active = false;
static uint32_t s_stem_start_ms = 0;

static esp_err_t stem_start_handler(httpd_req_t *req)
{
    if (s_stem_active) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"err\":\"already recording\"}");
        return ESP_OK;
    }
    stem_export_start(STEM_DIR);
    s_stem_active   = true;
    s_stem_start_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t stem_stop_handler(httpd_req_t *req)
{
    if (s_stem_active) {
        stem_export_stop();
        s_stem_active = false;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t stem_status_handler(httpd_req_t *req)
{
    char buf[128];
    uint32_t elapsed_ms = s_stem_active
        ? (xTaskGetTickCount() * portTICK_PERIOD_MS - s_stem_start_ms) : 0;
    snprintf(buf, sizeof(buf),
             "{\"active\":%s,\"seconds\":%.2f}",
             s_stem_active ? "true" : "false",
             (double)elapsed_ms / 1000.0);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

/* ── Wavetable import endpoint ───────────────────────────────────────────── */
/* POST /wt/upload?slot=1  — body: raw WAV file, saves to /sdcard/wt/slot_N.wav
 * then loads it into the custom wavetable slot.
 */
#define WT_SLOT_DIR "/sdcard/wt"
#define WT_MAX_UPLOAD (256 * 1024)   /* 256 KB max WAV */

static esp_err_t wt_upload_handler(httpd_req_t *req)
{
    char query[32] = "";
    char slot_str[8] = "1";
    httpd_req_get_url_query_str(req, query, sizeof(query));
    httpd_query_key_value(query, "slot", slot_str, sizeof(slot_str));
    int slot_n = atoi(slot_str);
    if (slot_n < 1 || slot_n > 4) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "slot must be 1-4");
        return ESP_OK;
    }
    sv_wave_t wave_slot = (sv_wave_t)(SV_WAVE_CUSTOM1 + slot_n - 1);

    /* Save incoming body to a temp file */
    char path[64];
    snprintf(path, sizeof(path), WT_SLOT_DIR "/slot_%d.wav", slot_n);
    /* Ensure directory */
    mkdir(WT_SLOT_DIR, 0755);

    FILE *fp = fopen(path, "wb");
    if (!fp) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "cannot write");
        return ESP_OK;
    }
    uint8_t buf[512];
    int remaining = (int)req->content_len;
    if (remaining > WT_MAX_UPLOAD) remaining = WT_MAX_UPLOAD;
    while (remaining > 0) {
        int chunk = remaining > (int)sizeof(buf) ? (int)sizeof(buf) : remaining;
        int got   = httpd_req_recv(req, (char *)buf, chunk);
        if (got <= 0) break;
        fwrite(buf, 1, got, fp);
        remaining -= got;
    }
    fclose(fp);

    if (sv_wt_load_custom(wave_slot, path)) {
        httpd_resp_set_type(req, "application/json");
        char resp[80];
        snprintf(resp, sizeof(resp), "{\"ok\":true,\"slot\":%d}", slot_n);
        httpd_resp_sendstr(req, resp);
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "load failed");
    }
    return ESP_OK;
}

/* ── Song templates list endpoint ────────────────────────────────────────── */
static esp_err_t templates_list_handler(httpd_req_t *req)
{
    static char list_buf[1024];
    int pos = 0;
    list_buf[pos++] = '[';

    char tdir[64];
    snprintf(tdir, sizeof(tdir), "%s/templates", SONG_DIR);
    DIR *d = opendir(tdir);
    bool first = true;
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL && pos < (int)sizeof(list_buf) - 64) {
            const char *n = ent->d_name;
            const char *ext = strrchr(n, '.');
            if (!ext || strcasecmp(ext, SONG_EXT) != 0) continue;
            size_t base_len = (size_t)(ext - n);
            if (!first) list_buf[pos++] = ',';
            first = false;
            list_buf[pos++] = '"';
            memcpy(list_buf + pos, n, base_len);
            pos += (int)base_len;
            list_buf[pos++] = '"';
        }
        closedir(d);
    }
    list_buf[pos++] = ']';
    list_buf[pos]   = '\0';

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, list_buf);
    return ESP_OK;
}

/* ── Server start ────────────────────────────────────────────────────────── */
void start_webserver(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.uri_match_fn    = httpd_uri_match_wildcard;
    cfg.max_uri_handlers = 28;
    cfg.stack_size       = 8192;
    httpd_handle_t server = NULL;
    ESP_ERROR_CHECK(httpd_start(&server, &cfg));

    static const httpd_uri_t idx_uri          = { .uri = "/",               .method = HTTP_GET,  .handler = index_get_handler          };
    static const httpd_uri_t css_uri          = { .uri = "/app.css",        .method = HTTP_GET,  .handler = app_css_handler            };
    static const httpd_uri_t js_uri           = { .uri = "/app.js",         .method = HTTP_GET,  .handler = app_js_handler             };
    static const httpd_uri_t cmd_uri          = { .uri = "/cmd/*",          .method = HTTP_POST, .handler = cmd_post_handler           };
    static const httpd_uri_t wifi_get_uri     = { .uri = "/settings/wifi",  .method = HTTP_GET,  .handler = settings_wifi_get_handler  };
    static const httpd_uri_t wifi_post_uri    = { .uri = "/settings/wifi",  .method = HTTP_POST, .handler = settings_wifi_post_handler };
    static const httpd_uri_t exp_start_uri    = { .uri = "/export/start",   .method = HTTP_POST, .handler = export_start_handler       };
    static const httpd_uri_t exp_stop_uri     = { .uri = "/export/stop",    .method = HTTP_POST, .handler = export_stop_handler        };
    static const httpd_uri_t exp_download_uri = { .uri = "/export/download",.method = HTTP_GET,  .handler = export_download_handler    };
    static const httpd_uri_t exp_status_uri   = { .uri = "/export/status",  .method = HTTP_GET,  .handler = export_status_handler      };
    static const httpd_uri_t stem_start_uri   = { .uri = "/stem/start",     .method = HTTP_POST, .handler = stem_start_handler         };
    static const httpd_uri_t stem_stop_uri    = { .uri = "/stem/stop",      .method = HTTP_POST, .handler = stem_stop_handler          };
    static const httpd_uri_t stem_status_uri  = { .uri = "/stem/status",    .method = HTTP_GET,  .handler = stem_status_handler        };
    static const httpd_uri_t wt_upload_uri    = { .uri = "/wt/upload",      .method = HTTP_POST, .handler = wt_upload_handler          };
    static const httpd_uri_t tpl_list_uri     = { .uri = "/songs/templates",.method = HTTP_GET,  .handler = templates_list_handler     };
    static const httpd_uri_t catchall_uri     = { .uri = "/*",              .method = HTTP_GET,  .handler = catchall_get_handler       };

    httpd_register_uri_handler(server, &idx_uri);
    httpd_register_uri_handler(server, &css_uri);
    httpd_register_uri_handler(server, &js_uri);
    httpd_register_uri_handler(server, &cmd_uri);
    httpd_register_uri_handler(server, &wifi_get_uri);
    httpd_register_uri_handler(server, &wifi_post_uri);
    httpd_register_uri_handler(server, &exp_start_uri);
    httpd_register_uri_handler(server, &exp_stop_uri);
    httpd_register_uri_handler(server, &exp_download_uri);
    httpd_register_uri_handler(server, &exp_status_uri);
    httpd_register_uri_handler(server, &stem_start_uri);
    httpd_register_uri_handler(server, &stem_stop_uri);
    httpd_register_uri_handler(server, &stem_status_uri);
    httpd_register_uri_handler(server, &wt_upload_uri);
    httpd_register_uri_handler(server, &tpl_list_uri);
    sounds_http_register(server);
    ws_server_register(server);
    httpd_register_uri_handler(server, &catchall_uri); /* catchall last */
}

/* ── SoftAP init ─────────────────────────────────────────────────────────── */
static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *e = (wifi_event_ap_staconnected_t *)data;
        ESP_LOGI(TAG, "station joined: " MACSTR, MAC2STR(e->mac));
    }
}

void wifi_init_softap(void)
{
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_cfg = {};
    size_t ssid_len = strnlen(s_ap_ssid, sizeof(s_ap_ssid) - 1);
    memcpy(wifi_cfg.ap.ssid, s_ap_ssid, ssid_len);
    wifi_cfg.ap.ssid_len       = (uint8_t)ssid_len;
    wifi_cfg.ap.channel        = AP_CHANNEL;
    memcpy(wifi_cfg.ap.password, s_ap_pass, strnlen(s_ap_pass, sizeof(s_ap_pass) - 1));
    wifi_cfg.ap.max_connection = AP_MAX_CONN;
    wifi_cfg.ap.authmode       = WIFI_AUTH_WPA2_PSK;
    wifi_cfg.ap.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "SoftAP up — SSID:%s  http://192.168.4.1", s_ap_ssid);
}
