/*
 * ws_cmd.c — WebSocket command dispatcher.
 *
 * Decodes binary WS_CMD_* frames from browser clients, mutates g_song /
 * lane_t / drum_seq_t / piano_roll_t, then calls ws_notify_change() so all
 * connected clients (including the sender) see the updated state.
 *
 * Frame format: first byte = WS_CMD_* type, remaining bytes = payload.
 * All multi-byte integers are little-endian.
 */

#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "lane.h"
#include "clock.h"
#include "settings.h"
#include "song.h"
#include "drum_seq.h"
#include "wav_player.h"
#include "piano_roll.h"
#include "synth.h"
#include "arp.h"
#include "fx.h"
#include "ws_server.h"
#include "ws_audio.h"
#include "ws_cmd.h"

static const char *TAG = "ws_cmd";

/* ── Little-endian read helpers ──────────────────────────────────────────── */

static inline uint16_t rd_u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static inline uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
}

static inline float rd_f32(const uint8_t *p)
{
    float v;
    memcpy(&v, p, 4);
    return v;
}

static inline float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

/* ── Validators ──────────────────────────────────────────────────────────── */

static bool lane_ok(int li)
{
    return li >= 0 && li < NUM_LANES;
}

static bool drum_row_ok(int li, int ri)
{
    return lane_ok(li) && g_song.lanes[li].drum_seq != NULL
        && ri >= 0 && ri < g_song.lanes[li].drum_seq->row_count;
}

/* ── Command handlers ────────────────────────────────────────────────────── */

/* WS_CMD_SET_LANE  [1 + payload]
 *  u8  lane_idx
 *  u8  field_mask  (bit 0=mute, 1=volume, 2=pan, 3=loop_len, 4=wav_path)
 *  u8  mute        (if mask & 1)
 *  f32 volume      (if mask & 2)
 *  f32 pan         (if mask & 4)
 *  u32 loop_len    (if mask & 8)
 *  [128] wav_path  (if mask & 16)
 *  u8  drum_step_count (if mask & 32)
 */
static void cmd_set_lane(const uint8_t *p, size_t len, int sender_fd)
{
    if (len < 3) return;
    int     li   = p[0];
    uint8_t mask = p[1];
    if (!lane_ok(li)) return;

    lane_t *l = &g_song.lanes[li];
    size_t off = 2;

    if ((mask & 0x01) && off < len) { l->mute   = p[off++] != 0; }
    if ((mask & 0x02) && off + 4 <= len) { l->volume = clampf(rd_f32(p + off), 0.0f, 1.0f); off += 4; }
    if ((mask & 0x04) && off + 4 <= len) { l->pan    = clampf(rd_f32(p + off), -1.0f, 1.0f); off += 4; }
    if ((mask & 0x08) && off + 4 <= len) { l->loop_len_ticks = rd_u32(p + off); off += 4; }
    if ((mask & 0x10) && off < len) {
        size_t rem = len - off;
        if (rem > sizeof(l->wav_path) - 1) rem = sizeof(l->wav_path) - 1;
        memcpy(l->wav_path, p + off, rem);
        l->wav_path[rem] = '\0';
        /* Open WAV into the lane's pool slot */
        if (l->type == LANE_TYPE_WAV) {
            if (l->wav_lane_slot < 0) {
                l->wav_lane_slot = wav_lane_alloc_slot();
                if (l->wav_lane_slot >= 0)
                    l->wav_lane = wav_lane_get(l->wav_lane_slot);
            }
            if (l->wav_lane_slot >= 0) {
                char fp[280];
                snprintf(fp, sizeof(fp), "/sdcard/%s", l->wav_path);
                wav_lane_open(l->wav_lane_slot, fp);
                if (l->wav_lane) l->wav_lane->active = true;
            }
        }
    }
    if ((mask & 0x20) && off < len && l->drum_seq) {
        /* Drum step count: rescale the pattern so hits keep their timing. */
        drum_seq_set_step_count(l->drum_seq, p[off++], l->loop_len_ticks);
    }
    g_song.dirty = true;
    ws_notify_except(WS_MSG_LANE_UPDATE, li, sender_fd);
}

/* WS_CMD_TOGGLE_MUTE  [1 byte payload]
 *  u8 lane_idx
 */
static void cmd_toggle_mute(const uint8_t *p, size_t len)
{
    if (len < 1) return;
    int li = p[0];
    if (!lane_ok(li)) return;
    g_song.lanes[li].mute = !g_song.lanes[li].mute;
    g_song.dirty = true;
    ws_notify_change(WS_MSG_LANE_UPDATE, li);
}

/* WS_CMD_TOGGLE_SOLO  [1 byte payload]
 *  u8 lane_idx
 * Toggles solo on the given lane.  If no lanes remain soloed after the toggle,
 * all lanes revert to their individual mute states (solo mode exits).
 */
static void cmd_toggle_solo(const uint8_t *p, size_t len)
{
    if (len < 1) return;
    int li = p[0];
    if (!lane_ok(li)) return;
    g_song.lanes[li].solo = !g_song.lanes[li].solo;
    g_song.dirty = true;
    /* Broadcast updated solo state for all lanes so clients reflect the change */
    for (int i = 0; i < NUM_LANES; i++) {
        if (g_song.lanes[i].active)
            ws_notify_change(WS_MSG_LANE_UPDATE, i);
    }
}

/* WS_CMD_RENAME_LANE  [1 + up to 31 bytes payload]
 *  u8    lane_idx
 *  char  name[31] (null-terminated, truncated if needed)
 */
static void cmd_rename_lane(const uint8_t *p, size_t len)
{
    if (len < 2) return;
    int li = p[0];
    if (!lane_ok(li)) return;
    /* wav_path holds the sample path; lane name lives as the basename of wav_path
     * for WAV/DRUM lanes.  For generic naming we embed it at the start of wav_path
     * with a sentinel — or simply store in wav_path for now (UI shows basename).
     * For synth/drum lanes with no wav_path, store name in wav_path as display name. */
    size_t name_len = len - 1;
    if (name_len > 127) name_len = 127;
    memset(g_song.lanes[li].wav_path, 0, sizeof(g_song.lanes[li].wav_path));
    memcpy(g_song.lanes[li].wav_path, p + 1, name_len);
    g_song.dirty = true;
    ws_notify_change(WS_MSG_LANE_UPDATE, li);
}

/* WS_CMD_DUPLICATE_LANE  [1 byte payload]
 *  u8 src_lane_idx
 * Copies src lane into the first inactive lane slot.
 */
static void cmd_duplicate_lane(const uint8_t *p, size_t len)
{
    if (len < 1) return;
    int src = p[0];
    if (!lane_ok(src) || !g_song.lanes[src].active) return;
    /* Find first inactive slot */
    int dst = -1;
    for (int i = 0; i < NUM_LANES; i++) {
        if (!g_song.lanes[i].active) { dst = i; break; }
    }
    if (dst < 0) return;  /* no free slot */
    /* Shallow copy — runtime pointers (wav_lane, drum_seq, synth, piano_roll)
     * are NOT copied; the duplicate starts as an identical config but inactive
     * engine state.  The audio task will reinitialise on next play. */
    lane_t *s = &g_song.lanes[src];
    lane_t *d = &g_song.lanes[dst];
    d->type           = s->type;
    d->active         = true;
    d->mute           = s->mute;
    d->solo           = false;
    d->volume         = s->volume;
    d->pan            = s->pan;
    d->loop_len_ticks = s->loop_len_ticks;
    memcpy(d->wav_path, s->wav_path, sizeof(d->wav_path));
    d->adsr           = s->adsr;
    /* drum_seq, piano_roll, synth, arp, lfo, fx left at defaults — not deep-copied */
    d->wav_lane   = NULL;
    d->drum_seq   = NULL;
    d->synth      = NULL;
    d->piano_roll = NULL;
    g_song.dirty = true;
    ws_notify_change(WS_MSG_LANE_UPDATE, dst);
}

/* WS_CMD_SET_DRUM_STEP  [5 bytes payload]
 *  u8  lane_idx
 *  u8  row_idx
 *  u8  step_idx
 *  u8  velocity  (0 = clear)
 *  u8  accent    (0/1)
 */
static void cmd_set_drum_step(const uint8_t *p, size_t len, int sender_fd)
{
    if (len < 5) return;
    int li = p[0], ri = p[1], si = p[2];
    uint8_t vel    = p[3];
    uint8_t accent = p[4];
    if (!drum_row_ok(li, ri)) return;
    drum_row_t *row = &g_song.lanes[li].drum_seq->rows[ri];
    if (si >= DRUM_MAX_STEPS) return;
    row->steps[si].velocity = vel;
    row->steps[si].accent   = accent != 0;
    g_song.dirty = true;
    ws_notify_except(WS_MSG_DRUM_STEP, (li << 8) | ri, sender_fd);
}

/* WS_CMD_ADD_NOTE  [13 bytes payload]
 *  u8  lane_idx
 *  u32 tick_start
 *  u32 tick_len
 *  u8  note     (semitone 0–127)
 *  u8  velocity
 */
static void cmd_add_note(const uint8_t *p, size_t len)
{
    if (len < 11) return;
    int li = p[0];
    if (!lane_ok(li)) return;
    piano_roll_t *pr = g_song.lanes[li].piano_roll;
    if (!pr) return;
    uint32_t ts  = rd_u32(p + 1);
    uint32_t tl  = rd_u32(p + 5);
    uint8_t  note = p[9];
    uint8_t  vel  = p[10];
    piano_roll_add_note(pr, ts, tl, note, vel);
    g_song.dirty = true;
    ws_notify_change(WS_MSG_NOTE_EVENT, li);
}

/* WS_CMD_DEL_NOTE  [6 bytes payload]
 *  u8  lane_idx
 *  u32 tick_start
 *  u8  note
 */
static void cmd_del_note(const uint8_t *p, size_t len)
{
    if (len < 6) return;
    int li = p[0];
    if (!lane_ok(li)) return;
    piano_roll_t *pr = g_song.lanes[li].piano_roll;
    if (!pr) return;
    uint32_t ts   = rd_u32(p + 1);
    uint8_t  note = p[5];
    piano_roll_remove_note(pr, ts, note);
    g_song.dirty = true;
    ws_notify_change(WS_MSG_NOTE_EVENT, li);
}

/* WS_CMD_SET_CLOCK  [9 bytes payload]
 *  f32 bpm
 *  u32 ppqn          (tick_rate)
 *  u32 beats_per_bar
 */
static void cmd_set_clock(const uint8_t *p, size_t len)
{
    if (len < 12) return;
    float    bpm   = clampf(rd_f32(p),      20.0f, 300.0f);
    uint32_t ppqn  = rd_u32(p + 4);
    uint32_t beats = rd_u32(p + 8);
    if (!ppqn)  ppqn  = 96;
    if (!beats) beats = 4;
    clock_set_bpm(&g_song.clock, bpm);
    g_song.clock.tick_rate     = ppqn;
    g_song.clock.beats_per_bar = beats;
    g_song.dirty = true;
    ws_notify_change(WS_MSG_CLOCK_UPDATE, -1);
}

/* WS_CMD_TRANSPORT  [1 byte payload]
 *  u8  action: 0=stop, 1=play, 2=tap
 */
static void cmd_transport(const uint8_t *p, size_t len)
{
    if (len < 1) return;
    switch (p[0]) {
    case 0: clock_stop(&g_song.clock);  break;
    case 1: clock_start(&g_song.clock); break;
    case 2:
        clock_tap(&g_song.clock);
        ws_notify_change(WS_MSG_CLOCK_UPDATE, -1);
        break;
    }
    ws_notify_change(WS_MSG_TRANSPORT, -1);
}

/* WS_CMD_SET_MASTER  [8 bytes payload]
 *  f32 volume
 *  f32 pan
 */
static void cmd_set_master(const uint8_t *p, size_t len, int sender_fd)
{
    if (len < 8) return;
    g_settings.master_volume = clampf(rd_f32(p),     0.0f, 1.0f);
    g_settings.master_pan    = clampf(rd_f32(p + 4), -1.0f, 1.0f);
    settings_save();
    ws_notify_except(WS_MSG_MASTER_UPDATE, -1, sender_fd);
}

/* WS_CMD_SET_ADSR  [1 + 16 bytes payload]
 *  u8  lane_idx
 *  f32 atk_ms
 *  f32 dcy_ms
 *  f32 sus   (0–1)
 *  f32 rel_ms
 */
static void cmd_set_adsr(const uint8_t *p, size_t len)
{
    if (len < 17) return;
    int li = p[0];
    if (!lane_ok(li)) return;
    lane_adsr_t *a = &g_song.lanes[li].adsr;
    lane_adsr_set_params(a, rd_f32(p + 1), rd_f32(p + 5),
                         clampf(rd_f32(p + 9), 0.0f, 1.0f), rd_f32(p + 13));
    g_song.dirty = true;
    ws_notify_change(WS_MSG_ADSR_UPDATE, li);
}

/* WS_CMD_SAVE_SONG  [0..127 bytes payload]
 *  [optional] null-terminated filename string (no path, no extension)
 */
static void cmd_save_song(const uint8_t *p, size_t len)
{
    char path[160];
    if (len > 0 && p[0]) {
        char name[64] = "";
        size_t nl = len < sizeof(name) - 1 ? len : sizeof(name) - 1;
        memcpy(name, p, nl);
        name[nl] = '\0';
        snprintf(path, sizeof(path), "%s/%s%s", SONG_DIR, name, SONG_EXT);
        strncpy(g_song.name, name, sizeof(g_song.name) - 1);
    } else {
        snprintf(path, sizeof(path), "%s/%s%s", SONG_DIR,
                 g_song.name[0] ? g_song.name : SONG_NAME_DEFAULT, SONG_EXT);
    }
    song_save(path);
    ws_notify_change(WS_MSG_FULL_STATE, -1);
}

/* WS_CMD_LOAD_SONG  [1..127 bytes payload]
 *  null-terminated filename (no path, no extension)
 */
static void cmd_load_song(const uint8_t *p, size_t len)
{
    if (len < 1) return;
    char name[64] = "";
    size_t nl = len < sizeof(name) - 1 ? len : sizeof(name) - 1;
    memcpy(name, p, nl);
    name[nl] = '\0';
    char path[160];
    snprintf(path, sizeof(path), "%s/%s%s", SONG_DIR, name, SONG_EXT);
    song_load(path);
    ws_notify_change(WS_MSG_FULL_STATE, -1);
}

/* WS_CMD_NEW_SONG  [0 bytes payload] */
static void cmd_new_song(void)
{
    song_new();
    ws_notify_change(WS_MSG_FULL_STATE, -1);
}

/* WS_CMD_LIST_SONGS  [0 bytes payload]
 * Sends WS_MSG_SONG_LIST frame: type byte followed by newline-separated filenames.
 * Called from within ws_cmd_dispatch which runs in the HTTP server task —
 * we broadcast to all (including sender) via ws_notify_change.
 * The song list is built here then sent as a single raw frame.
 */
static void cmd_list_songs(void)
{
    /* Build a newline-separated list of .s32 filenames */
    static char list_buf[2048];
    list_buf[0] = (char)WS_MSG_SONG_LIST;
    int pos = 1;

    mkdir(SONG_DIR, 0755);
    DIR *d = opendir(SONG_DIR);
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL && pos < (int)sizeof(list_buf) - 32) {
            const char *n = ent->d_name;
            /* Strip .s32 extension for display (case-insensitive — FATFS may
             * return uppercase extensions for some entries) */
            const char *ext = strrchr(n, '.');
            if (!ext || strcasecmp(ext, SONG_EXT) != 0) continue;
            size_t base_len = (size_t)(ext - n);
            memcpy(list_buf + pos, n, base_len);
            pos += (int)base_len;
            list_buf[pos++] = '\n';
        }
        closedir(d);
    } else {
        ESP_LOGW("ws_cmd", "opendir(%s) failed", SONG_DIR);
    }

    ws_broadcast_raw((const uint8_t *)list_buf, (size_t)pos);
}

/* WS_CMD_SET_SETTINGS  [1 + variable]
 *  sub-key 0x01: u8 upload_mode (informational — no firmware action needed)
 *  sub-key 0x02: f32 bpm_default
 *  sub-key 0x03: u8 playback_mode (0=LIVE, 1=SONG)
 */
static void cmd_set_settings(const uint8_t *p, size_t len)
{
    if (len < 1) return;
    uint8_t sub = p[0];
    switch (sub) {
    case 0x01:
        /* upload mode flag — HTTP upload always active, nothing to do */
        break;
    case 0x02:
        if (len < 5) return;
        g_settings.bpm = clampf(rd_f32(p + 1), 20.0f, 300.0f);
        settings_save();
        ws_notify_change(WS_MSG_SETTINGS, -1);
        break;
    case 0x03:
        if (len < 2) return;
        g_song.playback_mode = (p[1] == 1) ? PLAYBACK_SONG : PLAYBACK_LIVE;
        g_song.dirty = true;
        ws_notify_change(WS_MSG_SETTINGS, -1);
        break;
    case 0x04: /* send return level */
        if (len < 5) return;
        g_song.send_return_level = clampf(rd_f32(p + 1), 0.0f, 1.0f);
        g_song.dirty = true;
        ws_notify_change(WS_MSG_SETTINGS, -1);
        break;
    case 0x10: /* autosave interval in bars */
        if (len < 2) return;
        g_song.autosave_interval = p[1];
        g_song.dirty = true;
        break;
    default:
        ESP_LOGW(TAG, "SET_SETTINGS: unknown sub-key 0x%02x", sub);
        break;
    }
}

/* WS_CMD_ADD_LANE  [1 byte payload]
 *  u8 type (lane_type_t)
 */
static void cmd_add_lane(const uint8_t *p, size_t len)
{
    if (len < 1) return;
    lane_type_t type = (lane_type_t)p[0];
    /* Find first inactive slot */
    for (int i = 0; i < NUM_LANES; i++) {
        if (!g_song.lanes[i].active) {
            lane_reset(&g_song.lanes[i]);
            g_song.lanes[i].type   = type;
            g_song.lanes[i].active = true;
            g_song.dirty = true;
            ws_notify_change(WS_MSG_LANE_UPDATE, i);
            return;
        }
    }
    ESP_LOGW(TAG, "ADD_LANE: no free slot");
}

/* WS_CMD_DEL_LANE  [1 byte payload]
 *  u8 lane_idx
 */
static void cmd_del_lane(const uint8_t *p, size_t len)
{
    if (len < 1) return;
    int li = p[0];
    if (!lane_ok(li)) return;
    lane_reset(&g_song.lanes[li]);
    g_song.lanes[li].active = false;
    g_song.dirty = true;
    ws_notify_change(WS_MSG_LANE_UPDATE, li);
}

/* WS_CMD_DRUM_ROW_ASSIGN  [2 + variable payload]
 *  u8 lane_idx
 *  u8 row_idx
 *  [null-terminated wav path, relative to /sdcard/]
 */
static void cmd_drum_row_assign(const uint8_t *p, size_t len)
{
    if (len < 2) return;
    int li = p[0], ri = p[1];
    if (!drum_row_ok(li, ri)) return;
    drum_row_t *row = &g_song.lanes[li].drum_seq->rows[ri];
    size_t pl = len - 2;
    if (pl > sizeof(row->wav_path) - 1) pl = sizeof(row->wav_path) - 1;
    memcpy(row->wav_path, p + 2, pl);
    row->wav_path[pl] = '\0';
    /* Open the WAV into the row's ring so it can play immediately */
    char full_path[280];
    snprintf(full_path, sizeof(full_path), "/sdcard/%s", row->wav_path);
    drum_row_load_wav(row, full_path);
    g_song.dirty = true;
    ws_notify_change(WS_MSG_LANE_UPDATE, li);
}

/* WS_CMD_SET_SYNTH  [2 + variable payload]
 *  u8 lane_idx
 *  u8 type_id
 *  [param_id u8, value f32] pairs (optional, 5 bytes each)
 */
static void cmd_set_synth(const uint8_t *p, size_t len)
{
    if (len < 2) return;
    int li = p[0];
    if (!lane_ok(li)) return;
    lane_t *l = &g_song.lanes[li];
    uint8_t type_id = p[1];

    /* Re-create synth if type changed */
    if (!l->synth || l->synth->type_id != type_id) {
        synth_inst_t *ns = synth_new(type_id);
        if (!ns) return;
        l->synth = ns;
    }

    /* Apply param pairs */
    size_t off = 2;
    while (off + 5 <= len) {
        uint8_t pid = p[off];
        float   val = rd_f32(p + off + 1);
        if (l->synth->set_param)
            l->synth->set_param(l->synth, pid, val);
        off += 5;
    }
    g_song.dirty = true;
    ws_notify_change(WS_MSG_SYNTH_PARAMS, li);
}

/* WS_CMD_NOTE_ON  [3 bytes payload]
 *  u8 lane_idx
 *  u8 note
 *  u8 velocity
 */
static void cmd_note_on(const uint8_t *p, size_t len)
{
    if (len < 3) return;
    int li = p[0];
    if (!lane_ok(li)) return;
    lane_t *l = &g_song.lanes[li];
    if (!l->active || !l->synth) return;
    arp_note_on(&l->arp, l->synth, p[1], p[2]);
}

/* WS_CMD_NOTE_OFF  [2 bytes payload]
 *  u8 lane_idx
 *  u8 note
 */
static void cmd_note_off(const uint8_t *p, size_t len)
{
    if (len < 2) return;
    int li = p[0];
    if (!lane_ok(li)) return;
    lane_t *l = &g_song.lanes[li];
    if (!l->active || !l->synth) return;
    arp_note_off(&l->arp, l->synth, p[1]);
}

/* WS_CMD_DRUM_TRIGGER  [3 bytes payload]
 *  u8 lane_idx
 *  u8 row_idx
 *  u8 velocity
 *
 * Live drum trigger: immediately fires the row's sample (one-shot), routed
 * through the row's volume/pan and the lane FX chain. Works whether or not
 * the transport is running, so the LIVE pads feel like real finger-drumming.
 */
static void cmd_drum_trigger(const uint8_t *p, size_t len)
{
    if (len < 3) return;
    int li = p[0], ri = p[1];
    uint8_t vel = p[2];
    if (!drum_row_ok(li, ri)) return;
    drum_seq_trigger_row(g_song.lanes[li].drum_seq, ri, vel);
}

/* WS_CMD_SET_FX  [variable]
 *  u8  lane_idx
 *  u8  sub_cmd   0=set-params/en, 1=insert, 2=remove, 3=move
 *
 * sub_cmd 0 (set):
 *  u8  slot_idx
 *  u8  enabled
 *  f32 params[8]  (32 bytes, optional)
 *
 * sub_cmd 1 (insert):
 *  u8  slot_idx   insertion point (0 = prepend)
 *  u8  fx_type
 *
 * sub_cmd 2 (remove):
 *  u8  slot_idx
 *
 * sub_cmd 3 (move/swap):
 *  u8  src_slot
 *  u8  dst_slot
 */
/* lane index 0xFF is the master bus FX chain; 0xFE is the send-bus chain */
#define MASTER_FX_IDX 0xFF
#define SEND_FX_IDX   0xFE

static void cmd_set_fx(const uint8_t *p, size_t len)
{
    if (len < 2) return;
    int li      = p[0];
    uint8_t sub = p[1];

    fx_node_t **chain;
    int        *count;

    if ((uint8_t)li == MASTER_FX_IDX) {
        chain = g_song.master_fx;
        count = &g_song.master_fx_count;
    } else if ((uint8_t)li == SEND_FX_IDX) {
        chain = g_song.send_fx;
        count = &g_song.send_fx_count;
    } else {
        if (!lane_ok(li)) return;
        chain = g_song.lanes[li].fx;
        count = &g_song.lanes[li].fx_count;
    }

    switch (sub) {
    case 0: {  /* set params / enabled */
        if (len < 4) return;
        int slot = p[2];
        bool en  = p[3] != 0;
        if (slot >= *count || slot >= FX_MAX_PER_LANE || !chain[slot]) return;
        chain[slot]->enabled = en;
        if (len >= 4 + 32) {
            for (uint8_t i = 0; i < 8; i++) {
                float v = rd_f32(p + 4 + i * 4);
                chain[slot]->params[i] = v;
                if (chain[slot]->set_param) chain[slot]->set_param(chain[slot], i, v);
            }
        }
        break;
    }
    case 1: {  /* insert */
        if (len < 4) return;
        int slot        = p[2];
        fx_type_t ftype = (fx_type_t)p[3];
        fx_node_t *node = fx_new(ftype);
        if (!node) return;
        if (!fx_chain_insert(chain, count, slot, node)) {
            node->free(node);
            return;
        }
        break;
    }
    case 2: {  /* remove */
        if (len < 3) return;
        int slot = p[2];
        if (slot >= *count) return;
        fx_chain_remove(chain, count, slot);
        break;
    }
    case 3: {  /* move / swap */
        if (len < 4) return;
        int src = p[2], dst = p[3];
        if (src >= *count || dst >= *count) return;
        fx_chain_move(chain, *count, src, dst);
        break;
    }
    default: return;
    }

    g_song.dirty = true;
    ws_notify_change(WS_MSG_FX_UPDATE, li);
}

/* WS_CMD_SET_DRUM_ROW  [2 + variable payload]
 *  u8  lane_idx
 *  u8  row_idx
 *  u8  mask  (0x01=mute, 0x02=volume)
 *  u8  mute  (if mask & 0x01)
 *  f32 volume (if mask & 0x02)
 */
static void cmd_set_drum_row(const uint8_t *p, size_t len, int sender_fd)
{
    if (len < 3) return;
    int li = p[0], ri = p[1];
    uint8_t mask = p[2];
    if (!drum_row_ok(li, ri)) return;
    drum_row_t *row = &g_song.lanes[li].drum_seq->rows[ri];
    size_t off = 3;
    if ((mask & 0x01) && off < len) { row->mute   = p[off++] != 0; }
    if ((mask & 0x02) && off + 4 <= len) { row->volume = clampf(rd_f32(p + off), 0.0f, 1.0f); off += 4; }
    g_song.dirty = true;
    ws_notify_except(WS_MSG_DRUM_STEP, li * 256 + ri, sender_fd);
}

/* WS_CMD_SET_ARP  [1 + 9 bytes payload]
 *  u8  lane_idx
 *  u8  enabled
 *  u8  mode         (arp_mode_t)
 *  u8  octave_range (1–4)
 *  u8  step_div     (4/8/16/32)
 *  u8  gate_pct     (10–100)
 *  u8  vel_mode     (arp_vel_mode_t)
 *  u8  fixed_vel    (0–127)
 *  u8  latch        (0/1)
 *  u8  retrigger    (0/1)
 *  u8  swing_pct    (50–75)
 */
static void cmd_set_arp(const uint8_t *p, size_t len)
{
    if (len < 2) return;
    int li = p[0];
    if (!lane_ok(li)) return;
    lane_t *l = &g_song.lanes[li];
    arp_t *a = &l->arp;

    size_t off = 1;
    if (off < len) a->enabled      = p[off++] != 0;
    if (off < len) a->mode         = (arp_mode_t)p[off++];
    if (off < len) a->octave_range = p[off++];
    if (off < len) a->step_div     = p[off++];
    if (off < len) a->gate_pct     = p[off++];
    if (off < len) a->velocity_mode = (arp_vel_mode_t)p[off++];
    if (off < len) a->fixed_vel    = p[off++];
    if (off < len) a->latch        = p[off++] != 0;
    if (off < len) a->retrigger    = p[off++] != 0;
    if (off < len) a->swing_pct    = p[off++];

    arp_build_seq(a);
    g_song.dirty = true;
    /* No dedicated ARP_UPDATE message — reuse LANE_UPDATE as a hint */
    ws_notify_change(WS_MSG_LANE_UPDATE, li);
}

/* WS_CMD_SCENE_CAPTURE  [1 byte payload]
 *  u8 scene_idx (0–7)
 */
static void cmd_scene_capture(const uint8_t *p, size_t len)
{
    if (len < 1) return;
    if (p[0] >= SCENE_MAX) return;
    scene_capture(&g_song, (int)p[0], NULL);
    g_song.dirty = true;
    ws_notify_change(WS_MSG_FULL_STATE, -1);
}

/* WS_CMD_SCENE_RECALL  [1 byte payload]
 *  u8 scene_idx (0–7)
 */
static void cmd_scene_recall(const uint8_t *p, size_t len)
{
    if (len < 1) return;
    if (p[0] >= SCENE_MAX) return;
    scene_recall(&g_song, (int)p[0]);
    ws_notify_change(WS_MSG_FULL_STATE, -1);
}

/* WS_CMD_SET_GROOVE  [5 bytes payload]
 *  u8  lane_idx
 *  u8  swing_pct (50–75)
 *  i8  humanise  (0–20)
 *  u8  enabled
 */
static void cmd_set_groove(const uint8_t *p, size_t len)
{
    if (len < 4) return;
    int li = p[0];
    if (!lane_ok(li)) return;
    groove_t *g = &g_song.lanes[li].groove;
    g->swing_pct = p[1];
    g->humanise  = (int8_t)p[2];
    g->enabled   = p[3] != 0;
    g_song.dirty = true;
    ws_notify_change(WS_MSG_LANE_UPDATE, li);
}

/* WS_CMD_SET_METRONOME  [1 or 3 bytes]
 *  u8 enabled (sub-key 0: just enable/disable)
 *  -- or --
 *  u8 sub=0x01, u8 volume (0–100)
 */
static void cmd_set_metronome(const uint8_t *p, size_t len)
{
    if (len < 1) return;
    if (p[0] == 0x01 && len >= 2) {
        g_song.metronome_volume = (float)p[1] / 100.0f;
    } else {
        g_song.metronome_enabled = p[0] != 0;
    }
    g_song.dirty = true;
    ws_notify_change(WS_MSG_SETTINGS, -1);
}

/* WS_CMD_SAVE_VERSIONED  [0 bytes] */
static void cmd_save_versioned(void)
{
    char out[160];
    song_save_versioned(out, sizeof(out));
    ws_notify_change(WS_MSG_FULL_STATE, -1);
}

/* WS_CMD_SAVE_TEMPLATE  [variable: null-terminated name] */
static void cmd_save_template(const uint8_t *p, size_t len)
{
    char name[64] = "template";
    if (len > 0) {
        size_t nl = len < sizeof(name) - 1 ? len : sizeof(name) - 1;
        memcpy(name, p, nl);
        name[nl] = '\0';
    }
    song_save_template(name);
}

/* WS_CMD_LOAD_TEMPLATE  [variable: null-terminated name] */
static void cmd_load_template(const uint8_t *p, size_t len)
{
    char name[64] = "";
    if (len < 1) return;
    size_t nl = len < sizeof(name) - 1 ? len : sizeof(name) - 1;
    memcpy(name, p, nl);
    name[nl] = '\0';
    song_load_template(name);
    ws_notify_change(WS_MSG_FULL_STATE, -1);
}

/* WS_CMD_SET_ARRANGEMENT  [3 + N*2 bytes]
 *  u8  enabled
 *  u8  step_count (N)
 *  [N × { u8 scene_idx, u8 repeat }]
 */
static void cmd_set_arrangement(const uint8_t *p, size_t len)
{
    if (len < 2) return;
    bool    enabled = p[0] != 0;
    uint8_t n       = p[1];
    if (n > ARRANGEMENT_MAX_STEPS) n = ARRANGEMENT_MAX_STEPS;
    if (len < (size_t)(2 + n * 2)) return;
    arrangement_t *arr = &g_song.arrangement;
    arr->enabled = enabled;
    arr->count   = n;
    for (int i = 0; i < n; i++) {
        arr->steps[i].scene_idx = p[2 + i*2    ];
        arr->steps[i].repeat    = p[2 + i*2 + 1];
        if (!arr->steps[i].repeat) arr->steps[i].repeat = 1;
    }
    arr->current_step   = 0;
    arr->current_repeat = 0;
    g_song.dirty = true;
    ws_notify_change(WS_MSG_SETTINGS, -1);
}

/* WS_CMD_DRUM_EUCLIDEAN  [5 bytes]
 *  u8 lane_idx
 *  u8 row_idx
 *  u8 hits
 *  u8 steps
 *  u8 velocity
 */
static void cmd_drum_euclidean(const uint8_t *p, size_t len)
{
    if (len < 5) return;
    int li = p[0], ri = p[1];
    if (!drum_row_ok(li, ri)) return;
    drum_seq_euclidean(&g_song.lanes[li].drum_seq->rows[ri], p[2], p[3], p[4]);
    g_song.dirty = true;
    ws_notify_change(WS_MSG_DRUM_STEP, (li << 8) | ri);
}

/* WS_CMD_SET_NOTE_REPEAT  [4 bytes]
 *  u8 lane_idx
 *  u8 enabled
 *  u8 div_ticks
 */
static void cmd_set_note_repeat(const uint8_t *p, size_t len)
{
    if (len < 3) return;
    int li = p[0];
    if (!lane_ok(li)) return;
    g_song.lanes[li].note_repeat     = p[1] != 0;
    g_song.lanes[li].note_repeat_div = p[2];
    g_song.dirty = true;
    ws_notify_change(WS_MSG_LANE_UPDATE, li);
}

/* WS_CMD_SET_SEND_LEVEL  [3 bytes]
 *  u8 lane_idx
 *  u8 level_u8 (0–255 → 0.0–1.0)
 */
static void cmd_set_send_level(const uint8_t *p, size_t len, int sender_fd)
{
    if (len < 2) return;
    int li = p[0];
    if (!lane_ok(li)) return;
    g_song.lanes[li].send_level = (float)p[1] / 255.0f;
    g_song.dirty = true;
    ws_notify_except(WS_MSG_LANE_UPDATE, li, sender_fd);
}

/* ── Public dispatcher ───────────────────────────────────────────────────── */

void ws_cmd_dispatch(const uint8_t *frame, size_t len, int fd)
{
    if (len < 1) return;
    uint8_t type = frame[0];
    const uint8_t *p = frame + 1;
    size_t plen = len - 1;

    switch ((ws_cmd_type_t)type) {
    case WS_CMD_SET_LANE:        cmd_set_lane(p, plen, fd);        break;
    case WS_CMD_TOGGLE_MUTE:     cmd_toggle_mute(p, plen);         break;
    case WS_CMD_TOGGLE_SOLO:     cmd_toggle_solo(p, plen);         break;
    case WS_CMD_RENAME_LANE:     cmd_rename_lane(p, plen);         break;
    case WS_CMD_DUPLICATE_LANE:  cmd_duplicate_lane(p, plen);      break;
    case WS_CMD_SET_ARP:         cmd_set_arp(p, plen);             break;
    case WS_CMD_SET_DRUM_ROW:    cmd_set_drum_row(p, plen, fd);    break;
    case WS_CMD_SET_DRUM_STEP:   cmd_set_drum_step(p, plen, fd);   break;
    case WS_CMD_ADD_NOTE:        cmd_add_note(p, plen);            break;
    case WS_CMD_DEL_NOTE:        cmd_del_note(p, plen);            break;
    case WS_CMD_SET_FX:          cmd_set_fx(p, plen);              break;
    case WS_CMD_SET_ADSR:        cmd_set_adsr(p, plen);            break;
    case WS_CMD_SET_CLOCK:       cmd_set_clock(p, plen);           break;
    case WS_CMD_TRANSPORT:       cmd_transport(p, plen);           break;
    case WS_CMD_SET_MASTER:      cmd_set_master(p, plen, fd);      break;
    case WS_CMD_SAVE_SONG:       cmd_save_song(p, plen);           break;
    case WS_CMD_LOAD_SONG:       cmd_load_song(p, plen);           break;
    case WS_CMD_NEW_SONG:        cmd_new_song();                   break;
    case WS_CMD_LIST_SONGS:      cmd_list_songs();                 break;
    case WS_CMD_SET_SETTINGS:    cmd_set_settings(p, plen);        break;
    case WS_CMD_ADD_LANE:        cmd_add_lane(p, plen);            break;
    case WS_CMD_DEL_LANE:        cmd_del_lane(p, plen);            break;
    case WS_CMD_DRUM_ROW_ASSIGN: cmd_drum_row_assign(p, plen);     break;
    case WS_CMD_SET_SYNTH:       cmd_set_synth(p, plen);           break;
    case WS_CMD_NOTE_ON:         cmd_note_on(p, plen);             break;
    case WS_CMD_NOTE_OFF:        cmd_note_off(p, plen);            break;
    case WS_CMD_DRUM_TRIGGER:    cmd_drum_trigger(p, plen);        break;
    case WS_CMD_SCENE_CAPTURE:   cmd_scene_capture(p, plen);       break;
    case WS_CMD_SCENE_RECALL:    cmd_scene_recall(p, plen);        break;
    case WS_CMD_SET_GROOVE:      cmd_set_groove(p, plen);          break;
    case WS_CMD_SET_METRONOME:   cmd_set_metronome(p, plen);       break;
    case WS_CMD_SAVE_VERSIONED:  cmd_save_versioned();             break;
    case WS_CMD_SAVE_TEMPLATE:   cmd_save_template(p, plen);       break;
    case WS_CMD_LOAD_TEMPLATE:   cmd_load_template(p, plen);       break;
    case WS_CMD_SET_ARRANGEMENT: cmd_set_arrangement(p, plen);     break;
    case WS_CMD_DRUM_EUCLIDEAN:  cmd_drum_euclidean(p, plen);      break;
    case WS_CMD_SET_NOTE_REPEAT: cmd_set_note_repeat(p, plen);     break;
    case WS_CMD_SET_SEND_LEVEL:  cmd_set_send_level(p, plen, fd);  break;
    case WS_CMD_AUDIO_STREAM:
        /* payload: u8 enable (0=stop, 1=start) */
        if (plen >= 1 && p[0]) ws_audio_start(fd);
        else                   ws_audio_stop(fd);
        break;
    default:
        ESP_LOGW(TAG, "unknown cmd 0x%02X", type);
        break;
    }
}
