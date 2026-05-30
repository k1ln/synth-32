#pragma once
/*
 * piano_roll.h — Piano roll note storage + playback (Phase 2.6)
 *
 * Stores timestamped note events sorted by tick_start. The audio task
 * scans with piano_roll_tick() once per block; at 96 PPQN most blocks
 * have 0 events — the scan is a tight loop over a contiguous PSRAM array.
 *
 * synth_inst_t is forward-declared; the actual vtable lives in synth.h (Phase 3).
 */

#include <stdint.h>
#include <stdbool.h>
#include "synth.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PIANO_ROLL_MAX_NOTES  256

/* ── Note event ──────────────────────────────────────────────────────────── */
typedef struct {
    uint32_t tick_start;   /* tick within loop where note-on fires     */
    uint32_t tick_len;     /* duration in ticks (0 = hold to loop end) */
    uint8_t  note;         /* MIDI note number 0–127                   */
    uint8_t  velocity;     /* 1–127                                    */
} pr_note_t;

/* ── Piano roll ──────────────────────────────────────────────────────────── */
typedef struct piano_roll_s {
    pr_note_t  notes[PIANO_ROLL_MAX_NOTES];  /* sorted by tick_start; in PSRAM */
    uint16_t   note_count;

    /* Editor state (UI read/write, not used by audio task) */
    uint8_t    step_resolution;   /* snap grid: 4/8/16/32/64 steps per bar */
    uint8_t    view_octave;       /* topmost visible octave                 */
    uint32_t   view_tick_offset;  /* leftmost visible tick (h-scroll)       */
    uint32_t   view_ticks_wide;   /* ticks per screen width (zoom)          */
} piano_roll_t;

/* ── API ─────────────────────────────────────────────────────────────────── */

/* Allocate a piano_roll_t in PSRAM. */
piano_roll_t *piano_roll_alloc(void);

/* Insert a note and keep notes[] sorted by tick_start.
 * Returns false if note_count == PIANO_ROLL_MAX_NOTES. */
bool piano_roll_add_note(piano_roll_t *pr, uint32_t tick_start, uint32_t tick_len,
                         uint8_t note, uint8_t velocity);

/* Remove all notes matching note+tick_start. */
void piano_roll_remove_note(piano_roll_t *pr, uint32_t tick_start, uint8_t note);

/* Sort notes by tick_start. Call after bulk edits or paste. */
void piano_roll_sort(piano_roll_t *pr);

/* Reset playback cursor. Call on lane restart (LIVE/SONG mode). */
void piano_roll_reset(piano_roll_t *pr);

/* Called from audio task every block.
 * prev_tick: lane_tick at start of block (before tick_delta was added)
 * lane_tick : lane_tick after tick advance
 * synth     : target synth instance (may be NULL — events are silently dropped)
 *
 * Fires note_on / note_off on synth for any events in [prev_tick, lane_tick).
 * Integer scan — no allocation, no floats. */
/* loop_len: lane->loop_len_ticks — used to detect wrap-around */
void piano_roll_tick(piano_roll_t *pr, uint32_t prev_tick, uint32_t lane_tick,
                     uint32_t loop_len, synth_inst_t *synth);

#ifdef __cplusplus
}
#endif
