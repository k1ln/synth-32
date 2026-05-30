/*
 * song.c — Song save / load / new (M5)
 *
 * Format: JSON text, extension .s32, stored in /sdcard/songs/.
 * Parser: jsmn (zero-heap, token array on stack).
 * Writer: hand-rolled snprintf into a PSRAM-backed scratch buffer.
 *
 * Max serialised song size: ~64 KB (16 lanes × ~4 KB each).
 * Scratch buffer (PSRAM) allocated once in song_save/song_load, freed on return.
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define JSMN_STATIC
#include "jsmn.h"

#include "lane.h"
#include "clock.h"
#include "wav_player.h"
#include "drum_seq.h"
#include "piano_roll.h"
#include "arp.h"
#include "lfo.h"
#include "fx.h"
#include "synth.h"
#include "settings.h"
#include "song.h"

static const char *TAG = "song";

/* ── song_new ────────────────────────────────────────────────────────────── */
void song_new(void)
{
    mkdir(SONG_DIR, 0755);   /* ensure directory exists; harmless if already there */
    lane_init_all(&g_song);
    clock_set_bpm(&g_song.clock, g_settings.bpm);
    g_song.clock.tick_rate     = g_settings.ppqn;
    g_song.clock.beats_per_bar = g_settings.beats_per_bar;
    strncpy(g_song.name, SONG_NAME_DEFAULT, sizeof(g_song.name) - 1);
    g_song.dirty = false;
}

/* ══════════════════════════════════════════════════════════════════════════
 * SAVE
 * ══════════════════════════════════════════════════════════════════════════ */

/* Append helpers — write into a heap buffer with a running cursor. */
typedef struct { char *buf; size_t pos; size_t cap; } wr_t;

static void wr_str(wr_t *w, const char *s)
{
    size_t l = strlen(s);
    if (w->pos + l >= w->cap) return;
    memcpy(w->buf + w->pos, s, l);
    w->pos += l;
}

static void wr_fmt(wr_t *w, const char *fmt, ...)
{
    if (w->pos >= w->cap) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(w->buf + w->pos, w->cap - w->pos, fmt, ap);
    va_end(ap);
    if (n > 0) {
        size_t avail = w->cap - w->pos;
        w->pos += (n < (int)avail) ? (size_t)n : avail;
    }
}

/* JSON-escape a string (handles backslash and double-quote only; control
 * chars in paths are not expected on FAT). */
static void wr_json_str(wr_t *w, const char *s)
{
    wr_str(w, "\"");
    for (; *s; s++) {
        if (*s == '"')       wr_str(w, "\\\"");
        else if (*s == '\\') wr_str(w, "\\\\");
        else { char c[2] = {*s, 0}; wr_str(w, c); }
    }
    wr_str(w, "\"");
}

/* ── FX chain serialisation ───────────────────────────────────────────── */
/* fx_node_t stores params internally; we can only re-read them via set_param.
 * Instead, each fx_node carries its current params accessible via a
 * get_params accessor.  Since we don't have get_params yet, we store a
 * snapshot of the params at node creation time in a small side-array.
 *
 * For v1 we save only: type, enabled.  The node is recreated with defaults
 * on load.  This is a conscious v1 trade-off documented in song.h.
 * Full param save/load is a TODO for M6+ when the UI exposes per-FX knobs. */
static void save_fx_chain(wr_t *w, fx_node_t **chain, int count)
{
    wr_str(w, "      \"fx\": [\n");
    for (int i = 0; i < count; i++) {
        fx_node_t *n = chain[i];
        if (!n) continue;
        wr_fmt(w, "        {\"type\": %d, \"enabled\": %s}%s\n",
               (int)n->type,
               n->enabled ? "true" : "false",
               (i < count - 1) ? "," : "");
    }
    wr_str(w, "      ]");
}

/* ── ADSR ─────────────────────────────────────────────────────────────── */
static void save_adsr(wr_t *w, const lane_adsr_t *a)
{
    wr_fmt(w, "      \"adsr\": {\"atk\": %.3f, \"dcy\": %.3f, \"sus\": %.3f, \"rel\": %.3f}",
           (double)a->atk_ms, (double)a->dcy_ms,
           (double)a->sus,    (double)a->rel_ms);
}

/* ── LFO block ────────────────────────────────────────────────────────── */
static void save_lfo(wr_t *w, const lane_lfo_t *ll)
{
    wr_str(w, "      \"lfo\": [\n");
    for (int i = 0; i < NUM_LANE_LFOS; i++) {
        const lfo_t *l = &ll->lfo[i];
        wr_fmt(w, "        {\"en\": %s, \"shape\": %d, \"sync\": %d, "
                  "\"rate\": %.4f, \"depth\": %.4f}%s\n",
               l->enabled ? "true" : "false",
               (int)l->shape, (int)l->sync,
               (double)l->rate_hz, (double)l->depth,
               (i < NUM_LANE_LFOS - 1) ? "," : "");
    }
    wr_str(w, "      ]");
}

/* ── ARP ──────────────────────────────────────────────────────────────── */
static void save_arp(wr_t *w, const arp_t *a)
{
    wr_fmt(w, "      \"arp\": {\"en\": %s, \"mode\": %d, \"oct\": %d, "
              "\"div\": %d, \"gate\": %d, \"vmode\": %d, \"fvel\": %d, "
              "\"swing\": %d, \"latch\": %s, \"retrig\": %s}",
           a->enabled ? "true" : "false",
           (int)a->mode, (int)a->octave_range,
           (int)a->step_div, (int)a->gate_pct,
           (int)a->velocity_mode, (int)a->fixed_vel,
           (int)a->swing_pct,
           a->latch ? "true" : "false",
           a->retrigger ? "true" : "false");
}

/* ── Drum seq ─────────────────────────────────────────────────────────── */
static void save_drum_seq(wr_t *w, const drum_seq_t *seq)
{
    wr_fmt(w, "      \"drum\": {\"step_count\": %d, \"accent_gain\": %.3f, "
              "\"row_count\": %d,\n",
           (int)seq->step_count,
           (double)seq->accent_gain,
           (int)seq->row_count);
    wr_str(w, "        \"rows\": [\n");
    for (int r = 0; r < seq->row_count; r++) {
        const drum_row_t *row = &seq->rows[r];
        wr_str(w, "          {");
        wr_str(w, "\"path\": "); wr_json_str(w, row->wav_path); wr_str(w, ",\n");
        wr_fmt(w, "           \"mode\": %d, \"vol\": %.3f, \"pan\": %.3f, "
                  "\"pitch\": %.3f, \"mute\": %s, \"rev\": %s,\n",
               (int)row->play_mode,
               (double)row->volume, (double)row->pan, (double)row->pitch,
               row->mute ? "true" : "false",
               row->reverse ? "true" : "false");
        wr_fmt(w, "           \"step_count\": %d, \"steps\": [",
               (int)row->step_count);
        for (int s = 0; s < row->step_count; s++) {
            wr_fmt(w, "[%d,%d]%s",
                   (int)row->steps[s].velocity,
                   row->steps[s].accent ? 1 : 0,
                   (s < row->step_count - 1) ? "," : "");
        }
        wr_str(w, "]}");
        wr_str(w, (r < seq->row_count - 1) ? ",\n" : "\n");
    }
    wr_str(w, "        ]}");
}

/* ── Piano roll ───────────────────────────────────────────────────────── */
static void save_piano_roll(wr_t *w, const piano_roll_t *pr)
{
    wr_fmt(w, "      \"piano_roll\": {\"res\": %d, \"note_count\": %d, \"notes\": [",
           (int)pr->step_resolution, (int)pr->note_count);
    for (int i = 0; i < pr->note_count; i++) {
        const pr_note_t *n = &pr->notes[i];
        wr_fmt(w, "[%u,%u,%d,%d]%s",
               n->tick_start, n->tick_len,
               (int)n->note, (int)n->velocity,
               (i < pr->note_count - 1) ? "," : "");
    }
    wr_str(w, "]}");
}

/* ── One lane ─────────────────────────────────────────────────────────── */
static void save_lane(wr_t *w, const lane_t *lane, int idx)
{
    wr_fmt(w, "    {\n"
              "      \"idx\": %d, \"type\": %d, \"active\": %s,\n"
              "      \"mute\": %s, \"vol\": %.4f, \"pan\": %.4f,\n"
              "      \"loop_ticks\": %u,\n",
           idx,
           (int)lane->type,
           lane->active ? "true" : "false",
           lane->mute   ? "true" : "false",
           (double)lane->volume, (double)lane->pan,
           lane->loop_len_ticks);
    if (lane->name[0]) {
        wr_str(w, "      \"lane_name\": ");
        wr_json_str(w, lane->name);
        wr_str(w, ",\n");
    }

    if (lane->type == LANE_TYPE_WAV) {
        wr_str(w, "      \"wav_path\": "); wr_json_str(w, lane->wav_path); wr_str(w, ",\n");
    } else if (lane->type == LANE_TYPE_SYNTH && lane->synth) {
        wr_fmt(w, "      \"synth_type\": %d,\n", (int)lane->synth->type_id);
        /* Save synth params as compact JSON array (indices 0-12 + 20-23) */
        wr_str(w, "      \"sp\": [");
        for (int pi = 0; pi < 13; pi++)
            wr_fmt(w, "%.4f%s", (double)lane->synth_params[pi], pi < 12 ? "," : "");
        wr_str(w, ",");
        for (int pi = 20; pi <= 23; pi++)
            wr_fmt(w, "%.4f%s", (double)lane->synth_params[pi], pi < 23 ? "," : "");
        wr_str(w, "],\n");
        if (lane->piano_roll) {
            save_piano_roll(w, lane->piano_roll);
            wr_str(w, ",\n");
        }
        save_arp(w, &lane->arp);
        wr_str(w, ",\n");
    } else if (lane->type == LANE_TYPE_DRUM && lane->drum_seq) {
        save_drum_seq(w, lane->drum_seq);
        wr_str(w, ",\n");
    }

    save_lfo(w, &lane->lfo); wr_str(w, ",\n");
    save_adsr(w, &lane->adsr); wr_str(w, ",\n");
    save_fx_chain(w, (fx_node_t **)lane->fx, lane->fx_count);
    wr_str(w, "\n    }");
}

/* ── Top-level save ───────────────────────────────────────────────────── */
bool song_save(const char *path)
{
    /* Ensure songs directory exists */
    if (mkdir(SONG_DIR, 0755) != 0 && errno != EEXIST) {
        ESP_LOGW(TAG, "mkdir(%s) errno=%d", SONG_DIR, errno);
    }
    ESP_LOGI(TAG, "song_save: target=%s", path);

    /* Allocate scratch buffer in PSRAM */
    const size_t buf_cap = 256 * 1024;
    char *buf = heap_caps_malloc(buf_cap, MALLOC_CAP_SPIRAM);
    if (!buf) {
        ESP_LOGE(TAG, "song_save: PSRAM alloc failed");
        return false;
    }

    wr_t w = { .buf = buf, .pos = 0, .cap = buf_cap };

    wr_fmt(&w, "{\n  \"version\": 1,\n  \"name\": ");
    wr_json_str(&w, g_song.name);
    wr_fmt(&w, ",\n  \"bpm\": %.2f, \"ppqn\": %u, \"bpb\": %u,\n"
               "  \"playback_mode\": %d,\n  \"lanes\": [\n",
           (double)g_song.clock.bpm,
           g_song.clock.tick_rate,
           g_song.clock.beats_per_bar,
           (int)g_song.playback_mode);

    int saved_lanes = 0;
    for (int i = 0; i < NUM_LANES; i++) {
        if (!g_song.lanes[i].active) continue;
        if (saved_lanes > 0) wr_str(&w, ",\n");
        save_lane(&w, &g_song.lanes[i], i);
        saved_lanes++;
    }

    wr_str(&w, "\n  ]\n}\n");

    if (w.pos >= w.cap) {
        ESP_LOGE(TAG, "song_save: buffer overflow at %zu bytes", w.pos);
        heap_caps_free(buf);
        return false;
    }

    /* Atomic write — size matches caller's path buffer + ".tmp" suffix */
    char tmp_path[168];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);

    FILE *f = fopen(tmp_path, "w");
    if (!f) {
        ESP_LOGE(TAG, "song_save: cannot open %s", tmp_path);
        heap_caps_free(buf);
        return false;
    }
    size_t written = fwrite(buf, 1, w.pos, f);
    fclose(f);
    heap_caps_free(buf);

    if (written != w.pos) {
        ESP_LOGE(TAG, "song_save: short write %zu/%zu", written, w.pos);
        remove(tmp_path);
        return false;
    }
    /* FATFS rename() fails if the destination already exists, so remove the
     * old file first. The window where neither file exists is unavoidable on
     * FATFS — there is no atomic replace. */
    remove(path);
    if (rename(tmp_path, path) != 0) {
        ESP_LOGE(TAG, "song_save: rename failed errno=%d", errno);
        remove(tmp_path);
        return false;
    }

    g_song.dirty = false;
    ESP_LOGI(TAG, "saved %s (%zu bytes)", path, w.pos);
    return true;
}

/* ══════════════════════════════════════════════════════════════════════════
 * LOAD — jsmn helpers
 * ══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    const char   *js;
    jsmntok_t    *t;
    int           n;
} rd_t;

static bool rd_key_eq(const rd_t *rd, int i, const char *key)
{
    const jsmntok_t *t = &rd->t[i];
    int len = t->end - t->start;
    return (t->type == JSMN_STRING &&
            (int)strlen(key) == len &&
            memcmp(rd->js + t->start, key, len) == 0);
}

static void rd_str(const rd_t *rd, int i, char *out, int out_len)
{
    const jsmntok_t *t = &rd->t[i];
    int len = t->end - t->start;
    if (len >= out_len) len = out_len - 1;
    memcpy(out, rd->js + t->start, len);
    out[len] = '\0';
}

static float rd_float(const rd_t *rd, int i)
{
    char buf[32]; rd_str(rd, i, buf, sizeof(buf));
    return strtof(buf, NULL);
}

static int32_t rd_int(const rd_t *rd, int i)
{
    char buf[32]; rd_str(rd, i, buf, sizeof(buf));
    return (int32_t)strtol(buf, NULL, 10);
}

static uint32_t rd_u32(const rd_t *rd, int i)
{
    char buf[32]; rd_str(rd, i, buf, sizeof(buf));
    return (uint32_t)strtoul(buf, NULL, 10);
}

static bool rd_bool(const rd_t *rd, int i)
{
    const jsmntok_t *t = &rd->t[i];
    int len = t->end - t->start;
    return (len == 4 && memcmp(rd->js + t->start, "true", 4) == 0);
}

/* Skip over a token and all its children; returns index of first token after */
static int skip_token(const rd_t *rd, int i)
{
    int end = i + 1;
    if (rd->t[i].type == JSMN_ARRAY || rd->t[i].type == JSMN_OBJECT) {
        int children = rd->t[i].size;
        for (int c = 0; c < children; c++)
            end = skip_token(rd, end);
    }
    return end;
}

/* ── Load piano roll ──────────────────────────────────────────────────── */
/* token i points at the piano_roll object; returns first token after it */
static int load_piano_roll(const rd_t *rd, int i, piano_roll_t *pr)
{
    if (rd->t[i].type != JSMN_OBJECT) return skip_token(rd, i);
    int children = rd->t[i].size;
    i++;
    for (int c = 0; c < children; c++) {
        if (rd_key_eq(rd, i, "res")) {
            pr->step_resolution = (uint8_t)rd_int(rd, i + 1);
            i += 2;
        } else if (rd_key_eq(rd, i, "notes")) {
            i++; /* skip key, now on array */
            int nc = rd->t[i].size;
            i++; /* into array */
            for (int n = 0; n < nc; n++) {
                /* each note is a 4-element array: [tick_start, tick_len, note, vel] */
                if (rd->t[i].type == JSMN_ARRAY && rd->t[i].size == 4) {
                    uint32_t ts  = rd_u32(rd, i + 1);
                    uint32_t tl  = rd_u32(rd, i + 2);
                    uint8_t  nt  = (uint8_t)rd_int(rd, i + 3);
                    uint8_t  vel = (uint8_t)rd_int(rd, i + 4);
                    piano_roll_add_note(pr, ts, tl, nt, vel);
                }
                i = skip_token(rd, i);
            }
        } else if (rd_key_eq(rd, i, "note_count")) {
            i += 2; /* already populated by add_note */
        } else {
            i++;
            i = skip_token(rd, i);
        }
    }
    return i;
}

/* ── Load ARP ─────────────────────────────────────────────────────────── */
static int load_arp(const rd_t *rd, int i, arp_t *a)
{
    if (rd->t[i].type != JSMN_OBJECT) return skip_token(rd, i);
    int children = rd->t[i].size;
    i++;
    for (int c = 0; c < children; c++) {
        if      (rd_key_eq(rd, i, "en"))     { a->enabled        = rd_bool(rd, i+1); i+=2; }
        else if (rd_key_eq(rd, i, "mode"))   { a->mode           = (arp_mode_t)rd_int(rd, i+1); i+=2; }
        else if (rd_key_eq(rd, i, "oct"))    { a->octave_range   = (uint8_t)rd_int(rd, i+1); i+=2; }
        else if (rd_key_eq(rd, i, "div"))    { a->step_div       = (uint8_t)rd_int(rd, i+1); i+=2; }
        else if (rd_key_eq(rd, i, "gate"))   { a->gate_pct       = (uint8_t)rd_int(rd, i+1); i+=2; }
        else if (rd_key_eq(rd, i, "vmode"))  { a->velocity_mode  = (arp_vel_mode_t)rd_int(rd, i+1); i+=2; }
        else if (rd_key_eq(rd, i, "fvel"))   { a->fixed_vel      = (uint8_t)rd_int(rd, i+1); i+=2; }
        else if (rd_key_eq(rd, i, "swing"))  { a->swing_pct      = (uint8_t)rd_int(rd, i+1); i+=2; }
        else if (rd_key_eq(rd, i, "latch"))  { a->latch          = rd_bool(rd, i+1); i+=2; }
        else if (rd_key_eq(rd, i, "retrig")) { a->retrigger      = rd_bool(rd, i+1); i+=2; }
        else { i++; i = skip_token(rd, i); }
    }
    return i;
}

/* ── Load LFO block ───────────────────────────────────────────────────── */
static int load_lfo(const rd_t *rd, int i, lane_lfo_t *ll)
{
    if (rd->t[i].type != JSMN_ARRAY) return skip_token(rd, i);
    int count = rd->t[i].size;
    i++;
    for (int li = 0; li < count && li < NUM_LANE_LFOS; li++) {
        lfo_t *l = &ll->lfo[li];
        if (rd->t[i].type != JSMN_OBJECT) { i = skip_token(rd, i); continue; }
        int ch = rd->t[i].size; i++;
        for (int c = 0; c < ch; c++) {
            if      (rd_key_eq(rd, i, "en"))    { l->enabled  = rd_bool(rd, i+1);              i+=2; }
            else if (rd_key_eq(rd, i, "shape")) { l->shape    = (lfo_shape_t)rd_int(rd, i+1);  i+=2; }
            else if (rd_key_eq(rd, i, "sync"))  { l->sync     = (lfo_sync_t)rd_int(rd, i+1);   i+=2; }
            else if (rd_key_eq(rd, i, "rate"))  { l->rate_hz  = rd_float(rd, i+1);              i+=2; }
            else if (rd_key_eq(rd, i, "depth")) { l->depth    = rd_float(rd, i+1);              i+=2; }
            else { i++; i = skip_token(rd, i); }
        }
    }
    return i;
}

/* ── Load ADSR ────────────────────────────────────────────────────────── */
static int load_adsr(const rd_t *rd, int i, lane_adsr_t *a)
{
    if (rd->t[i].type != JSMN_OBJECT) return skip_token(rd, i);
    int ch = rd->t[i].size; i++;
    float atk = a->atk_ms, dcy = a->dcy_ms, sus = a->sus, rel = a->rel_ms;
    for (int c = 0; c < ch; c++) {
        if      (rd_key_eq(rd, i, "atk")) { atk = rd_float(rd, i+1); i+=2; }
        else if (rd_key_eq(rd, i, "dcy")) { dcy = rd_float(rd, i+1); i+=2; }
        else if (rd_key_eq(rd, i, "sus")) { sus = rd_float(rd, i+1); i+=2; }
        else if (rd_key_eq(rd, i, "rel")) { rel = rd_float(rd, i+1); i+=2; }
        else { i++; i = skip_token(rd, i); }
    }
    lane_adsr_init(a, atk, dcy, sus, rel);
    return i;
}

/* ── Load FX chain ────────────────────────────────────────────────────── */
static int load_fx_chain(const rd_t *rd, int i, lane_t *lane)
{
    if (rd->t[i].type != JSMN_ARRAY) return skip_token(rd, i);
    int count = rd->t[i].size; i++;
    for (int fi = 0; fi < count; fi++) {
        if (rd->t[i].type != JSMN_OBJECT) { i = skip_token(rd, i); continue; }
        int ch = rd->t[i].size; i++;
        fx_type_t type = FX_TYPE_NONE;
        bool enabled = true;
        for (int c = 0; c < ch; c++) {
            if      (rd_key_eq(rd, i, "type"))    { type    = (fx_type_t)rd_int(rd, i+1); i+=2; }
            else if (rd_key_eq(rd, i, "enabled")) { enabled = rd_bool(rd, i+1); i+=2; }
            else { i++; i = skip_token(rd, i); }
        }
        if (type != FX_TYPE_NONE) {
            fx_node_t *node = fx_new(type);
            if (node) {
                node->enabled = enabled;
                fx_chain_insert(lane->fx, &lane->fx_count, lane->fx_count, node);
            }
        }
    }
    return i;
}

/* ── Load drum seq ────────────────────────────────────────────────────── */
static int load_drum_seq(const rd_t *rd, int i, lane_t *lane)
{
    if (rd->t[i].type != JSMN_OBJECT) return skip_token(rd, i);

    drum_seq_t *seq = drum_seq_alloc();
    if (!seq) return skip_token(rd, i);

    int ch = rd->t[i].size; i++;
    for (int c = 0; c < ch; c++) {
        if (rd_key_eq(rd, i, "step_count")) {
            seq->step_count = (uint8_t)rd_int(rd, i+1); i+=2;
        } else if (rd_key_eq(rd, i, "accent_gain")) {
            seq->accent_gain = rd_float(rd, i+1); i+=2;
        } else if (rd_key_eq(rd, i, "row_count")) {
            seq->row_count = (uint8_t)rd_int(rd, i+1); i+=2;
        } else if (rd_key_eq(rd, i, "rows")) {
            i++; /* skip key, now on array */
            int row_arr_count = rd->t[i].size; i++;
            for (int r = 0; r < row_arr_count && r < DRUM_MAX_ROWS; r++) {
                if (rd->t[i].type != JSMN_OBJECT) { i = skip_token(rd, i); continue; }
                drum_row_t *row = &seq->rows[r];
                int rch = rd->t[i].size; i++;
                for (int rc = 0; rc < rch; rc++) {
                    if (rd_key_eq(rd, i, "path")) {
                        rd_str(rd, i+1, row->wav_path, sizeof(row->wav_path)); i+=2;
                    } else if (rd_key_eq(rd, i, "mode")) {
                        row->play_mode = (sample_play_mode_t)rd_int(rd, i+1); i+=2;
                    } else if (rd_key_eq(rd, i, "vol")) {
                        row->volume = rd_float(rd, i+1); i+=2;
                    } else if (rd_key_eq(rd, i, "pan")) {
                        row->pan = rd_float(rd, i+1); i+=2;
                    } else if (rd_key_eq(rd, i, "pitch")) {
                        row->pitch = rd_float(rd, i+1); i+=2;
                    } else if (rd_key_eq(rd, i, "mute")) {
                        row->mute = rd_bool(rd, i+1); i+=2;
                    } else if (rd_key_eq(rd, i, "rev")) {
                        row->reverse = rd_bool(rd, i+1); i+=2;
                    } else if (rd_key_eq(rd, i, "step_count")) {
                        row->step_count = (uint8_t)rd_int(rd, i+1); i+=2;
                    } else if (rd_key_eq(rd, i, "steps")) {
                        i++; /* key → array */
                        int sc = rd->t[i].size; i++;
                        /* Use array size as authoritative step count if not yet set */
                        if (row->step_count == 0) row->step_count = (uint8_t)(sc < DRUM_MAX_STEPS ? sc : DRUM_MAX_STEPS);
                        for (int s = 0; s < sc && s < DRUM_MAX_STEPS; s++) {
                            if (rd->t[i].type == JSMN_ARRAY && rd->t[i].size >= 2) {
                                row->steps[s].velocity    = (uint8_t)rd_int(rd, i+1);
                                row->steps[s].accent      = (bool)rd_int(rd, i+2);
                                row->steps[s].probability = 100;
                                row->steps[s].ratchet     = 1;
                                row->steps[s].condition   = STEP_COND_ALWAYS;
                            }
                            i = skip_token(rd, i);
                        }
                    } else {
                        i++; i = skip_token(rd, i);
                    }
                }
            }
        } else {
            i++; i = skip_token(rd, i);
        }
    }

    lane->drum_seq = seq;
    if (seq->step_count == 0) seq->step_count = 16;
    if (seq->accent_gain == 0.0f) seq->accent_gain = 1.5f;
    drum_seq_update_timing(seq, lane->loop_len_ticks);

    /* Open WAV files for any rows that have a path saved */
    for (int r = 0; r < seq->row_count; r++) {
        drum_row_t *row = &seq->rows[r];
        if (row->wav_path[0]) {
            char full_path[280];
            snprintf(full_path, sizeof(full_path), "/sdcard/%s", row->wav_path);
            drum_row_load_wav(row, full_path);
        }
    }
    return i;
}

/* ── Load one lane object ─────────────────────────────────────────────── */
static int load_lane(const rd_t *rd, int i, song_t *song)
{
    if (rd->t[i].type != JSMN_OBJECT) return skip_token(rd, i);
    int ch = rd->t[i].size; i++;

    int lane_idx = -1;
    /* first pass: get idx */
    {
        int ii = i;
        for (int c = 0; c < ch; c++) {
            if (rd_key_eq(rd, ii, "idx")) { lane_idx = rd_int(rd, ii+1); break; }
            ii++; ii = skip_token(rd, ii);
        }
    }
    if (lane_idx < 0 || lane_idx >= NUM_LANES) {
        for (int c = 0; c < ch; c++) { i++; i = skip_token(rd, i); }
        return i;
    }

    lane_t *lane = &song->lanes[lane_idx];
    lane_reset(lane);

    for (int c = 0; c < ch; c++) {
        if (rd_key_eq(rd, i, "idx"))          { i+=2; }
        else if (rd_key_eq(rd, i, "type"))    { lane->type         = (lane_type_t)rd_int(rd, i+1); i+=2; }
        else if (rd_key_eq(rd, i, "active"))  { lane->active       = rd_bool(rd, i+1); i+=2; }
        else if (rd_key_eq(rd, i, "mute"))    { lane->mute         = rd_bool(rd, i+1); i+=2; }
        else if (rd_key_eq(rd, i, "vol"))     { lane->volume       = rd_float(rd, i+1); i+=2; }
        else if (rd_key_eq(rd, i, "pan"))     { lane->pan          = rd_float(rd, i+1); i+=2; }
        else if (rd_key_eq(rd, i, "loop_ticks")) { lane->loop_len_ticks = rd_u32(rd, i+1); i+=2; }
        else if (rd_key_eq(rd, i, "lane_name")) {
            rd_str(rd, i+1, lane->name, sizeof(lane->name)); i+=2;
        } else if (rd_key_eq(rd, i, "wav_path")) {
            rd_str(rd, i+1, lane->wav_path, sizeof(lane->wav_path)); i+=2;
        } else if (rd_key_eq(rd, i, "synth_type")) {
            uint8_t st = (uint8_t)rd_int(rd, i+1);
            lane->synth = synth_new(st);
            i+=2;
        } else if (rd_key_eq(rd, i, "sp")) {
            /* synth param array: [0-12 standard, 20-23 extra] stored as 17 floats */
            i++;
            if (rd->t[i].type == JSMN_ARRAY) {
                int sp_count = rd->t[i].size; i++;
                static const int SP_IDS[17] = {0,1,2,3,4,5,6,7,8,9,10,11,12,20,21,22,23};
                for (int pi = 0; pi < sp_count && pi < 17; pi++) {
                    int sid = SP_IDS[pi];
                    lane->synth_params[sid] = rd_float(rd, i); i++;
                }
            } else {
                i = skip_token(rd, i);
            }
        } else if (rd_key_eq(rd, i, "piano_roll")) {
            lane->piano_roll = piano_roll_alloc();
            if (lane->piano_roll) { i++; i = load_piano_roll(rd, i, lane->piano_roll); }
            else { i++; i = skip_token(rd, i); }
        } else if (rd_key_eq(rd, i, "arp")) {
            i++; i = load_arp(rd, i, &lane->arp);
        } else if (rd_key_eq(rd, i, "drum")) {
            i++; i = load_drum_seq(rd, i, lane);
        } else if (rd_key_eq(rd, i, "lfo")) {
            i++; i = load_lfo(rd, i, &lane->lfo);
        } else if (rd_key_eq(rd, i, "adsr")) {
            i++; i = load_adsr(rd, i, &lane->adsr);
        } else if (rd_key_eq(rd, i, "fx")) {
            i++; i = load_fx_chain(rd, i, lane);
        } else {
            i++; i = skip_token(rd, i);
        }
    }
    /* Apply synth_params to synth now that all keys are loaded */
    if (lane->synth) {
        static const int SP_APPLY[17] = {0,1,2,3,4,5,6,7,8,9,10,11,12,20,21,22,23};
        for (int pi = 0; pi < 17; pi++) {
            int sid = SP_APPLY[pi];
            if (sid < 24)
                lane->synth->set_param(lane->synth, (uint8_t)sid,
                                       lane->synth_params[sid]);
        }
    }
    /* Drum-synth lanes: pattern persistence is a follow-up; rebuild the
     * default 808 kit so a reloaded lane is still playable. */
    if (lane->type == LANE_TYPE_DRUMSYNTH && !lane->dsyn) {
        lane->dsyn = dsyn_alloc();
        if (lane->dsyn) {
            dsyn_update_timing(lane->dsyn, lane->loop_len_ticks);
            dsyn_reset(lane->dsyn, lane->lane_tick);
        }
    }
    /* Open WAV file for WAV lanes that have a path */
    if (lane->type == LANE_TYPE_WAV && lane->wav_path[0]) {
        if (lane->wav_lane_slot < 0) {
            lane->wav_lane_slot = wav_lane_alloc_slot();
            if (lane->wav_lane_slot >= 0)
                lane->wav_lane = wav_lane_get(lane->wav_lane_slot);
        }
        if (lane->wav_lane_slot >= 0) {
            char fp[280];
            snprintf(fp, sizeof(fp), "/sdcard/%s", lane->wav_path);
            wav_lane_open(lane->wav_lane_slot, fp);
            if (lane->wav_lane) lane->wav_lane->active = true;
        }
    }
    return i;
}

/* ── Top-level load ───────────────────────────────────────────────────── */
bool song_load(const char *path)
{
    /* Read file into PSRAM buffer */
    const size_t buf_cap = 256 * 1024;
    char *buf = heap_caps_malloc(buf_cap, MALLOC_CAP_SPIRAM);
    if (!buf) {
        ESP_LOGE(TAG, "song_load: PSRAM alloc failed");
        return false;
    }

    FILE *f = fopen(path, "r");
    if (!f) {
        ESP_LOGW(TAG, "song_load: file not found: %s", path);
        heap_caps_free(buf);
        return false;
    }
    size_t n = fread(buf, 1, buf_cap - 1, f);
    fclose(f);
    if (n == 0) {
        heap_caps_free(buf);
        return false;
    }
    buf[n] = '\0';

    /* Tokenise — 16 drum lanes × 16 rows × 64 steps × 3 tokens/step ≈ 49k tokens */
    const int MAX_TOKENS = 65536;
    jsmntok_t *tokens = heap_caps_malloc(MAX_TOKENS * sizeof(jsmntok_t), MALLOC_CAP_SPIRAM);
    if (!tokens) {
        ESP_LOGE(TAG, "song_load: token alloc failed");
        heap_caps_free(buf);
        return false;
    }

    jsmn_parser p;
    jsmn_init(&p);
    int r = jsmn_parse(&p, buf, n, tokens, MAX_TOKENS);
    if (r == JSMN_ERROR_NOMEM) {
        ESP_LOGE(TAG, "song_load: too many tokens (file too large or corrupt?)");
        heap_caps_free(tokens);
        heap_caps_free(buf);
        return false;
    }
    if (r < 1 || tokens[0].type != JSMN_OBJECT) {
        ESP_LOGW(TAG, "song_load: parse failed (%d)", r);
        heap_caps_free(tokens);
        heap_caps_free(buf);
        return false;
    }

    rd_t rd = { .js = buf, .t = tokens, .n = r };

    /* Reset song state. lane_init_all() deactivates+clears every lane first, so
     * the audio task stops touching the old rings; then wav_pool_reset() frees
     * their preload buffers and rewinds the slot allocator so this load reuses
     * the pool instead of leaking it. */
    lane_init_all(&g_song);
    wav_pool_reset();

    /* Parse top-level object */
    int ch = tokens[0].size;
    int i  = 1;
    for (int c = 0; c < ch; c++) {
        if (rd_key_eq(&rd, i, "name")) {
            rd_str(&rd, i+1, g_song.name, sizeof(g_song.name)); i+=2;
        } else if (rd_key_eq(&rd, i, "bpm")) {
            clock_set_bpm(&g_song.clock, rd_float(&rd, i+1)); i+=2;
        } else if (rd_key_eq(&rd, i, "ppqn")) {
            g_song.clock.tick_rate = rd_u32(&rd, i+1); i+=2;
        } else if (rd_key_eq(&rd, i, "bpb")) {
            g_song.clock.beats_per_bar = rd_u32(&rd, i+1); i+=2;
        } else if (rd_key_eq(&rd, i, "playback_mode")) {
            g_song.playback_mode = (playback_mode_t)rd_int(&rd, i+1); i+=2;
        } else if (rd_key_eq(&rd, i, "lanes")) {
            i++; /* key → array */
            int lane_count = tokens[i].size; i++;
            for (int l = 0; l < lane_count; l++)
                i = load_lane(&rd, i, &g_song);
        } else {
            i++; i = skip_token(&rd, i);
        }
    }

    heap_caps_free(tokens);
    heap_caps_free(buf);

    g_song.dirty = false;
    ESP_LOGI(TAG, "loaded %s", path);
    return true;
}

/* ── Versioned save ──────────────────────────────────────────────────────── */

bool song_save_versioned(char *out_path, size_t out_len)
{
    /* Find next available _NNN suffix */
    char base[64];
    strncpy(base, g_song.name[0] ? g_song.name : "Untitled", sizeof(base) - 1);
    base[sizeof(base) - 1] = '\0';
    /* Strip existing _NNN suffix if present */
    int blen = (int)strlen(base);
    if (blen > 4 && base[blen-4] == '_') {
        bool all_digits = true;
        for (int i = blen-3; i < blen; i++)
            if (base[i] < '0' || base[i] > '9') { all_digits = false; break; }
        if (all_digits) base[blen-4] = '\0';
    }
    for (int n = 1; n <= 999; n++) {
        snprintf(out_path, out_len, "%s/%s_%03d%s", SONG_DIR, base, n, SONG_EXT);
        FILE *f = fopen(out_path, "r");
        if (!f) break;
        fclose(f);
    }
    return song_save(out_path);
}

/* ── Template save/load ─────────────────────────────────────────────────── */

bool song_save_template(const char *name)
{
    char dir[128];
    snprintf(dir, sizeof(dir), "%s/templates", SONG_DIR);
    mkdir(dir, 0755);
    char path[160];
    snprintf(path, sizeof(path), "%s/%s%s", dir, name, SONG_EXT);
    /* Save normally — template is just a song saved to a different folder */
    return song_save(path);
}

bool song_load_template(const char *name)
{
    char path[160];
    snprintf(path, sizeof(path), "%s/templates/%s%s", SONG_DIR, name, SONG_EXT);
    if (!song_load(path)) return false;
    /* Clear all pattern/note data, keep lane types and FX */
    for (int i = 0; i < NUM_LANES; i++) {
        lane_t *lane = &g_song.lanes[i];
        if (lane->type == LANE_TYPE_DRUM && lane->drum_seq) {
            for (int r = 0; r < lane->drum_seq->row_count; r++) {
                memset(lane->drum_seq->rows[r].steps, 0,
                       sizeof(lane->drum_seq->rows[r].steps));
            }
        } else if (lane->type == LANE_TYPE_SYNTH && lane->piano_roll) {
            piano_roll_reset(lane->piano_roll);
        }
    }
    g_song.dirty = true;
    return true;
}

/* ── Auto-save ───────────────────────────────────────────────────────────── */

void song_autosave_tick(uint32_t master_bar)
{
    if (!g_song.dirty) return;
    if (g_song.autosave_interval == 0) return;
    if (master_bar - g_song.autosave_bar < g_song.autosave_interval) return;

    char path[160];
    snprintf(path, sizeof(path), "%s/.autosave%s", SONG_DIR, SONG_EXT);
    if (song_save(path)) {
        g_song.autosave_bar = master_bar;
        g_song.dirty = true; /* keep dirty — not the canonical save */
    }
}

/* ── Time-based autosave task (core 0, never blocks audio) ──────────────── */
/* Saves the canonical song path at most once per 60 s when g_song.dirty.
 * Skips unnamed songs (name empty or "Untitled") to avoid saving garbage. */
#define AUTOSAVE_DEBOUNCE_MS  60000   /* 60 s */

static void song_autosave_task(void *arg)
{
    (void)arg;
    int64_t last_save_ms = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000)); /* check every 5 s, light on CPU */
        if (!g_song.dirty) continue;
        if (!g_song.name[0] || strcmp(g_song.name, "Untitled") == 0) continue;

        int64_t now_ms = esp_timer_get_time() / 1000;
        if (now_ms - last_save_ms < AUTOSAVE_DEBOUNCE_MS) continue;

        char path[160];
        snprintf(path, sizeof(path), "%s/%s%s", SONG_DIR, g_song.name, SONG_EXT);
        if (song_save(path)) {
            last_save_ms = now_ms;
            /* Update last_song in settings but don't call settings_save()
             * here — keep this task SD-write minimal. */
            strncpy(g_settings.last_song, path, sizeof(g_settings.last_song) - 1);
            ESP_LOGI(TAG, "autosaved: %s", path);
        }
    }
}

void song_autosave_task_start(void)
{
    xTaskCreatePinnedToCore(song_autosave_task, "autosave", 4096, NULL, 3, NULL, 0);
}
