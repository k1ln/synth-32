#pragma once
/*
 * synth_voice.h — Shared types: polyphonic voice pool, per-voice ADSR,
 *                 wavetable lookup, and note-to-frequency table.
 *
 * Used by all synth implementations. Integer/Q-point math throughout;
 * floats only at init / param-set time, never in the render hot path.
 */

#include <stdint.h>
#include <stdbool.h>
#include <math.h>    /* tanhf — soft clipper */
#include "audio.h"   /* SAMPLE_RATE */

#ifdef __cplusplus
extern "C" {
#endif

/* ── Wavetable ───────────────────────────────────────────────────────────── */
#define SV_WT_SIZE   256           /* must be power of 2                     */
#define SV_WT_AMP    28000         /* 16-bit headroom for mixing             */

/* Shape IDs — stored in synth param, also index into s_sv_wt[] */
typedef enum {
    SV_WAVE_SINE = 0,
    SV_WAVE_SAW,
    SV_WAVE_SQUARE,
    SV_WAVE_TRIANGLE,
    SV_WAVE_NOISE,      /* white noise — generated live, no table            */
    SV_WAVE_CUSTOM1,    /* user-uploaded single-cycle wavetable              */
    SV_WAVE_CUSTOM2,
    SV_WAVE_CUSTOM3,
    SV_WAVE_CUSTOM4,
    SV_WAVE_COUNT
} sv_wave_t;

/* sv_wt[]: built-in waveforms occupy [0..SV_WAVE_NOISE-1];
 * custom slots [SV_WAVE_CUSTOM1..SV_WAVE_CUSTOM4-1] are user-writable.
 * SV_WAVE_NOISE itself has no table (generated live). */
extern int16_t sv_wt[SV_WAVE_COUNT][SV_WT_SIZE];

/* Call once at startup to fill built-in wavetables. */
void sv_wt_init(void);

/* Load a single-cycle WAV file from SD into custom slot (SV_WAVE_CUSTOM1..4).
 * Resamples to SV_WT_SIZE samples.  Returns false on error. */
bool sv_wt_load_custom(sv_wave_t slot, const char *wav_path);

/* ── Note → frequency ────────────────────────────────────────────────────── */
/* Return frequency (Hz) for MIDI note 0–127. */
float sv_note_to_hz(uint8_t note);

/* Phase step for a given Hz into a SV_WT_SIZE table @ SAMPLE_RATE.
 * Q24 fixed-point: step = (hz * WT_SIZE / SR) * 2^24               */
static inline uint32_t sv_hz_to_step(float hz)
{
    return (uint32_t)(hz * (float)SV_WT_SIZE / (float)SAMPLE_RATE * (1u << 24));
}

/* ── Per-voice ADSR ──────────────────────────────────────────────────────── */
typedef enum {
    SV_ST_IDLE = 0,
    SV_ST_ATTACK,
    SV_ST_DECAY,
    SV_ST_SUSTAIN,
    SV_ST_RELEASE,
} sv_stage_t;

typedef struct {
    sv_stage_t stage;
    uint32_t   cnt;       /* sample counter within current stage             */
    uint32_t   atk;       /* attack  samples                                 */
    uint32_t   dcy;       /* decay   samples                                 */
    uint32_t   rel;       /* release samples                                 */
    float      sus;       /* sustain level 0.0–1.0                           */
    float      level;     /* current amplitude 0.0–1.0                       */
    float      rel_lvl;   /* level snapshot at release start                 */
} sv_adsr_t;

/* Set ADSR from millisecond values. Safe to call from UI task. */
void sv_adsr_set(sv_adsr_t *a, float atk_ms, float dcy_ms,
                 float sus, float rel_ms);

/* Trigger (note-on) — restarts from current level to avoid click. */
void sv_adsr_gate_on (sv_adsr_t *a);
/* Release (note-off). */
void sv_adsr_gate_off(sv_adsr_t *a);

/* Advance one sample; returns current envelope level. Integer-fast path.
 * Inline for speed — called inside the render loop. */
static inline float sv_adsr_tick(sv_adsr_t *a)
{
    switch (a->stage) {
    case SV_ST_ATTACK:
        a->level = a->atk ? (float)a->cnt / (float)a->atk : 1.0f;
        if (a->cnt++ >= a->atk) {
            a->level = 1.0f; a->cnt = 0; a->stage = SV_ST_DECAY;
        }
        break;
    case SV_ST_DECAY:
        a->level = 1.0f - (1.0f - a->sus) * (float)a->cnt
                        / (float)(a->dcy ? a->dcy : 1);
        if (a->cnt++ >= a->dcy) {
            a->level = a->sus; a->stage = SV_ST_SUSTAIN;
        }
        break;
    case SV_ST_SUSTAIN:
        a->level = a->sus;
        break;
    case SV_ST_RELEASE:
        a->level = a->rel_lvl * (1.0f - (float)a->cnt
                        / (float)(a->rel ? a->rel : 1));
        if (a->cnt++ >= a->rel) {
            a->level = 0.0f; a->stage = SV_ST_IDLE;
        }
        break;
    default:
        a->level = 0.0f;
        break;
    }
    return a->level;
}

/* ── Single oscillator voice ─────────────────────────────────────────────── */
#define SV_MAX_VOICES  8

typedef struct {
    bool      active;
    uint8_t   note;
    uint8_t   vel;        /* 1–127                                           */
    uint32_t  phase;      /* Q24 phase accumulator                           */
    uint32_t  step;       /* Q24 phase step                                  */
    sv_adsr_t adsr;
    int16_t   lfsr;       /* 16-bit LFSR for noise oscillator                */
} sv_voice_t;

/* Steal oldest releasing voice or first idle. Returns voice index. */
int sv_voice_steal(sv_voice_t *pool, int pool_size, uint8_t note);
void sv_voice_note_on (sv_voice_t *v, uint8_t note, uint8_t vel,
                       uint32_t step, sv_adsr_t *adsr_template);
void sv_voice_note_off(sv_voice_t *v);

/* Render one voice sample from wavetable; returns int32 scaled by velocity. */
static inline int32_t sv_voice_render(sv_voice_t *v, const int16_t *wt,
                                      sv_wave_t wave)
{
    float env = sv_adsr_tick(&v->adsr);
    if (v->adsr.stage == SV_ST_IDLE) { v->active = false; return 0; }
    int32_t s;
    if (wave == SV_WAVE_NOISE) {
        /* Galois LFSR — period 65535, cheap white noise */
        v->lfsr = (int16_t)((uint16_t)v->lfsr >> 1) ^ (((uint16_t)v->lfsr & 1) ? 0xB400 : 0);
        s = v->lfsr;
    } else {
        uint8_t idx = (uint8_t)(v->phase >> 24);
        s = wt[idx];
    }
    v->phase += v->step;
    return (int32_t)(s * env * v->vel / 127);
}

/* ── Simple biquad (Chamberlin SVF) — used by subtractive / noise synths ── */
typedef struct {
    float lp, hp, bp;   /* last outputs */
    float f, q;         /* frequency coefficient, damping */
} sv_svf_t;

/* Set SVF coefficients from Hz and resonance Q (0.5–20). */
void sv_svf_set(sv_svf_t *f, float hz, float q);

/* Process one sample through SVF; returns LP output by default. */
static inline float sv_svf_tick(sv_svf_t *f, float in)
{
    f->hp = in - f->lp - f->q * f->bp;
    f->bp = f->f * f->hp + f->bp;
    f->lp = f->f * f->bp + f->lp;
    return f->lp;
}

/* ── Soft clipper ────────────────────────────────────────────────────────── */
/* Tanh-knee limiter for an int32 sample, returning an int16-ranged value.
 * Below SV_SOFTCLIP_TH the signal passes through bit-exact (the common case,
 * and cheap); above it the level is smoothly compressed toward 0 dBFS so chord
 * peaks and in-phase voice pile-ups saturate gently instead of hard-clipping or
 * wrapping the int16 cast.  Used both per-synth (chord sum) and on the master
 * bus (sum of lanes). */
#define SV_SOFTCLIP_TH 0.75f
static inline int32_t sv_soft_clip(int32_t v)
{
    float x = (float)v * (1.0f / 32768.0f);
    float a = x < 0.0f ? -x : x;
    if (a <= SV_SOFTCLIP_TH) return v;                 /* linear region */
    float over = (a - SV_SOFTCLIP_TH) / (1.0f - SV_SOFTCLIP_TH);
    float y = SV_SOFTCLIP_TH + (1.0f - SV_SOFTCLIP_TH) * tanhf(over);
    int32_t o = (int32_t)(y * 32768.0f);
    if (o > 32767) o = 32767;
    return x < 0.0f ? -o : o;
}

#ifdef __cplusplus
}
#endif
