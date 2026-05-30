#pragma once
/*
 * ws_cmd.h — WebSocket command dispatcher.
 *
 * ws_cmd_dispatch() decodes a WS_CMD_* binary frame, writes the mutation
 * into g_song / lane_t / drum_seq_t, then calls ws_notify_change() to
 * broadcast the resulting WS_MSG_* diff to all clients.
 */

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* fd — the sender's socket fd; needed for WS_CMD_AUDIO_STREAM */
void ws_cmd_dispatch(const uint8_t *frame, size_t len, int fd);

#ifdef __cplusplus
}
#endif
