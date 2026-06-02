/*
 * ws_server.c — WebSocket server implementation.
 *
 * Registers /ws with the IDF HTTP server.  Up to WS_MAX_CLIENTS concurrent
 * connections.  On connect: sends full state dump.  On message: delegates to
 * ws_cmd_dispatch().  Periodic timers broadcast meter (10 Hz) and playhead
 * (25 Hz) to all active clients.
 *
 * ws_notify_change() is the outbound interface used by both the touch UI
 * (ui.c) and the command dispatcher (ws_cmd.c) after any mutation.
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_http_server.h"
#include "lane.h"
#include "clock.h"
#include "settings.h"
#include "song.h"
#include "audio.h"
#include "drum_seq.h"
#include "piano_roll.h"
#include "fx.h"
#include "ws_server.h"
#include "ws_cmd.h"
#include "ws_audio.h"

static const char *TAG = "ws_server";

/* ── Client slot table ───────────────────────────────────────────────────── */
#define WS_MAX_CLIENTS 4

typedef struct {
    int  fd;
    bool active;
} ws_client_t;

static ws_client_t   s_clients[WS_MAX_CLIENTS];
static SemaphoreHandle_t s_clients_mx;   /* protects s_clients[] */
static httpd_handle_t    s_server = NULL;

/* ── Frame send helpers ──────────────────────────────────────────────────── */

static void ws_send_to(int fd, const uint8_t *data, size_t len)
{
    httpd_ws_frame_t frm = {
        .type       = HTTPD_WS_TYPE_BINARY,
        .payload    = (uint8_t *)data,
        .len        = len,
        .final      = true,
        .fragmented = false,
    };
    esp_err_t err = httpd_ws_send_frame_async(s_server, fd, &frm);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "send fd=%d err=%d", fd, err);
        /* Mark client inactive — will be cleaned up on next broadcast */
        xSemaphoreTake(s_clients_mx, portMAX_DELAY);
        for (int i = 0; i < WS_MAX_CLIENTS; i++) {
            if (s_clients[i].fd == fd) {
                s_clients[i].active = false;
                break;
            }
        }
        xSemaphoreGive(s_clients_mx);
    }
}

static void ws_broadcast(const uint8_t *data, size_t len)
{
    /* The touch UI/audio tasks can call this before ws_server_register() has
     * created the mutex (e.g. a screen tap during boot, before the network is
     * up). No server means no browser clients, so just drop the notification. */
    if (!s_clients_mx) return;
    xSemaphoreTake(s_clients_mx, portMAX_DELAY);
    for (int i = 0; i < WS_MAX_CLIENTS; i++) {
        if (s_clients[i].active) {
            int fd = s_clients[i].fd;
            xSemaphoreGive(s_clients_mx);
            ws_send_to(fd, data, len);
            xSemaphoreTake(s_clients_mx, portMAX_DELAY);
        }
    }
    xSemaphoreGive(s_clients_mx);
}

/* Like ws_broadcast but skips one fd (the command sender). */
static void ws_broadcast_except(const uint8_t *data, size_t len, int skip_fd)
{
    if (!s_clients_mx) return;
    xSemaphoreTake(s_clients_mx, portMAX_DELAY);
    for (int i = 0; i < WS_MAX_CLIENTS; i++) {
        if (s_clients[i].active && s_clients[i].fd != skip_fd) {
            int fd = s_clients[i].fd;
            xSemaphoreGive(s_clients_mx);
            ws_send_to(fd, data, len);
            xSemaphoreTake(s_clients_mx, portMAX_DELAY);
        }
    }
    xSemaphoreGive(s_clients_mx);
}

void ws_broadcast_raw(const uint8_t *data, size_t len)
{
    ws_broadcast(data, len);
}

/* ── Serialisers ─────────────────────────────────────────────────────────── */

/*
 * All frames: first byte = ws_msg_type_t, rest = payload.
 * Fixed-size binary encoding, no JSON overhead.
 */

/* WS_MSG_CLOCK_UPDATE  [1 + 12 bytes]
 *   bpm     f32
 *   ppqn    u32
 *   beats   u32  (beats_per_bar)
 */
static size_t ser_clock(uint8_t *buf)
{
    buf[0] = WS_MSG_CLOCK_UPDATE;
    float bpm = g_song.clock.bpm;
    uint32_t ppqn  = g_song.clock.tick_rate;
    uint32_t beats = g_song.clock.beats_per_bar;
    memcpy(buf + 1,  &bpm,   4);
    memcpy(buf + 5,  &ppqn,  4);
    memcpy(buf + 9,  &beats, 4);
    return 13;
}

/* WS_MSG_TRANSPORT  [1 + 1 byte]
 *   running  u8 (0/1)
 */
static size_t ser_transport(uint8_t *buf)
{
    buf[0] = WS_MSG_TRANSPORT;
    buf[1] = g_song.clock.running ? 1 : 0;
    return 2;
}

/* WS_MSG_LANE_UPDATE  [1 + lane payload bytes]
 *   lane_idx   u8
 *   active     u8
 *   type       u8
 *   mute       u8
 *   solo       u8
 *   volume     f32
 *   pan        f32
 *   loop_len   u32  (loop_len_ticks)
 *   wav_path   [128] (null-padded)
 */
static size_t ser_lane(uint8_t *buf, int li)
{
    const lane_t *l = &g_song.lanes[li];
    buf[0] = WS_MSG_LANE_UPDATE;
    buf[1] = (uint8_t)li;
    buf[2] = l->active ? 1 : 0;
    buf[3] = (uint8_t)l->type;
    buf[4] = l->mute   ? 1 : 0;
    buf[5] = l->solo   ? 1 : 0;
    memcpy(buf + 6,  &l->volume,         4);
    memcpy(buf + 10, &l->pan,            4);
    memcpy(buf + 14, &l->loop_len_ticks, 4);
    memset(buf + 18, 0, 128);
    strncpy((char *)(buf + 18), l->wav_path[0] ? l->wav_path : "", 127);
    return 146;
}

/* WS_MSG_DRUM_STEP  [1 + 1 + 1 + 1 + DRUM_MAX_STEPS bytes]
 *   lane_idx   u8
 *   row_idx    u8
 *   step_count u8
 *   steps[64]  u8  (velocity, 0 = off)
 */
static size_t ser_drum_row(uint8_t *buf, int li, int ri)
{
    const lane_t *l = &g_song.lanes[li];
    if (!l->drum_seq || ri >= l->drum_seq->row_count) return 0;
    const drum_row_t *row = &l->drum_seq->rows[ri];
    buf[0] = WS_MSG_DRUM_STEP;
    buf[1] = (uint8_t)li;
    buf[2] = (uint8_t)ri;
    buf[3] = row->step_count;
    for (int s = 0; s < DRUM_MAX_STEPS; s++)
        buf[4 + s] = (s < row->step_count) ? row->steps[s].velocity : 0;
    /* Extended: mute(u8) + volume(f32) after steps */
    size_t ext = 4 + DRUM_MAX_STEPS;
    buf[ext] = row->mute ? 1 : 0;
    memcpy(buf + ext + 1, &row->volume, 4);
    return ext + 5;
}

/* WS_MSG_DSYN_ROW  — full state for one 808 drum-synth row.
 *   [0] msg  [1] li  [2] ri  [3] voice  [4] step_count  [5] row_count
 *   [6 ..33] 7 × f32 knobs: tune,decay,tone,snap,drive,level,pan
 *   [34..97] velocity[64]   [98..161] accent[64]
 */
static size_t ser_dsyn_row(uint8_t *buf, int li, int ri)
{
    const lane_t *l = &g_song.lanes[li];
    if (!l->dsyn || ri >= l->dsyn->row_count) return 0;
    const dsyn_t        *ds = l->dsyn;
    const dsyn_params_t *p  = &ds->params[ri];
    buf[0] = WS_MSG_DSYN_ROW;
    buf[1] = (uint8_t)li;
    buf[2] = (uint8_t)ri;
    buf[3] = (uint8_t)p->type;
    buf[4] = ds->step_count;
    buf[5] = ds->row_count;
    const float knobs[7] = { p->tune, p->decay, p->tone, p->snap,
                             p->drive, p->level, p->pan };
    memcpy(buf + 6, knobs, sizeof(knobs));
    size_t vo = 6 + sizeof(knobs);          /* 34 */
    size_t ao = vo + DSYN_MAX_STEPS;        /* 98 */
    for (int s = 0; s < DSYN_MAX_STEPS; s++) {
        buf[vo + s] = ds->steps[ri][s].velocity;
        buf[ao + s] = ds->steps[ri][s].accent ? 1 : 0;
    }
    return ao + DSYN_MAX_STEPS;             /* 162 */
}

/* WS_MSG_FX_UPDATE  [1 + 1 + 1 + 1 + 1 + 8*4 bytes per slot]
 * Sends all active FX slots for one lane as a sequence of fixed-size records:
 *   msg_type   u8    WS_MSG_FX_UPDATE
 *   lane_idx   u8
 *   fx_count   u8    number of records that follow
 *   per-slot:
 *     slot_idx u8
 *     fx_type  u8
 *     enabled  u8
 *     params   f32 × 8  (32 bytes) — current param values, 0 if unused
 */
#define FX_SER_SLOT_BYTES  (1 + 1 + 1 + 32)   /* slot + type + en + 8 params */

static size_t ser_fx_chain(uint8_t *buf, int li)
{
    const lane_t *l = &g_song.lanes[li];
    buf[0] = WS_MSG_FX_UPDATE;
    buf[1] = (uint8_t)li;
    buf[2] = (uint8_t)l->fx_count;
    size_t off = 3;
    for (int s = 0; s < l->fx_count && s < FX_MAX_PER_LANE; s++) {
        const fx_node_t *n = l->fx[s];
        if (!n) {
            memset(buf + off, 0, FX_SER_SLOT_BYTES);
            off += FX_SER_SLOT_BYTES;
            continue;
        }
        buf[off + 0] = (uint8_t)s;
        buf[off + 1] = (uint8_t)n->type;
        buf[off + 2] = n->enabled ? 1 : 0;
        memcpy(buf + off + 3, n->params, 32);
        off += FX_SER_SLOT_BYTES;
    }
    return off;
}

/* WS_MSG_FX_UPDATE for master bus (lane_idx = 0xFF) */
static size_t ser_master_fx_chain(uint8_t *buf)
{
    buf[0] = WS_MSG_FX_UPDATE;
    buf[1] = 0xFF;  /* sentinel: master bus */
    buf[2] = (uint8_t)g_song.master_fx_count;
    size_t off = 3;
    for (int s = 0; s < g_song.master_fx_count && s < FX_MAX_PER_LANE; s++) {
        const fx_node_t *n = g_song.master_fx[s];
        if (!n) { memset(buf + off, 0, FX_SER_SLOT_BYTES); off += FX_SER_SLOT_BYTES; continue; }
        buf[off + 0] = (uint8_t)s;
        buf[off + 1] = (uint8_t)n->type;
        buf[off + 2] = n->enabled ? 1 : 0;
        memcpy(buf + off + 3, n->params, 32);
        off += FX_SER_SLOT_BYTES;
    }
    return off;
}

/* WS_MSG_FX_UPDATE for send bus (lane_idx = 0xFE) */
static size_t ser_send_fx_chain(uint8_t *buf)
{
    buf[0] = WS_MSG_FX_UPDATE;
    buf[1] = 0xFE;  /* sentinel: send bus */
    buf[2] = (uint8_t)g_song.send_fx_count;
    size_t off = 3;
    for (int s = 0; s < g_song.send_fx_count && s < FX_MAX_PER_LANE; s++) {
        const fx_node_t *n = g_song.send_fx[s];
        if (!n) { memset(buf + off, 0, FX_SER_SLOT_BYTES); off += FX_SER_SLOT_BYTES; continue; }
        buf[off + 0] = (uint8_t)s;
        buf[off + 1] = (uint8_t)n->type;
        buf[off + 2] = n->enabled ? 1 : 0;
        memcpy(buf + off + 3, n->params, 32);
        off += FX_SER_SLOT_BYTES;
    }
    return off;
}

/* WS_MSG_METER  [1 + 8 bytes]
 *   peak_l f32, peak_r f32
 */
static size_t ser_meter(uint8_t *buf)
{
    buf[0] = WS_MSG_METER;
    float pl = 0.0f, pr_f = 0.0f;
    /* Sum lane bus values as a rough approximation */
    for (int i = 0; i < NUM_LANES; i++) {
        const lane_t *l = &g_song.lanes[i];
        if (!l->active || l->mute) continue;
        float vl = (float)l->bus_l / 32768.0f;
        float vr = (float)l->bus_r / 32768.0f;
        if (vl < 0) vl = -vl;
        if (vr < 0) vr = -vr;
        if (vl > pl) pl = vl;
        if (vr > pr_f) pr_f = vr;
    }
    memcpy(buf + 1, &pl,   4);
    memcpy(buf + 5, &pr_f, 4);
    return 9;
}

/* WS_MSG_PLAYHEAD  [1 + 4 + NUM_LANES bytes]
 *   master_tick  u32
 *   step[16]     u8   (current_step per lane, 0xFF if not drum)
 */
static size_t ser_playhead(uint8_t *buf)
{
    buf[0] = WS_MSG_PLAYHEAD;
    uint32_t tick = g_song.clock.tick_count;
    memcpy(buf + 1, &tick, 4);
    for (int i = 0; i < NUM_LANES; i++) {
        const lane_t *l = &g_song.lanes[i];
        buf[5 + i] = (l->active && l->drum_seq) ? l->drum_seq->current_step : 0xFF;
    }
    return 5 + NUM_LANES;
}

/* WS_MSG_FULL_STATE  — concatenate all known message types */
static void send_full_state(int fd)
{
    uint8_t buf[512];
    size_t n;

    n = ser_clock(buf);
    ws_send_to(fd, buf, n);

    n = ser_transport(buf);
    ws_send_to(fd, buf, n);

    /* Settings: playback_mode + default_bpm */
    buf[0] = WS_MSG_SETTINGS;
    buf[1] = (uint8_t)g_song.playback_mode;
    float def_bpm = g_settings.bpm;
    memcpy(buf + 2, &def_bpm, 4);
    ws_send_to(fd, buf, 6);

    /* Master update: type byte + vol f32 + pan f32 */
    buf[0] = WS_MSG_MASTER_UPDATE;
    float mv = g_settings.master_volume, mp = g_settings.master_pan;
    memcpy(buf + 1, &mv, 4);
    memcpy(buf + 5, &mp, 4);
    ws_send_to(fd, buf, 9);

    for (int i = 0; i < NUM_LANES; i++) {
        if (!g_song.lanes[i].active) continue;
        n = ser_lane(buf, i);
        ws_send_to(fd, buf, n);

        if (g_song.lanes[i].fx_count > 0) {
            n = ser_fx_chain(buf, i);
            if (n) ws_send_to(fd, buf, n);
        }

        if (g_song.lanes[i].drum_seq) {
            for (int r = 0; r < g_song.lanes[i].drum_seq->row_count; r++) {
                n = ser_drum_row(buf, i, r);
                if (n) ws_send_to(fd, buf, n);
            }
        }

        if (g_song.lanes[i].dsyn) {
            for (int r = 0; r < g_song.lanes[i].dsyn->row_count; r++) {
                n = ser_dsyn_row(buf, i, r);
                if (n) ws_send_to(fd, buf, n);
            }
        }
    }

    /* Master FX chain */
    if (g_song.master_fx_count > 0) {
        n = ser_master_fx_chain(buf);
        if (n) ws_send_to(fd, buf, n);
    }

    /* Send-bus FX chain */
    if (g_song.send_fx_count > 0) {
        n = ser_send_fx_chain(buf);
        if (n) ws_send_to(fd, buf, n);
    }

    /* Send a FULL_STATE marker so the client knows we're done */
    buf[0] = WS_MSG_FULL_STATE;
    ws_send_to(fd, buf, 1);
}

/* ── ws_send_to_fd (public) ──────────────────────────────────────────────── */

bool ws_send_to_fd(int fd, const uint8_t *data, size_t len)
{
    if (!s_clients_mx) return false;
    xSemaphoreTake(s_clients_mx, portMAX_DELAY);
    bool found = false;
    for (int i = 0; i < WS_MAX_CLIENTS; i++) {
        if (s_clients[i].active && s_clients[i].fd == fd) {
            found = true;
            break;
        }
    }
    xSemaphoreGive(s_clients_mx);
    if (!found) return false;
    ws_send_to(fd, data, len);
    return true;
}

/* ── ws_notify_change (public, callable from any task) ───────────────────── */

void ws_notify_change(ws_msg_type_t type, int idx)
{
    uint8_t buf[512];
    size_t  n = 0;

    switch (type) {
    case WS_MSG_CLOCK_UPDATE:  n = ser_clock(buf);           break;
    case WS_MSG_TRANSPORT:     n = ser_transport(buf);        break;
    case WS_MSG_LANE_UPDATE:
        if (idx >= 0 && idx < NUM_LANES) n = ser_lane(buf, idx);
        break;
    case WS_MSG_DRUM_STEP:
        /* idx encodes lane*256 + row */
        if (idx >= 0) {
            int li = (idx >> 8) & 0xFF;
            int ri =  idx       & 0xFF;
            n = ser_drum_row(buf, li, ri);
        }
        break;
    case WS_MSG_DSYN_ROW:
        /* idx encodes lane*256 + row */
        if (idx >= 0) {
            int li = (idx >> 8) & 0xFF;
            int ri =  idx       & 0xFF;
            n = ser_dsyn_row(buf, li, ri);
        }
        break;
    case WS_MSG_FX_UPDATE:
        if (idx == 0xFF) n = ser_master_fx_chain(buf);
        else if (idx == 0xFE) n = ser_send_fx_chain(buf);
        else if (idx >= 0 && idx < NUM_LANES) n = ser_fx_chain(buf, idx);
        break;
    case WS_MSG_METER:    n = ser_meter(buf);    break;
    case WS_MSG_PLAYHEAD: n = ser_playhead(buf); break;
    case WS_MSG_MASTER_UPDATE: {
        buf[0] = WS_MSG_MASTER_UPDATE;
        float mv = g_settings.master_volume, mp = g_settings.master_pan;
        memcpy(buf + 1, &mv, 4);
        memcpy(buf + 5, &mp, 4);
        n = 9;
        break;
    }
    case WS_MSG_SETTINGS: {
        buf[0] = WS_MSG_SETTINGS;
        buf[1] = (uint8_t)g_song.playback_mode;  /* 0=LIVE, 1=SONG */
        float def_bpm = g_settings.bpm;
        memcpy(buf + 2, &def_bpm, 4);
        n = 6;
        break;
    }
    default:
        /* For types not yet serialised, send just the type byte as a hint */
        buf[0] = (uint8_t)type;
        n = 1;
        break;
    }

    if (n) ws_broadcast(buf, n);
}

/* Like ws_notify_change but skips one fd — use from command handlers so the
 * sender doesn't receive its own echo (prevents slow-motion slider effect). */
void ws_notify_except(ws_msg_type_t type, int idx, int skip_fd)
{
    uint8_t buf[512];
    size_t  n = 0;

    switch (type) {
    case WS_MSG_CLOCK_UPDATE:  n = ser_clock(buf);           break;
    case WS_MSG_TRANSPORT:     n = ser_transport(buf);        break;
    case WS_MSG_LANE_UPDATE:
        if (idx >= 0 && idx < NUM_LANES) n = ser_lane(buf, idx);
        break;
    case WS_MSG_DRUM_STEP:
        if (idx >= 0) {
            int li = (idx >> 8) & 0xFF;
            int ri =  idx       & 0xFF;
            n = ser_drum_row(buf, li, ri);
        }
        break;
    case WS_MSG_FX_UPDATE:
        if (idx == 0xFF) n = ser_master_fx_chain(buf);
        else if (idx == 0xFE) n = ser_send_fx_chain(buf);
        else if (idx >= 0 && idx < NUM_LANES) n = ser_fx_chain(buf, idx);
        break;
    case WS_MSG_MASTER_UPDATE: {
        buf[0] = WS_MSG_MASTER_UPDATE;
        float mv = g_settings.master_volume, mp = g_settings.master_pan;
        memcpy(buf + 1, &mv, 4);
        memcpy(buf + 5, &mp, 4);
        n = 9;
        break;
    }
    case WS_MSG_SETTINGS: {
        buf[0] = WS_MSG_SETTINGS;
        buf[1] = (uint8_t)g_song.playback_mode;
        float def_bpm = g_settings.bpm;
        memcpy(buf + 2, &def_bpm, 4);
        n = 6;
        break;
    }
    default:
        buf[0] = (uint8_t)type;
        n = 1;
        break;
    }

    if (n) ws_broadcast_except(buf, n, skip_fd);
}

/* ── Periodic timer callbacks ────────────────────────────────────────────── */

static void meter_timer_cb(void *arg)
{
    (void)arg;
    ws_notify_change(WS_MSG_METER, -1);
}

static void playhead_timer_cb(void *arg)
{
    (void)arg;
    ws_notify_change(WS_MSG_PLAYHEAD, -1);
}

/* ── WebSocket URI handler ───────────────────────────────────────────────── */

static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        /* Handshake: register this client */
        int fd = httpd_req_to_sockfd(req);
        ESP_LOGI(TAG, "client connected fd=%d", fd);

        xSemaphoreTake(s_clients_mx, portMAX_DELAY);
        bool added = false;
        for (int i = 0; i < WS_MAX_CLIENTS; i++) {
            if (!s_clients[i].active) {
                s_clients[i].fd     = fd;
                s_clients[i].active = true;
                added = true;
                break;
            }
        }
        xSemaphoreGive(s_clients_mx);

        if (!added) {
            ESP_LOGW(TAG, "client table full, rejecting fd=%d", fd);
            return ESP_FAIL;
        }

        send_full_state(fd);
        return ESP_OK;
    }

    /* Receive a binary command frame */
    httpd_ws_frame_t frm = { .type = HTTPD_WS_TYPE_BINARY };
    uint8_t buf[256] = {0};
    frm.payload = buf;

    esp_err_t err = httpd_ws_recv_frame(req, &frm, sizeof(buf) - 1);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "recv err=%d", err);
        return err;
    }

    if (frm.type == HTTPD_WS_TYPE_CLOSE) {
        int fd = httpd_req_to_sockfd(req);
        ESP_LOGI(TAG, "client disconnected fd=%d", fd);
        ws_audio_client_closed(fd);
        xSemaphoreTake(s_clients_mx, portMAX_DELAY);
        for (int i = 0; i < WS_MAX_CLIENTS; i++) {
            if (s_clients[i].fd == fd) {
                s_clients[i].active = false;
                break;
            }
        }
        xSemaphoreGive(s_clients_mx);
        return ESP_OK;
    }

    if (frm.type == HTTPD_WS_TYPE_BINARY && frm.len > 0)
        ws_cmd_dispatch(buf, frm.len, httpd_req_to_sockfd(req));

    return ESP_OK;
}

/* ── Public: register with HTTP server ──────────────────────────────────── */

httpd_handle_t ws_server_handle(void) { return s_server; }

void ws_server_register(httpd_handle_t server)
{
    s_server    = server;
    s_clients_mx = xSemaphoreCreateMutex();
    memset(s_clients, 0, sizeof(s_clients));

    static const httpd_uri_t ws_uri = {
        .uri          = "/ws",
        .method       = HTTP_GET,
        .handler      = ws_handler,
        .is_websocket = true,
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &ws_uri));

    /* Meter timer: 100 ms → 10 Hz */
    esp_timer_handle_t meter_timer;
    const esp_timer_create_args_t meter_args = {
        .callback = meter_timer_cb,
        .name     = "ws_meter",
    };
    ESP_ERROR_CHECK(esp_timer_create(&meter_args, &meter_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(meter_timer, 100000));  /* µs */

    /* Playhead timer: 40 ms → 25 Hz */
    esp_timer_handle_t ph_timer;
    const esp_timer_create_args_t ph_args = {
        .callback = playhead_timer_cb,
        .name     = "ws_playhead",
    };
    ESP_ERROR_CHECK(esp_timer_create(&ph_args, &ph_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(ph_timer, 40000));  /* µs */

    ESP_LOGI(TAG, "WebSocket server registered at /ws");
}
