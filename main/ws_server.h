#pragma once
/*
 * ws_server.h — WebSocket server: client slots, broadcast, periodic timers.
 *
 * Call ws_server_register(server) once after httpd_start().
 * After any song mutation, call ws_notify_change(type, idx) to broadcast
 * a diff to all connected WS clients.
 */

#include <stdint.h>
#include <stddef.h>
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Server → client message types ──────────────────────────────────────── */
typedef enum {
    WS_MSG_FULL_STATE    = 0x01,
    WS_MSG_LANE_UPDATE   = 0x02,
    WS_MSG_DRUM_STEP     = 0x03,
    WS_MSG_NOTE_EVENT    = 0x04,
    WS_MSG_FX_UPDATE     = 0x05,
    WS_MSG_ADSR_UPDATE   = 0x06,
    WS_MSG_CLOCK_UPDATE  = 0x07,
    WS_MSG_TRANSPORT     = 0x08,
    WS_MSG_MASTER_UPDATE = 0x09,
    WS_MSG_SONG_LIST     = 0x0A,
    WS_MSG_SETTINGS      = 0x0B,
    WS_MSG_METER         = 0x0C,
    WS_MSG_PLAYHEAD      = 0x0D,
    WS_MSG_SYNTH_PARAMS  = 0x0E,
    WS_MSG_AUDIO_DROPPED = 0x0F,
} ws_msg_type_t;

/* ── Client → server command types ──────────────────────────────────────── */
typedef enum {
    WS_CMD_SET_LANE        = 0x80,
    WS_CMD_SET_DRUM_STEP   = 0x81,
    WS_CMD_ADD_NOTE        = 0x82,
    WS_CMD_DEL_NOTE        = 0x83,
    WS_CMD_SET_FX          = 0x84,
    WS_CMD_SET_ADSR        = 0x85,
    WS_CMD_SET_CLOCK       = 0x86,
    WS_CMD_TRANSPORT       = 0x87,
    WS_CMD_SET_MASTER      = 0x88,
    WS_CMD_SAVE_SONG       = 0x89,
    WS_CMD_LOAD_SONG       = 0x8A,
    WS_CMD_NEW_SONG        = 0x8B,
    WS_CMD_LIST_SONGS      = 0x8C,
    WS_CMD_SET_SETTINGS    = 0x8D,
    WS_CMD_ADD_LANE        = 0x8E,
    WS_CMD_DEL_LANE        = 0x8F,
    WS_CMD_DRUM_ROW_ASSIGN = 0x90,
    WS_CMD_SET_SYNTH       = 0x91,
    WS_CMD_NOTE_ON         = 0x92,
    WS_CMD_NOTE_OFF        = 0x93,
    WS_CMD_DRUM_TRIGGER    = 0x94,
    WS_CMD_TOGGLE_MUTE     = 0x95,
    WS_CMD_TOGGLE_SOLO     = 0x96,
    WS_CMD_RENAME_LANE     = 0x97,
    WS_CMD_DUPLICATE_LANE  = 0x98,
    WS_CMD_SET_ARP         = 0x99,
    WS_CMD_SET_DRUM_ROW    = 0x9A,
    WS_CMD_SCENE_CAPTURE   = 0x9B,
    WS_CMD_SCENE_RECALL    = 0x9C,
    WS_CMD_SET_GROOVE      = 0x9D,
    WS_CMD_SET_METRONOME   = 0x9E,
    WS_CMD_STEM_EXPORT     = 0x9F,
    WS_CMD_SAVE_VERSIONED  = 0xA0,
    WS_CMD_SAVE_TEMPLATE   = 0xA1,
    WS_CMD_LOAD_TEMPLATE   = 0xA2,
    WS_CMD_SET_ARRANGEMENT = 0xA3,
    WS_CMD_DRUM_EUCLIDEAN  = 0xA4,
    WS_CMD_SET_NOTE_REPEAT = 0xA5,
    WS_CMD_SET_SEND_LEVEL  = 0xA6,
    WS_CMD_AUDIO_STREAM    = 0xF0,
} ws_cmd_type_t;

/* Register the /ws URI handler with an already-running httpd server. */
void ws_server_register(httpd_handle_t server);
httpd_handle_t ws_server_handle(void);

/*
 * Send a raw binary frame to a single connected client by fd.
 * Safe to call from any task.  Returns false if fd is not a known active
 * client or the send fails.
 */
bool ws_send_to_fd(int fd, const uint8_t *data, size_t len);

/*
 * Broadcast a diff to all active WebSocket clients.
 *
 * type  — which WS_MSG_* to send
 * idx   — lane index (for LANE_UPDATE, DRUM_STEP, etc.)  or -1 if not needed
 *
 * May be called from any task; uses httpd_ws_send_frame_async internally.
 */
void ws_notify_change(ws_msg_type_t type, int idx);

/*
 * Like ws_notify_change but skips the client identified by skip_fd.
 * Use from command handlers so the sender doesn't receive its own echo.
 * skip_fd = -1 behaves identically to ws_notify_change (broadcasts to all).
 */
void ws_notify_except(ws_msg_type_t type, int idx, int skip_fd);

/*
 * Broadcast a raw pre-built binary frame to all active clients.
 * First byte must be the ws_msg_type_t.  May be called from any task.
 */
void ws_broadcast_raw(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif
