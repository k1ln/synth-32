#pragma once

#include "esp_http_server.h"

/*
 * sounds_http_register() — call once after httpd_start() to install:
 *   GET  /sounds/manifest  — stream current .manifest.json from SD
 *   PUT  /sounds/upload    — stream-write a WAV file to SD (4 KB chunks, no full-file buffer)
 *   POST /sounds/commit    — atomically replace .manifest.json with uploaded body
 */
void sounds_http_register(httpd_handle_t server);

/*
 * sounds_check_sd() — call once at boot after sdcard_init().
 *
 * Compares the manifest baked into firmware (main/sounds_manifest.json, embedded
 * via COMPONENT_EMBED_TXTFILES) against /sdcard/sounds/.manifest.json.
 *
 * Logs to serial:
 *   - Total files expected vs present
 *   - Each missing file  (LOGW "missing: <path>")
 *   - Each changed file  (LOGW "changed: <path>")
 *   - Summary line       (LOGI "SD check: N ok, M missing, K changed")
 *
 * Never reads audio files — only compares JSON manifests.
 * Returns the number of files that need uploading (0 = SD is in sync).
 */
int sounds_check_sd(void);
