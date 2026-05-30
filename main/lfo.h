#pragma once
/*
 * lfo.h — 4 LFOs per lane + modulation matrix (Phase 3)
 *
 * Integer hot path: LFO phase is uint32_t Q24, shape lookup in 256-entry
 * wavetable. Rate can be Hz (free-running) or clock-synced (1/4–1/32 note).
 * Evaluated once per audio block, not per sample.
 *
 * Modulation matrix: 4 sources × 8 destinations × depth.
 * Applied in lane_apply_mod() once per block, before the audio task renders.
 */

#include <stdint.h>
#include <stdbool.h>
#include "synth.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── LFO shape ───────────────────────────────────────────────────────────── */
typedef enum {
    LFO_SINE = 0,
    LFO_TRIANGLE,
    LFO_SQUARE,
    LFO_SAW,
    LFO_SAMPLE_HOLD,   /* S&H: new random value on each cycle                */
    LFO_SHAPE_COUNT
} lfo_shape_t;

/* ── LFO sync mode ───────────────────────────────────────────────────────── */
typedef enum {
    LFO_FREE = 0,       /* rate in Hz                                        */
    LFO_SYNC_1_4,       /* 1/4 note                                          */
    LFO_SYNC_1_8,
    LFO_SYNC_1_16,
    LFO_SYNC_1_32,
} lfo_sync_t;

/* ── LFO ─────────────────────────────────────────────────────────────────── */
#define NUM_LANE_LFOS  4

typedef struct {
    lfo_shape_t shape;
    lfo_sync_t  sync;
    float       rate_hz;   /* used when sync == LFO_FREE                     */
    float       depth;     /* output scale: output ∈ [-depth, +depth]        */
    uint32_t    phase;     /* Q24 accumulator                                */
    uint32_t    step;      /* Q24 step per audio block                       */
    float       output;    /* current block output (updated by lfo_tick)     */
    bool        enabled;
    uint16_t    sh_lfsr;   /* LFSR for S&H mode                             */
    bool        sh_prev_phase_high; /* edge detection for S&H trigger        */
} lfo_t;

/* ── Modulation destinations ─────────────────────────────────────────────── */
typedef enum {
    MOD_DEST_PITCH = 0,       /* synth pitch detune in semitones             */
    MOD_DEST_AMPLITUDE,       /* multiply lane output level                  */
    MOD_DEST_FILTER_CUTOFF,   /* synth filter cutoff (if applicable)         */
    MOD_DEST_PAN,             /* lane pan position                           */
    MOD_DEST_ARP_STEP_DIV,    /* arp step division (step_div field)          */
    MOD_DEST_FX_PARAM0,       /* first FX parameter in the chain             */
    MOD_DEST_FX_PARAM1,
    MOD_DEST_FX_PARAM2,
    MOD_DEST_COUNT
} mod_dest_t;

/* ── Modulation source ───────────────────────────────────────────────────── */
typedef enum {
    MOD_SRC_LFO1 = 0,
    MOD_SRC_LFO2,
    MOD_SRC_LFO3,
    MOD_SRC_LFO4,
    MOD_SRC_VELOCITY,   /* last note-on velocity (static per note)          */
    MOD_SRC_NOTE,       /* MIDI note number (normalised 0–1)                */
    MOD_SRC_RANDOM,     /* random value refreshed each block                */
    MOD_SRC_ENV,        /* lane ADSR envelope follower output (0–1)         */
    MOD_SRC_COUNT
} mod_src_t;

/* ── Modulation matrix entry ─────────────────────────────────────────────── */
typedef struct {
    mod_src_t  src;
    mod_dest_t dest;
    float      depth;   /* signed: -1.0 to +1.0 × dest range               */
    bool       enabled;
} mod_route_t;

#define MOD_MATRIX_SIZE  (NUM_LANE_LFOS * MOD_DEST_COUNT)

/* ── Per-lane LFO + mod-matrix block ─────────────────────────────────────── */
typedef struct {
    lfo_t       lfo[NUM_LANE_LFOS];
    mod_route_t routes[MOD_MATRIX_SIZE];
    int         route_count;

    /* Evaluated outputs (one per destination, refreshed each block) */
    float       mod_out[MOD_DEST_COUNT];

    /* External source values written by audio task before lane_lfo_tick() */
    float       env_input;      /* current ADSR output (0–1) for MOD_SRC_ENV */
    float       velocity_input; /* last note velocity (0–1) for MOD_SRC_VELOCITY */
    float       note_input;     /* last MIDI note normalised 0–1             */
} lane_lfo_t;

/* ── API ─────────────────────────────────────────────────────────────────── */

/* Initialise to defaults (all disabled). */
void lane_lfo_init(lane_lfo_t *ll);

/* Set LFO rate. sync == LFO_FREE: use rate_hz.
 * sync != LFO_FREE: step computed from master clock.
 * tick_rate = PPQN; bpm used for clock-sync rate computation. */
void lfo_set_rate(lfo_t *l, lfo_sync_t sync, float rate_hz,
                  float bpm, uint32_t tick_rate);

/* Advance all LFOs by one audio block and evaluate all mod routes.
 * Stores results in ll->mod_out[]. Call once per block from audio task. */
void lane_lfo_tick(lane_lfo_t *ll, uint32_t block_size);

/* Add a mod route. Returns false if MOD_MATRIX_SIZE exceeded. */
bool lane_lfo_add_route(lane_lfo_t *ll, mod_src_t src,
                        mod_dest_t dest, float depth);

/* Query a mod output (call after lane_lfo_tick). */
static inline float lane_lfo_get(const lane_lfo_t *ll, mod_dest_t dest)
{
    return ll->mod_out[dest];
}

#ifdef __cplusplus
}
#endif
