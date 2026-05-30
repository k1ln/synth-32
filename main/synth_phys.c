/*
 * synth_phys.c — Synth types 6–11 (Phase 3)
 *
 *  6  Plucked String (Karplus-Strong + all-pass loop filter)
 *  7  Physical Model Bell (modal synthesis, 8 partials)
 *  8  Pad / Additive (16 sine partials, LFO amplitude)
 *  9  Noise Synth (filtered noise + VCF + ADSR)
 * 10  Bass Synth (saw/square + 2-pole LP + drive + portamento)
 * 11  Lead Synth (mono, PWM + vibrato)
 */

#include <string.h>
#include <math.h>
#include "esp_heap_caps.h"
#include "synth.h"
#include "synth_voice.h"

#define SYNTH_ALLOC(T) ((T *)heap_caps_calloc(1, sizeof(T), MALLOC_CAP_SPIRAM))
static void generic_free(synth_inst_t *self) { heap_caps_free(self); }

/* ── Param IDs ───────────────────────────────────────────────────────────── */
enum {
    P_FEEDBACK = 0, P_DECAY, P_EXCITER,     /* KS */
    P_INHARMONIC, P_DAMPING,                 /* Bell */
    P_PARTIAL_BASE = 0,                      /* Pad: 16 params */
    P_NOISE_COLOR = 0, P_FILTER_FREQ, P_FILTER_RES, /* Noise */
    P_CUTOFF = 0, P_RESONANCE, P_DRIVE, P_PORTAMENTO, /* Bass */
    P_PWM_WIDTH = 0, P_VIBRATO_RATE, P_VIBRATO_DEPTH, /* Lead */
    P_ATK = 20, P_DCY, P_SUS, P_REL,
};

/* ═══════════════════════════════════════════════════════════════════════════
 * Type 6 — Karplus-Strong plucked string
 * ═══════════════════════════════════════════════════════════════════════════ */

#define KS_BUF_MAX  2048   /* max delay line length — fits ~23 Hz @ 48kHz */

typedef struct {
    synth_inst_t hdr;
    int16_t  *buf;         /* PSRAM delay line */
    uint16_t  buf_len;     /* delay in samples for current note */
    uint16_t  pos;
    float     feedback;    /* 0.9–1.0 */
    float     ap_s;        /* all-pass filter state */
    float     ap_coeff;    /* all-pass coefficient for "stretching" */
    bool      active;
    sv_adsr_t adsr;
    sv_adsr_t adsr_t;
} ks_t;

/* White noise exciter burst to prime the delay line */
static void ks_excite(ks_t *s)
{
    uint16_t lfsr = 0xACE1;
    for (int i = 0; i < s->buf_len; i++) {
        lfsr = (uint16_t)(lfsr >> 1) ^ ((lfsr & 1) ? 0xB400 : 0);
        s->buf[i] = (int16_t)lfsr;
    }
    s->pos  = 0;
    s->ap_s = 0.0f;
}

static void ks_note_on(synth_inst_t *self, uint8_t note, uint8_t vel)
{
    ks_t *s = (ks_t *)self;
    float hz = sv_note_to_hz(note);
    uint16_t len = (uint16_t)((float)SAMPLE_RATE / hz + 0.5f);
    if (len < 2)       len = 2;
    if (len > KS_BUF_MAX) len = KS_BUF_MAX;
    s->buf_len = len;
    ks_excite(s);
    /* Scale exciter by velocity */
    float scale = (float)vel / 127.0f;
    for (int i = 0; i < len; i++) s->buf[i] = (int16_t)(s->buf[i] * scale);
    s->adsr = s->adsr_t;
    sv_adsr_gate_on(&s->adsr);
    s->active = true;
}

static void ks_note_off(synth_inst_t *self, uint8_t note)
{
    (void)note;
    ks_t *s = (ks_t *)self;
    sv_adsr_gate_off(&s->adsr);
}

static void ks_render(synth_inst_t *self, int16_t *out_l, int16_t *out_r, int n)
{
    ks_t *s = (ks_t *)self;
    if (!s->active || !s->buf) return;
    uint16_t len = s->buf_len;
    float fb = s->feedback;

    for (int i = 0; i < n; i++) {
        float env = sv_adsr_tick(&s->adsr);
        if (s->adsr.stage == SV_ST_IDLE) { s->active = false; break; }

        /* Read from delay line */
        float cur = (float)s->buf[s->pos];
        uint16_t next_pos = (uint16_t)((s->pos + 1) % len);
        float nxt = (float)s->buf[next_pos];

        /* 2-point average low-pass (KS feedback filter) */
        float avg = (cur + nxt) * 0.5f * fb;

        /* All-pass for fractional delay / pitch stretching */
        float ap_out = -s->ap_coeff * avg + s->ap_s;
        s->ap_s = avg + s->ap_coeff * ap_out;

        s->buf[s->pos] = (int16_t)(ap_out);
        s->pos = next_pos;

        int32_t samp = (int32_t)(ap_out * env);
        if (samp >  32767) samp =  32767;
        if (samp < -32768) samp = -32768;
        out_l[i] = (int16_t)(samp);
        out_r[i] = (int16_t)(samp);
    }
}

static void ks_set_param(synth_inst_t *self, uint8_t id, float v)
{
    ks_t *s = (ks_t *)self;
    switch (id) {
    case 0 /* FEEDBACK */: s->feedback = v < 0.5f ? 0.5f : (v > 0.999f ? 0.999f : v); break;
    case 2 /* AP_COEFF */: s->ap_coeff = v; break;
    case P_ATK: s->adsr_t.atk = (uint32_t)(v * SAMPLE_RATE / 1000.0f); break;
    case P_REL: s->adsr_t.rel = (uint32_t)(v * SAMPLE_RATE / 1000.0f); break;
    }
}

synth_inst_t *synth_ks_new(void)
{
    ks_t *s = SYNTH_ALLOC(ks_t);
    if (!s) return NULL;
    s->buf = (int16_t *)heap_caps_calloc(KS_BUF_MAX, sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!s->buf) { heap_caps_free(s); return NULL; }
    s->hdr.type_id     = 6;
    s->hdr.voice_count = 1;
    s->hdr.render      = ks_render;
    s->hdr.note_on     = ks_note_on;
    s->hdr.note_off    = ks_note_off;
    s->hdr.set_param   = ks_set_param;
    s->hdr.free        = generic_free;
    s->feedback  = 0.996f;
    s->ap_coeff  = 0.3f;
    sv_adsr_set(&s->adsr_t, 1.0f, 10.0f, 0.0f, 2000.0f);
    return &s->hdr;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Type 7 — Bell (modal synthesis, 8 partials with independent decay)
 * ═══════════════════════════════════════════════════════════════════════════ */

#define BELL_PARTIALS 8

typedef struct {
    synth_inst_t hdr;
    uint32_t  phase[BELL_PARTIALS];
    uint32_t  step[BELL_PARTIALS];
    float     amp[BELL_PARTIALS];      /* current amplitude */
    float     decay[BELL_PARTIALS];    /* per-sample decay multiplier */
    float     inharmonic;              /* inharmonicity factor 1.0–2.0 */
    float     damping;                 /* overall decay speed */
    bool      active;
} bell_t;

/* Partial frequency ratios (inharmonic — approximate bell partials) */
static const float bell_ratios[BELL_PARTIALS] = {
    1.0f, 2.756f, 5.404f, 8.933f, 13.34f, 18.64f, 24.82f, 31.87f
};
static const float bell_amps_init[BELL_PARTIALS] = {
    1.0f, 0.7f, 0.45f, 0.3f, 0.2f, 0.15f, 0.1f, 0.07f
};

static void bell_note_on(synth_inst_t *self, uint8_t note, uint8_t vel)
{
    bell_t *s = (bell_t *)self;
    float base = sv_note_to_hz(note);
    float vel_scale = (float)vel / 127.0f;
    for (int p = 0; p < BELL_PARTIALS; p++) {
        float freq = base * bell_ratios[p] * s->inharmonic;
        if (freq > 20000.0f) freq = 20000.0f;
        s->step[p]  = sv_hz_to_step(freq);
        s->phase[p] = 0;
        s->amp[p]   = bell_amps_init[p] * vel_scale;
        /* Decay: higher partials decay faster */
        s->decay[p] = 1.0f - s->damping * bell_ratios[p] / SAMPLE_RATE;
        if (s->decay[p] < 0.0f) s->decay[p] = 0.0f;
    }
    s->active = true;
}

static void bell_note_off(synth_inst_t *self, uint8_t note) { (void)self; (void)note; }

static void bell_render(synth_inst_t *self, int16_t *out_l, int16_t *out_r, int n)
{
    bell_t *s = (bell_t *)self;
    if (!s->active) return;
    for (int i = 0; i < n; i++) {
        int32_t mix = 0;
        bool any = false;
        for (int p = 0; p < BELL_PARTIALS; p++) {
            if (s->amp[p] < 0.001f) continue;
            any = true;
            int16_t samp = sv_wt[SV_WAVE_SINE][(s->phase[p] >> 24) & 0xFF];
            mix += (int32_t)(samp * s->amp[p]);
            s->phase[p] += s->step[p];
            s->amp[p]   *= s->decay[p];
        }
        if (!any) { s->active = false; break; }
        mix /= BELL_PARTIALS;
        if (mix >  32767) mix =  32767;
        if (mix < -32768) mix = -32768;
        out_l[i] = (int16_t)(mix);
        out_r[i] = (int16_t)(mix);
    }
}

static void bell_set_param(synth_inst_t *self, uint8_t id, float v)
{
    bell_t *s = (bell_t *)self;
    switch (id) {
    case 0: s->inharmonic = v < 0.5f ? 0.5f : v; break;
    case 1: s->damping    = v < 0.0f ? 0.0f : v; break;
    }
}

synth_inst_t *synth_bell_new(void)
{
    bell_t *s = SYNTH_ALLOC(bell_t);
    if (!s) return NULL;
    s->hdr.type_id     = 7;
    s->hdr.voice_count = 1;
    s->hdr.render      = bell_render;
    s->hdr.note_on     = bell_note_on;
    s->hdr.note_off    = bell_note_off;
    s->hdr.set_param   = bell_set_param;
    s->hdr.free        = generic_free;
    s->inharmonic = 1.0f;
    s->damping    = 2.0f;
    return &s->hdr;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Type 8 — Pad / Additive  (16 sine partials + slow LFO amplitude mod)
 * ═══════════════════════════════════════════════════════════════════════════ */

#define PAD_PARTIALS 16

typedef struct {
    synth_inst_t hdr;
    uint32_t  phase[PAD_PARTIALS];
    uint32_t  step[PAD_PARTIALS];
    float     level[PAD_PARTIALS]; /* user-set level per partial */
    sv_adsr_t adsr;
    sv_adsr_t adsr_t;
    /* LFO for slow amplitude shimmer */
    uint32_t  lfo_phase;
    uint32_t  lfo_step;
    float     lfo_depth;
    bool      active;
    uint8_t   note;
} pad_t;

static void pad_note_on(synth_inst_t *self, uint8_t note, uint8_t vel)
{
    pad_t *s = (pad_t *)self;
    s->note = note;
    float base = sv_note_to_hz(note);
    for (int p = 0; p < PAD_PARTIALS; p++) {
        s->step[p]  = sv_hz_to_step(base * (float)(p + 1));
        s->phase[p] = (uint32_t)p * (0xFFFFFFFF / PAD_PARTIALS);
    }
    s->adsr = s->adsr_t;
    sv_adsr_gate_on(&s->adsr);
    s->active = true;
    (void)vel;
}

static void pad_note_off(synth_inst_t *self, uint8_t note)
{
    pad_t *s = (pad_t *)self;
    if (s->note == note) sv_adsr_gate_off(&s->adsr);
}

static void pad_render(synth_inst_t *self, int16_t *out_l, int16_t *out_r, int n)
{
    pad_t *s = (pad_t *)self;
    if (!s->active) return;
    for (int i = 0; i < n; i++) {
        float env = sv_adsr_tick(&s->adsr);
        if (s->adsr.stage == SV_ST_IDLE) { s->active = false; break; }

        /* LFO shimmer */
        float lfo = sv_wt[SV_WAVE_SINE][(s->lfo_phase >> 24) & 0xFF]
                    * s->lfo_depth / (float)SV_WT_AMP;
        s->lfo_phase += s->lfo_step;

        int32_t mix = 0;
        float total_level = 0.0f;
        for (int p = 0; p < PAD_PARTIALS; p++) {
            if (s->level[p] < 0.001f) { s->phase[p] += s->step[p]; continue; }
            float samp = sv_wt[SV_WAVE_SINE][(s->phase[p] >> 24) & 0xFF];
            mix += (int32_t)(samp * s->level[p] * (1.0f + lfo));
            s->phase[p] += s->step[p];
            total_level += s->level[p];
        }
        if (total_level > 0.0f) mix = (int32_t)(mix / total_level * env);
        if (mix >  32767) mix =  32767;
        if (mix < -32768) mix = -32768;
        out_l[i] = (int16_t)(mix);
        out_r[i] = (int16_t)(mix);
    }
}

static void pad_set_param(synth_inst_t *self, uint8_t id, float v)
{
    pad_t *s = (pad_t *)self;
    if (id < PAD_PARTIALS) { s->level[id] = v < 0 ? 0 : (v > 1 ? 1 : v); return; }
    switch (id) {
    case P_ATK: s->adsr_t.atk = (uint32_t)(v * SAMPLE_RATE / 1000.0f); break;
    case P_DCY: s->adsr_t.dcy = (uint32_t)(v * SAMPLE_RATE / 1000.0f); break;
    case P_SUS: s->adsr_t.sus = v; break;
    case P_REL: s->adsr_t.rel = (uint32_t)(v * SAMPLE_RATE / 1000.0f); break;
    }
}

synth_inst_t *synth_pad_new(void)
{
    pad_t *s = SYNTH_ALLOC(pad_t);
    if (!s) return NULL;
    s->hdr.type_id     = 8;
    s->hdr.voice_count = 1;
    s->hdr.render      = pad_render;
    s->hdr.note_on     = pad_note_on;
    s->hdr.note_off    = pad_note_off;
    s->hdr.set_param   = pad_set_param;
    s->hdr.free        = generic_free;
    /* Harmonic series amplitude falloff */
    for (int p = 0; p < PAD_PARTIALS; p++)
        s->level[p] = 1.0f / (float)(p + 1);
    s->lfo_step  = sv_hz_to_step(0.3f);
    s->lfo_depth = 0.08f;
    sv_adsr_set(&s->adsr_t, 500.0f, 200.0f, 0.9f, 1000.0f);
    return &s->hdr;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Type 9 — Noise Synth  (colored noise + SVF + ADSR)
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef enum { NOISE_WHITE, NOISE_PINK, NOISE_BROWN } noise_color_t;

typedef struct {
    synth_inst_t hdr;
    uint16_t  lfsr;
    /* Pink noise pinking filter state (3-pole Paul Kellett approximation) */
    float     pk[3];
    sv_svf_t  filt;
    sv_adsr_t adsr;
    sv_adsr_t adsr_t;
    noise_color_t color;
    bool      active;
} noise_t;

static void noise_note_on(synth_inst_t *self, uint8_t note, uint8_t vel)
{
    noise_t *s = (noise_t *)self;
    s->adsr = s->adsr_t;
    sv_adsr_gate_on(&s->adsr);
    s->active = true;
    (void)note; (void)vel;
}

static void noise_note_off(synth_inst_t *self, uint8_t note)
{
    noise_t *s = (noise_t *)self;
    sv_adsr_gate_off(&s->adsr);
    (void)note;
}

static void noise_render(synth_inst_t *self, int16_t *out_l, int16_t *out_r, int n)
{
    noise_t *s = (noise_t *)self;
    if (!s->active) return;
    for (int i = 0; i < n; i++) {
        float env = sv_adsr_tick(&s->adsr);
        if (s->adsr.stage == SV_ST_IDLE) { s->active = false; break; }

        /* White noise LFSR */
        s->lfsr = (uint16_t)(s->lfsr >> 1) ^ ((s->lfsr & 1) ? 0xB400 : 0);
        float w = (float)(int16_t)s->lfsr / 32768.0f;

        float out;
        if (s->color == NOISE_WHITE) {
            out = w;
        } else if (s->color == NOISE_PINK) {
            /* Paul Kellett pink filter */
            s->pk[0] = 0.99886f * s->pk[0] + w * 0.0555179f;
            s->pk[1] = 0.99332f * s->pk[1] + w * 0.0750759f;
            s->pk[2] = 0.96900f * s->pk[2] + w * 0.1538520f;
            out = s->pk[0] + s->pk[1] + s->pk[2] + w * 0.5362f;
            out *= 0.11f;
        } else {
            /* Brown noise: integrate white noise */
            s->pk[0] = (s->pk[0] + 0.02f * w) * 0.99f;
            out = s->pk[0];
        }

        /* SVF filter */
        float filtered = sv_svf_tick(&s->filt, out);
        int32_t samp = (int32_t)(filtered * SV_WT_AMP * env);
        if (samp >  32767) samp =  32767;
        if (samp < -32768) samp = -32768;
        out_l[i] = (int16_t)(samp);
        out_r[i] = (int16_t)(samp);
    }
}

static void noise_set_param(synth_inst_t *self, uint8_t id, float v)
{
    noise_t *s = (noise_t *)self;
    switch (id) {
    case 0: s->color = (noise_color_t)(int)v; break;
    case 1: sv_svf_set(&s->filt, v, 1.0f); break;  /* filter freq */
    case 2: sv_svf_set(&s->filt, 1000.0f, v); break; /* filter res */
    case P_ATK: s->adsr_t.atk = (uint32_t)(v * SAMPLE_RATE / 1000.0f); break;
    case P_DCY: s->adsr_t.dcy = (uint32_t)(v * SAMPLE_RATE / 1000.0f); break;
    case P_SUS: s->adsr_t.sus = v; break;
    case P_REL: s->adsr_t.rel = (uint32_t)(v * SAMPLE_RATE / 1000.0f); break;
    }
}

synth_inst_t *synth_noise_new(void)
{
    noise_t *s = SYNTH_ALLOC(noise_t);
    if (!s) return NULL;
    s->hdr.type_id     = 9;
    s->hdr.voice_count = 1;
    s->hdr.render      = noise_render;
    s->hdr.note_on     = noise_note_on;
    s->hdr.note_off    = noise_note_off;
    s->hdr.set_param   = noise_set_param;
    s->hdr.free        = generic_free;
    s->color = NOISE_PINK;
    s->lfsr  = 0xACE1;
    sv_svf_set(&s->filt, 4000.0f, 1.0f);
    sv_adsr_set(&s->adsr_t, 5.0f, 200.0f, 0.7f, 300.0f);
    return &s->hdr;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Type 10 — Bass Synth  (saw/square + 2-pole LP + drive + portamento)
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    synth_inst_t hdr;
    uint32_t  phase;
    uint32_t  cur_step;     /* gliding step */
    uint32_t  target_step;
    uint32_t  glide_inc;    /* step change per sample during portamento */
    sv_svf_t  filt;
    sv_adsr_t adsr;
    sv_adsr_t adsr_t;
    float     cutoff;
    float     resonance;
    float     drive;
    float     portamento_ms;
    bool      active;
    sv_wave_t wave;
} bass_t;

static void bass_note_on(synth_inst_t *self, uint8_t note, uint8_t vel)
{
    bass_t *s = (bass_t *)self;
    float hz = sv_note_to_hz(note);
    s->target_step = sv_hz_to_step(hz);
    if (s->portamento_ms < 1.0f || !s->active) {
        s->cur_step = s->target_step;
        s->glide_inc = 0;
    } else {
        uint32_t samps = (uint32_t)(s->portamento_ms * SAMPLE_RATE / 1000.0f);
        if (samps == 0) samps = 1;
        int32_t diff = (int32_t)s->target_step - (int32_t)s->cur_step;
        s->glide_inc = (uint32_t)((diff > 0 ? diff : -diff) / (int32_t)samps);
        if (diff < 0) s->glide_inc = (uint32_t)(-(int32_t)s->glide_inc);
    }
    s->adsr = s->adsr_t;
    sv_adsr_gate_on(&s->adsr);
    s->active = true;
    (void)vel;
}

static void bass_note_off(synth_inst_t *self, uint8_t note)
{
    bass_t *s = (bass_t *)self;
    (void)note;
    sv_adsr_gate_off(&s->adsr);
}

static void bass_render(synth_inst_t *self, int16_t *out_l, int16_t *out_r, int n)
{
    bass_t *s = (bass_t *)self;
    if (!s->active) return;
    const int16_t *wt = sv_wt[s->wave < SV_WAVE_NOISE ? s->wave : SV_WAVE_SAW];
    for (int i = 0; i < n; i++) {
        /* Portamento glide */
        if (s->glide_inc) {
            int32_t diff = (int32_t)s->target_step - (int32_t)s->cur_step;
            int32_t inc  = (int32_t)s->glide_inc;
            if ((diff > 0 && inc > diff) || (diff < 0 && inc < diff)) {
                s->cur_step = s->target_step; s->glide_inc = 0;
            } else {
                s->cur_step = (uint32_t)((int32_t)s->cur_step + inc);
            }
        }
        float env = sv_adsr_tick(&s->adsr);
        if (s->adsr.stage == SV_ST_IDLE) { s->active = false; break; }

        float raw = (float)wt[(s->phase >> 24) & 0xFF] * env * s->drive;
        s->phase += s->cur_step;

        /* Soft clip */
        if (raw >  SV_WT_AMP) raw =  SV_WT_AMP;
        if (raw < -SV_WT_AMP) raw = -SV_WT_AMP;

        float filtered = sv_svf_tick(&s->filt, raw);
        int32_t samp = (int32_t)filtered;
        if (samp >  32767) samp =  32767;
        if (samp < -32768) samp = -32768;
        out_l[i] = (int16_t)(samp);
        out_r[i] = (int16_t)(samp);
    }
}

static void bass_set_param(synth_inst_t *self, uint8_t id, float v)
{
    bass_t *s = (bass_t *)self;
    switch (id) {
    case 0: s->cutoff = v; sv_svf_set(&s->filt, v, s->resonance); break;
    case 1: s->resonance = v; sv_svf_set(&s->filt, s->cutoff, v); break;
    case 2: s->drive = v < 0.01f ? 0.01f : v; break;
    case 3: s->portamento_ms = v; break;
    case P_ATK: s->adsr_t.atk = (uint32_t)(v * SAMPLE_RATE / 1000.0f); break;
    case P_DCY: s->adsr_t.dcy = (uint32_t)(v * SAMPLE_RATE / 1000.0f); break;
    case P_SUS: s->adsr_t.sus = v; break;
    case P_REL: s->adsr_t.rel = (uint32_t)(v * SAMPLE_RATE / 1000.0f); break;
    }
}

synth_inst_t *synth_bass_new(void)
{
    bass_t *s = SYNTH_ALLOC(bass_t);
    if (!s) return NULL;
    s->hdr.type_id     = 10;
    s->hdr.voice_count = 1;
    s->hdr.render      = bass_render;
    s->hdr.note_on     = bass_note_on;
    s->hdr.note_off    = bass_note_off;
    s->hdr.set_param   = bass_set_param;
    s->hdr.free        = generic_free;
    s->wave          = SV_WAVE_SAW;
    s->cutoff        = 800.0f;
    s->resonance     = 3.0f;
    s->drive         = 1.2f;
    s->portamento_ms = 0.0f;
    sv_svf_set(&s->filt, 800.0f, 3.0f);
    sv_adsr_set(&s->adsr_t, 2.0f, 60.0f, 0.5f, 150.0f);
    return &s->hdr;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Type 11 — Lead Synth  (mono PWM + vibrato LFO)
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    synth_inst_t hdr;
    uint32_t  phase;
    uint32_t  step;
    uint32_t  lfo_phase;
    uint32_t  lfo_step;
    float     pwm_width;     /* 0.1–0.9: duty cycle of pulse wave */
    float     vib_depth;     /* semitones ±2 */
    sv_adsr_t adsr;
    sv_adsr_t adsr_t;
    bool      active;
    uint8_t   note;
    float     base_hz;
} lead_t;

static void lead_note_on(synth_inst_t *self, uint8_t note, uint8_t vel)
{
    lead_t *s = (lead_t *)self;
    s->note    = note;
    s->base_hz = sv_note_to_hz(note);
    s->step    = sv_hz_to_step(s->base_hz);
    s->adsr    = s->adsr_t;
    sv_adsr_gate_on(&s->adsr);
    s->active  = true;
    (void)vel;
}

static void lead_note_off(synth_inst_t *self, uint8_t note)
{
    lead_t *s = (lead_t *)self;
    if (s->note == note) sv_adsr_gate_off(&s->adsr);
}

static void lead_render(synth_inst_t *self, int16_t *out_l, int16_t *out_r, int n)
{
    lead_t *s = (lead_t *)self;
    if (!s->active) return;
    for (int i = 0; i < n; i++) {
        float env = sv_adsr_tick(&s->adsr);
        if (s->adsr.stage == SV_ST_IDLE) { s->active = false; break; }

        /* Vibrato: LFO modulates frequency */
        float lfo_val = sv_wt[SV_WAVE_SINE][(s->lfo_phase >> 24) & 0xFF]
                        / (float)SV_WT_AMP;
        s->lfo_phase += s->lfo_step;
        float vib_semi = lfo_val * s->vib_depth;
        uint32_t vib_step = sv_hz_to_step(s->base_hz * powf(2.0f, vib_semi / 12.0f));

        /* PWM oscillator: compare phase fraction to pwm_width */
        float phase_norm = (float)(s->phase >> 8) / (float)(1u << 24);
        int32_t samp = (phase_norm < s->pwm_width) ? SV_WT_AMP : -SV_WT_AMP;
        s->phase += vib_step;

        int32_t out = (int32_t)(samp * env);
        if (out >  32767) out =  32767;
        if (out < -32768) out = -32768;
        out_l[i] = (int16_t)(out);
        out_r[i] = (int16_t)(out);
    }
}

static void lead_set_param(synth_inst_t *self, uint8_t id, float v)
{
    lead_t *s = (lead_t *)self;
    switch (id) {
    case 0: s->pwm_width  = v < 0.05f ? 0.05f : (v > 0.95f ? 0.95f : v); break;
    case 1: s->lfo_step   = sv_hz_to_step(v); break;  /* vibrato rate */
    case 2: s->vib_depth  = v; break;
    case P_ATK: s->adsr_t.atk = (uint32_t)(v * SAMPLE_RATE / 1000.0f); break;
    case P_DCY: s->adsr_t.dcy = (uint32_t)(v * SAMPLE_RATE / 1000.0f); break;
    case P_SUS: s->adsr_t.sus = v; break;
    case P_REL: s->adsr_t.rel = (uint32_t)(v * SAMPLE_RATE / 1000.0f); break;
    }
}

synth_inst_t *synth_lead_new(void)
{
    lead_t *s = SYNTH_ALLOC(lead_t);
    if (!s) return NULL;
    s->hdr.type_id     = 11;
    s->hdr.voice_count = 1;
    s->hdr.render      = lead_render;
    s->hdr.note_on     = lead_note_on;
    s->hdr.note_off    = lead_note_off;
    s->hdr.set_param   = lead_set_param;
    s->hdr.free        = generic_free;
    s->pwm_width = 0.5f;
    s->lfo_step  = sv_hz_to_step(5.5f);
    s->vib_depth = 0.3f;
    sv_adsr_set(&s->adsr_t, 3.0f, 50.0f, 0.8f, 200.0f);
    return &s->hdr;
}
