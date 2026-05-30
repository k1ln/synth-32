/*
 * synth_extra.c — Synth types 12–19 (Phase 3)
 *
 * 12  Chord Synth     stacked intervals, optional auto-arp
 * 13  Drum Synth BD   sine + pitch env + click
 * 14  Drum Synth SD   tone + noise + bandpass
 * 15  Drum Synth HH   6 square osc + HP filter
 * 16  Organ           9 drawbar × sine (Hammond style)
 * 17  Wavetable Morph 4-point WT interpolation
 * 18  Vowel Synth     3 formant bandpass filters (A-E-I-O-U morph)
 * 19  Bit-Crush Synth wavetable + bit-depth + SR reduction
 */

#include <string.h>
#include <math.h>
#include "esp_heap_caps.h"
#include "synth.h"
#include "synth_voice.h"

#define SYNTH_ALLOC(T) ((T *)heap_caps_calloc(1, sizeof(T), MALLOC_CAP_SPIRAM))
static void generic_free(synth_inst_t *self) { heap_caps_free(self); }
static void noop_set_param(synth_inst_t *self, uint8_t id, float v) { (void)self; (void)id; (void)v; }

/* ── Param IDs ───────────────────────────────────────────────────────────── */
#define P_ATK 20
#define P_DCY 21
#define P_SUS 22
#define P_REL 23

/* ═══════════════════════════════════════════════════════════════════════════
 * Type 12 — Chord Synth  (up to 5 stacked intervals)
 * ═══════════════════════════════════════════════════════════════════════════ */

#define CHORD_MAX_NOTES 5

static const int8_t chord_intervals[][CHORD_MAX_NOTES] = {
    {0, 4, 7,  0,  0},   /* major triad     */
    {0, 3, 7,  0,  0},   /* minor triad     */
    {0, 4, 7, 11,  0},   /* maj7            */
    {0, 3, 7, 10,  0},   /* min7            */
    {0, 4, 7, 10,  0},   /* dom7            */
    {0, 5, 7,  0,  0},   /* sus4            */
    {0, 2, 7,  0,  0},   /* sus2            */
    {0, 3, 6,  0,  0},   /* diminished      */
};
static const int chord_sizes[] = {3, 3, 4, 4, 4, 3, 3, 3};

typedef struct {
    synth_inst_t hdr;
    uint32_t  phase[CHORD_MAX_NOTES];
    uint32_t  step[CHORD_MAX_NOTES];
    sv_adsr_t adsr[CHORD_MAX_NOTES];
    sv_adsr_t adsr_t;
    uint8_t   chord_type;    /* 0–7 */
    uint8_t   active_count;
    bool      active;
} chord_t;

static void chord_note_on(synth_inst_t *self, uint8_t note, uint8_t vel)
{
    chord_t *s = (chord_t *)self;
    int sz = chord_sizes[s->chord_type & 7];
    s->active_count = (uint8_t)sz;
    for (int i = 0; i < sz; i++) {
        int8_t semi = chord_intervals[s->chord_type & 7][i];
        int n = (int)note + semi;
        if (n < 0) n = 0;
        if (n > 127) n = 127;
        s->step[i]  = sv_hz_to_step(sv_note_to_hz((uint8_t)n));
        s->phase[i] = 0;
        s->adsr[i]  = s->adsr_t;
        sv_adsr_gate_on(&s->adsr[i]);
    }
    s->active = true;
    (void)vel;
}

static void chord_note_off(synth_inst_t *self, uint8_t note)
{
    chord_t *s = (chord_t *)self;
    for (int i = 0; i < s->active_count; i++) sv_adsr_gate_off(&s->adsr[i]);
    (void)note;
}

static void chord_render(synth_inst_t *self, int16_t *out_l, int16_t *out_r, int n)
{
    chord_t *s = (chord_t *)self;
    if (!s->active) return;
    for (int i = 0; i < n; i++) {
        int32_t mix = 0; bool any = false;
        for (int v = 0; v < s->active_count; v++) {
            float env = sv_adsr_tick(&s->adsr[v]);
            if (s->adsr[v].stage != SV_ST_IDLE) any = true;
            int16_t samp = sv_wt[SV_WAVE_SAW][(s->phase[v] >> 24) & 0xFF];
            mix += (int32_t)(samp * env);
            s->phase[v] += s->step[v];
        }
        if (!any) { s->active = false; break; }
        mix /= s->active_count;
        out_l[i] = (int16_t)(mix);
        out_r[i] = (int16_t)(mix);
    }
}

static void chord_set_param(synth_inst_t *self, uint8_t id, float v)
{
    chord_t *s = (chord_t *)self;
    switch (id) {
    case 0: s->chord_type = (uint8_t)(int)v & 7; break;
    case P_ATK: s->adsr_t.atk = (uint32_t)(v * SAMPLE_RATE / 1000.0f); break;
    case P_DCY: s->adsr_t.dcy = (uint32_t)(v * SAMPLE_RATE / 1000.0f); break;
    case P_SUS: s->adsr_t.sus = v; break;
    case P_REL: s->adsr_t.rel = (uint32_t)(v * SAMPLE_RATE / 1000.0f); break;
    }
}

synth_inst_t *synth_chord_new(void)
{
    chord_t *s = SYNTH_ALLOC(chord_t);
    if (!s) return NULL;
    s->hdr.type_id    = 12; s->hdr.voice_count = CHORD_MAX_NOTES;
    s->hdr.render     = chord_render; s->hdr.note_on    = chord_note_on;
    s->hdr.note_off   = chord_note_off; s->hdr.set_param = chord_set_param;
    s->hdr.free       = generic_free;
    s->chord_type = 0;
    sv_adsr_set(&s->adsr_t, 5.0f, 100.0f, 0.7f, 300.0f);
    return &s->hdr;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Type 13 — Drum Synth BD  (sine pitch-env + click)
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    synth_inst_t hdr;
    uint32_t phase;
    float    cur_hz;
    float    target_hz;
    float    pitch_decay;   /* per-sample Hz decay */
    float    click_level;
    sv_adsr_t adsr; sv_adsr_t adsr_t;
    bool     active;
    uint16_t lfsr;   /* for click noise */
} drum_bd_t;

static void drum_bd_note_on(synth_inst_t *self, uint8_t note, uint8_t vel)
{
    drum_bd_t *s = (drum_bd_t *)self;
    s->cur_hz    = sv_note_to_hz(note) * 2.0f;   /* start 1 octave up */
    s->target_hz = sv_note_to_hz(note);
    s->adsr = s->adsr_t; sv_adsr_gate_on(&s->adsr);
    s->phase = 0; s->active = true;
    (void)vel;
}

static void drum_bd_note_off(synth_inst_t *self, uint8_t note)
{ drum_bd_t *s = (drum_bd_t *)self; sv_adsr_gate_off(&s->adsr); (void)note; }

static void drum_bd_render(synth_inst_t *self, int16_t *out_l, int16_t *out_r, int n)
{
    drum_bd_t *s = (drum_bd_t *)self;
    if (!s->active) return;
    for (int i = 0; i < n; i++) {
        float env = sv_adsr_tick(&s->adsr);
        if (s->adsr.stage == SV_ST_IDLE) { s->active = false; break; }
        /* Pitch envelope */
        if (s->cur_hz > s->target_hz) {
            s->cur_hz -= s->pitch_decay;
            if (s->cur_hz < s->target_hz) s->cur_hz = s->target_hz;
        }
        uint32_t step = sv_hz_to_step(s->cur_hz);
        int32_t body = sv_wt[SV_WAVE_SINE][(s->phase >> 24) & 0xFF];
        s->phase += step;
        /* Click: white noise at attack */
        s->lfsr = (uint16_t)(s->lfsr >> 1) ^ ((s->lfsr & 1) ? 0xB400 : 0);
        int32_t click = (int16_t)s->lfsr;
        int32_t samp = (int32_t)((body + click * s->click_level) * env);
        if (samp >  32767) samp =  32767;
        if (samp < -32768) samp = -32768;
        out_l[i] = (int16_t)(samp);
        out_r[i] = (int16_t)(samp);
    }
}

static void drum_bd_set_param(synth_inst_t *self, uint8_t id, float v)
{
    drum_bd_t *s = (drum_bd_t *)self;
    switch (id) {
    case 0: s->pitch_decay  = v; break;
    case 1: s->click_level  = v; break;
    case P_ATK: s->adsr_t.atk = (uint32_t)(v * SAMPLE_RATE / 1000.0f); break;
    case P_REL: s->adsr_t.rel = (uint32_t)(v * SAMPLE_RATE / 1000.0f); break;
    }
}

synth_inst_t *synth_drum_bd_new(void)
{
    drum_bd_t *s = SYNTH_ALLOC(drum_bd_t);
    if (!s) return NULL;
    s->hdr.type_id = 13; s->hdr.voice_count = 1;
    s->hdr.render = drum_bd_render; s->hdr.note_on = drum_bd_note_on;
    s->hdr.note_off = drum_bd_note_off; s->hdr.set_param = drum_bd_set_param;
    s->hdr.free = generic_free;
    s->pitch_decay = 4.0f; s->click_level = 0.15f; s->lfsr = 0xACE1;
    sv_adsr_set(&s->adsr_t, 1.0f, 0.0f, 0.0f, 400.0f);
    return &s->hdr;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Type 14 — Drum Synth SD  (tone + noise + bandpass filter)
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    synth_inst_t hdr;
    uint32_t  tone_phase, tone_step;
    uint16_t  lfsr;
    sv_svf_t  bp;           /* bandpass on noise */
    float     tone_mix;     /* 0=all noise, 1=all tone */
    sv_adsr_t adsr; sv_adsr_t adsr_t;
    bool      active;
} drum_sd_t;

static void drum_sd_note_on(synth_inst_t *self, uint8_t note, uint8_t vel)
{
    drum_sd_t *s = (drum_sd_t *)self;
    s->tone_step  = sv_hz_to_step(sv_note_to_hz(note));
    s->tone_phase = 0;
    s->adsr = s->adsr_t; sv_adsr_gate_on(&s->adsr);
    s->active = true; (void)vel;
}

static void drum_sd_note_off(synth_inst_t *self, uint8_t note)
{ drum_sd_t *s = (drum_sd_t *)self; sv_adsr_gate_off(&s->adsr); (void)note; }

static void drum_sd_render(synth_inst_t *self, int16_t *out_l, int16_t *out_r, int n)
{
    drum_sd_t *s = (drum_sd_t *)self;
    if (!s->active) return;
    for (int i = 0; i < n; i++) {
        float env = sv_adsr_tick(&s->adsr);
        if (s->adsr.stage == SV_ST_IDLE) { s->active = false; break; }
        /* Tone */
        float tone = sv_wt[SV_WAVE_TRIANGLE][(s->tone_phase >> 24) & 0xFF];
        s->tone_phase += s->tone_step;
        /* Noise through BP */
        s->lfsr = (uint16_t)(s->lfsr >> 1) ^ ((s->lfsr & 1) ? 0xB400 : 0);
        float noise = sv_svf_tick(&s->bp, (float)(int16_t)s->lfsr);
        /* Mix */
        float mix = tone * s->tone_mix + noise * (1.0f - s->tone_mix);
        int32_t samp = (int32_t)(mix * env);
        if (samp >  32767) samp =  32767;
        if (samp < -32768) samp = -32768;
        out_l[i] = (int16_t)(samp);
        out_r[i] = (int16_t)(samp);
    }
}

static void drum_sd_set_param(synth_inst_t *self, uint8_t id, float v)
{
    drum_sd_t *s = (drum_sd_t *)self;
    switch (id) {
    case 0: s->tone_mix = v; break;
    case 1: sv_svf_set(&s->bp, v, 1.5f); break;  /* BP freq */
    case P_ATK: s->adsr_t.atk = (uint32_t)(v * SAMPLE_RATE / 1000.0f); break;
    case P_REL: s->adsr_t.rel = (uint32_t)(v * SAMPLE_RATE / 1000.0f); break;
    }
}

synth_inst_t *synth_drum_sd_new(void)
{
    drum_sd_t *s = SYNTH_ALLOC(drum_sd_t);
    if (!s) return NULL;
    s->hdr.type_id = 14; s->hdr.voice_count = 1;
    s->hdr.render = drum_sd_render; s->hdr.note_on = drum_sd_note_on;
    s->hdr.note_off = drum_sd_note_off; s->hdr.set_param = drum_sd_set_param;
    s->hdr.free = generic_free;
    s->lfsr = 0xACE1; s->tone_mix = 0.4f;
    sv_svf_set(&s->bp, 2500.0f, 1.5f);
    sv_adsr_set(&s->adsr_t, 1.0f, 0.0f, 0.0f, 200.0f);
    return &s->hdr;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Type 15 — Drum Synth HH  (6 square osc + HP filter)
 * ═══════════════════════════════════════════════════════════════════════════ */

#define HH_OSCS 6
static const float hh_ratios[HH_OSCS] = {1.0f, 1.483f, 1.932f, 2.415f, 2.94f, 3.67f};

typedef struct {
    synth_inst_t hdr;
    uint32_t  phase[HH_OSCS], step[HH_OSCS];
    sv_svf_t  hp;
    sv_adsr_t adsr; sv_adsr_t adsr_t;
    float     freq_spread;   /* scale on ratios */
    bool      active;
} drum_hh_t;

static void drum_hh_note_on(synth_inst_t *self, uint8_t note, uint8_t vel)
{
    drum_hh_t *s = (drum_hh_t *)self;
    float base = sv_note_to_hz(note);
    for (int i = 0; i < HH_OSCS; i++) {
        s->step[i]  = sv_hz_to_step(base * hh_ratios[i] * s->freq_spread);
        s->phase[i] = (uint32_t)i * (0xFFFFFFFF / HH_OSCS);
    }
    s->adsr = s->adsr_t; sv_adsr_gate_on(&s->adsr);
    s->active = true; (void)vel;
}

static void drum_hh_note_off(synth_inst_t *self, uint8_t note)
{ drum_hh_t *s = (drum_hh_t *)self; sv_adsr_gate_off(&s->adsr); (void)note; }

static void drum_hh_render(synth_inst_t *self, int16_t *out_l, int16_t *out_r, int n)
{
    drum_hh_t *s = (drum_hh_t *)self;
    if (!s->active) return;
    for (int i = 0; i < n; i++) {
        float env = sv_adsr_tick(&s->adsr);
        if (s->adsr.stage == SV_ST_IDLE) { s->active = false; break; }
        int32_t mix = 0;
        for (int o = 0; o < HH_OSCS; o++) {
            mix += (s->phase[o] >> 31) ? SV_WT_AMP : -SV_WT_AMP;  /* square via MSB */
            s->phase[o] += s->step[o];
        }
        mix /= HH_OSCS;
        /* HP filter */
        sv_svf_tick(&s->hp, (float)mix);
        float hp_out = s->hp.hp;
        int32_t samp = (int32_t)(hp_out * env);
        if (samp >  32767) samp =  32767;
        if (samp < -32768) samp = -32768;
        out_l[i] = (int16_t)(samp);
        out_r[i] = (int16_t)(samp);
    }
}

synth_inst_t *synth_drum_hh_new(void)
{
    drum_hh_t *s = SYNTH_ALLOC(drum_hh_t);
    if (!s) return NULL;
    s->hdr.type_id = 15; s->hdr.voice_count = 1;
    s->hdr.render = drum_hh_render; s->hdr.note_on = drum_hh_note_on;
    s->hdr.note_off = drum_hh_note_off; s->hdr.set_param = noop_set_param;
    s->hdr.free = generic_free;
    s->freq_spread = 1.0f;
    sv_svf_set(&s->hp, 7000.0f, 0.7f);
    sv_adsr_set(&s->adsr_t, 1.0f, 0.0f, 0.0f, 80.0f);
    return &s->hdr;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Type 16 — Organ  (9 drawbars × sine, Hammond-style + Leslie rotary)
 * ═══════════════════════════════════════════════════════════════════════════ */

#define ORGAN_BARS 9
/* Hammond drawbar footage ratios relative to base note */
static const float organ_ratios[ORGAN_BARS] =
    {0.5f, 1.0f, 1.5f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 8.0f};

typedef struct {
    synth_inst_t hdr;
    uint32_t  phase[ORGAN_BARS];
    uint32_t  step[ORGAN_BARS];
    float     draw[ORGAN_BARS];   /* 0.0–1.0 per drawbar */
    /* Leslie rotary: amplitude + frequency tremolo */
    uint32_t  leslie_phase;
    uint32_t  leslie_step;
    float     leslie_depth;
    bool      active;
    uint8_t   note;
} organ_t;

static void organ_note_on(synth_inst_t *self, uint8_t note, uint8_t vel)
{
    organ_t *s = (organ_t *)self;
    s->note = note;
    float base = sv_note_to_hz(note);
    for (int i = 0; i < ORGAN_BARS; i++) {
        s->step[i]  = sv_hz_to_step(base * organ_ratios[i]);
        s->phase[i] = 0;
    }
    s->active = true; (void)vel;
}

static void organ_note_off(synth_inst_t *self, uint8_t note)
{ organ_t *s = (organ_t *)self; if (s->note == note) s->active = false; }

static void organ_render(synth_inst_t *self, int16_t *out_l, int16_t *out_r, int n)
{
    organ_t *s = (organ_t *)self;
    if (!s->active) return;
    for (int i = 0; i < n; i++) {
        /* Leslie tremolo */
        float leslie_lfo = sv_wt[SV_WAVE_SINE][(s->leslie_phase >> 24) & 0xFF]
                           * s->leslie_depth / (float)SV_WT_AMP;
        s->leslie_phase += s->leslie_step;

        int32_t mix = 0; float total = 0.0f;
        for (int b = 0; b < ORGAN_BARS; b++) {
            if (s->draw[b] < 0.01f) { s->phase[b] += s->step[b]; continue; }
            int16_t samp = sv_wt[SV_WAVE_SINE][(s->phase[b] >> 24) & 0xFF];
            mix += (int32_t)(samp * s->draw[b]);
            s->phase[b] += s->step[b];
            total += s->draw[b];
        }
        if (total > 0.0f) mix = (int32_t)(mix / total);
        /* Apply rotary modulation */
        int32_t amp = (int32_t)(mix * (1.0f + leslie_lfo));
        if (amp >  32767) amp =  32767;
        if (amp < -32768) amp = -32768;
        /* Pan slightly L/R with sine offset for Leslie spread */
        float pan_lfo = sv_wt[SV_WAVE_SINE][((s->leslie_phase + 0x40000000) >> 24) & 0xFF]
                        * s->leslie_depth * 0.5f / (float)SV_WT_AMP;
        int32_t l = (int32_t)(amp * (1.0f + pan_lfo));
        int32_t r = (int32_t)(amp * (1.0f - pan_lfo));
        if (l >  32767) l =  32767;
        if (l < -32768) l = -32768;
        if (r >  32767) r =  32767;
        if (r < -32768) r = -32768;
        out_l[i] = (int16_t)(l);
        out_r[i] = (int16_t)(r);
    }
}

static void organ_set_param(synth_inst_t *self, uint8_t id, float v)
{
    organ_t *s = (organ_t *)self;
    if (id < ORGAN_BARS) { s->draw[id] = v < 0 ? 0 : (v > 1 ? 1 : v); return; }
    switch (id) {
    case 9:  s->leslie_step  = sv_hz_to_step(v); break;  /* rotary rate Hz */
    case 10: s->leslie_depth = v; break;
    }
}

synth_inst_t *synth_organ_new(void)
{
    organ_t *s = SYNTH_ALLOC(organ_t);
    if (!s) return NULL;
    s->hdr.type_id = 16; s->hdr.voice_count = 1;
    s->hdr.render = organ_render; s->hdr.note_on = organ_note_on;
    s->hdr.note_off = organ_note_off; s->hdr.set_param = organ_set_param;
    s->hdr.free = generic_free;
    /* Default: all drawbars at 0.8 */
    for (int i = 0; i < ORGAN_BARS; i++) s->draw[i] = 0.8f;
    s->leslie_step  = sv_hz_to_step(6.7f);   /* ~6.7 Hz slow rotation */
    s->leslie_depth = 0.15f;
    return &s->hdr;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Type 17 — Wavetable Morph  (4-point interpolation between 4 wavetables)
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    synth_inst_t hdr;
    uint32_t  phase;
    uint32_t  step;
    float     morph;     /* 0.0–3.0: interpolates across the 4 waveforms */
    uint32_t  lfo_phase;
    uint32_t  lfo_step;
    float     lfo_depth; /* scan range added to morph */
    sv_adsr_t adsr; sv_adsr_t adsr_t;
    bool      active;
    uint8_t   note;
    /* 4 wavetable slots: default sine/triangle/saw/square */
    sv_wave_t waves[4];
} morph_t;

static void morph_note_on(synth_inst_t *self, uint8_t note, uint8_t vel)
{
    morph_t *s = (morph_t *)self;
    s->note = note; s->step = sv_hz_to_step(sv_note_to_hz(note));
    s->adsr = s->adsr_t; sv_adsr_gate_on(&s->adsr);
    s->active = true; (void)vel;
}

static void morph_note_off(synth_inst_t *self, uint8_t note)
{ morph_t *s = (morph_t *)self; if (s->note == note) sv_adsr_gate_off(&s->adsr); }

static void morph_render(synth_inst_t *self, int16_t *out_l, int16_t *out_r, int n)
{
    morph_t *s = (morph_t *)self;
    if (!s->active) return;
    for (int i = 0; i < n; i++) {
        float env = sv_adsr_tick(&s->adsr);
        if (s->adsr.stage == SV_ST_IDLE) { s->active = false; break; }

        /* LFO-driven morph scan */
        float lfo = sv_wt[SV_WAVE_SINE][(s->lfo_phase >> 24) & 0xFF]
                    * s->lfo_depth / (float)SV_WT_AMP;
        s->lfo_phase += s->lfo_step;
        float m = s->morph + lfo;
        if (m < 0.0f) m = 0.0f;
        if (m > 3.0f) m = 3.0f;

        /* Linear interpolation between adjacent waveforms */
        int   idx_a = (int)m;
        float frac  = m - (float)idx_a;
        int   idx_b = idx_a < 3 ? idx_a + 1 : 3;

        uint8_t phase_idx = (uint8_t)(s->phase >> 24);
        s->phase += s->step;

        float sa = (s->waves[idx_a] < SV_WAVE_NOISE)
                 ? sv_wt[s->waves[idx_a]][phase_idx] : 0;
        float sb = (s->waves[idx_b] < SV_WAVE_NOISE)
                 ? sv_wt[s->waves[idx_b]][phase_idx] : 0;
        float samp = sa + (sb - sa) * frac;

        int32_t out = (int32_t)(samp * env);
        if (out >  32767) out =  32767;
        if (out < -32768) out = -32768;
        out_l[i] = (int16_t)(out);
        out_r[i] = (int16_t)(out);
    }
}

static void morph_set_param(synth_inst_t *self, uint8_t id, float v)
{
    morph_t *s = (morph_t *)self;
    switch (id) {
    case 0: s->morph      = v < 0 ? 0 : (v > 3 ? 3 : v); break;
    case 1: s->lfo_step   = sv_hz_to_step(v); break;
    case 2: s->lfo_depth  = v; break;
    case P_ATK: s->adsr_t.atk = (uint32_t)(v * SAMPLE_RATE / 1000.0f); break;
    case P_DCY: s->adsr_t.dcy = (uint32_t)(v * SAMPLE_RATE / 1000.0f); break;
    case P_SUS: s->adsr_t.sus = v; break;
    case P_REL: s->adsr_t.rel = (uint32_t)(v * SAMPLE_RATE / 1000.0f); break;
    }
}

synth_inst_t *synth_morph_new(void)
{
    morph_t *s = SYNTH_ALLOC(morph_t);
    if (!s) return NULL;
    s->hdr.type_id = 17; s->hdr.voice_count = 1;
    s->hdr.render = morph_render; s->hdr.note_on = morph_note_on;
    s->hdr.note_off = morph_note_off; s->hdr.set_param = morph_set_param;
    s->hdr.free = generic_free;
    s->waves[0] = SV_WAVE_SINE; s->waves[1] = SV_WAVE_TRIANGLE;
    s->waves[2] = SV_WAVE_SAW;  s->waves[3] = SV_WAVE_SQUARE;
    s->morph = 0.0f; s->lfo_step = sv_hz_to_step(0.1f); s->lfo_depth = 0.5f;
    sv_adsr_set(&s->adsr_t, 5.0f, 100.0f, 0.8f, 400.0f);
    return &s->hdr;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Type 18 — Vowel Synth  (3 formant bandpass filters, A-E-I-O-U morph)
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Formant frequencies for 5 vowels × 3 formants [Hz] */
static const float vowel_f[5][3] = {
    {800,  1150, 2900},  /* A */
    {350,  2000, 2800},  /* E */
    {270,  2140, 2950},  /* I */
    {450,   800, 2830},  /* O */
    {325,   700, 2530},  /* U */
};
static const float vowel_bw[3] = {80.0f, 90.0f, 120.0f};

typedef struct {
    synth_inst_t hdr;
    uint32_t  phase, step;
    sv_svf_t  f[3];         /* 3 bandpass filters */
    float     vowel;        /* 0.0–4.0: morphs across A/E/I/O/U */
    sv_adsr_t adsr; sv_adsr_t adsr_t;
    bool      active; uint8_t note;
} vowel_t;

static void vowel_update_filters(vowel_t *s)
{
    int v0 = (int)s->vowel;
    float fr = s->vowel - (float)v0;
    int v1 = v0 < 4 ? v0 + 1 : 4;
    for (int i = 0; i < 3; i++) {
        float freq = vowel_f[v0][i] + (vowel_f[v1][i] - vowel_f[v0][i]) * fr;
        float q = freq / vowel_bw[i];
        sv_svf_set(&s->f[i], freq, q);
    }
}

static void vowel_note_on(synth_inst_t *self, uint8_t note, uint8_t vel)
{
    vowel_t *s = (vowel_t *)self;
    s->note = note; s->step = sv_hz_to_step(sv_note_to_hz(note));
    s->adsr = s->adsr_t; sv_adsr_gate_on(&s->adsr);
    s->active = true; (void)vel;
}

static void vowel_note_off(synth_inst_t *self, uint8_t note)
{ vowel_t *s = (vowel_t *)self; if (s->note == note) sv_adsr_gate_off(&s->adsr); }

static void vowel_render(synth_inst_t *self, int16_t *out_l, int16_t *out_r, int n)
{
    vowel_t *s = (vowel_t *)self;
    if (!s->active) return;
    for (int i = 0; i < n; i++) {
        float env = sv_adsr_tick(&s->adsr);
        if (s->adsr.stage == SV_ST_IDLE) { s->active = false; break; }
        float saw = (float)sv_wt[SV_WAVE_SAW][(s->phase >> 24) & 0xFF];
        s->phase += s->step;
        /* Sum 3 bandpass filters */
        float out = 0.0f;
        for (int f = 0; f < 3; f++) {
            sv_svf_tick(&s->f[f], saw);
            out += s->f[f].bp;
        }
        int32_t samp = (int32_t)(out / 3.0f * env);
        if (samp >  32767) samp =  32767;
        if (samp < -32768) samp = -32768;
        out_l[i] = (int16_t)(samp);
        out_r[i] = (int16_t)(samp);
    }
}

static void vowel_set_param(synth_inst_t *self, uint8_t id, float v)
{
    vowel_t *s = (vowel_t *)self;
    switch (id) {
    case 0: s->vowel = v < 0 ? 0 : (v > 4 ? 4 : v); vowel_update_filters(s); break;
    case P_ATK: s->adsr_t.atk = (uint32_t)(v * SAMPLE_RATE / 1000.0f); break;
    case P_REL: s->adsr_t.rel = (uint32_t)(v * SAMPLE_RATE / 1000.0f); break;
    }
}

synth_inst_t *synth_vowel_new(void)
{
    vowel_t *s = SYNTH_ALLOC(vowel_t);
    if (!s) return NULL;
    s->hdr.type_id = 18; s->hdr.voice_count = 1;
    s->hdr.render = vowel_render; s->hdr.note_on = vowel_note_on;
    s->hdr.note_off = vowel_note_off; s->hdr.set_param = vowel_set_param;
    s->hdr.free = generic_free;
    s->vowel = 0.0f; vowel_update_filters(s);
    sv_adsr_set(&s->adsr_t, 5.0f, 100.0f, 0.7f, 300.0f);
    return &s->hdr;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Type 19 — Bit-Crush Synth  (wavetable + bit-depth reduction + SR divider)
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    synth_inst_t hdr;
    uint32_t  phase, step;
    sv_wave_t wave;
    uint8_t   bit_depth;   /* 1–16 */
    uint8_t   sr_div;      /* 1–32: sample-rate divider */
    uint8_t   sr_cnt;      /* counter */
    int32_t   held_samp;   /* last output before SR div */
    sv_adsr_t adsr; sv_adsr_t adsr_t;
    bool      active; uint8_t note;
} bitcrush_t;

static void bitcrush_note_on(synth_inst_t *self, uint8_t note, uint8_t vel)
{
    bitcrush_t *s = (bitcrush_t *)self;
    s->note = note; s->step = sv_hz_to_step(sv_note_to_hz(note));
    s->adsr = s->adsr_t; sv_adsr_gate_on(&s->adsr);
    s->active = true; (void)vel;
}

static void bitcrush_note_off(synth_inst_t *self, uint8_t note)
{ bitcrush_t *s = (bitcrush_t *)self; if (s->note == note) sv_adsr_gate_off(&s->adsr); }

static void bitcrush_render(synth_inst_t *self, int16_t *out_l, int16_t *out_r, int n)
{
    bitcrush_t *s = (bitcrush_t *)self;
    if (!s->active) return;
    int32_t quant_step = 1 << (16 - s->bit_depth);
    for (int i = 0; i < n; i++) {
        float env = sv_adsr_tick(&s->adsr);
        if (s->adsr.stage == SV_ST_IDLE) { s->active = false; break; }

        /* Sample rate divider */
        if (s->sr_cnt == 0) {
            const int16_t *wt = (s->wave < SV_WAVE_NOISE) ? sv_wt[s->wave]
                               : sv_wt[SV_WAVE_SQUARE];
            int32_t raw = wt[(s->phase >> 24) & 0xFF];
            s->phase += s->step;
            /* Bit-depth quantise */
            raw = (raw / quant_step) * quant_step;
            s->held_samp = raw;
        }
        s->sr_cnt = (s->sr_cnt + 1) % s->sr_div;

        int32_t samp = (int32_t)(s->held_samp * env);
        if (samp >  32767) samp =  32767;
        if (samp < -32768) samp = -32768;
        out_l[i] = (int16_t)(samp);
        out_r[i] = (int16_t)(samp);
    }
}

static void bitcrush_set_param(synth_inst_t *self, uint8_t id, float v)
{
    bitcrush_t *s = (bitcrush_t *)self;
    switch (id) {
    case 0: s->wave      = (sv_wave_t)(int)v; break;
    case 1: s->bit_depth = (uint8_t)(int)v;
            if (!s->bit_depth) s->bit_depth = 1;
            if (s->bit_depth > 16) s->bit_depth = 16;
            break;
    case 2: s->sr_div = (uint8_t)(int)v;
            if (!s->sr_div) s->sr_div = 1;
            if (s->sr_div > 32) s->sr_div = 32;
            break;
    case P_ATK: s->adsr_t.atk = (uint32_t)(v * SAMPLE_RATE / 1000.0f); break;
    case P_REL: s->adsr_t.rel = (uint32_t)(v * SAMPLE_RATE / 1000.0f); break;
    }
}

synth_inst_t *synth_bitcrush_new(void)
{
    bitcrush_t *s = SYNTH_ALLOC(bitcrush_t);
    if (!s) return NULL;
    s->hdr.type_id = 19; s->hdr.voice_count = 1;
    s->hdr.render = bitcrush_render; s->hdr.note_on = bitcrush_note_on;
    s->hdr.note_off = bitcrush_note_off; s->hdr.set_param = bitcrush_set_param;
    s->hdr.free = generic_free;
    s->wave = SV_WAVE_SQUARE; s->bit_depth = 8; s->sr_div = 4;
    sv_adsr_set(&s->adsr_t, 2.0f, 50.0f, 0.7f, 150.0f);
    return &s->hdr;
}

/* ── Raw factory ─────────────────────────────────────────────────────────── */
/* Builds the bare (single-voice for most types) instrument by type_id.  Used
 * directly by the polyphony wrapper for each of its inner voices. */
static synth_inst_t *synth_new_raw(uint8_t type_id)
{
    switch (type_id) {
    case SYNTH_TYPE_MONO_WT:     return synth_mono_wt_new();
    case SYNTH_TYPE_POLY_WT:     return synth_poly_wt_new();
    case SYNTH_TYPE_SUPERSAW:    return synth_supersaw_new();
    case SYNTH_TYPE_FM2:         return synth_fm2_new();
    case SYNTH_TYPE_FM4:         return synth_fm4_new();
    case SYNTH_TYPE_SUBTRACTIVE: return synth_subtractive_new();
    case SYNTH_TYPE_KS:          return synth_ks_new();
    case SYNTH_TYPE_BELL:        return synth_bell_new();
    case SYNTH_TYPE_PAD:         return synth_pad_new();
    case SYNTH_TYPE_NOISE:       return synth_noise_new();
    case SYNTH_TYPE_BASS:        return synth_bass_new();
    case SYNTH_TYPE_LEAD:        return synth_lead_new();
    case SYNTH_TYPE_CHORD:       return synth_chord_new();
    case SYNTH_TYPE_DRUM_BD:     return synth_drum_bd_new();
    case SYNTH_TYPE_DRUM_SD:     return synth_drum_sd_new();
    case SYNTH_TYPE_DRUM_HH:     return synth_drum_hh_new();
    case SYNTH_TYPE_ORGAN:       return synth_organ_new();
    case SYNTH_TYPE_MORPH:       return synth_morph_new();
    case SYNTH_TYPE_VOWEL:       return synth_vowel_new();
    case SYNTH_TYPE_BITCRUSH:    return synth_bitcrush_new();
    default:                     return NULL;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Polyphony wrapper — turns an inherently-monophonic synth into a per-key voice
 * pool so any instrument layers when several keys are held.  It holds PW_VOICES
 * independent instances of the wrapped type, routes each note-on to a free (or
 * stolen) voice, note-off to the matching voice, and sums their renders with
 * the same fixed-headroom + soft-clip scheme as the poly wavetable engine — so
 * chords swell instead of ducking, and peaks saturate gently.
 * ═══════════════════════════════════════════════════════════════════════════ */
#define PW_VOICES    6   /* simultaneous notes per live synth lane            */
#define PW_HEADROOM  3   /* voices that reach 0 dBFS together (see synth_osc)  */

typedef struct {
    synth_inst_t  hdr;
    synth_inst_t *v[PW_VOICES];
    uint8_t       note[PW_VOICES];   /* note each voice last played            */
    bool          gated[PW_VOICES];  /* key still held (not yet released)      */
    uint32_t      age[PW_VOICES];    /* allocation order — lowest = oldest     */
    uint32_t      age_ctr;
} pw_t;

static void pw_note_on(synth_inst_t *self, uint8_t note, uint8_t vel)
{
    pw_t *s = (pw_t *)self;
    int pick = -1;
    /* 1. same note already gated → retrigger that voice */
    for (int i = 0; i < PW_VOICES; i++)
        if (s->gated[i] && s->note[i] == note) { pick = i; break; }
    /* 2. oldest released/idle voice */
    if (pick < 0) {
        uint32_t best = UINT32_MAX;
        for (int i = 0; i < PW_VOICES; i++)
            if (!s->gated[i] && s->age[i] < best) { best = s->age[i]; pick = i; }
    }
    /* 3. all gated → steal the oldest */
    if (pick < 0) {
        uint32_t best = UINT32_MAX;
        for (int i = 0; i < PW_VOICES; i++)
            if (s->age[i] < best) { best = s->age[i]; pick = i; }
    }
    s->note[pick]  = note;
    s->gated[pick] = true;
    s->age[pick]   = ++s->age_ctr;
    if (s->v[pick] && s->v[pick]->note_on) s->v[pick]->note_on(s->v[pick], note, vel);
}

static void pw_note_off(synth_inst_t *self, uint8_t note)
{
    pw_t *s = (pw_t *)self;
    for (int i = 0; i < PW_VOICES; i++)
        if (s->gated[i] && s->note[i] == note) {
            s->gated[i] = false;   /* keep age → preferred for next steal */
            if (s->v[i] && s->v[i]->note_off) s->v[i]->note_off(s->v[i], note);
        }
}

static void pw_render(synth_inst_t *self, int16_t *out_l, int16_t *out_r, int n)
{
    pw_t *s = (pw_t *)self;
    /* Audio task is single-threaded → static scratch/accumulators are safe. */
    static int16_t sl[AUDIO_BUF_FRAMES], sr[AUDIO_BUF_FRAMES];
    static int32_t al[AUDIO_BUF_FRAMES], ar[AUDIO_BUF_FRAMES];
    for (int i = 0; i < n; i++) { al[i] = 0; ar[i] = 0; }
    for (int vx = 0; vx < PW_VOICES; vx++) {
        synth_inst_t *iv = s->v[vx];
        if (!iv || !iv->render) continue;
        /* Inner renders overwrite, and early-return (leaving the buffer) when
         * idle — so clear scratch first; idle voices then add silence. */
        for (int i = 0; i < n; i++) { sl[i] = 0; sr[i] = 0; }
        iv->render(iv, sl, sr, n);
        for (int i = 0; i < n; i++) { al[i] += sl[i]; ar[i] += sr[i]; }
    }
    for (int i = 0; i < n; i++) {
        out_l[i] = (int16_t)sv_soft_clip(al[i] / PW_HEADROOM);
        out_r[i] = (int16_t)sv_soft_clip(ar[i] / PW_HEADROOM);
    }
}

static void pw_set_param(synth_inst_t *self, uint8_t id, float v)
{
    pw_t *s = (pw_t *)self;
    for (int i = 0; i < PW_VOICES; i++)
        if (s->v[i] && s->v[i]->set_param) s->v[i]->set_param(s->v[i], id, v);
}

static void pw_free(synth_inst_t *self)
{
    pw_t *s = (pw_t *)self;
    for (int i = 0; i < PW_VOICES; i++)
        if (s->v[i] && s->v[i]->free) s->v[i]->free(s->v[i]);
    heap_caps_free(s);
}

/* Types that already manage their own polyphony / are one-shot percussion and
 * should not be wrapped. */
static bool type_wraps_poly(uint8_t t)
{
    switch (t) {
    case SYNTH_TYPE_POLY_WT:   /* already an 8-voice pool                */
    case SYNTH_TYPE_DRUM_BD:   /* percussive one-shots, sequencer-driven */
    case SYNTH_TYPE_DRUM_SD:
    case SYNTH_TYPE_DRUM_HH:
        return false;
    default:
        return true;
    }
}

static synth_inst_t *synth_poly_wrap_new(uint8_t inner_type)
{
    pw_t *s = SYNTH_ALLOC(pw_t);
    if (!s) return NULL;
    for (int i = 0; i < PW_VOICES; i++) {
        s->v[i] = synth_new_raw(inner_type);
        if (!s->v[i]) {                        /* alloc failed → unwind */
            for (int j = 0; j < i; j++)
                if (s->v[j] && s->v[j]->free) s->v[j]->free(s->v[j]);
            heap_caps_free(s);
            return NULL;
        }
    }
    s->hdr.type_id     = inner_type;   /* report wrapped type for save/load/UI */
    s->hdr.voice_count = PW_VOICES;
    s->hdr.render      = pw_render;
    s->hdr.note_on     = pw_note_on;
    s->hdr.note_off    = pw_note_off;
    s->hdr.set_param   = pw_set_param;
    s->hdr.free        = pw_free;
    return &s->hdr;
}

/* ── Dispatcher ──────────────────────────────────────────────────────────── */
synth_inst_t *synth_new(uint8_t type_id)
{
    if (type_id >= SYNTH_TYPE_COUNT) return NULL;
    if (type_wraps_poly(type_id)) return synth_poly_wrap_new(type_id);
    return synth_new_raw(type_id);
}

void synth_free(synth_inst_t *inst)
{
    if (inst && inst->free) inst->free(inst);
}
