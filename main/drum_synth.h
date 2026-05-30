#pragma once
/*
 * drum_synth.h — Analog drum synthesizer lane (TR-808 style)
 *
 * A LANE_TYPE_DRUMSYNTH lane owns a dsyn_t: a bank of DSYN_MAX_ROWS rows,
 * each row an analog drum *voice* (no samples) plus its own step grid.
 * Voices are generated procedurally — sine bodies + pitch envelopes for the
 * membranes (kick / toms / congas), tuned-square clusters through band/high-
 * pass for the metals (hats / cymbal / cowbell), and filtered noise for the
 * snare / clap / shakers. Every voice exposes a small set of normalized 0..1
 * "knobs" (tune / decay / tone / snap / drive / level) so the sound can be
 * reshaped live — the whole point of synthesizing rather than sampling.
 *
 * Audio-task interface mirrors drum_seq_tick(): dsyn_tick() is called once
 * per lane per audio block with the current lane_tick, advances the grid,
 * retriggers voices, and renders all active voices into the caller's int32
 * bus pair.  No heap allocation in the tick path — the dsyn_t is one PSRAM
 * block allocated up front.
 */

#include <stdint.h>
#include <stdbool.h>
#include "synth_voice.h"   /* sv_svf_t, sv_wt sine table, SAMPLE_RATE via audio.h */
#include "drum_seq.h"      /* drum_step_t — reuse the step-cell layout            */

#ifdef __cplusplus
extern "C" {
#endif

/* ── Limits ──────────────────────────────────────────────────────────────── */
#define DSYN_MAX_ROWS   16
#define DSYN_MAX_STEPS  DRUM_MAX_STEPS   /* keep grids interchangeable with drum_seq */
#define DSYN_NUM_OSC    6                /* metallic cluster uses 6 oscillators       */

/* ── Voice algorithms (the full 808 kit) ─────────────────────────────────── */
typedef enum {
    DSYN_BD = 0,   /* bass drum   — sine + pitch sweep                         */
    DSYN_SD,       /* snare       — twin sine body + band-passed noise         */
    DSYN_LT,       /* low  tom    — sine + pitch sweep                         */
    DSYN_MT,       /* mid  tom                                                 */
    DSYN_HT,       /* high tom                                                 */
    DSYN_LC,       /* low  conga                                              */
    DSYN_MC,       /* mid  conga                                              */
    DSYN_HC,       /* high conga                                              */
    DSYN_RS,       /* rim shot    — short resonant pulse                       */
    DSYN_CP,       /* hand clap   — retriggered noise bursts + tail            */
    DSYN_CB,       /* cowbell     — two squares → band-pass                    */
    DSYN_CY,       /* cymbal      — 6-osc cluster → HP, long decay             */
    DSYN_OH,       /* open  hat   — 6-osc cluster → HP, medium decay           */
    DSYN_CH,       /* closed hat  — 6-osc cluster → HP, short decay            */
    DSYN_MA,       /* maracas     — high-passed noise, very short              */
    DSYN_CL,       /* claves      — single high sine, short                    */
    DSYN_VOICE_COUNT
} dsyn_voice_t;

/* ── Per-row params — normalized 0..1 "knobs" (pan is -1..+1) ────────────── */
typedef struct {
    dsyn_voice_t type;
    float        tune;     /* pitch offset / fundamental                       */
    float        decay;    /* amplitude tail length                            */
    float        tone;     /* brightness — filter cutoff / harmonic balance    */
    float        snap;     /* noise / snappy amount, or pitch-sweep depth       */
    float        drive;    /* tanh saturation 0 = clean                         */
    float        level;    /* 0..1 output gain                                 */
    float        pan;      /* -1 (L) … +1 (R)                                  */
} dsyn_params_t;

/* ── Per-row runtime voice state (audio-task owned) ──────────────────────── */
typedef struct {
    bool      active;
    float     vel;                 /* trigger velocity × level, 0..1           */
    float     pan_l, pan_r;        /* resolved pan gains                       */

    /* Primary amplitude envelope (exponential decay) */
    float     amp, amp_coef;
    /* Secondary amplitude envelope (snare body / clap stages / hat sub) */
    float     amp2, amp2_coef;

    /* Pitch envelope (membrane voices) */
    float     pitch_env, pitch_coef;

    /* Oscillator bank — phases in [0,1), freqs in Hz */
    float     ph[DSYN_NUM_OSC];
    float     freq[DSYN_NUM_OSC];
    float     base_freq;           /* membrane fundamental for pitch sweep     */
    float     sweep_hz;            /* added to base at trigger, decays away    */

    /* Noise generator */
    uint32_t  lfsr;

    /* Filters (set per trigger) */
    sv_svf_t  f1, f2;

    /* Clap retrigger state */
    uint8_t   burst;               /* remaining quick re-bursts                */
    float     burst_t;             /* seconds until next burst                 */
} dsyn_state_t;

/* ── Drum-synth lane ─────────────────────────────────────────────────────── */
typedef struct dsyn_s {
    dsyn_params_t params[DSYN_MAX_ROWS];
    dsyn_state_t  state [DSYN_MAX_ROWS];
    drum_step_t   steps [DSYN_MAX_ROWS][DSYN_MAX_STEPS];

    uint8_t       row_count;
    uint8_t       step_count;        /* global grid: 8 / 16 / 32 / 64          */
    float         accent_gain;       /* accented-step multiplier (default 1.5) */

    /* Sequencer timing — recomputed when loop_len_ticks / step_count change */
    uint32_t      ticks_per_step;
    uint8_t       cur_step      [DSYN_MAX_ROWS];
    uint32_t      last_step_tick[DSYN_MAX_ROWS];
} dsyn_t;

/* ── API ─────────────────────────────────────────────────────────────────── */

/* Allocate a dsyn_t in PSRAM, zero-init, and seed a default 4-row 808 kit
 * (BD / SD / CH / OH). Returns NULL on allocation failure. */
dsyn_t *dsyn_alloc(void);

/* Apply the canonical default knob set + base pattern for `type` to row `ri`. */
void dsyn_row_set_voice(dsyn_t *ds, int ri, dsyn_voice_t type);

/* Recompute ticks_per_step from loop_len_ticks and step_count. */
void dsyn_update_timing(dsyn_t *ds, uint32_t loop_len_ticks);

/* Reset all step cursors so the next tick fires step 0 at the loop origin. */
void dsyn_reset(dsyn_t *ds, uint32_t lane_tick_now);

/* Immediately (re)trigger row `ri` at `velocity` (1..127; 0 → default).
 * Works while the transport is stopped — used for live finger-drumming. */
void dsyn_trigger_row(dsyn_t *ds, int ri, uint8_t velocity);

/* Audio-task render. Advances the grid by `tick_delta`, retriggers due steps,
 * and ADDS all active voices into out_l/out_r (int32, n_frames long). */
void dsyn_tick(dsyn_t *ds, uint32_t lane_tick, uint32_t tick_delta,
               int32_t *out_l, int32_t *out_r, int n_frames);

/* Human-readable 2–3 char voice label (e.g. "BD", "CH"). */
const char *dsyn_voice_name(dsyn_voice_t type);

#ifdef __cplusplus
}
#endif
