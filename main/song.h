#pragma once
/*
 * song.h — Song save / load / new (M5)
 *
 * Songs are stored as .s32 files (JSON text) on the SD card under
 * /sdcard/songs/.  Atomic write is done via a .tmp file + rename.
 *
 * song_new()  — reset g_song to a blank song with settings defaults.
 * song_save() — serialise g_song to path atomically.
 * song_load() — parse a .s32 file into g_song (replaces current state).
 *
 * Serialised per-lane fields:
 *   type, active, mute, volume, pan, loop_len_ticks, wav_path (WAV lanes),
 *   synth type_id (SYNTH lanes),
 *   piano_roll notes (SYNTH lanes),
 *   drum_seq rows: wav_path, step_count, steps[vel+accent],
 *                  play_mode, volume, pan, pitch, mute, reverse
 *   arp: enabled, mode, octave_range, step_div, gate_pct, vel_mode, fixed_vel,
 *        swing_pct, latch, retrigger
 *   lfo[4]: enabled, shape, sync, rate_hz, depth
 *   adsr: atk_ms, dcy_ms, sus, rel_ms
 *   fx chain: type, enabled, params (flat float array, 8 params max per node)
 *
 * synth_inst_t params are NOT serialised in v1 — the synth is rebuilt with
 * default params from synth_new(type_id) on load.  Param serialisation can
 * be added per-type later without breaking the file format.
 */

#include <stdbool.h>
#include "lane.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SONG_DIR          "/sdcard/songs"
#define SONG_EXT          ".s32"
#define SONG_NAME_DEFAULT "Untitled"

/* Reset g_song to a blank song, applying g_settings clock defaults. */
void song_new(void);

/* Atomically serialise g_song to path.
 * Sets g_song.dirty = false on success.
 * Returns false on SD write failure. */
bool song_save(const char *path);

/* Load a .s32 file into g_song (frees existing lane allocations first).
 * Sets g_song.dirty = false on success.
 * Returns false if file is missing, corrupt, or SD not mounted. */
bool song_load(const char *path);

/* Save with auto-incremented version suffix: mysong_001.s32, _002.s32 …
 * Returns the actual path written into out_path (must be at least 160 bytes). */
bool song_save_versioned(char *out_path, size_t out_len);

/* Save lane layout only (no pattern/note data) as a template file.
 * Templates go to SONG_DIR/templates/. */
bool song_save_template(const char *name);

/* Load a template (lane types/names/FX only, no patterns). */
bool song_load_template(const char *name);

/* Write autosave file if dirty and interval elapsed.
 * Call from audio task or a periodic timer. */
void song_autosave_tick(uint32_t master_bar);

/* Start the time-based autosave background task (core 0, prio 3).
 * Saves the canonical song path at most once per 60 s when dirty.
 * Call once from app_main after song_new(). */
void song_autosave_task_start(void);

#ifdef __cplusplus
}
#endif
