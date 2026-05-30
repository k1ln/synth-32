#pragma once
/*
 * ws_audio.h — WebSocket audio streaming (M8).
 *
 * Single audio client at a time.  When a client sends WS_CMD_AUDIO_STREAM
 * enable=1 any existing client is evicted (WS_MSG_AUDIO_DROPPED), the PA is
 * muted, and the new client claims the slot.  On release (enable=0 or socket
 * close) the PA is re-enabled.
 *
 * Call ws_audio_push() from the audio task immediately after each I2S write.
 * Call ws_audio_start() / ws_audio_stop() from ws_cmd.c.
 * Call ws_audio_client_closed() from the WS server on socket close.
 */

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialise ring buffer in PSRAM and spawn the ws_audio_task. */
void ws_audio_init(void);

/*
 * Called by audio task (core 1) after every I2S block.
 * buf  — interleaved int16 stereo, n_frames samples per channel.
 * Thread-safe: writes ring head, audio task reads tail.
 */
void ws_audio_push(const int16_t *buf, int n_frames);

/*
 * Claim the audio stream for fd.  Drops existing client first if needed.
 * Safe to call from WS server task.
 */
void ws_audio_start(int fd);

/*
 * Release the audio stream (enable=0 or client disconnect).
 * fd is used only to guard against stale calls from a different client.
 * Pass -1 to force-release regardless of current owner.
 */
void ws_audio_stop(int fd);

/* Called by ws_server.c on HTTPD_WS_TYPE_CLOSE to clean up the audio slot. */
void ws_audio_client_closed(int fd);

#ifdef __cplusplus
}
#endif
