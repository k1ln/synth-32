#pragma once
/*
 * drum_seq.h — Drum step sequencer (Phase 2.5)
 *
 * A drum lane holds DRUM_MAX_ROWS instrument rows. Each row has its own
 * sample, independent playback mode, and a step grid. All rows share the
 * lane's loop_len_ticks and step resolution.
 *
 * Audio-task interface: drum_seq_tick() is called once per lane per
 * audio block with the current lane_tick. It retriggers rows as needed
 * and drains all active rings into the caller's int32 bus pair.
 *
 * No heap allocation inside tick path — all rings pre-allocated in PSRAM.
 */

#include <stdint.h>
#include <stdbool.h>
#include "wav_player.h"   /* wav_lane_t, STREAM_BUF_FRAMES */

#ifdef __cplusplus
extern "C" {
#endif

/* ── Constants ───────────────────────────────────────────────────────────── */
#define DRUM_MAX_ROWS      16
#define DRUM_MAX_STEPS     64
#define DRUM_MULTI_HIT_MAX  8

/* ── Step conditions ─────────────────────────────────────────────────────── */
typedef enum {
    STEP_COND_ALWAYS = 0,   /* fire every time                                */
    STEP_COND_FILL,         /* fire only on the last bar of a phrase          */
    STEP_COND_NOT_FILL,     /* fire on every bar except the last              */
    STEP_COND_EVERY2,       /* fire every 2nd pass                            */
    STEP_COND_EVERY3,       /* fire every 3rd pass                            */
    STEP_COND_EVERY4,       /* fire every 4th pass                            */
} step_cond_t;

/* ── Step cell ───────────────────────────────────────────────────────────── */
typedef struct {
    uint8_t    velocity;    /* 0 = off; 1–127 = on                            */
    bool       accent;      /* louder hit, scaled by drum_seq_t.accent_gain   */
    uint8_t    probability; /* 0–100 %; 100 = always fire (default)           */
    uint8_t    ratchet;     /* 1 = single hit; 2–4 = subdivided repeats       */
    step_cond_t condition;  /* conditional trigger rule                       */
} drum_step_t;

/* ── Sample playback mode ────────────────────────────────────────────────── */
typedef enum {
    SAMPLE_PLAY_ONE_SHOT = 0,
    SAMPLE_PLAY_LOOP,
    SAMPLE_PLAY_LOOP_PING,
    SAMPLE_PLAY_GATE,
    SAMPLE_PLAY_SLICE,
} sample_play_mode_t;

/* ── Drum row ────────────────────────────────────────────────────────────── */
typedef struct drum_seq_s drum_seq_t;   /* forward for back-ref in trigger fn */

typedef struct {
    /* Sample identity */
    char                wav_path[128];

    /* Primary ring — allocated in PSRAM by drum_row_alloc_rings() */
    wav_lane_t         *ring;

    /* Playback mode */
    sample_play_mode_t  play_mode;
    uint32_t            loop_start;     /* byte offset into PCM data          */
    uint32_t            loop_end;       /* 0 = EOF                            */
    uint32_t            slice_start;
    uint32_t            slice_end;

    /* Multi-hit chain */
    uint8_t             multi_hit_count;   /* 0/1 = normal; 2–8 = chain       */
    char                multi_wav[DRUM_MULTI_HIT_MAX][128];
    uint16_t            multi_gap_ms[DRUM_MULTI_HIT_MAX];
    wav_lane_t         *multi_ring[DRUM_MULTI_HIT_MAX];  /* PSRAM, pre-alloc  */
    uint8_t             multi_idx;         /* runtime: active hit in chain    */
    uint32_t            multi_next_tick;   /* tick when next chained hit fires */

    /* Step grid */
    drum_step_t         steps[DRUM_MAX_STEPS];
    uint8_t             step_count;        /* active steps; 0 = use seq global  */

    /* Per-row mix params */
    float               volume;            /* 0.0 – 1.0                         */
    float               pan;               /* -1.0 (L) … +1.0 (R)               */
    float               pitch;             /* ±24 semitones (rate shift)         */
    bool                mute;
    bool                solo;
    bool                reverse;           /* fill ring backwards                */
    float               start_offset;      /* 0.0–1.0: skip into sample          */
    uint8_t             mute_group;        /* 0 = no group; 1–8 = exclusive group */
    uint8_t             current_step;      /* per-row step counter (polyrhythm)   */
    uint32_t            last_step_tick;    /* per-row tick of last trigger        */
    uint8_t             pass_count;        /* loop pass counter for conditions    */

    /* Runtime: which wav_lane slot this row occupies */
    int                 lane_slot;         /* index into wav engine lane pool     */
} drum_row_t;

/* ── Drum sequencer ──────────────────────────────────────────────────────── */
struct drum_seq_s {
    drum_row_t  rows[DRUM_MAX_ROWS];
    uint8_t     row_count;
    uint8_t     step_count;       /* global step count: 8 / 16 / 32 / 64       */
    uint8_t     current_step;     /* global step counter (for shared-step rows) */
    float       accent_gain;      /* multiplier for accented steps (default 1.5)*/

    /* Ticks-per-step cache — recomputed when loop_len_ticks or step_count changes */
    uint32_t    ticks_per_step;   /* loop_len_ticks / step_count                */
    uint32_t    last_step_tick;   /* lane_tick at which the last global step triggered */

    uint32_t    bar_count;        /* absolute bar counter, increments each loop wrap */
    uint8_t     phrase_len;       /* bars per phrase for FILL conditions (default 4) */

    /* Mute-group last-triggered row: mute_group_last[g] = row index, 0xFF = none */
    uint8_t     mute_group_last[8];
};

/* Fill euclidean pattern for row using Bjorklund algorithm.
 * hits: number of active steps; steps: total steps in row. */
void drum_seq_euclidean(drum_row_t *row, uint8_t hits, uint8_t steps, uint8_t velocity);

/* ── API ─────────────────────────────────────────────────────────────────── */

/* Allocate a drum_seq_t in PSRAM and zero-initialise it. */
drum_seq_t *drum_seq_alloc(void);

/* Allocate a wav_lane pool slot for a row (if not already allocated) and open
 * the WAV file at sdcard_path (absolute path, e.g. "/sdcard/sounds/kit/x.wav").
 * Sets row->ring and row->lane_slot. Returns true on success. */
bool drum_row_load_wav(drum_row_t *row, const char *sdcard_path);

/* Allocate wav_lane rings in PSRAM for all rows in a drum lane.
 * wav_lane_pool: pointer to the engine's lane array (from wav_player).
 * pool_start / pool_count: which slots in the pool this drum lane may use.
 * Returns false if insufficient pool space. */
bool drum_seq_alloc_rings(drum_seq_t *seq, wav_lane_t *pool,
                          int pool_start, int pool_count);

/* Recompute ticks_per_step from loop_len_ticks and step_count.
 * Call whenever either changes. */
void drum_seq_update_timing(drum_seq_t *seq, uint32_t loop_len_ticks);

/* Change the global step count to new_sc and proportionally rescale every row's
 * pattern so existing hits keep their position in musical time ("stretch": a hit
 * on step 4-of-16 becomes step 8-of-32). Per-row step counts are scaled by the
 * same ratio, so polyrhythm rows stay independent. Recomputes timing. */
void drum_seq_set_step_count(drum_seq_t *seq, uint8_t new_sc, uint32_t loop_len_ticks);

/* Reset current_step to 0 and last_step_tick to lane_tick_now.
 * Called from lane restart (LIVE/SONG mode). */
void drum_seq_reset(drum_seq_t *seq, uint32_t lane_tick_now);

/* Remove row at index ri, shifting subsequent rows down.
 * Stops and clears the row's wav ring before removal.
 * Returns false if ri is out of range. */
bool drum_seq_remove_row(drum_seq_t *seq, int ri);

/* Called from audio task for each active drum lane.
 * lane_tick : current lane_tick after lanes_tick()
 * tick_delta: ticks advanced this block
 * out_l/out_r: int32 accumulators (AUDIO_BUF_FRAMES long) — ADD into them
 * n_frames  : AUDIO_BUF_FRAMES */
void drum_seq_tick(drum_seq_t *seq, uint32_t lane_tick, uint32_t tick_delta,
                   int32_t *out_l, int32_t *out_r, int n_frames);

/* Immediate one-shot trigger of a single row, for LIVE finger-drumming.
 * Fires the row's primary sample right now (independent of the sequencer
 * clock — works while transport is stopped) at the given velocity. The hit
 * is drained through the row's volume/pan and the owning lane's FX chain by
 * the next drum_seq_tick, so it sounds exactly like the song row. Ignores
 * muted rows and does not fire the multi-hit chain. */
void drum_seq_trigger_row(drum_seq_t *seq, int ri, uint8_t velocity);

#ifdef __cplusplus
}
#endif
