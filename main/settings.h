#pragma once
/*
 * settings.h — Global device settings (M5)
 *
 * Persisted to /sdcard/settings.json at write time and loaded once at boot
 * (after sdcard_init(), before lane_init_all() / audio_init()).
 *
 * Fields deliberately minimal — only things that must survive a reboot and
 * that the user would want to persist independently of any song file.
 */

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SETTINGS_PATH "/sdcard/settings.json"

typedef struct {
    /* Clock defaults written into g_song.clock on every boot */
    float    bpm;            /* 20–300 */
    uint32_t ppqn;           /* ticks per beat, usually 96 */
    uint32_t beats_per_bar;  /* time sig numerator, usually 4 */

    /* WiFi AP credentials (mirrors wifi_http.c NVS keys for reference) */
    char     wifi_ssid[33];
    char     wifi_pass[65];

    /* Last loaded song path (auto-reload on boot when non-empty) */
    char     last_song[128];

    /* Master output volume 0.0–1.0 and pan -1.0–1.0 */
    float    master_volume;
    float    master_pan;
} settings_t;

/* Global settings instance — initialised to defaults by settings_load(). */
extern settings_t g_settings;

/* Load from SETTINGS_PATH. Missing file → fills g_settings with defaults.
 * Always succeeds (missing SD card → defaults). */
void settings_load(void);

/* Atomically write g_settings to SETTINGS_PATH (.tmp + rename).
 * Returns false only on SD write failure. */
bool settings_save(void);

#ifdef __cplusplus
}
#endif
