/*
 * synth_osc.c — Synth types 0–5 (Phase 3)
 *
 *  0  Mono Wavetable     1 osc, 1 voice, ADSR
 *  1  Poly Wavetable     5-voice polyphonic
 *  2  Super Saw          7 detuned sawtooth osc, stereo width
 *  3  FM 2-op            carrier + modulator, ratio, depth
 *  4  FM 4-op            DX7-style 4 operators, 8 algorithm presets
 *  5  Subtractive        saw/square osc → resonant SVF → ADSR
 *
 * All allocations in PSRAM. No malloc / no floats in render hot path
 * (floats only at init / set_param time).
 */

#include <string.h>
#include <math.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "synth.h"
#include "synth_voice.h"

static const char *TAG = "synth_osc";

/* ── Helper: alloc synth struct in PSRAM ─────────────────────────────────── */
#define SYNTH_ALLOC(T) ((T *)heap_caps_calloc(1, sizeof(T), MALLOC_CAP_SPIRAM))

/* ── Param IDs shared across all mono/poly synths ────────────────────────── */
enum {
    P_WAVEFORM = 0,
    P_TUNE,       /* semitone detune ±24 */
    P_ATK, P_DCY, P_SUS, P_REL,
    P_DETUNE,     /* for poly/supersaw: spread cents */
    P_WIDTH,      /* supersaw: stereo width 0–1 */
    P_FM_RATIO,
    P_FM_DEPTH,
    P_FILTER_FREQ,
    P_FILTER_RES,
    P_DRIVE,
    P_ALGO,       /* FM-4op algorithm preset 0–7 */
};

/* ═══════════════════════════════════════════════════════════════════════════
 * Type 0 — Mono Wavetable
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    synth_inst_t  hdr;
    sv_voice_t    voice;
    sv_adsr_t     adsr_tmpl;
    sv_wave_t     wave;
    int8_t        tune;        /* semitone offset */
} mono_wt_t;

static void mono_wt_note_on(synth_inst_t *self, uint8_t note, uint8_t vel)
{
    mono_wt_t *s = (mono_wt_t *)self;
    float hz = sv_note_to_hz((uint8_t)((int)note + s->tune));
    uint32_t step = sv_hz_to_step(hz);
    sv_voice_note_on(&s->voice, note, vel, step, &s->adsr_tmpl);
}

static void mono_wt_note_off(synth_inst_t *self, uint8_t note)
{
    mono_wt_t *s = (mono_wt_t *)self;
    if (s->voice.note == note) sv_voice_note_off(&s->voice);
}

static void mono_wt_render(synth_inst_t *self, int16_t *out_l, int16_t *out_r, int n)
{
    mono_wt_t *s = (mono_wt_t *)self;
    if (!s->voice.active) return;
    const int16_t *wt = (s->wave < SV_WAVE_NOISE) ? sv_wt[s->wave] : NULL;
    for (int i = 0; i < n; i++) {
        int32_t samp = sv_voice_render(&s->voice, wt, s->wave);
        out_l[i] = (int16_t)(samp);
        out_r[i] = (int16_t)(samp);
    }
}

static void mono_wt_set_param(synth_inst_t *self, uint8_t id, float v)
{
    mono_wt_t *s = (mono_wt_t *)self;
    switch (id) {
    case P_WAVEFORM: s->wave = (sv_wave_t)(int)v; break;
    case P_TUNE:     s->tune = (int8_t)(int)v;   break;
    case P_ATK: sv_adsr_set(&s->adsr_tmpl, v,
                    s->adsr_tmpl.dcy * 1000.0f / SAMPLE_RATE,
                    s->adsr_tmpl.sus,
                    s->adsr_tmpl.rel * 1000.0f / SAMPLE_RATE); break;
    case P_DCY: sv_adsr_set(&s->adsr_tmpl,
                    s->adsr_tmpl.atk * 1000.0f / SAMPLE_RATE, v,
                    s->adsr_tmpl.sus,
                    s->adsr_tmpl.rel * 1000.0f / SAMPLE_RATE); break;
    case P_SUS: s->adsr_tmpl.sus = v; break;
    case P_REL: sv_adsr_set(&s->adsr_tmpl,
                    s->adsr_tmpl.atk * 1000.0f / SAMPLE_RATE,
                    s->adsr_tmpl.dcy * 1000.0f / SAMPLE_RATE,
                    s->adsr_tmpl.sus, v); break;
    }
}

static void generic_free(synth_inst_t *self) { heap_caps_free(self); }

synth_inst_t *synth_mono_wt_new(void)
{
    mono_wt_t *s = SYNTH_ALLOC(mono_wt_t);
    if (!s) return NULL;
    s->hdr.type_id    = 0;
    s->hdr.voice_count = 1;
    s->hdr.render     = mono_wt_render;
    s->hdr.note_on    = mono_wt_note_on;
    s->hdr.note_off   = mono_wt_note_off;
    s->hdr.set_param  = mono_wt_set_param;
    s->hdr.free       = generic_free;
    s->wave = SV_WAVE_SQUARE;
    sv_adsr_set(&s->adsr_tmpl, 2.0f, 50.0f, 0.7f, 200.0f);
    return &s->hdr;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Type 1 — Poly Wavetable  (5 voices)
 * ═══════════════════════════════════════════════════════════════════════════ */

#define POLY_VOICES 8

/* Fixed mix headroom for the poly engine.  Voices are summed at a *constant*
 * per-voice level instead of being divided by the live note count, so holding a
 * chord swells naturally instead of ducking every note (and releasing a key no
 * longer makes the survivors jump up in level).  POLY_HEADROOM ≈ how many
 * simultaneous voices reach 0 dBFS together: a 3-note chord lands near full
 * scale, single notes sit ~9 dB below it, and the master-bus soft clipper in
 * audio.c catches the rare moment when many voices align in phase.  Tune by ear:
 * lower = louder single notes / more limiting on big chords, higher = the
 * reverse. */
#define POLY_HEADROOM 3

typedef struct {
    synth_inst_t hdr;
    sv_voice_t   voices[POLY_VOICES];
    sv_adsr_t    adsr_tmpl;
    sv_wave_t    wave;
    float        detune_cents;  /* spread in cents across voices */
    int8_t       tune;
} poly_wt_t;

static void poly_wt_note_on(synth_inst_t *self, uint8_t note, uint8_t vel)
{
    poly_wt_t *s = (poly_wt_t *)self;
    int v = sv_voice_steal(s->voices, POLY_VOICES, note);
    float hz = sv_note_to_hz((uint8_t)((int)note + s->tune));
    /* Small detune spread across voices */
    float d = s->detune_cents / 100.0f / 12.0f;
    float detune = (POLY_VOICES > 1) ? (d * ((float)v - (POLY_VOICES - 1) / 2.0f)) : 0.0f;
    uint32_t step = sv_hz_to_step(hz * powf(2.0f, detune));
    sv_voice_note_on(&s->voices[v], note, vel, step, &s->adsr_tmpl);
}

static void poly_wt_note_off(synth_inst_t *self, uint8_t note)
{
    poly_wt_t *s = (poly_wt_t *)self;
    for (int v = 0; v < POLY_VOICES; v++)
        if (s->voices[v].active && s->voices[v].note == note)
            sv_voice_note_off(&s->voices[v]);
}

static void poly_wt_render(synth_inst_t *self, int16_t *out_l, int16_t *out_r, int n)
{
    poly_wt_t *s = (poly_wt_t *)self;
    const int16_t *wt = (s->wave < SV_WAVE_NOISE) ? sv_wt[s->wave] : NULL;
    int active = 0;
    for (int v = 0; v < POLY_VOICES; v++)
        if (s->voices[v].active) active++;
    if (active == 0) return;
    for (int i = 0; i < n; i++) {
        int32_t mix = 0;
        for (int v = 0; v < POLY_VOICES; v++)
            if (s->voices[v].active)
                mix += sv_voice_render(&s->voices[v], wt, s->wave);
        mix /= POLY_HEADROOM;        /* fixed headroom — no per-note ducking  */
        mix = sv_soft_clip(mix);     /* tame in-phase chord peaks gently       */
        out_l[i] = (int16_t)(mix);
        out_r[i] = (int16_t)(mix);
    }
}

static void poly_wt_set_param(synth_inst_t *self, uint8_t id, float v)
{
    poly_wt_t *s = (poly_wt_t *)self;
    switch (id) {
    case P_WAVEFORM: s->wave = (sv_wave_t)(int)v; break;
    case P_TUNE:     s->tune = (int8_t)(int)v; break;
    case P_DETUNE:   s->detune_cents = v; break;
    case P_ATK: s->adsr_tmpl.atk = (uint32_t)(v * SAMPLE_RATE / 1000.0f); break;
    case P_DCY: s->adsr_tmpl.dcy = (uint32_t)(v * SAMPLE_RATE / 1000.0f); break;
    case P_SUS: s->adsr_tmpl.sus = v; break;
    case P_REL: s->adsr_tmpl.rel = (uint32_t)(v * SAMPLE_RATE / 1000.0f); break;
    }
}

synth_inst_t *synth_poly_wt_new(void)
{
    poly_wt_t *s = SYNTH_ALLOC(poly_wt_t);
    if (!s) return NULL;
    s->hdr.type_id     = 1;
    s->hdr.voice_count = POLY_VOICES;
    s->hdr.render      = poly_wt_render;
    s->hdr.note_on     = poly_wt_note_on;
    s->hdr.note_off    = poly_wt_note_off;
    s->hdr.set_param   = poly_wt_set_param;
    s->hdr.free        = generic_free;
    s->wave = SV_WAVE_SAW;
    s->detune_cents = 10.0f;
    sv_adsr_set(&s->adsr_tmpl, 5.0f, 100.0f, 0.7f, 300.0f);
    return &s->hdr;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Type 2 — Super Saw  (7 detuned saws, stereo spread)
 * ═══════════════════════════════════════════════════════════════════════════ */

#define SSAW_OSCS  7

typedef struct {
    synth_inst_t hdr;
    uint32_t     phase[SSAW_OSCS];
    uint32_t     step[SSAW_OSCS];   /* steps pre-detuned at note-on */
    sv_adsr_t    adsr;
    sv_adsr_t    adsr_tmpl;
    bool         active;
    float        detune;   /* spread: 0.0–0.5 semitones total */
    float        width;    /* stereo width 0.0–1.0 */
    uint8_t      note;
} ssaw_t;

static void ssaw_note_on(synth_inst_t *self, uint8_t note, uint8_t vel)
{
    ssaw_t *s = (ssaw_t *)self;
    s->note = note;
    float base_hz = sv_note_to_hz(note);
    for (int i = 0; i < SSAW_OSCS; i++) {
        /* Spread: osc i gets detune offset centred around 0 */
        float offset = (SSAW_OSCS > 1)
            ? s->detune * ((float)i / (SSAW_OSCS - 1) - 0.5f)
            : 0.0f;
        float hz = base_hz * powf(2.0f, offset / 12.0f);
        s->step[i]  = sv_hz_to_step(hz);
        s->phase[i] = (uint32_t)i * (0xFFFFFFFF / SSAW_OSCS); /* stagger phases */
    }
    s->adsr = s->adsr_tmpl;
    sv_adsr_gate_on(&s->adsr);
    s->active = true;
    (void)vel;
}

static void ssaw_note_off(synth_inst_t *self, uint8_t note)
{
    ssaw_t *s = (ssaw_t *)self;
    if (s->note == note) sv_adsr_gate_off(&s->adsr);
}

static void ssaw_render(synth_inst_t *self, int16_t *out_l, int16_t *out_r, int n)
{
    ssaw_t *s = (ssaw_t *)self;
    if (!s->active) return;
    for (int i = 0; i < n; i++) {
        float env = sv_adsr_tick(&s->adsr);
        if (s->adsr.stage == SV_ST_IDLE) { s->active = false; break; }
        int32_t suml = 0, sumr = 0;
        for (int o = 0; o < SSAW_OSCS; o++) {
            int16_t samp = sv_wt[SV_WAVE_SAW][(s->phase[o] >> 24) & 0xFF];
            s->phase[o] += s->step[o];
            /* Spread: odd/even oscs go to different channels */
            float pan = (o & 1) ? s->width : -s->width;
            int32_t l_frac = (int32_t)((1.0f - (pan > 0 ? pan : 0)) * 256);
            int32_t r_frac = (int32_t)((1.0f + (pan < 0 ? pan : 0)) * 256);
            suml += ((int32_t)samp * l_frac) >> 8;
            sumr += ((int32_t)samp * r_frac) >> 8;
        }
        int32_t sl = (int32_t)(suml / SSAW_OSCS * env);
        int32_t sr = (int32_t)(sumr / SSAW_OSCS * env);
        out_l[i] = (int16_t)(sl);
        out_r[i] = (int16_t)(sr);
    }
}

static void ssaw_set_param(synth_inst_t *self, uint8_t id, float v)
{
    ssaw_t *s = (ssaw_t *)self;
    switch (id) {
    case P_DETUNE: s->detune = v; break;
    case P_WIDTH:  s->width  = v < 0 ? 0 : (v > 1 ? 1 : v); break;
    case P_ATK: s->adsr_tmpl.atk = (uint32_t)(v * SAMPLE_RATE / 1000.0f); break;
    case P_DCY: s->adsr_tmpl.dcy = (uint32_t)(v * SAMPLE_RATE / 1000.0f); break;
    case P_SUS: s->adsr_tmpl.sus = v; break;
    case P_REL: s->adsr_tmpl.rel = (uint32_t)(v * SAMPLE_RATE / 1000.0f); break;
    }
}

synth_inst_t *synth_supersaw_new(void)
{
    ssaw_t *s = SYNTH_ALLOC(ssaw_t);
    if (!s) return NULL;
    s->hdr.type_id    = 2;
    s->hdr.voice_count = 1;
    s->hdr.render     = ssaw_render;
    s->hdr.note_on    = ssaw_note_on;
    s->hdr.note_off   = ssaw_note_off;
    s->hdr.set_param  = ssaw_set_param;
    s->hdr.free       = generic_free;
    s->detune = 0.3f;
    s->width  = 0.6f;
    sv_adsr_set(&s->adsr_tmpl, 10.0f, 200.0f, 0.8f, 500.0f);
    return &s->hdr;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Type 3 — FM 2-op  (carrier + modulator)
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    synth_inst_t hdr;
    /* Carrier */
    uint32_t c_phase, c_step;
    sv_adsr_t c_adsr, c_adsr_t;
    /* Modulator */
    uint32_t m_phase, m_step;
    sv_adsr_t m_adsr, m_adsr_t;
    float ratio;   /* modulator freq = carrier freq × ratio */
    float depth;   /* FM index 0–8 */
    bool  active;
    uint8_t note;
} fm2_t;

static void fm2_note_on(synth_inst_t *self, uint8_t note, uint8_t vel)
{
    fm2_t *s = (fm2_t *)self;
    s->note = note;
    float hz = sv_note_to_hz(note);
    s->c_step = sv_hz_to_step(hz);
    s->m_step = sv_hz_to_step(hz * s->ratio);
    s->c_phase = s->m_phase = 0;
    s->c_adsr = s->c_adsr_t; sv_adsr_gate_on(&s->c_adsr);
    s->m_adsr = s->m_adsr_t; sv_adsr_gate_on(&s->m_adsr);
    s->active = true;
    (void)vel;
}

static void fm2_note_off(synth_inst_t *self, uint8_t note)
{
    fm2_t *s = (fm2_t *)self;
    if (s->note == note) {
        sv_adsr_gate_off(&s->c_adsr);
        sv_adsr_gate_off(&s->m_adsr);
    }
}

static void fm2_render(synth_inst_t *self, int16_t *out_l, int16_t *out_r, int n)
{
    fm2_t *s = (fm2_t *)self;
    if (!s->active) return;
    float depth = s->depth;
    for (int i = 0; i < n; i++) {
        float c_env = sv_adsr_tick(&s->c_adsr);
        float m_env = sv_adsr_tick(&s->m_adsr);
        if (s->c_adsr.stage == SV_ST_IDLE) { s->active = false; break; }

        /* Modulator output: sine at m_phase */
        float m_out = sv_wt[SV_WAVE_SINE][(s->m_phase >> 24) & 0xFF]
                      * m_env / (float)SV_WT_AMP;
        s->m_phase += s->m_step;

        /* Modulate carrier phase */
        uint32_t mod_offset = (uint32_t)(m_out * depth * (1u << 24));
        uint8_t c_idx = (uint8_t)((s->c_phase + mod_offset) >> 24);
        int32_t samp = (int32_t)(sv_wt[SV_WAVE_SINE][c_idx] * c_env);
        s->c_phase += s->c_step;

        out_l[i] = (int16_t)(samp);
        out_r[i] = (int16_t)(samp);
    }
}

static void fm2_set_param(synth_inst_t *self, uint8_t id, float v)
{
    fm2_t *s = (fm2_t *)self;
    switch (id) {
    case P_FM_RATIO: s->ratio = v > 0.01f ? v : 0.01f; break;
    case P_FM_DEPTH: s->depth = v; break;
    case P_ATK: s->c_adsr_t.atk = (uint32_t)(v * SAMPLE_RATE / 1000.0f); break;
    case P_DCY: s->c_adsr_t.dcy = (uint32_t)(v * SAMPLE_RATE / 1000.0f); break;
    case P_SUS: s->c_adsr_t.sus = v; break;
    case P_REL: s->c_adsr_t.rel = (uint32_t)(v * SAMPLE_RATE / 1000.0f); break;
    }
}

synth_inst_t *synth_fm2_new(void)
{
    fm2_t *s = SYNTH_ALLOC(fm2_t);
    if (!s) return NULL;
    s->hdr.type_id     = 3;
    s->hdr.voice_count = 1;
    s->hdr.render      = fm2_render;
    s->hdr.note_on     = fm2_note_on;
    s->hdr.note_off    = fm2_note_off;
    s->hdr.set_param   = fm2_set_param;
    s->hdr.free        = generic_free;
    s->ratio = 2.0f; s->depth = 2.0f;
    sv_adsr_set(&s->c_adsr_t, 2.0f, 100.0f, 0.7f, 200.0f);
    sv_adsr_set(&s->m_adsr_t, 1.0f,  80.0f, 0.5f, 150.0f);
    return &s->hdr;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Type 4 — FM 4-op  (DX7-style, 8 algorithm presets)
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * Operators: op[0]=A, op[1]=B, op[2]=C, op[3]=D
 * 8 algorithms defined as a routing table:
 *   alg_mod[algo][op] = bitmask of operators that modulate op
 *   alg_out[algo]     = bitmask of operators that contribute to output
 */

#define FM4_OPS 4

/* fm4_mod reserved for future detailed modulation routing */
/* Simplified: just define which op modulates which carrier for 8 presets    */
/* algo_carrier[a] = which ops feed output (bit per op)                       */
static const uint8_t algo_carrier[8] = {
    0x08, /* 0: A→B→C→D, out=D          */
    0x0C, /* 1: A→B, C→D, out=B+D       */
    0x0E, /* 2: A→B, A→C, A→D, out=B+C+D */
    0x0F, /* 3: all independent, out=all */
    0x04, /* 4: A→B→C, D free, out=C+D  */
    0x06, /* 5: A→B, C free, out=B+C+D  */
    0x0E, /* 6: A→B→C, out=C (D fb)     */
    0x08, /* 7: (A+B)→C→D, out=D        */
};
/* mod source for each op per algorithm (simplified; 0=no modulator) */
static const uint8_t algo_mod_src[8][FM4_OPS] = {
    {0xFF, 0, 1, 2},   /* 0 */
    {0xFF, 0, 0xFF, 2}, /* 1 */
    {0xFF, 0, 0, 0},   /* 2 */
    {0xFF, 0xFF, 0xFF, 0xFF}, /* 3 */
    {0xFF, 0, 1, 0xFF}, /* 4 */
    {0xFF, 0, 0xFF, 0xFF}, /* 5 */
    {0xFF, 0, 1, 0xFF}, /* 6 */
    {0xFF, 0xFF, 0, 1}, /* 7 */
};

typedef struct {
    synth_inst_t hdr;
    uint32_t  phase[FM4_OPS];
    uint32_t  step[FM4_OPS];
    sv_adsr_t adsr[FM4_OPS];
    sv_adsr_t adsr_t[FM4_OPS];
    float     level[FM4_OPS]; /* op output level 0–1 */
    float     depth;          /* global FM index */
    uint8_t   algo;
    uint8_t   ratio[FM4_OPS]; /* integer ratio for each op */
    bool      active;
    uint8_t   note;
} fm4_t;

static void fm4_note_on(synth_inst_t *self, uint8_t note, uint8_t vel)
{
    fm4_t *s = (fm4_t *)self;
    s->note = note;
    float base = sv_note_to_hz(note);
    for (int i = 0; i < FM4_OPS; i++) {
        s->step[i]  = sv_hz_to_step(base * s->ratio[i]);
        s->phase[i] = 0;
        s->adsr[i]  = s->adsr_t[i];
        sv_adsr_gate_on(&s->adsr[i]);
    }
    s->active = true;
    (void)vel;
}

static void fm4_note_off(synth_inst_t *self, uint8_t note)
{
    fm4_t *s = (fm4_t *)self;
    if (s->note != note) return;
    for (int i = 0; i < FM4_OPS; i++) sv_adsr_gate_off(&s->adsr[i]);
}

static void fm4_render(synth_inst_t *self, int16_t *out_l, int16_t *out_r, int n)
{
    fm4_t *s = (fm4_t *)self;
    if (!s->active) return;
    uint8_t a = s->algo & 7;
    uint8_t car_mask = algo_carrier[a];

    for (int i = 0; i < n; i++) {
        float env[FM4_OPS];
        bool any = false;
        for (int o = 0; o < FM4_OPS; o++) {
            env[o] = sv_adsr_tick(&s->adsr[o]);
            if (s->adsr[o].stage != SV_ST_IDLE) any = true;
        }
        if (!any) { s->active = false; break; }

        float op_out[FM4_OPS];
        for (int o = 0; o < FM4_OPS; o++) {
            uint8_t mod_src = algo_mod_src[a][o];
            float mod = (mod_src < FM4_OPS) ? op_out[mod_src] * s->depth : 0.0f;
            uint32_t mod_off = (uint32_t)(mod * (1u << 24));
            uint8_t idx = (uint8_t)((s->phase[o] + mod_off) >> 24);
            op_out[o] = sv_wt[SV_WAVE_SINE][idx] * env[o] * s->level[o] / (float)SV_WT_AMP;
            s->phase[o] += s->step[o];
        }

        int32_t mix = 0;
        for (int o = 0; o < FM4_OPS; o++)
            if (car_mask & (1 << o))
                mix += (int32_t)(op_out[o] * SV_WT_AMP);
        mix /= __builtin_popcount(car_mask);

        out_l[i] = (int16_t)(mix);
        out_r[i] = (int16_t)(mix);
    }
}

static void fm4_set_param(synth_inst_t *self, uint8_t id, float v)
{
    fm4_t *s = (fm4_t *)self;
    if (id == P_ALGO)       { s->algo = (uint8_t)(int)v & 7; return; }
    if (id == P_FM_DEPTH)   { s->depth = v; return; }
    /* P_ATK..P_REL apply to all operators */
    for (int o = 0; o < FM4_OPS; o++) {
        switch (id) {
        case P_ATK: s->adsr_t[o].atk = (uint32_t)(v * SAMPLE_RATE / 1000.0f); break;
        case P_DCY: s->adsr_t[o].dcy = (uint32_t)(v * SAMPLE_RATE / 1000.0f); break;
        case P_SUS: s->adsr_t[o].sus = v; break;
        case P_REL: s->adsr_t[o].rel = (uint32_t)(v * SAMPLE_RATE / 1000.0f); break;
        }
    }
}

synth_inst_t *synth_fm4_new(void)
{
    fm4_t *s = SYNTH_ALLOC(fm4_t);
    if (!s) return NULL;
    s->hdr.type_id     = 4;
    s->hdr.voice_count = 1;
    s->hdr.render      = fm4_render;
    s->hdr.note_on     = fm4_note_on;
    s->hdr.note_off    = fm4_note_off;
    s->hdr.set_param   = fm4_set_param;
    s->hdr.free        = generic_free;
    s->algo  = 0;
    s->depth = 2.0f;
    static const uint8_t def_ratio[FM4_OPS] = {1, 2, 3, 4};
    for (int o = 0; o < FM4_OPS; o++) {
        s->ratio[o]   = def_ratio[o];
        s->level[o]   = 1.0f;
        sv_adsr_set(&s->adsr_t[o], 2.0f, 80.0f, 0.6f, 200.0f);
    }
    return &s->hdr;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Type 5 — Subtractive  (osc → SVF → ADSR)
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    synth_inst_t hdr;
    sv_voice_t   voice;
    sv_adsr_t    adsr_tmpl;
    sv_wave_t    wave;
    sv_svf_t     filt;
    float        filt_freq;
    float        filt_res;
    float        drive;        /* pre-filter gain */
    int8_t       tune;
} sub_t;

static void sub_note_on(synth_inst_t *self, uint8_t note, uint8_t vel)
{
    sub_t *s = (sub_t *)self;
    float hz = sv_note_to_hz((uint8_t)((int)note + s->tune));
    sv_voice_note_on(&s->voice, note, vel, sv_hz_to_step(hz), &s->adsr_tmpl);
}

static void sub_note_off(synth_inst_t *self, uint8_t note)
{
    sub_t *s = (sub_t *)self;
    if (s->voice.note == note) sv_voice_note_off(&s->voice);
}

static void sub_render(synth_inst_t *self, int16_t *out_l, int16_t *out_r, int n)
{
    sub_t *s = (sub_t *)self;
    if (!s->voice.active) return;
    const int16_t *wt = (s->wave < SV_WAVE_NOISE) ? sv_wt[s->wave] : NULL;
    for (int i = 0; i < n; i++) {
        int32_t raw = sv_voice_render(&s->voice, wt, s->wave);
        /* Drive */
        float f = (float)raw * s->drive;
        /* Filter */
        float filtered = sv_svf_tick(&s->filt, f);
        int32_t samp = (int32_t)filtered;
        if (samp >  32767) samp =  32767;
        if (samp < -32768) samp = -32768;
        out_l[i] = (int16_t)(samp);
        out_r[i] = (int16_t)(samp);
    }
}

static void sub_set_param(synth_inst_t *self, uint8_t id, float v)
{
    sub_t *s = (sub_t *)self;
    switch (id) {
    case P_WAVEFORM:    s->wave = (sv_wave_t)(int)v; break;
    case P_TUNE:        s->tune = (int8_t)(int)v; break;
    case P_FILTER_FREQ: s->filt_freq = v; sv_svf_set(&s->filt, v, s->filt_res); break;
    case P_FILTER_RES:  s->filt_res  = v; sv_svf_set(&s->filt, s->filt_freq, v); break;
    case P_DRIVE:       s->drive = v < 0.01f ? 0.01f : v; break;
    case P_ATK: s->adsr_tmpl.atk = (uint32_t)(v * SAMPLE_RATE / 1000.0f); break;
    case P_DCY: s->adsr_tmpl.dcy = (uint32_t)(v * SAMPLE_RATE / 1000.0f); break;
    case P_SUS: s->adsr_tmpl.sus = v; break;
    case P_REL: s->adsr_tmpl.rel = (uint32_t)(v * SAMPLE_RATE / 1000.0f); break;
    }
}

synth_inst_t *synth_subtractive_new(void)
{
    sub_t *s = SYNTH_ALLOC(sub_t);
    if (!s) return NULL;
    s->hdr.type_id     = 5;
    s->hdr.voice_count = 1;
    s->hdr.render      = sub_render;
    s->hdr.note_on     = sub_note_on;
    s->hdr.note_off    = sub_note_off;
    s->hdr.set_param   = sub_set_param;
    s->hdr.free        = generic_free;
    s->wave       = SV_WAVE_SAW;
    s->filt_freq  = 2000.0f;
    s->filt_res   = 2.0f;
    s->drive      = 1.0f;
    sv_svf_set(&s->filt, 2000.0f, 2.0f);
    sv_adsr_set(&s->adsr_tmpl, 5.0f, 150.0f, 0.6f, 300.0f);
    return &s->hdr;
}
