/*
 * settings.c — Global device settings persist/restore (M5)
 *
 * JSON format (settings.json):
 * {
 *   "bpm": 120.0,
 *   "ppqn": 96,
 *   "beats_per_bar": 4,
 *   "wifi_ssid": "synth-32",
 *   "wifi_pass": "",
 *   "last_song": "",
 *   "master_volume": 1.0
 * }
 *
 * jsmn is used for parsing (single-header, zero heap, token array on stack).
 * Writing uses snprintf directly — no JSON library needed for output.
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "esp_log.h"
#include "clock.h"
#include "settings.h"

#define JSMN_STATIC
#include "jsmn.h"

static const char *TAG = "settings";

settings_t g_settings;

/* ── Defaults ────────────────────────────────────────────────────────────── */
static void settings_defaults(void)
{
    g_settings.bpm            = CLOCK_DEFAULT_BPM;
    g_settings.ppqn           = CLOCK_DEFAULT_PPQN;
    g_settings.beats_per_bar  = CLOCK_DEFAULT_BEATS;
    g_settings.master_volume  = 1.0f;
    memset(g_settings.wifi_ssid, 0, sizeof(g_settings.wifi_ssid));
    memset(g_settings.wifi_pass, 0, sizeof(g_settings.wifi_pass));
    memset(g_settings.last_song, 0, sizeof(g_settings.last_song));
    strncpy(g_settings.wifi_ssid, "synth-32", sizeof(g_settings.wifi_ssid) - 1);
}

/* ── jsmn helpers ────────────────────────────────────────────────────────── */
static int tok_eq(const char *js, const jsmntok_t *t, const char *key)
{
    int len = t->end - t->start;
    return (t->type == JSMN_STRING &&
            (int)strlen(key) == len &&
            memcmp(js + t->start, key, len) == 0);
}

static void tok_str(const char *js, const jsmntok_t *t, char *out, int out_len)
{
    int len = t->end - t->start;
    if (len >= out_len) len = out_len - 1;
    memcpy(out, js + t->start, len);
    out[len] = '\0';
}

static float tok_float(const char *js, const jsmntok_t *t)
{
    char buf[32];
    tok_str(js, t, buf, sizeof(buf));
    return strtof(buf, NULL);
}

static uint32_t tok_u32(const char *js, const jsmntok_t *t)
{
    char buf[32];
    tok_str(js, t, buf, sizeof(buf));
    return (uint32_t)strtoul(buf, NULL, 10);
}

/* ── Load ────────────────────────────────────────────────────────────────── */
void settings_load(void)
{
    settings_defaults();

    FILE *f = fopen(SETTINGS_PATH, "r");
    if (!f) {
        ESP_LOGI(TAG, "no settings.json — using defaults");
        return;
    }

    /* Read whole file into stack buffer (settings.json is tiny) */
    char buf[512];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (n == 0) return;
    buf[n] = '\0';

    jsmntok_t tokens[64];
    jsmn_parser p;
    jsmn_init(&p);
    int r = jsmn_parse(&p, buf, n, tokens, 64);
    if (r < 1 || tokens[0].type != JSMN_OBJECT) {
        ESP_LOGW(TAG, "settings.json parse failed — using defaults");
        return;
    }

    for (int i = 1; i < r - 1; i += 2) {
        const jsmntok_t *key = &tokens[i];
        const jsmntok_t *val = &tokens[i + 1];
        if (tok_eq(buf, key, "bpm"))
            g_settings.bpm = tok_float(buf, val);
        else if (tok_eq(buf, key, "ppqn"))
            g_settings.ppqn = tok_u32(buf, val);
        else if (tok_eq(buf, key, "beats_per_bar"))
            g_settings.beats_per_bar = tok_u32(buf, val);
        else if (tok_eq(buf, key, "master_volume"))
            g_settings.master_volume = tok_float(buf, val);
        else if (tok_eq(buf, key, "wifi_ssid"))
            tok_str(buf, val, g_settings.wifi_ssid, sizeof(g_settings.wifi_ssid));
        else if (tok_eq(buf, key, "wifi_pass"))
            tok_str(buf, val, g_settings.wifi_pass, sizeof(g_settings.wifi_pass));
        else if (tok_eq(buf, key, "last_song"))
            tok_str(buf, val, g_settings.last_song, sizeof(g_settings.last_song));
    }

    /* Clamp loaded values to valid ranges */
    if (g_settings.bpm < CLOCK_BPM_MIN || g_settings.bpm > CLOCK_BPM_MAX)
        g_settings.bpm = CLOCK_DEFAULT_BPM;
    if (g_settings.ppqn == 0) g_settings.ppqn = CLOCK_DEFAULT_PPQN;
    if (g_settings.beats_per_bar == 0) g_settings.beats_per_bar = CLOCK_DEFAULT_BEATS;
    if (g_settings.master_volume < 0.0f || g_settings.master_volume > 1.0f)
        g_settings.master_volume = 1.0f;

    ESP_LOGI(TAG, "loaded: bpm=%.1f ppqn=%u last_song=%s",
             g_settings.bpm, g_settings.ppqn, g_settings.last_song);
}

/* ── Save ────────────────────────────────────────────────────────────────── */
bool settings_save(void)
{
    char buf[512];
    int n = snprintf(buf, sizeof(buf),
        "{\n"
        "  \"bpm\": %.2f,\n"
        "  \"ppqn\": %u,\n"
        "  \"beats_per_bar\": %u,\n"
        "  \"wifi_ssid\": \"%s\",\n"
        "  \"wifi_pass\": \"%s\",\n"
        "  \"last_song\": \"%s\",\n"
        "  \"master_volume\": %.4f\n"
        "}\n",
        (double)g_settings.bpm,
        (unsigned)g_settings.ppqn,
        (unsigned)g_settings.beats_per_bar,
        g_settings.wifi_ssid,
        g_settings.wifi_pass,
        g_settings.last_song,
        (double)g_settings.master_volume);

    if (n <= 0 || n >= (int)sizeof(buf)) {
        ESP_LOGE(TAG, "settings_save: buffer overflow");
        return false;
    }

    const char *tmp = SETTINGS_PATH ".tmp";
    FILE *f = fopen(tmp, "w");
    if (!f) {
        ESP_LOGE(TAG, "settings_save: cannot open %s", tmp);
        return false;
    }
    size_t written = fwrite(buf, 1, n, f);
    fclose(f);
    if ((int)written != n) {
        ESP_LOGE(TAG, "settings_save: short write");
        remove(tmp);
        return false;
    }

    if (rename(tmp, SETTINGS_PATH) != 0) {
        ESP_LOGE(TAG, "settings_save: rename failed");
        remove(tmp);
        return false;
    }

    ESP_LOGI(TAG, "saved");
    return true;
}
