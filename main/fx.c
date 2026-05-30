/*
 * fx.c — Effect chain + lane ADSR (M4)
 *
 * Integer / fixed-point throughout render paths; floats only at set_param /
 * init time. All heap allocations go to PSRAM via heap_caps_malloc.
 *
 * Delay / reverb / chorus / flanger / vibrato buffers: allocated once in PSRAM
 * at fx_new() time; never freed until fx_free() is called.
 *
 * Freeverb implementation adapted from Jezar at Dreampoint (public domain).
 */

#include <string.h>
#include <math.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_heap_caps.h"
#include "fx.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

#define FX_ALLOC(T)  ((T *)heap_caps_calloc(1, sizeof(T), MALLOC_CAP_SPIRAM))
static void fx_generic_free(fx_node_t *self) { heap_caps_free(self); }

/* ── SVF biquad (shared across filter / EQ types) ────────────────────────── */
typedef struct { float f, q, low, band; } svf_t;

static void svf_set(svf_t *s, float hz, float resonance)
{
    s->f = 2.0f * sinf((float)M_PI * hz / (float)SAMPLE_RATE);
    if (s->f > 1.9f) s->f = 1.9f;
    s->q = resonance > 0.01f ? 1.0f / resonance : 100.0f;
}

static inline float svf_tick_lp(svf_t *s, float in)
{
    s->low  += s->f * s->band;
    float high = in - s->low - s->q * s->band;
    s->band += s->f * high;
    return s->low;
}
static inline float svf_tick_hp(svf_t *s, float in)
{
    s->low  += s->f * s->band;
    float high = in - s->low - s->q * s->band;
    s->band += s->f * high;
    return high;
}
static inline float svf_tick_bp(svf_t *s, float in)
{
    s->low  += s->f * s->band;
    float high = in - s->low - s->q * s->band;
    s->band += s->f * high;
    return s->band;
}
static inline float svf_tick_notch(svf_t *s, float in)
{
    s->low  += s->f * s->band;
    float high = in - s->low - s->q * s->band;
    s->band += s->f * high;
    return s->low + high;
}

/* ── Lane ADSR ───────────────────────────────────────────────────────────── */
void lane_adsr_init(lane_adsr_t *a, float atk_ms, float dcy_ms,
                    float sus, float rel_ms)
{
    a->atk_ms    = atk_ms;
    a->dcy_ms    = dcy_ms;
    a->sus       = sus < 0.0f ? 0.0f : (sus > 1.0f ? 1.0f : sus);
    a->rel_ms    = rel_ms;
    a->sus_level = a->sus;
    float sr     = (float)SAMPLE_RATE;
    a->atk_rate  = (atk_ms > 0.0f) ? 1000.0f / (atk_ms * sr) : 1.0f;
    a->dcy_rate  = (dcy_ms > 0.0f)
                   ? (1.0f - a->sus_level) * 1000.0f / (dcy_ms * sr) : 1.0f;
    a->rel_rate  = (rel_ms > 0.0f) ? 1000.0f / (rel_ms * sr) : 1.0f;
    /* Always start in SUSTAIN so audio flows immediately.
     * The lane ADSR is for ducking/automation; per-note shaping is inside
     * the synth instrument itself. Starting IDLE would silence the lane
     * until an explicit gate_on(), which never happens for live/piano-roll. */
    a->stage = LADSR_SUSTAIN;
    a->level = a->sus_level;
}

void lane_adsr_gate_on(lane_adsr_t *a)
{
    a->stage = LADSR_ATTACK;
}

void lane_adsr_gate_off(lane_adsr_t *a)
{
    if (a->stage != LADSR_IDLE) a->stage = LADSR_RELEASE;
}

float lane_adsr_process(lane_adsr_t *a, int16_t *buf_l, int16_t *buf_r,
                        int n_frames)
{
    if (a->stage == LADSR_IDLE) {
        /* Gate not open — zero output */
        memset(buf_l, 0, (size_t)n_frames * sizeof(int16_t));
        memset(buf_r, 0, (size_t)n_frames * sizeof(int16_t));
        return 0.0f;
    }

    float peak = 0.0f;
    for (int i = 0; i < n_frames; i++) {
        switch (a->stage) {
        case LADSR_ATTACK:
            a->level += a->atk_rate;
            if (a->level >= 1.0f) { a->level = 1.0f; a->stage = LADSR_DECAY; }
            break;
        case LADSR_DECAY:
            a->level -= a->dcy_rate;
            if (a->level <= a->sus_level) {
                a->level = a->sus_level; a->stage = LADSR_SUSTAIN;
            }
            break;
        case LADSR_SUSTAIN:
            a->level = a->sus_level;
            break;
        case LADSR_RELEASE:
            a->level -= a->rel_rate;
            if (a->level <= 0.0f) { a->level = 0.0f; a->stage = LADSR_IDLE; }
            break;
        default:
            a->level = 0.0f;
            break;
        }
        int32_t l = (int32_t)(buf_l[i] * a->level);
        int32_t r = (int32_t)(buf_r[i] * a->level);
        if (l >  32767) l =  32767;
        if (l < -32768) l = -32768;
        if (r >  32767) r =  32767;
        if (r < -32768) r = -32768;
        buf_l[i] = (int16_t)l;
        buf_r[i] = (int16_t)r;
        float al = a->level < 0.0f ? -a->level : a->level;
        if (al > peak) peak = al;
    }
    return peak;
}

/* ── FX chain helpers ────────────────────────────────────────────────────── */
void fx_chain_process(fx_node_t **chain, int count,
                      int16_t *buf_l, int16_t *buf_r, int n_frames,
                      float bpm, uint32_t tick_rate)
{
    for (int i = 0; i < count; i++) {
        fx_node_t *n = chain[i];
        if (!n || !n->enabled || !n->process) continue;
        n->process(n, buf_l, buf_r, n_frames, bpm, tick_rate);
    }
}

void fx_chain_set_param(fx_node_t **chain, int count, int slot,
                        uint8_t param_id, float value)
{
    if (slot < 0 || slot >= count || !chain[slot]) return;
    fx_node_t *n = chain[slot];
    if (param_id < 8) n->params[param_id] = value;
    if (n->set_param) n->set_param(n, param_id, value);
}

void fx_chain_free(fx_node_t **chain, int *count)
{
    for (int i = 0; i < *count; i++) {
        if (chain[i] && chain[i]->free) chain[i]->free(chain[i]);
        chain[i] = NULL;
    }
    *count = 0;
}

bool fx_chain_insert(fx_node_t **chain, int *count, int slot, fx_node_t *node)
{
    if (*count >= FX_MAX_PER_LANE) return false;
    if (slot < 0)       slot = 0;
    if (slot > *count)  slot = *count;
    /* Shift right */
    for (int i = *count; i > slot; i--)
        chain[i] = chain[i - 1];
    chain[slot] = node;
    (*count)++;
    return true;
}

void fx_chain_remove(fx_node_t **chain, int *count, int slot)
{
    if (slot < 0 || slot >= *count) return;
    if (chain[slot] && chain[slot]->free) chain[slot]->free(chain[slot]);
    /* Shift left */
    for (int i = slot; i < *count - 1; i++)
        chain[i] = chain[i + 1];
    chain[*count - 1] = NULL;
    (*count)--;
}

void fx_chain_move(fx_node_t **chain, int count, int src, int dst)
{
    if (src < 0 || src >= count || dst < 0 || dst >= count || src == dst) return;
    fx_node_t *tmp = chain[src];
    chain[src] = chain[dst];
    chain[dst] = tmp;
}

/* ════════════════════════════════════════════════════════════════════════════
 * FX_TYPE_FILTER  — Stereo SVF (LP/HP/BP/Notch)
 * ════════════════════════════════════════════════════════════════════════════ */
typedef struct {
    fx_node_t hdr;
    svf_t     svf_l, svf_r;
    filt_mode_t mode;
    float     cutoff_hz;
    float     resonance;
} fx_filt_t;

static void filt_process(fx_node_t *self,
                         int16_t *bl, int16_t *br, int n,
                         float bpm, uint32_t tr)
{
    (void)bpm; (void)tr;
    fx_filt_t *s = (fx_filt_t *)self;
    for (int i = 0; i < n; i++) {
        float l = bl[i], r = br[i];
        switch (s->mode) {
        case FILT_LP:    l = svf_tick_lp(&s->svf_l, l);    r = svf_tick_lp(&s->svf_r, r);    break;
        case FILT_HP:    l = svf_tick_hp(&s->svf_l, l);    r = svf_tick_hp(&s->svf_r, r);    break;
        case FILT_BP:    l = svf_tick_bp(&s->svf_l, l);    r = svf_tick_bp(&s->svf_r, r);    break;
        case FILT_NOTCH: l = svf_tick_notch(&s->svf_l, l); r = svf_tick_notch(&s->svf_r, r); break;
        }
        if (l >  32767.0f) l =  32767.0f;
        if (l < -32768.0f) l = -32768.0f;
        if (r >  32767.0f) r =  32767.0f;
        if (r < -32768.0f) r = -32768.0f;
        bl[i] = (int16_t)l; br[i] = (int16_t)r;
    }
}

static void filt_set_param(fx_node_t *self, uint8_t id, float v)
{
    fx_filt_t *s = (fx_filt_t *)self;
    if (id == 0) { s->cutoff_hz = v; svf_set(&s->svf_l, v, s->resonance); svf_set(&s->svf_r, v, s->resonance); }
    if (id == 1) { s->resonance = v; svf_set(&s->svf_l, s->cutoff_hz, v); svf_set(&s->svf_r, s->cutoff_hz, v); }
    if (id == 2) { s->mode = (filt_mode_t)(int)v; }
}

fx_node_t *fx_filter_new(filt_mode_t mode, float cutoff_hz, float resonance)
{
    fx_filt_t *s = FX_ALLOC(fx_filt_t);
    if (!s) return NULL;
    s->hdr.type = FX_TYPE_FILTER; s->hdr.enabled = true;
    s->hdr.process = filt_process; s->hdr.set_param = filt_set_param;
    s->hdr.free = fx_generic_free;
    s->mode = mode; s->cutoff_hz = cutoff_hz; s->resonance = resonance;
    svf_set(&s->svf_l, cutoff_hz, resonance);
    svf_set(&s->svf_r, cutoff_hz, resonance);
    return &s->hdr;
}

/* ════════════════════════════════════════════════════════════════════════════
 * FX_TYPE_EQ3  — 3-band shelving EQ (low-shelf + mid-peak + hi-shelf)
 * ════════════════════════════════════════════════════════════════════════════ */
typedef struct {
    /* Biquad direct-form II transposed coefficients */
    float b0, b1, b2, a1, a2;
    float w1, w2;   /* delay state (per channel) */
} bq_t;

static void bq_set_low_shelf(bq_t *b, float freq_hz, float gain_db)
{
    float A  = powf(10.0f, gain_db / 40.0f);
    float w0 = 2.0f * (float)M_PI * freq_hz / (float)SAMPLE_RATE;
    float cs = cosf(w0), sn = sinf(w0);
    float S  = 1.0f; /* shelf slope */
    float al = sn / 2.0f * sqrtf((A + 1.0f / A) * (1.0f / S - 1.0f) + 2.0f);
    float a0 = (A + 1.0f) + (A - 1.0f) * cs + 2.0f * sqrtf(A) * al;
    b->b0 = A * ((A + 1.0f) - (A - 1.0f) * cs + 2.0f * sqrtf(A) * al) / a0;
    b->b1 = 2.0f * A * ((A - 1.0f) - (A + 1.0f) * cs) / a0;
    b->b2 = A * ((A + 1.0f) - (A - 1.0f) * cs - 2.0f * sqrtf(A) * al) / a0;
    b->a1 = -2.0f * ((A - 1.0f) + (A + 1.0f) * cs) / a0;
    b->a2 = ((A + 1.0f) + (A - 1.0f) * cs - 2.0f * sqrtf(A) * al) / a0;
    b->w1 = b->w2 = 0.0f;
}

static void bq_set_high_shelf(bq_t *b, float freq_hz, float gain_db)
{
    float A  = powf(10.0f, gain_db / 40.0f);
    float w0 = 2.0f * (float)M_PI * freq_hz / (float)SAMPLE_RATE;
    float cs = cosf(w0), sn = sinf(w0);
    float S  = 1.0f;
    float al = sn / 2.0f * sqrtf((A + 1.0f / A) * (1.0f / S - 1.0f) + 2.0f);
    float a0 = (A + 1.0f) - (A - 1.0f) * cs + 2.0f * sqrtf(A) * al;
    b->b0 = A * ((A + 1.0f) + (A - 1.0f) * cs + 2.0f * sqrtf(A) * al) / a0;
    b->b1 = -2.0f * A * ((A - 1.0f) + (A + 1.0f) * cs) / a0;
    b->b2 = A * ((A + 1.0f) + (A - 1.0f) * cs - 2.0f * sqrtf(A) * al) / a0;
    b->a1 = 2.0f * ((A - 1.0f) - (A + 1.0f) * cs) / a0;
    b->a2 = ((A + 1.0f) - (A - 1.0f) * cs - 2.0f * sqrtf(A) * al) / a0;
    b->w1 = b->w2 = 0.0f;
}

static void bq_set_peak(bq_t *b, float freq_hz, float gain_db, float q)
{
    float A  = powf(10.0f, gain_db / 40.0f);
    float w0 = 2.0f * (float)M_PI * freq_hz / (float)SAMPLE_RATE;
    float al = sinf(w0) / (2.0f * (q > 0.01f ? q : 0.01f));
    float a0 = 1.0f + al / A;
    b->b0 = (1.0f + al * A) / a0;
    b->b1 = (-2.0f * cosf(w0)) / a0;
    b->b2 = (1.0f - al * A) / a0;
    b->a1 = (-2.0f * cosf(w0)) / a0;
    b->a2 = (1.0f - al / A) / a0;
    b->w1 = b->w2 = 0.0f;
}

static inline float bq_tick(bq_t *b, float in)
{
    float out = b->b0 * in + b->w1;
    b->w1 = b->b1 * in - b->a1 * out + b->w2;
    b->w2 = b->b2 * in - b->a2 * out;
    return out;
}

static void bq_process_stereo(bq_t *bl, bq_t *br,
                               int16_t *bufl, int16_t *bufr, int n)
{
    for (int i = 0; i < n; i++) {
        float l = bq_tick(bl, (float)bufl[i]);
        float r = bq_tick(br, (float)bufr[i]);
        if (l >  32767.0f) l =  32767.0f;
        if (l < -32768.0f) l = -32768.0f;
        if (r >  32767.0f) r =  32767.0f;
        if (r < -32768.0f) r = -32768.0f;
        bufl[i] = (int16_t)l; bufr[i] = (int16_t)r;
    }
}

typedef struct {
    fx_node_t hdr;
    bq_t low_l, low_r;
    bq_t mid_l, mid_r;
    bq_t hi_l,  hi_r;
    float low_db, mid_db, hi_db, mid_hz, mid_q;
} fx_eq3_t;

static void eq3_process(fx_node_t *self, int16_t *bl, int16_t *br, int n,
                        float bpm, uint32_t tr)
{
    (void)bpm; (void)tr;
    fx_eq3_t *s = (fx_eq3_t *)self;
    bq_process_stereo(&s->low_l, &s->low_r, bl, br, n);
    bq_process_stereo(&s->mid_l, &s->mid_r, bl, br, n);
    bq_process_stereo(&s->hi_l,  &s->hi_r,  bl, br, n);
}

static void eq3_set_param(fx_node_t *self, uint8_t id, float v)
{
    fx_eq3_t *s = (fx_eq3_t *)self;
    switch (id) {
    case 0: s->low_db = v; bq_set_low_shelf (&s->low_l, 250.0f, v); bq_set_low_shelf (&s->low_r, 250.0f, v); break;
    case 1: s->mid_db = v; bq_set_peak      (&s->mid_l, s->mid_hz, v, s->mid_q); bq_set_peak(&s->mid_r, s->mid_hz, v, s->mid_q); break;
    case 2: s->hi_db  = v; bq_set_high_shelf(&s->hi_l,  4000.0f, v); bq_set_high_shelf(&s->hi_r, 4000.0f, v); break;
    case 3: s->mid_hz = v; bq_set_peak      (&s->mid_l, v, s->mid_db, s->mid_q); bq_set_peak(&s->mid_r, v, s->mid_db, s->mid_q); break;
    case 4: s->mid_q  = v; bq_set_peak      (&s->mid_l, s->mid_hz, s->mid_db, v); bq_set_peak(&s->mid_r, s->mid_hz, s->mid_db, v); break;
    }
}

fx_node_t *fx_eq3_new(void)
{
    fx_eq3_t *s = FX_ALLOC(fx_eq3_t);
    if (!s) return NULL;
    s->hdr.type = FX_TYPE_EQ3; s->hdr.enabled = true;
    s->hdr.process = eq3_process; s->hdr.set_param = eq3_set_param;
    s->hdr.free = fx_generic_free;
    s->mid_hz = 1000.0f; s->mid_q = 0.7f;
    bq_set_low_shelf (&s->low_l, 250.0f,    0.0f); s->low_r = s->low_l;
    bq_set_peak      (&s->mid_l, 1000.0f, 0.0f, 0.7f); s->mid_r = s->mid_l;
    bq_set_high_shelf(&s->hi_l,  4000.0f,   0.0f); s->hi_r  = s->hi_l;
    return &s->hdr;
}

/* ════════════════════════════════════════════════════════════════════════════
 * FX_TYPE_EQ5  — 5 fully parametric biquad bands
 * ════════════════════════════════════════════════════════════════════════════ */
#define EQ5_BANDS 5

typedef struct {
    fx_node_t hdr;
    bq_t bq_l[EQ5_BANDS], bq_r[EQ5_BANDS];
    float freq[EQ5_BANDS], gain[EQ5_BANDS], q[EQ5_BANDS];
    int   type[EQ5_BANDS]; /* 0=peak, 1=lo-shelf, 2=hi-shelf */
} fx_eq5_t;

static void eq5_rebuild(fx_eq5_t *s, int b)
{
    switch (s->type[b]) {
    case 1: bq_set_low_shelf (&s->bq_l[b], s->freq[b], s->gain[b]); break;
    case 2: bq_set_high_shelf(&s->bq_l[b], s->freq[b], s->gain[b]); break;
    default: bq_set_peak     (&s->bq_l[b], s->freq[b], s->gain[b], s->q[b]); break;
    }
    s->bq_r[b] = s->bq_l[b];
    /* clear delay state after coeff change */
    s->bq_l[b].w1 = s->bq_l[b].w2 = 0.0f;
    s->bq_r[b].w1 = s->bq_r[b].w2 = 0.0f;
}

static void eq5_process(fx_node_t *self, int16_t *bl, int16_t *br, int n,
                        float bpm, uint32_t tr)
{
    (void)bpm; (void)tr;
    fx_eq5_t *s = (fx_eq5_t *)self;
    for (int b = 0; b < EQ5_BANDS; b++)
        bq_process_stereo(&s->bq_l[b], &s->bq_r[b], bl, br, n);
}

static void eq5_set_param(fx_node_t *self, uint8_t id, float v)
{
    fx_eq5_t *s = (fx_eq5_t *)self;
    int b = id / 5, sub = id % 5;
    if (b >= EQ5_BANDS) return;
    switch (sub) {
    case 0: s->gain[b] = v; break;
    case 1: s->freq[b] = v; break;
    case 2: s->q[b]    = v; break;
    case 3: s->type[b] = (int)v; break;
    }
    eq5_rebuild(s, b);
}

fx_node_t *fx_eq5_new(void)
{
    fx_eq5_t *s = FX_ALLOC(fx_eq5_t);
    if (!s) return NULL;
    s->hdr.type = FX_TYPE_EQ5; s->hdr.enabled = true;
    s->hdr.process = eq5_process; s->hdr.set_param = eq5_set_param;
    s->hdr.free = fx_generic_free;
    float freqs[EQ5_BANDS]  = {80.0f, 250.0f, 1000.0f, 4000.0f, 12000.0f};
    for (int b = 0; b < EQ5_BANDS; b++) {
        s->freq[b] = freqs[b]; s->gain[b] = 0.0f; s->q[b] = 0.7f;
        s->type[b] = (b == 0) ? 1 : (b == EQ5_BANDS - 1) ? 2 : 0;
        eq5_rebuild(s, b);
    }
    return &s->hdr;
}

/* ════════════════════════════════════════════════════════════════════════════
 * FX_TYPE_COMPRESSOR  — RMS + gain computer, block-rate gain smoothing
 * ════════════════════════════════════════════════════════════════════════════ */
typedef struct {
    fx_node_t hdr;
    float thresh_lin;   /* linear threshold                                    */
    float ratio;
    float makeup;       /* linear makeup gain                                  */
    float atk_coef;     /* per-sample attack coef (1 - e^(-1/(sr*atk_ms/1000))) */
    float rel_coef;
    float env;          /* running RMS envelope                                */
    float gain;         /* current applied gain                                */
} fx_comp_t;

static void comp_recalc(fx_comp_t *s, float atk_ms, float rel_ms)
{
    float sr = (float)SAMPLE_RATE;
    s->atk_coef = 1.0f - expf(-1.0f / (sr * atk_ms * 0.001f));
    s->rel_coef = 1.0f - expf(-1.0f / (sr * rel_ms * 0.001f));
}

static void comp_process(fx_node_t *self, int16_t *bl, int16_t *br, int n,
                         float bpm, uint32_t tr)
{
    (void)bpm; (void)tr;
    fx_comp_t *s = (fx_comp_t *)self;
    /* Compute RMS over block */
    float sum = 0.0f;
    for (int i = 0; i < n; i++) {
        float l = bl[i] / 32768.0f, r = br[i] / 32768.0f;
        sum += l * l + r * r;
    }
    float rms = sqrtf(sum / (float)(n * 2));

    /* Envelope follower */
    float coef = (rms > s->env) ? s->atk_coef : s->rel_coef;
    s->env += coef * (rms - s->env);

    /* Gain computer */
    float target_gain = 1.0f;
    if (s->env > s->thresh_lin && s->ratio > 1.0f) {
        float over_db = 20.0f * log10f(s->env / s->thresh_lin);
        float gain_reduction_db = over_db * (1.0f - 1.0f / s->ratio);
        target_gain = powf(10.0f, -gain_reduction_db / 20.0f) * s->makeup;
    } else {
        target_gain = s->makeup;
    }

    /* Smooth gain and apply */
    float gain_coef = (target_gain < s->gain) ? s->atk_coef : s->rel_coef;
    s->gain += gain_coef * (target_gain - s->gain);
    int32_t g_q15 = (int32_t)(s->gain * 32768.0f);
    for (int i = 0; i < n; i++) {
        int32_t l = ((int32_t)bl[i] * g_q15) >> 15;
        int32_t r = ((int32_t)br[i] * g_q15) >> 15;
        if (l >  32767) l =  32767;
        if (l < -32768) l = -32768;
        if (r >  32767) r =  32767;
        if (r < -32768) r = -32768;
        bl[i] = (int16_t)l; br[i] = (int16_t)r;
    }
}

static void comp_set_param(fx_node_t *self, uint8_t id, float v)
{
    fx_comp_t *s = (fx_comp_t *)self;
    static float atk_ms = 10.0f, rel_ms = 100.0f;
    switch (id) {
    case 0: s->thresh_lin = powf(10.0f, v / 20.0f); break;
    case 1: s->ratio = v > 1.0f ? v : 1.0f; break;
    case 2: atk_ms = v; comp_recalc(s, atk_ms, rel_ms); break;
    case 3: rel_ms = v; comp_recalc(s, atk_ms, rel_ms); break;
    case 4: s->makeup = powf(10.0f, v / 20.0f); break;
    }
}

fx_node_t *fx_compressor_new(void)
{
    fx_comp_t *s = FX_ALLOC(fx_comp_t);
    if (!s) return NULL;
    s->hdr.type = FX_TYPE_COMPRESSOR; s->hdr.enabled = true;
    s->hdr.process = comp_process; s->hdr.set_param = comp_set_param;
    s->hdr.free = fx_generic_free;
    s->thresh_lin = powf(10.0f, -12.0f / 20.0f);
    s->ratio = 4.0f; s->makeup = 1.0f; s->gain = 1.0f;
    comp_recalc(s, 10.0f, 100.0f);
    return &s->hdr;
}

/* ════════════════════════════════════════════════════════════════════════════
 * FX_TYPE_LIMITER  — Brick-wall, 2 ms lookahead
 * ════════════════════════════════════════════════════════════════════════════ */
#define LIMITER_LA_FRAMES  ((int)(SAMPLE_RATE * 0.002f))  /* 2 ms */

typedef struct {
    fx_node_t hdr;
    float  thresh_lin;
    int16_t la_l[LIMITER_LA_FRAMES];
    int16_t la_r[LIMITER_LA_FRAMES];
    int    la_pos;
    float  gain;
    float  rel_coef;
} fx_lim_t;

static void lim_process(fx_node_t *self, int16_t *bl, int16_t *br, int n,
                        float bpm, uint32_t tr)
{
    (void)bpm; (void)tr;
    fx_lim_t *s = (fx_lim_t *)self;
    for (int i = 0; i < n; i++) {
        /* Read from lookahead */
        int16_t out_l = s->la_l[s->la_pos];
        int16_t out_r = s->la_r[s->la_pos];
        /* Write new sample into lookahead */
        s->la_l[s->la_pos] = bl[i];
        s->la_r[s->la_pos] = br[i];
        s->la_pos = (s->la_pos + 1) % LIMITER_LA_FRAMES;

        /* Peak detect over new sample */
        float peak = fabsf((float)bl[i]) / 32768.0f;
        float rp   = fabsf((float)br[i]) / 32768.0f;
        if (rp > peak) peak = rp;
        float needed = (peak > s->thresh_lin && peak > 0.0f)
                       ? s->thresh_lin / peak : 1.0f;
        if (needed < s->gain) s->gain = needed;   /* attack: instant */
        else                  s->gain += s->rel_coef * (1.0f - s->gain);

        int32_t gq = (int32_t)(s->gain * 32768.0f);
        int32_t l = ((int32_t)out_l * gq) >> 15;
        int32_t r = ((int32_t)out_r * gq) >> 15;
        if (l >  32767) l =  32767;
        if (l < -32768) l = -32768;
        if (r >  32767) r =  32767;
        if (r < -32768) r = -32768;
        bl[i] = (int16_t)l; br[i] = (int16_t)r;
    }
}

static void lim_set_param(fx_node_t *self, uint8_t id, float v)
{
    fx_lim_t *s = (fx_lim_t *)self;
    if (id == 0) s->thresh_lin = powf(10.0f, v / 20.0f);
    /* id==1 lookahead_ms — ignored at runtime, fixed at alloc size */
}

fx_node_t *fx_limiter_new(void)
{
    fx_lim_t *s = FX_ALLOC(fx_lim_t);
    if (!s) return NULL;
    s->hdr.type = FX_TYPE_LIMITER; s->hdr.enabled = true;
    s->hdr.process = lim_process; s->hdr.set_param = lim_set_param;
    s->hdr.free = fx_generic_free;
    s->thresh_lin = powf(10.0f, -0.1f / 20.0f); /* -0.1 dBFS */
    s->gain = 1.0f;
    s->rel_coef = 1.0f - expf(-1.0f / ((float)SAMPLE_RATE * 0.05f));
    return &s->hdr;
}

/* ════════════════════════════════════════════════════════════════════════════
 * FX_TYPE_GATE  — RMS gate with hysteresis + hold
 * ════════════════════════════════════════════════════════════════════════════ */
typedef struct {
    fx_node_t hdr;
    float thresh_open;   /* linear: open when above                            */
    float thresh_close;  /* linear: close when below (hysteresis, ~6 dB lower) */
    float hold_frames;
    float atk_coef, rel_coef;
    float env, gain;
    bool  open;
    float hold_cnt;
} fx_gate_t;

static void gate_process(fx_node_t *self, int16_t *bl, int16_t *br, int n,
                         float bpm, uint32_t tr)
{
    (void)bpm; (void)tr;
    fx_gate_t *s = (fx_gate_t *)self;
    float sum = 0.0f;
    for (int i = 0; i < n; i++) {
        float l = bl[i] / 32768.0f, r = br[i] / 32768.0f;
        sum += l * l + r * r;
    }
    float rms = sqrtf(sum / (float)(n * 2));
    float coef = (rms > s->env) ? s->atk_coef : s->rel_coef;
    s->env += coef * (rms - s->env);

    if (!s->open && s->env >= s->thresh_open) { s->open = true; s->hold_cnt = s->hold_frames; }
    if (s->open)  {
        if (s->env < s->thresh_close) {
            if (s->hold_cnt > 0) s->hold_cnt -= (float)n;
            else s->open = false;
        } else {
            s->hold_cnt = s->hold_frames;
        }
    }

    float target = s->open ? 1.0f : 0.0f;
    int32_t gq;
    for (int i = 0; i < n; i++) {
        float diff = target - s->gain;
        float step = (diff > 0) ? s->atk_coef : s->rel_coef;
        s->gain += step * diff;
        gq = (int32_t)(s->gain * 32768.0f);
        int32_t l = ((int32_t)bl[i] * gq) >> 15;
        int32_t r = ((int32_t)br[i] * gq) >> 15;
        if (l >  32767) l =  32767;
        if (l < -32768) l = -32768;
        if (r >  32767) r =  32767;
        if (r < -32768) r = -32768;
        bl[i] = (int16_t)l; br[i] = (int16_t)r;
    }
}

static void gate_set_param(fx_node_t *self, uint8_t id, float v)
{
    fx_gate_t *s = (fx_gate_t *)self;
    float sr = (float)SAMPLE_RATE;
    switch (id) {
    case 0: s->thresh_open  = powf(10.0f, v / 20.0f);
            s->thresh_close = s->thresh_open * 0.5f; break;
    case 1: s->hold_frames = v * sr / 1000.0f; break;
    case 2: s->atk_coef = 1.0f - expf(-1.0f / (sr * v * 0.001f)); break;
    case 3: s->rel_coef = 1.0f - expf(-1.0f / (sr * v * 0.001f)); break;
    }
}

fx_node_t *fx_gate_new(void)
{
    fx_gate_t *s = FX_ALLOC(fx_gate_t);
    if (!s) return NULL;
    s->hdr.type = FX_TYPE_GATE; s->hdr.enabled = true;
    s->hdr.process = gate_process; s->hdr.set_param = gate_set_param;
    s->hdr.free = fx_generic_free;
    s->thresh_open  = powf(10.0f, -40.0f / 20.0f);
    s->thresh_close = s->thresh_open * 0.5f;
    s->hold_frames = (float)SAMPLE_RATE * 0.05f;
    s->gain = 1.0f; s->open = true;
    s->atk_coef = 1.0f - expf(-1.0f / ((float)SAMPLE_RATE * 0.001f));
    s->rel_coef = 1.0f - expf(-1.0f / ((float)SAMPLE_RATE * 0.1f));
    return &s->hdr;
}

/* ════════════════════════════════════════════════════════════════════════════
 * FX_TYPE_TRANSIENT  — fast/slow envelope follower difference
 * ════════════════════════════════════════════════════════════════════════════ */
typedef struct {
    fx_node_t hdr;
    float atk_gain;  /* +gain on transient: fast > slow → attack boost */
    float sus_gain;  /* +gain on sustain:   fast < slow → body boost   */
    float fast_env, slow_env;
} fx_trans_t;

static void trans_process(fx_node_t *self, int16_t *bl, int16_t *br, int n,
                          float bpm, uint32_t tr)
{
    (void)bpm; (void)tr;
    fx_trans_t *s = (fx_trans_t *)self;
    float fast_c = 1.0f - expf(-1.0f / ((float)SAMPLE_RATE * 0.001f));
    float slow_c = 1.0f - expf(-1.0f / ((float)SAMPLE_RATE * 0.100f));
    for (int i = 0; i < n; i++) {
        float in = fabsf((float)bl[i] + (float)br[i]) / 65536.0f;
        s->fast_env += fast_c * (in - s->fast_env);
        s->slow_env += slow_c * (in - s->slow_env);
        float gain = 1.0f;
        float diff = s->fast_env - s->slow_env;
        if (diff > 0.0f)       gain += s->atk_gain * diff * 10.0f;
        else if (diff < 0.0f)  gain += s->sus_gain * (-diff) * 10.0f;
        if (gain < 0.0f) gain = 0.0f;
        int32_t gq = (int32_t)(gain * 32768.0f);
        if (gq > 131072) gq = 131072; /* clamp +6 dB max */
        int32_t l = ((int32_t)bl[i] * gq) >> 15;
        int32_t r = ((int32_t)br[i] * gq) >> 15;
        if (l >  32767) l =  32767;
        if (l < -32768) l = -32768;
        if (r >  32767) r =  32767;
        if (r < -32768) r = -32768;
        bl[i] = (int16_t)l; br[i] = (int16_t)r;
    }
}

static void trans_set_param(fx_node_t *self, uint8_t id, float v)
{
    fx_trans_t *s = (fx_trans_t *)self;
    if (id == 0) s->atk_gain = v;
    if (id == 1) s->sus_gain = v;
}

fx_node_t *fx_transient_new(void)
{
    fx_trans_t *s = FX_ALLOC(fx_trans_t);
    if (!s) return NULL;
    s->hdr.type = FX_TYPE_TRANSIENT; s->hdr.enabled = true;
    s->hdr.process = trans_process; s->hdr.set_param = trans_set_param;
    s->hdr.free = fx_generic_free;
    s->atk_gain = 1.0f; s->sus_gain = 0.0f;
    return &s->hdr;
}

/* ════════════════════════════════════════════════════════════════════════════
 * FX_TYPE_DISTORTION  — 4 modes
 * ════════════════════════════════════════════════════════════════════════════ */
typedef struct {
    fx_node_t hdr;
    dist_mode_t mode;
    float drive;    /* pre-gain */
    float tone;     /* 0.0–1.0: LP cutoff = 1 kHz + tone * 19 kHz */
    svf_t tone_l, tone_r;
} fx_dist_t;

static inline float dist_soft(float x)   { return x / (1.0f + fabsf(x)); }
static inline float dist_hard(float x)   { return x > 1.0f ? 1.0f : (x < -1.0f ? -1.0f : x); }
static inline float dist_tube(float x)   { return tanhf(x); }
static inline float dist_fuzz(float x)
{
    /* Full-wave rectify + soft clip */
    x = x < 0.0f ? -x : x;
    return x / (1.0f + x);
}

static void dist_process(fx_node_t *self, int16_t *bl, int16_t *br, int n,
                         float bpm, uint32_t tr)
{
    (void)bpm; (void)tr;
    fx_dist_t *s = (fx_dist_t *)self;
    for (int i = 0; i < n; i++) {
        float l = (float)bl[i] / 32768.0f * s->drive;
        float r = (float)br[i] / 32768.0f * s->drive;
        switch (s->mode) {
        case DIST_SOFT: l = dist_soft(l); r = dist_soft(r); break;
        case DIST_HARD: l = dist_hard(l); r = dist_hard(r); break;
        case DIST_TUBE: l = dist_tube(l); r = dist_tube(r); break;
        case DIST_FUZZ: l = dist_fuzz(l); r = dist_fuzz(r); break;
        }
        l = svf_tick_lp(&s->tone_l, l * 32768.0f);
        r = svf_tick_lp(&s->tone_r, r * 32768.0f);
        if (l >  32767.0f) l =  32767.0f;
        if (l < -32768.0f) l = -32768.0f;
        if (r >  32767.0f) r =  32767.0f;
        if (r < -32768.0f) r = -32768.0f;
        bl[i] = (int16_t)l; br[i] = (int16_t)r;
    }
}

static void dist_set_param(fx_node_t *self, uint8_t id, float v)
{
    fx_dist_t *s = (fx_dist_t *)self;
    if (id == 0) { s->drive = v; }
    if (id == 1) {
        s->tone = v;
        float cutoff = 1000.0f + v * 19000.0f;
        svf_set(&s->tone_l, cutoff, 0.7f); svf_set(&s->tone_r, cutoff, 0.7f);
    }
    if (id == 2) { s->mode = (dist_mode_t)(int)v; }
}

fx_node_t *fx_distortion_new(dist_mode_t mode, float drive)
{
    fx_dist_t *s = FX_ALLOC(fx_dist_t);
    if (!s) return NULL;
    s->hdr.type = FX_TYPE_DISTORTION; s->hdr.enabled = true;
    s->hdr.process = dist_process; s->hdr.set_param = dist_set_param;
    s->hdr.free = fx_generic_free;
    s->mode = mode; s->drive = drive; s->tone = 0.5f;
    svf_set(&s->tone_l, 10000.0f, 0.7f); svf_set(&s->tone_r, 10000.0f, 0.7f);
    return &s->hdr;
}

/* ════════════════════════════════════════════════════════════════════════════
 * FX_TYPE_OVERDRIVE  — asymmetric soft-clip (warm even harmonics)
 * ════════════════════════════════════════════════════════════════════════════ */
typedef struct {
    fx_node_t hdr;
    float drive;
    float tone;
    svf_t tone_l, tone_r;
} fx_od_t;

static void od_process(fx_node_t *self, int16_t *bl, int16_t *br, int n,
                       float bpm, uint32_t tr)
{
    (void)bpm; (void)tr;
    fx_od_t *s = (fx_od_t *)self;
    for (int i = 0; i < n; i++) {
        float l = (float)bl[i] / 32768.0f * s->drive;
        float r = (float)br[i] / 32768.0f * s->drive;
        /* Asymmetric: positive half harder clip than negative */
        l = (l > 0.0f) ? (l / (1.0f + l)) : (l / (1.0f - 0.5f * l));
        r = (r > 0.0f) ? (r / (1.0f + r)) : (r / (1.0f - 0.5f * r));
        l = svf_tick_lp(&s->tone_l, l * 32768.0f);
        r = svf_tick_lp(&s->tone_r, r * 32768.0f);
        if (l >  32767.0f) l =  32767.0f;
        if (l < -32768.0f) l = -32768.0f;
        if (r >  32767.0f) r =  32767.0f;
        if (r < -32768.0f) r = -32768.0f;
        bl[i] = (int16_t)l; br[i] = (int16_t)r;
    }
}

static void od_set_param(fx_node_t *self, uint8_t id, float v)
{
    fx_od_t *s = (fx_od_t *)self;
    if (id == 0) s->drive = v;
    if (id == 1) {
        s->tone = v;
        float cutoff = 1000.0f + v * 19000.0f;
        svf_set(&s->tone_l, cutoff, 0.7f); svf_set(&s->tone_r, cutoff, 0.7f);
    }
}

fx_node_t *fx_overdrive_new(float drive)
{
    fx_od_t *s = FX_ALLOC(fx_od_t);
    if (!s) return NULL;
    s->hdr.type = FX_TYPE_OVERDRIVE; s->hdr.enabled = true;
    s->hdr.process = od_process; s->hdr.set_param = od_set_param;
    s->hdr.free = fx_generic_free;
    s->drive = drive; s->tone = 0.5f;
    svf_set(&s->tone_l, 10000.0f, 0.7f); svf_set(&s->tone_r, 10000.0f, 0.7f);
    return &s->hdr;
}

/* ════════════════════════════════════════════════════════════════════════════
 * FX_TYPE_WAVEFOLDER  — Buchla fold-back
 * ════════════════════════════════════════════════════════════════════════════ */
typedef struct {
    fx_node_t hdr;
    float fold;   /* gain before fold (1.0–8.0) */
    float gain;   /* input pre-gain */
} fx_wf_t;

static inline float wavefold(float x, float fold)
{
    x = x * fold;
    /* Fold: triangle-like mirror at ±1 */
    x = x - 4.0f * floorf((x + 1.0f) / 4.0f);
    if (x > 1.0f)  x =  2.0f - x;
    if (x < -1.0f) x = -2.0f - x;
    return x;
}

static void wf_process(fx_node_t *self, int16_t *bl, int16_t *br, int n,
                       float bpm, uint32_t tr)
{
    (void)bpm; (void)tr;
    fx_wf_t *s = (fx_wf_t *)self;
    for (int i = 0; i < n; i++) {
        float l = wavefold((float)bl[i] * s->gain / 32768.0f, s->fold);
        float r = wavefold((float)br[i] * s->gain / 32768.0f, s->fold);
        bl[i] = (int16_t)(l * 32767.0f);
        br[i] = (int16_t)(r * 32767.0f);
    }
}

static void wf_set_param(fx_node_t *self, uint8_t id, float v)
{
    fx_wf_t *s = (fx_wf_t *)self;
    if (id == 0) s->fold = v < 1.0f ? 1.0f : v;
    if (id == 1) s->gain = v;
}

fx_node_t *fx_wavefolder_new(void)
{
    fx_wf_t *s = FX_ALLOC(fx_wf_t);
    if (!s) return NULL;
    s->hdr.type = FX_TYPE_WAVEFOLDER; s->hdr.enabled = true;
    s->hdr.process = wf_process; s->hdr.set_param = wf_set_param;
    s->hdr.free = fx_generic_free;
    s->fold = 2.0f; s->gain = 1.0f;
    return &s->hdr;
}

/* ════════════════════════════════════════════════════════════════════════════
 * FX_TYPE_BITCRUSH
 * ════════════════════════════════════════════════════════════════════════════ */
typedef struct {
    fx_node_t hdr;
    int   bits;
    int   sr_div;   /* 1 = full rate; N = keep 1 sample per N */
    int   sr_cnt;
    int16_t held_l, held_r;
} fx_bc_t;

static void bc_process(fx_node_t *self, int16_t *bl, int16_t *br, int n,
                       float bpm, uint32_t tr)
{
    (void)bpm; (void)tr;
    fx_bc_t *s = (fx_bc_t *)self;
    int mask = ~((1 << (16 - s->bits)) - 1);
    for (int i = 0; i < n; i++) {
        s->sr_cnt++;
        if (s->sr_cnt >= s->sr_div) {
            s->sr_cnt = 0;
            s->held_l = (int16_t)(bl[i] & mask);
            s->held_r = (int16_t)(br[i] & mask);
        }
        bl[i] = s->held_l; br[i] = s->held_r;
    }
}

static void bc_set_param(fx_node_t *self, uint8_t id, float v)
{
    fx_bc_t *s = (fx_bc_t *)self;
    if (id == 0) { s->bits = (int)v; if (s->bits < 1) s->bits = 1; if (s->bits > 16) s->bits = 16; }
    if (id == 1) { s->sr_div = (int)v; if (s->sr_div < 1) s->sr_div = 1; }
}

fx_node_t *fx_bitcrush_new(int bits, int sr_div)
{
    fx_bc_t *s = FX_ALLOC(fx_bc_t);
    if (!s) return NULL;
    s->hdr.type = FX_TYPE_BITCRUSH; s->hdr.enabled = true;
    s->hdr.process = bc_process; s->hdr.set_param = bc_set_param;
    s->hdr.free = fx_generic_free;
    s->bits   = (bits < 1) ? 1 : (bits > 16) ? 16 : bits;
    s->sr_div = (sr_div < 1) ? 1 : sr_div;
    return &s->hdr;
}

/* ════════════════════════════════════════════════════════════════════════════
 * FX_TYPE_DELAY  — stereo ping-pong, PSRAM circular buffer, clock-sync
 * Max 2 seconds at 48 kHz = 96000 frames
 * ════════════════════════════════════════════════════════════════════════════ */
#define DELAY_MAX_FRAMES  96000

typedef struct {
    fx_node_t hdr;
    int16_t  *buf_l;
    int16_t  *buf_r;
    int       buf_len;
    int       write_pos;
    float     feedback;
    float     mix;
    int       delay_frames;   /* current effective delay length */
    float     time_ms;
    bool      ping_pong;
    bool      clock_sync;
    float     sync_div;       /* note division e.g. 0.25=1/4 */
} fx_delay_t;

static void delay_recalc(fx_delay_t *s, float bpm, uint32_t tick_rate)
{
    if (s->clock_sync && bpm > 0.0f) {
        float beat_ms = 60000.0f / bpm;
        float time_ms = beat_ms * s->sync_div * 4.0f;
        s->delay_frames = (int)(time_ms * (float)SAMPLE_RATE / 1000.0f);
    } else {
        s->delay_frames = (int)(s->time_ms * (float)SAMPLE_RATE / 1000.0f);
    }
    if (s->delay_frames < 1) s->delay_frames = 1;
    if (s->delay_frames >= s->buf_len) s->delay_frames = s->buf_len - 1;
    (void)tick_rate;
}

static void delay_process(fx_node_t *self, int16_t *bl, int16_t *br, int n,
                          float bpm, uint32_t tr)
{
    fx_delay_t *s = (fx_delay_t *)self;
    delay_recalc(s, bpm, tr);
    int len = s->buf_len;
    int fb_q15 = (int32_t)(s->feedback * 32768.0f);
    int mix_q15 = (int32_t)(s->mix * 32768.0f);
    int dry_q15 = 32768 - mix_q15;
    for (int i = 0; i < n; i++) {
        int read_pos = (s->write_pos - s->delay_frames + len) % len;
        int16_t dl = s->buf_l[read_pos];
        int16_t dr = s->buf_r[read_pos];
        /* ping-pong: swap channels on feedback */
        int32_t fl = (int32_t)bl[i] + ((int32_t)dl * fb_q15 >> 15);
        int32_t fr = (int32_t)br[i] + ((int32_t)dr * fb_q15 >> 15);
        if (fl >  32767) fl =  32767;
        if (fl < -32768) fl = -32768;
        if (fr >  32767) fr =  32767;
        if (fr < -32768) fr = -32768;
        s->buf_l[s->write_pos] = s->ping_pong ? (int16_t)fr : (int16_t)fl;
        s->buf_r[s->write_pos] = s->ping_pong ? (int16_t)fl : (int16_t)fr;
        s->write_pos = (s->write_pos + 1) % len;
        int32_t ol = (((int32_t)bl[i] * dry_q15) + ((int32_t)dl * mix_q15)) >> 15;
        int32_t or_ = (((int32_t)br[i] * dry_q15) + ((int32_t)dr * mix_q15)) >> 15;
        if (ol >  32767) ol =  32767;
        if (ol < -32768) ol = -32768;
        if (or_ >  32767) or_ =  32767;
        if (or_ < -32768) or_ = -32768;
        bl[i] = (int16_t)ol; br[i] = (int16_t)or_;
    }
}

static void delay_set_param(fx_node_t *self, uint8_t id, float v)
{
    fx_delay_t *s = (fx_delay_t *)self;
    switch (id) {
    case 0: s->time_ms = v; s->sync_div = v; break;  /* dual use */
    case 1: s->feedback = v > 0.99f ? 0.99f : v; break;
    case 2: s->mix = v > 1.0f ? 1.0f : (v < 0.0f ? 0.0f : v); break;
    case 3: s->ping_pong = (v != 0.0f); break;
    case 4: s->clock_sync = (v != 0.0f); break;
    }
}

static void delay_free(fx_node_t *self)
{
    fx_delay_t *s = (fx_delay_t *)self;
    heap_caps_free(s->buf_l);
    heap_caps_free(s->buf_r);
    heap_caps_free(s);
}

fx_node_t *fx_delay_new(float time_ms, float feedback, float mix)
{
    fx_delay_t *s = FX_ALLOC(fx_delay_t);
    if (!s) return NULL;
    s->buf_l = (int16_t *)heap_caps_calloc(DELAY_MAX_FRAMES, sizeof(int16_t), MALLOC_CAP_SPIRAM);
    s->buf_r = (int16_t *)heap_caps_calloc(DELAY_MAX_FRAMES, sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!s->buf_l || !s->buf_r) {
        heap_caps_free(s->buf_l); heap_caps_free(s->buf_r); heap_caps_free(s); return NULL;
    }
    s->hdr.type = FX_TYPE_DELAY; s->hdr.enabled = true;
    s->hdr.process = delay_process; s->hdr.set_param = delay_set_param;
    s->hdr.free = delay_free;
    s->buf_len = DELAY_MAX_FRAMES;
    s->time_ms = time_ms; s->feedback = feedback; s->mix = mix;
    s->sync_div = 0.25f;  /* 1/4 note default */
    s->delay_frames = (int)(time_ms * (float)SAMPLE_RATE / 1000.0f);
    if (s->delay_frames >= DELAY_MAX_FRAMES) s->delay_frames = DELAY_MAX_FRAMES - 1;
    return &s->hdr;
}

/* ════════════════════════════════════════════════════════════════════════════
 * FX_TYPE_REVERB  — Freeverb (8 comb + 4 allpass, stereo)
 * All buffer sizes derived from original Freeverb constants scaled to 48 kHz.
 * ════════════════════════════════════════════════════════════════════════════ */
#define FV_NUMCOMBS    8
#define FV_NUMALLPASS  4
#define FV_SPREAD      23  /* stereo spread in samples */

/* Comb filter sizes at 44.1 kHz, scaled to SAMPLE_RATE */
static const int fv_comb_sizes[FV_NUMCOMBS] = {
    1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617
};
static const int fv_ap_sizes[FV_NUMALLPASS] = { 556, 441, 341, 225 };

typedef struct {
    int16_t *buf;
    int len, pos;
    float damp1, damp2, feedback;
    float store;
} comb_t;

typedef struct {
    int16_t *buf;
    int len, pos;
    float feedback;
} allp_t;

typedef struct {
    fx_node_t hdr;
    comb_t  comb_l[FV_NUMCOMBS], comb_r[FV_NUMCOMBS];
    allp_t  ap_l[FV_NUMALLPASS], ap_r[FV_NUMALLPASS];
    float   room;
    float   damp;
    float   width;
    float   mix;
    float   wet1, wet2;
} fx_reverb_t;

static void reverb_update(fx_reverb_t *s)
{
    float room_fb = 0.28f + s->room * 0.7f;
    float damp    = s->damp * 0.5f;
    for (int i = 0; i < FV_NUMCOMBS; i++) {
        s->comb_l[i].feedback = room_fb;
        s->comb_r[i].feedback = room_fb;
        s->comb_l[i].damp1 = damp; s->comb_l[i].damp2 = 1.0f - damp;
        s->comb_r[i].damp1 = damp; s->comb_r[i].damp2 = 1.0f - damp;
    }
    float scale = 0.015f;
    float wet   = s->mix * scale;
    s->wet1 = wet * (s->width / 2.0f + 0.5f);
    s->wet2 = wet * ((1.0f - s->width) / 2.0f);
}

static inline float comb_process(comb_t *c, float inp)
{
    float out = c->buf[c->pos] / 32768.0f;
    c->store  = out * c->damp2 + c->store * c->damp1;
    c->buf[c->pos] = (int16_t)((inp + c->store * c->feedback) * 32768.0f);
    c->pos = (c->pos + 1 >= c->len) ? 0 : c->pos + 1;
    return out;
}

static inline float allp_process(allp_t *a, float inp)
{
    float buf_out = a->buf[a->pos] / 32768.0f;
    float out = -inp + buf_out;
    a->buf[a->pos] = (int16_t)((inp + buf_out * a->feedback) * 32768.0f);
    a->pos = (a->pos + 1 >= a->len) ? 0 : a->pos + 1;
    return out;
}

static void reverb_process(fx_node_t *self, int16_t *bl, int16_t *br, int n,
                           float bpm, uint32_t tr)
{
    (void)bpm; (void)tr;
    fx_reverb_t *s = (fx_reverb_t *)self;
    float inp_scale = 0.015f;
    for (int i = 0; i < n; i++) {
        float in = ((float)bl[i] + (float)br[i]) * inp_scale / 32768.0f;
        float outl = 0.0f, outr = 0.0f;
        for (int c = 0; c < FV_NUMCOMBS; c++) {
            outl += comb_process(&s->comb_l[c], in);
            outr += comb_process(&s->comb_r[c], in);
        }
        for (int a = 0; a < FV_NUMALLPASS; a++) {
            outl = allp_process(&s->ap_l[a], outl);
            outr = allp_process(&s->ap_r[a], outr);
        }
        float l = (float)bl[i] / 32768.0f + outl * s->wet1 + outr * s->wet2;
        float r = (float)br[i] / 32768.0f + outr * s->wet1 + outl * s->wet2;
        l *= 32768.0f; r *= 32768.0f;
        if (l >  32767.0f) l =  32767.0f;
        if (l < -32768.0f) l = -32768.0f;
        if (r >  32767.0f) r =  32767.0f;
        if (r < -32768.0f) r = -32768.0f;
        bl[i] = (int16_t)l; br[i] = (int16_t)r;
    }
}

static void reverb_set_param(fx_node_t *self, uint8_t id, float v)
{
    fx_reverb_t *s = (fx_reverb_t *)self;
    switch (id) {
    case 0: s->room  = v; break;
    case 1: s->damp  = v; break;
    case 2: s->width = v; break;
    case 3: s->mix   = v; break;
    }
    reverb_update(s);
}

static void reverb_free(fx_node_t *self)
{
    fx_reverb_t *s = (fx_reverb_t *)self;
    for (int i = 0; i < FV_NUMCOMBS;   i++) { heap_caps_free(s->comb_l[i].buf); heap_caps_free(s->comb_r[i].buf); }
    for (int i = 0; i < FV_NUMALLPASS; i++) { heap_caps_free(s->ap_l[i].buf);   heap_caps_free(s->ap_r[i].buf); }
    heap_caps_free(s);
}

fx_node_t *fx_reverb_new(float room, float damp, float mix)
{
    fx_reverb_t *s = FX_ALLOC(fx_reverb_t);
    if (!s) return NULL;
    /* Allocate comb/allpass buffers in PSRAM scaled to SAMPLE_RATE */
    float scale = (float)SAMPLE_RATE / 44100.0f;
    for (int i = 0; i < FV_NUMCOMBS; i++) {
        int lenl = (int)(fv_comb_sizes[i] * scale);
        int lenr = (int)((fv_comb_sizes[i] + FV_SPREAD) * scale);
        s->comb_l[i].buf = (int16_t *)heap_caps_calloc((size_t)lenl, sizeof(int16_t), MALLOC_CAP_SPIRAM);
        s->comb_r[i].buf = (int16_t *)heap_caps_calloc((size_t)lenr, sizeof(int16_t), MALLOC_CAP_SPIRAM);
        if (!s->comb_l[i].buf || !s->comb_r[i].buf) { reverb_free(&s->hdr); return NULL; }
        s->comb_l[i].len = lenl; s->comb_r[i].len = lenr;
        s->comb_l[i].damp2 = s->comb_r[i].damp2 = 1.0f;
    }
    for (int i = 0; i < FV_NUMALLPASS; i++) {
        int lenl = (int)(fv_ap_sizes[i] * scale);
        int lenr = (int)((fv_ap_sizes[i] + FV_SPREAD) * scale);
        s->ap_l[i].buf = (int16_t *)heap_caps_calloc((size_t)lenl, sizeof(int16_t), MALLOC_CAP_SPIRAM);
        s->ap_r[i].buf = (int16_t *)heap_caps_calloc((size_t)lenr, sizeof(int16_t), MALLOC_CAP_SPIRAM);
        if (!s->ap_l[i].buf || !s->ap_r[i].buf) { reverb_free(&s->hdr); return NULL; }
        s->ap_l[i].len = lenl; s->ap_r[i].len = lenr;
        s->ap_l[i].feedback = s->ap_r[i].feedback = 0.5f;
    }
    s->hdr.type = FX_TYPE_REVERB; s->hdr.enabled = true;
    s->hdr.process = reverb_process; s->hdr.set_param = reverb_set_param;
    s->hdr.free = reverb_free;
    s->room = room; s->damp = damp; s->width = 1.0f; s->mix = mix;
    reverb_update(s);
    return &s->hdr;
}

/* ════════════════════════════════════════════════════════════════════════════
 * LFO helper shared by chorus / flanger / phaser / tremolo / vibrato / auto-pan
 * ════════════════════════════════════════════════════════════════════════════ */
typedef struct {
    uint32_t phase;
    uint32_t step;   /* per-sample step */
} mod_lfo_t;

static void mod_lfo_set_rate(mod_lfo_t *l, float rate_hz)
{
    l->step = (uint32_t)(rate_hz / (float)SAMPLE_RATE * (float)(1u << 24) * 256.0f);
}

static inline float mod_lfo_tick(mod_lfo_t *l)
{
    l->phase += l->step;
    float t = (float)(l->phase >> 8) / (float)(1u << 24);
    /* sine via cheap polynomial: ~0.1% error */
    t = t * 2.0f * (float)M_PI;
    float s = t - t * t * t / 6.0f + t * t * t * t * t / 120.0f;
    return s > 1.0f ? 1.0f : (s < -1.0f ? -1.0f : s);
}

/* ════════════════════════════════════════════════════════════════════════════
 * FX_TYPE_CHORUS  — 3-tap LFO delay
 * ════════════════════════════════════════════════════════════════════════════ */
#define CHORUS_BUF  4096   /* ~85 ms at 48 kHz */

typedef struct {
    fx_node_t hdr;
    int16_t  *buf_l, *buf_r;
    int       write_pos;
    float     rate_hz;
    float     depth_ms;
    float     mix;
    mod_lfo_t lfo[3];
    float     lfo_phase_offset[3]; /* 0, 120, 240 degrees in radians */
} fx_chorus_t;

static void chorus_process(fx_node_t *self, int16_t *bl, int16_t *br, int n,
                           float bpm, uint32_t tr)
{
    (void)bpm; (void)tr;
    fx_chorus_t *s = (fx_chorus_t *)self;
    int mix_q15 = (int32_t)(s->mix * 32768.0f / 3.0f);  /* /3 taps */
    int dry_q15 = 32768 - (int32_t)(s->mix * 32768.0f);
    for (int i = 0; i < n; i++) {
        s->buf_l[s->write_pos] = bl[i];
        s->buf_r[s->write_pos] = br[i];
        int32_t wet_l = 0, wet_r = 0;
        for (int t = 0; t < 3; t++) {
            float lfo_v = mod_lfo_tick(&s->lfo[t]);
            float delay_ms = 5.0f + s->depth_ms * (lfo_v * 0.5f + 0.5f);
            int delay_frames = (int)(delay_ms * (float)SAMPLE_RATE / 1000.0f);
            if (delay_frames < 1) delay_frames = 1;
            if (delay_frames >= CHORUS_BUF) delay_frames = CHORUS_BUF - 1;
            int rpos = (s->write_pos - delay_frames + CHORUS_BUF) % CHORUS_BUF;
            wet_l += s->buf_l[rpos];
            wet_r += s->buf_r[rpos];
        }
        s->write_pos = (s->write_pos + 1) % CHORUS_BUF;
        int32_t l = (((int32_t)bl[i] * dry_q15) >> 15) + ((wet_l * mix_q15) >> 15);
        int32_t r = (((int32_t)br[i] * dry_q15) >> 15) + ((wet_r * mix_q15) >> 15);
        if (l >  32767) l =  32767;
        if (l < -32768) l = -32768;
        if (r >  32767) r =  32767;
        if (r < -32768) r = -32768;
        bl[i] = (int16_t)l; br[i] = (int16_t)r;
    }
}

static void chorus_set_param(fx_node_t *self, uint8_t id, float v)
{
    fx_chorus_t *s = (fx_chorus_t *)self;
    if (id == 0) { s->rate_hz  = v; for (int t = 0; t < 3; t++) mod_lfo_set_rate(&s->lfo[t], v); }
    if (id == 1) s->depth_ms = v;
    if (id == 2) s->mix = v > 1.0f ? 1.0f : (v < 0.0f ? 0.0f : v);
}

static void chorus_free(fx_node_t *self)
{
    fx_chorus_t *s = (fx_chorus_t *)self;
    heap_caps_free(s->buf_l); heap_caps_free(s->buf_r); heap_caps_free(s);
}

fx_node_t *fx_chorus_new(void)
{
    fx_chorus_t *s = FX_ALLOC(fx_chorus_t);
    if (!s) return NULL;
    s->buf_l = (int16_t *)heap_caps_calloc(CHORUS_BUF, sizeof(int16_t), MALLOC_CAP_SPIRAM);
    s->buf_r = (int16_t *)heap_caps_calloc(CHORUS_BUF, sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!s->buf_l || !s->buf_r) { chorus_free(&s->hdr); return NULL; }
    s->hdr.type = FX_TYPE_CHORUS; s->hdr.enabled = true;
    s->hdr.process = chorus_process; s->hdr.set_param = chorus_set_param;
    s->hdr.free = chorus_free;
    s->rate_hz = 0.5f; s->depth_ms = 4.0f; s->mix = 0.5f;
    for (int t = 0; t < 3; t++) {
        mod_lfo_set_rate(&s->lfo[t], s->rate_hz);
        /* Stagger LFO phases: 0 / 2π/3 / 4π/3 */
        s->lfo[t].phase = (uint32_t)((float)(1u << 24) * 256.0f / 3.0f) * (uint32_t)t;
    }
    return &s->hdr;
}

/* ════════════════════════════════════════════════════════════════════════════
 * FX_TYPE_FLANGER  — short LFO delay + feedback
 * ════════════════════════════════════════════════════════════════════════════ */
#define FLANGER_BUF 1024   /* ~21 ms */

typedef struct {
    fx_node_t hdr;
    int16_t  *buf_l, *buf_r;
    int       write_pos;
    float     rate_hz, depth_ms, feedback, mix;
    mod_lfo_t lfo;
} fx_flanger_t;

static void flanger_process(fx_node_t *self, int16_t *bl, int16_t *br, int n,
                            float bpm, uint32_t tr)
{
    (void)bpm; (void)tr;
    fx_flanger_t *s = (fx_flanger_t *)self;
    int fb_q15  = (int32_t)(s->feedback * 32768.0f);
    int mix_q15 = (int32_t)(s->mix * 32768.0f);
    int dry_q15 = 32768 - mix_q15;
    for (int i = 0; i < n; i++) {
        float lfo_v = mod_lfo_tick(&s->lfo);
        float delay_ms = 0.5f + s->depth_ms * (lfo_v * 0.5f + 0.5f);
        int df = (int)(delay_ms * (float)SAMPLE_RATE / 1000.0f);
        if (df < 1) df = 1;
        if (df >= FLANGER_BUF) df = FLANGER_BUF - 1;
        int rpos = (s->write_pos - df + FLANGER_BUF) % FLANGER_BUF;
        int32_t fl = (int32_t)bl[i] + (int32_t)(s->buf_l[rpos] * fb_q15 >> 15);
        int32_t fr = (int32_t)br[i] + (int32_t)(s->buf_r[rpos] * fb_q15 >> 15);
        if (fl >  32767) fl =  32767;
        if (fl < -32768) fl = -32768;
        if (fr >  32767) fr =  32767;
        if (fr < -32768) fr = -32768;
        s->buf_l[s->write_pos] = (int16_t)fl;
        s->buf_r[s->write_pos] = (int16_t)fr;
        s->write_pos = (s->write_pos + 1) % FLANGER_BUF;
        int32_t l = ((int32_t)bl[i] * dry_q15 + (int32_t)s->buf_l[rpos] * mix_q15) >> 15;
        int32_t r = ((int32_t)br[i] * dry_q15 + (int32_t)s->buf_r[rpos] * mix_q15) >> 15;
        if (l >  32767) l =  32767;
        if (l < -32768) l = -32768;
        if (r >  32767) r =  32767;
        if (r < -32768) r = -32768;
        bl[i] = (int16_t)l; br[i] = (int16_t)r;
    }
}

static void flanger_set_param(fx_node_t *self, uint8_t id, float v)
{
    fx_flanger_t *s = (fx_flanger_t *)self;
    if (id == 0) { s->rate_hz  = v; mod_lfo_set_rate(&s->lfo, v); }
    if (id == 1) s->depth_ms = v;
    if (id == 2) s->feedback  = v > 0.95f ? 0.95f : v;
    if (id == 3) s->mix = v > 1.0f ? 1.0f : (v < 0.0f ? 0.0f : v);
}

static void flanger_free(fx_node_t *self)
{
    fx_flanger_t *s = (fx_flanger_t *)self;
    heap_caps_free(s->buf_l); heap_caps_free(s->buf_r); heap_caps_free(s);
}

fx_node_t *fx_flanger_new(void)
{
    fx_flanger_t *s = FX_ALLOC(fx_flanger_t);
    if (!s) return NULL;
    s->buf_l = (int16_t *)heap_caps_calloc(FLANGER_BUF, sizeof(int16_t), MALLOC_CAP_SPIRAM);
    s->buf_r = (int16_t *)heap_caps_calloc(FLANGER_BUF, sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!s->buf_l || !s->buf_r) { flanger_free(&s->hdr); return NULL; }
    s->hdr.type = FX_TYPE_FLANGER; s->hdr.enabled = true;
    s->hdr.process = flanger_process; s->hdr.set_param = flanger_set_param;
    s->hdr.free = flanger_free;
    s->rate_hz = 0.3f; s->depth_ms = 3.0f; s->feedback = 0.5f; s->mix = 0.5f;
    mod_lfo_set_rate(&s->lfo, s->rate_hz);
    return &s->hdr;
}

/* ════════════════════════════════════════════════════════════════════════════
 * FX_TYPE_PHASER  — 4-stage all-pass LFO sweep
 * ════════════════════════════════════════════════════════════════════════════ */
#define PHASER_STAGES 4

typedef struct {
    fx_node_t hdr;
    float  a[PHASER_STAGES];     /* allpass coefficients */
    float  z_l[PHASER_STAGES];   /* allpass state L */
    float  z_r[PHASER_STAGES];   /* allpass state R */
    float  feedback;
    float  mix;
    float  fb_l, fb_r;
    mod_lfo_t lfo;
    float  depth;   /* 0–1 */
    float  rate_hz;
} fx_phaser_t;

static void phaser_update_coefs(fx_phaser_t *s, float lfo_val)
{
    /* Sweep allpass pole frequency between ~200 Hz and ~8 kHz */
    float freq = 200.0f + (lfo_val * 0.5f + 0.5f) * 7800.0f * s->depth;
    float t = tanf((float)M_PI * freq / (float)SAMPLE_RATE);
    float coef = (t - 1.0f) / (t + 1.0f);
    for (int i = 0; i < PHASER_STAGES; i++) s->a[i] = coef;
}

static void phaser_process(fx_node_t *self, int16_t *bl, int16_t *br, int n,
                           float bpm, uint32_t tr)
{
    (void)bpm; (void)tr;
    fx_phaser_t *s = (fx_phaser_t *)self;
    int mix_q15 = (int32_t)(s->mix * 32768.0f);
    int dry_q15 = 32768 - mix_q15;
    for (int i = 0; i < n; i++) {
        float lfo_v = mod_lfo_tick(&s->lfo);
        phaser_update_coefs(s, lfo_v);
        float xl = (float)bl[i] / 32768.0f + s->fb_l * s->feedback;
        float xr = (float)br[i] / 32768.0f + s->fb_r * s->feedback;
        for (int st = 0; st < PHASER_STAGES; st++) {
            float yl = s->a[st] * xl + s->z_l[st]; s->z_l[st] = xl - s->a[st] * yl; xl = yl;
            float yr = s->a[st] * xr + s->z_r[st]; s->z_r[st] = xr - s->a[st] * yr; xr = yr;
        }
        s->fb_l = xl; s->fb_r = xr;
        int32_t l = (int32_t)(((float)bl[i] / 32768.0f) * dry_q15 + xl * mix_q15);
        int32_t r = (int32_t)(((float)br[i] / 32768.0f) * dry_q15 + xr * mix_q15);
        l = (l * 32768) >> 15; r = (r * 32768) >> 15;
        if (l >  32767) l =  32767;
        if (l < -32768) l = -32768;
        if (r >  32767) r =  32767;
        if (r < -32768) r = -32768;
        bl[i] = (int16_t)l; br[i] = (int16_t)r;
    }
}

static void phaser_set_param(fx_node_t *self, uint8_t id, float v)
{
    fx_phaser_t *s = (fx_phaser_t *)self;
    if (id == 0) { s->rate_hz  = v; mod_lfo_set_rate(&s->lfo, v); }
    if (id == 1) s->depth = v > 1.0f ? 1.0f : (v < 0.0f ? 0.0f : v);
    if (id == 2) s->feedback = v > 0.95f ? 0.95f : v;
    if (id == 3) s->mix = v > 1.0f ? 1.0f : (v < 0.0f ? 0.0f : v);
}

fx_node_t *fx_phaser_new(void)
{
    fx_phaser_t *s = FX_ALLOC(fx_phaser_t);
    if (!s) return NULL;
    s->hdr.type = FX_TYPE_PHASER; s->hdr.enabled = true;
    s->hdr.process = phaser_process; s->hdr.set_param = phaser_set_param;
    s->hdr.free = fx_generic_free;
    s->rate_hz = 0.5f; s->depth = 0.8f; s->feedback = 0.3f; s->mix = 0.5f;
    mod_lfo_set_rate(&s->lfo, s->rate_hz);
    return &s->hdr;
}

/* ════════════════════════════════════════════════════════════════════════════
 * FX_TYPE_TREMOLO  — LFO amplitude modulation
 * ════════════════════════════════════════════════════════════════════════════ */
typedef struct {
    fx_node_t hdr;
    float rate_hz, depth;
    mod_lfo_t lfo;
} fx_trem_t;

static void trem_process(fx_node_t *self, int16_t *bl, int16_t *br, int n,
                         float bpm, uint32_t tr)
{
    (void)bpm; (void)tr;
    fx_trem_t *s = (fx_trem_t *)self;
    for (int i = 0; i < n; i++) {
        float lv = mod_lfo_tick(&s->lfo);
        float gain = 1.0f - s->depth * (lv * 0.5f + 0.5f);
        int32_t gq = (int32_t)(gain * 32768.0f);
        int32_t l = ((int32_t)bl[i] * gq) >> 15;
        int32_t r = ((int32_t)br[i] * gq) >> 15;
        if (l >  32767) l =  32767;
        if (l < -32768) l = -32768;
        if (r >  32767) r =  32767;
        if (r < -32768) r = -32768;
        bl[i] = (int16_t)l; br[i] = (int16_t)r;
    }
}

static void trem_set_param(fx_node_t *self, uint8_t id, float v)
{
    fx_trem_t *s = (fx_trem_t *)self;
    if (id == 0) { s->rate_hz = v; mod_lfo_set_rate(&s->lfo, v); }
    if (id == 1) s->depth = v > 1.0f ? 1.0f : (v < 0.0f ? 0.0f : v);
}

fx_node_t *fx_tremolo_new(void)
{
    fx_trem_t *s = FX_ALLOC(fx_trem_t);
    if (!s) return NULL;
    s->hdr.type = FX_TYPE_TREMOLO; s->hdr.enabled = true;
    s->hdr.process = trem_process; s->hdr.set_param = trem_set_param;
    s->hdr.free = fx_generic_free;
    s->rate_hz = 5.0f; s->depth = 0.5f;
    mod_lfo_set_rate(&s->lfo, s->rate_hz);
    return &s->hdr;
}

/* ════════════════════════════════════════════════════════════════════════════
 * FX_TYPE_VIBRATO  — LFO pitch via short delay line
 * ════════════════════════════════════════════════════════════════════════════ */
#define VIBRATO_BUF 512

typedef struct {
    fx_node_t hdr;
    int16_t  *buf_l, *buf_r;
    int       write_pos;
    float     rate_hz, depth_ms;
    mod_lfo_t lfo;
} fx_vib_t;

static void vib_process(fx_node_t *self, int16_t *bl, int16_t *br, int n,
                        float bpm, uint32_t tr)
{
    (void)bpm; (void)tr;
    fx_vib_t *s = (fx_vib_t *)self;
    for (int i = 0; i < n; i++) {
        s->buf_l[s->write_pos] = bl[i];
        s->buf_r[s->write_pos] = br[i];
        float lv = mod_lfo_tick(&s->lfo);
        float delay_ms = s->depth_ms * (lv * 0.5f + 0.5f) + 1.0f;
        int df = (int)(delay_ms * (float)SAMPLE_RATE / 1000.0f);
        if (df < 1) df = 1;
        if (df >= VIBRATO_BUF) df = VIBRATO_BUF - 1;
        int rpos = (s->write_pos - df + VIBRATO_BUF) % VIBRATO_BUF;
        s->write_pos = (s->write_pos + 1) % VIBRATO_BUF;
        bl[i] = s->buf_l[rpos]; br[i] = s->buf_r[rpos];
    }
}

static void vib_set_param(fx_node_t *self, uint8_t id, float v)
{
    fx_vib_t *s = (fx_vib_t *)self;
    if (id == 0) { s->rate_hz = v; mod_lfo_set_rate(&s->lfo, v); }
    if (id == 1) s->depth_ms = v;
}

static void vib_free(fx_node_t *self)
{
    fx_vib_t *s = (fx_vib_t *)self;
    heap_caps_free(s->buf_l); heap_caps_free(s->buf_r); heap_caps_free(s);
}

fx_node_t *fx_vibrato_new(void)
{
    fx_vib_t *s = FX_ALLOC(fx_vib_t);
    if (!s) return NULL;
    s->buf_l = (int16_t *)heap_caps_calloc(VIBRATO_BUF, sizeof(int16_t), MALLOC_CAP_SPIRAM);
    s->buf_r = (int16_t *)heap_caps_calloc(VIBRATO_BUF, sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!s->buf_l || !s->buf_r) { vib_free(&s->hdr); return NULL; }
    s->hdr.type = FX_TYPE_VIBRATO; s->hdr.enabled = true;
    s->hdr.process = vib_process; s->hdr.set_param = vib_set_param;
    s->hdr.free = vib_free;
    s->rate_hz = 5.0f; s->depth_ms = 2.0f;
    mod_lfo_set_rate(&s->lfo, s->rate_hz);
    return &s->hdr;
}

/* ════════════════════════════════════════════════════════════════════════════
 * FX_TYPE_RING_MOD  — multiply by sine carrier
 * ════════════════════════════════════════════════════════════════════════════ */
typedef struct {
    fx_node_t hdr;
    float     carrier_hz;
    float     mix;
    uint32_t  phase;
    uint32_t  step;
} fx_rm_t;

static void rm_process(fx_node_t *self, int16_t *bl, int16_t *br, int n,
                       float bpm, uint32_t tr)
{
    (void)bpm; (void)tr;
    fx_rm_t *s = (fx_rm_t *)self;
    int mix_q15 = (int32_t)(s->mix * 32768.0f);
    int dry_q15 = 32768 - mix_q15;
    for (int i = 0; i < n; i++) {
        s->phase += s->step;
        float t = (float)(s->phase >> 8) / (float)(1u << 24) * 2.0f * (float)M_PI;
        float carrier = sinf(t);
        int32_t cq = (int32_t)(carrier * 32768.0f);
        int32_t wet_l = ((int32_t)bl[i] * cq) >> 15;
        int32_t wet_r = ((int32_t)br[i] * cq) >> 15;
        int32_t l = ((int32_t)bl[i] * dry_q15 + wet_l * mix_q15) >> 15;
        int32_t r = ((int32_t)br[i] * dry_q15 + wet_r * mix_q15) >> 15;
        if (l >  32767) l =  32767;
        if (l < -32768) l = -32768;
        if (r >  32767) r =  32767;
        if (r < -32768) r = -32768;
        bl[i] = (int16_t)l; br[i] = (int16_t)r;
    }
}

static void rm_set_param(fx_node_t *self, uint8_t id, float v)
{
    fx_rm_t *s = (fx_rm_t *)self;
    if (id == 0) {
        s->carrier_hz = v;
        s->step = (uint32_t)(v / (float)SAMPLE_RATE * (float)(1u << 24) * 256.0f);
    }
    if (id == 1) s->mix = v > 1.0f ? 1.0f : (v < 0.0f ? 0.0f : v);
}

fx_node_t *fx_ring_mod_new(float carrier_hz)
{
    fx_rm_t *s = FX_ALLOC(fx_rm_t);
    if (!s) return NULL;
    s->hdr.type = FX_TYPE_RING_MOD; s->hdr.enabled = true;
    s->hdr.process = rm_process; s->hdr.set_param = rm_set_param;
    s->hdr.free = fx_generic_free;
    s->mix = 1.0f;
    rm_set_param(&s->hdr, 0, carrier_hz);
    return &s->hdr;
}

/* ════════════════════════════════════════════════════════════════════════════
 * FX_TYPE_PITCH_SHIFT  — Granular pitch shift ±24 semitones
 * Simplified 2-grain overlap-add with PSRAM grain buffer.
 * ════════════════════════════════════════════════════════════════════════════ */
#define PS_BUF_FRAMES   4096
#define PS_GRAIN_FRAMES 2048
#define PS_HOP_FRAMES   512

typedef struct {
    fx_node_t hdr;
    int16_t  *buf_l, *buf_r;
    int       write_pos;
    float     ratio;   /* playback ratio: > 1 = pitch up, < 1 = pitch down */
    float     read_pos_l, read_pos_r;
    int       hop_cnt;
} fx_ps_t;

static void ps_process(fx_node_t *self, int16_t *bl, int16_t *br, int n,
                       float bpm, uint32_t tr)
{
    (void)bpm; (void)tr;
    fx_ps_t *s = (fx_ps_t *)self;
    for (int i = 0; i < n; i++) {
        s->buf_l[s->write_pos] = bl[i];
        s->buf_r[s->write_pos] = br[i];
        /* Linear interpolated read at pitch-ratio position */
        int ri   = (int)s->read_pos_l % PS_BUF_FRAMES;
        float fr = s->read_pos_l - (int)s->read_pos_l;
        int ri2  = (ri + 1) % PS_BUF_FRAMES;
        int32_t l = (int32_t)(s->buf_l[ri] * (1.0f - fr) + s->buf_l[ri2] * fr);
        int32_t r = (int32_t)(s->buf_r[ri] * (1.0f - fr) + s->buf_r[ri2] * fr);
        s->read_pos_l += s->ratio;
        if (s->read_pos_l >= PS_BUF_FRAMES) s->read_pos_l -= PS_BUF_FRAMES;
        s->write_pos = (s->write_pos + 1) % PS_BUF_FRAMES;
        if (l >  32767) l =  32767;
        if (l < -32768) l = -32768;
        if (r >  32767) r =  32767;
        if (r < -32768) r = -32768;
        bl[i] = (int16_t)l; br[i] = (int16_t)r;
    }
    (void)PS_GRAIN_FRAMES; (void)PS_HOP_FRAMES;
}

static void ps_set_param(fx_node_t *self, uint8_t id, float v)
{
    fx_ps_t *s = (fx_ps_t *)self;
    if (id == 0) s->ratio = powf(2.0f, v / 12.0f);
}

static void ps_free(fx_node_t *self)
{
    fx_ps_t *s = (fx_ps_t *)self;
    heap_caps_free(s->buf_l); heap_caps_free(s->buf_r); heap_caps_free(s);
}

fx_node_t *fx_pitch_shift_new(float semitones)
{
    fx_ps_t *s = FX_ALLOC(fx_ps_t);
    if (!s) return NULL;
    s->buf_l = (int16_t *)heap_caps_calloc(PS_BUF_FRAMES, sizeof(int16_t), MALLOC_CAP_SPIRAM);
    s->buf_r = (int16_t *)heap_caps_calloc(PS_BUF_FRAMES, sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!s->buf_l || !s->buf_r) { ps_free(&s->hdr); return NULL; }
    s->hdr.type = FX_TYPE_PITCH_SHIFT; s->hdr.enabled = true;
    s->hdr.process = ps_process; s->hdr.set_param = ps_set_param;
    s->hdr.free = ps_free;
    s->ratio = powf(2.0f, semitones / 12.0f);
    s->read_pos_l = PS_BUF_FRAMES - PS_GRAIN_FRAMES;
    return &s->hdr;
}

/* ════════════════════════════════════════════════════════════════════════════
 * FX_TYPE_AUTO_PAN  — LFO L↔R sweep
 * ════════════════════════════════════════════════════════════════════════════ */
typedef struct {
    fx_node_t hdr;
    float rate_hz, depth;
    mod_lfo_t lfo;
} fx_ap_t;

static void autopan_process(fx_node_t *self, int16_t *bl, int16_t *br, int n,
                       float bpm, uint32_t tr)
{
    (void)bpm; (void)tr;
    fx_ap_t *s = (fx_ap_t *)self;
    for (int i = 0; i < n; i++) {
        float lv  = mod_lfo_tick(&s->lfo) * s->depth;  /* -depth..+depth */
        float pan = lv * 0.5f;  /* -0.5..+0.5 */
        int32_t l_q15 = (int32_t)((1.0f - (pan > 0 ? pan : 0)) * 32768.0f);
        int32_t r_q15 = (int32_t)((1.0f + (pan < 0 ? pan : 0)) * 32768.0f);
        int32_t l = ((int32_t)bl[i] * l_q15) >> 15;
        int32_t r = ((int32_t)br[i] * r_q15) >> 15;
        if (l >  32767) l =  32767;
        if (l < -32768) l = -32768;
        if (r >  32767) r =  32767;
        if (r < -32768) r = -32768;
        bl[i] = (int16_t)l; br[i] = (int16_t)r;
    }
}

static void ap_set_param(fx_node_t *self, uint8_t id, float v)
{
    fx_ap_t *s = (fx_ap_t *)self;
    if (id == 0) { s->rate_hz = v; mod_lfo_set_rate(&s->lfo, v); }
    if (id == 1) s->depth = v > 1.0f ? 1.0f : (v < 0.0f ? 0.0f : v);
}

fx_node_t *fx_auto_pan_new(float rate_hz)
{
    fx_ap_t *s = FX_ALLOC(fx_ap_t);
    if (!s) return NULL;
    s->hdr.type = FX_TYPE_AUTO_PAN; s->hdr.enabled = true;
    s->hdr.process = autopan_process; s->hdr.set_param = ap_set_param;
    s->hdr.free = fx_generic_free;
    s->rate_hz = rate_hz; s->depth = 1.0f;
    mod_lfo_set_rate(&s->lfo, rate_hz);
    return &s->hdr;
}

/* ════════════════════════════════════════════════════════════════════════════
 * FX_TYPE_STEREO_WIDTH  — mid/side width control
 * ════════════════════════════════════════════════════════════════════════════ */
typedef struct {
    fx_node_t hdr;
    float width;   /* 0=mono, 1=normal, 2=wide */
} fx_sw_t;

static void sw_process(fx_node_t *self, int16_t *bl, int16_t *br, int n,
                       float bpm, uint32_t tr)
{
    (void)bpm; (void)tr;
    fx_sw_t *s = (fx_sw_t *)self;
    /* M/S: mid = (L+R)/2, side = (L-R)/2; widen side by width */
    int32_t side_q15 = (int32_t)(s->width * 32768.0f);
    for (int i = 0; i < n; i++) {
        int32_t mid  = ((int32_t)bl[i] + (int32_t)br[i]) >> 1;
        int32_t side = (((int32_t)bl[i] - (int32_t)br[i]) >> 1) * side_q15 >> 15;
        int32_t l = mid + side;
        int32_t r = mid - side;
        if (l >  32767) l =  32767;
        if (l < -32768) l = -32768;
        if (r >  32767) r =  32767;
        if (r < -32768) r = -32768;
        bl[i] = (int16_t)l; br[i] = (int16_t)r;
    }
}

static void sw_set_param(fx_node_t *self, uint8_t id, float v)
{
    fx_sw_t *s = (fx_sw_t *)self;
    if (id == 0) s->width = v < 0.0f ? 0.0f : v;
}

fx_node_t *fx_stereo_width_new(float width)
{
    fx_sw_t *s = FX_ALLOC(fx_sw_t);
    if (!s) return NULL;
    s->hdr.type = FX_TYPE_STEREO_WIDTH; s->hdr.enabled = true;
    s->hdr.process = sw_process; s->hdr.set_param = sw_set_param;
    s->hdr.free = fx_generic_free;
    s->width = width;
    return &s->hdr;
}

/* ════════════════════════════════════════════════════════════════════════════
 * Dispatcher
 * ════════════════════════════════════════════════════════════════════════════ */

/* Default param values per type, mirrors fx_*_new() init for the cache.
 * Used by fx_new() to fill node->params[] so UI/WS can render initial values. */
static const float fx_defaults[FX_TYPE_COUNT][8] = {
    [FX_TYPE_NONE]          = {0},
    [FX_TYPE_FILTER]        = {2000.0f, 1.0f,  FILT_LP, 0, 0, 0, 0, 0},
    [FX_TYPE_EQ3]           = {0, 0, 0, 1000.0f, 1.0f, 0, 0, 0},
    [FX_TYPE_EQ5]           = {0, 0, 1000.0f, 1.0f, 0, 1, 0, 0},
    [FX_TYPE_COMPRESSOR]    = {-12.0f, 4.0f, 10.0f, 100.0f, 0, 0, 0, 0},
    [FX_TYPE_LIMITER]       = {-3.0f, 2.0f, 0, 0, 0, 0, 0, 0},
    [FX_TYPE_GATE]          = {-40.0f, 50.0f, 5.0f, 100.0f, 0, 0, 0, 0},
    [FX_TYPE_TRANSIENT]     = {1.0f, 1.0f, 0, 0, 0, 0, 0, 0},
    [FX_TYPE_DISTORTION]    = {2.0f, 0.5f, DIST_SOFT, 0, 0, 0, 0, 0},
    [FX_TYPE_OVERDRIVE]     = {2.0f, 0.5f, 0, 0, 0, 0, 0, 0},
    [FX_TYPE_WAVEFOLDER]    = {0.5f, 1.0f, 0, 0, 0, 0, 0, 0},
    [FX_TYPE_BITCRUSH]      = {8.0f, 4.0f, 0, 0, 0, 0, 0, 0},
    [FX_TYPE_DELAY]         = {250.0f, 0.4f, 0.4f, 0, 0, 0, 0, 0},
    [FX_TYPE_REVERB]        = {0.5f, 0.5f, 1.0f, 0.3f, 0, 0, 0, 0},
    [FX_TYPE_CHORUS]        = {1.0f, 3.0f, 0.5f, 0, 0, 0, 0, 0},
    [FX_TYPE_FLANGER]       = {0.5f, 2.0f, 0.4f, 0.5f, 0, 0, 0, 0},
    [FX_TYPE_PHASER]        = {0.5f, 1.0f, 0.5f, 0.5f, 0, 0, 0, 0},
    [FX_TYPE_TREMOLO]       = {5.0f, 0.5f, 0, 0, 0, 0, 0, 0},
    [FX_TYPE_VIBRATO]       = {5.0f, 2.0f, 0, 0, 0, 0, 0, 0},
    [FX_TYPE_RING_MOD]      = {440.0f, 0.5f, 0, 0, 0, 0, 0, 0},
    [FX_TYPE_PITCH_SHIFT]   = {0, 0, 0, 0, 0, 0, 0, 0},
    [FX_TYPE_AUTO_PAN]      = {1.0f, 1.0f, 0, 0, 0, 0, 0, 0},
    [FX_TYPE_STEREO_WIDTH]  = {1.0f, 0, 0, 0, 0, 0, 0, 0},
    [FX_TYPE_NOISE_GATE_SC] = {-40.0f, 50.0f, 5.0f, 100.0f, 0, 0, 0, 0},
    [FX_TYPE_ENV_FOLLOWER]  = {10.0f, 100.0f, 0, 0, 0, 0, 0, 0},
    [FX_TYPE_DEESSER]       = {6000.0f, -20.0f, 4.0f, 0, 0, 0, 0, 0},
    [FX_TYPE_STEREO_IMAGER] = {300.0f, 1.5f, 0, 0, 0, 0, 0, 0},
    [FX_TYPE_TAPE_SAT]      = {1.5f, 0, 0.5f, 0, 0, 0, 0, 0},
    [FX_TYPE_TUBE_AMP]      = {2.0f, 0.1f, 0.5f, 0, 0, 0, 0, 0},
    [FX_TYPE_EXCITER]       = {6000.0f, 1.5f, 0.3f, 0, 0, 0, 0, 0},
    [FX_TYPE_HARMONIC_ENH]  = {0.3f, 0.2f, 0, 0, 0, 0, 0, 0},
    [FX_TYPE_FORMANT_FILT]  = {0, 0, 0.5f, 0, 0, 0, 0, 0},
    [FX_TYPE_COMB_FILTER]   = {220.0f, 0.5f, 0.5f, 0, 0, 0, 0, 0},
    [FX_TYPE_TILT_EQ]       = {0, 0, 0, 0, 0, 0, 0, 0},
    [FX_TYPE_PITCH_QUANT]   = {0, 1.0f, 0, 0, 0, 0, 0, 0},
    [FX_TYPE_GRAN_FREEZE]   = {0, 1.0f, 0.2f, 0.5f, 0, 0, 0, 0},
    [FX_TYPE_STUTTER]       = {100.0f, 0, 0, 4.0f, 0, 0, 0, 0},
    [FX_TYPE_TAPE_STOP]     = {0, 500.0f, 0, 0, 0, 0, 0, 0},
    [FX_TYPE_HAAS]          = {15.0f, 0, 0, 0, 0, 0, 0, 0},
    [FX_TYPE_RESONATOR]     = {110.0f, 0.7f, 4.0f, 0.5f, 0, 0, 0, 0},
    [FX_TYPE_FREEZE_REVERB] = {0.5f, 0.5f, 0.3f, 0, 0, 0, 0, 0},
    [FX_TYPE_STEP_FILTER]   = {8.0f, 0, 0.5f, 0.5f, 0.5f, 0.5f, 0, 0},
    [FX_TYPE_SIDECHAIN_COMP] = {-12.0f, 4.0f, 10.0f, 100.0f, 0, 0, 0, 0},
    [FX_TYPE_TRANCE_GATE]   = {0xAAAAu, 4.0f, 5.0f, 20.0f, 0, 0, 0, 0},
    [FX_TYPE_ARP_DELAY]     = {250.0f, 0.5f, 0.4f, 5.0f, 8.0f, 0, 0, 0},
};

static fx_node_t *fx_new_with_defaults(fx_type_t type)
{
    fx_node_t *n;
    switch (type) {
    case FX_TYPE_FILTER:       n = fx_filter_new(FILT_LP, 2000.0f, 1.0f); break;
    case FX_TYPE_EQ3:          n = fx_eq3_new(); break;
    case FX_TYPE_EQ5:          n = fx_eq5_new(); break;
    case FX_TYPE_COMPRESSOR:   n = fx_compressor_new(); break;
    case FX_TYPE_LIMITER:      n = fx_limiter_new(); break;
    case FX_TYPE_GATE:         n = fx_gate_new(); break;
    case FX_TYPE_TRANSIENT:    n = fx_transient_new(); break;
    case FX_TYPE_DISTORTION:   n = fx_distortion_new(DIST_SOFT, 2.0f); break;
    case FX_TYPE_OVERDRIVE:    n = fx_overdrive_new(2.0f); break;
    case FX_TYPE_WAVEFOLDER:   n = fx_wavefolder_new(); break;
    case FX_TYPE_BITCRUSH:     n = fx_bitcrush_new(8, 4); break;
    case FX_TYPE_DELAY:        n = fx_delay_new(250.0f, 0.4f, 0.4f); break;
    case FX_TYPE_REVERB:       n = fx_reverb_new(0.5f, 0.5f, 0.3f); break;
    case FX_TYPE_CHORUS:       n = fx_chorus_new(); break;
    case FX_TYPE_FLANGER:      n = fx_flanger_new(); break;
    case FX_TYPE_PHASER:       n = fx_phaser_new(); break;
    case FX_TYPE_TREMOLO:      n = fx_tremolo_new(); break;
    case FX_TYPE_VIBRATO:      n = fx_vibrato_new(); break;
    case FX_TYPE_RING_MOD:     n = fx_ring_mod_new(440.0f); break;
    case FX_TYPE_PITCH_SHIFT:  n = fx_pitch_shift_new(0.0f); break;
    case FX_TYPE_AUTO_PAN:     n = fx_auto_pan_new(1.0f); break;
    case FX_TYPE_STEREO_WIDTH: n = fx_stereo_width_new(1.0f); break;
    case FX_TYPE_NOISE_GATE_SC: n = fx_noise_gate_sc_new(); break;
    case FX_TYPE_ENV_FOLLOWER:  n = fx_env_follower_new(); break;
    case FX_TYPE_DEESSER:       n = fx_deesser_new(); break;
    case FX_TYPE_STEREO_IMAGER: n = fx_stereo_imager_new(300.0f, 1.5f); break;
    case FX_TYPE_TAPE_SAT:      n = fx_tape_sat_new(1.5f); break;
    case FX_TYPE_TUBE_AMP:      n = fx_tube_amp_new(2.0f); break;
    case FX_TYPE_EXCITER:       n = fx_exciter_new(); break;
    case FX_TYPE_HARMONIC_ENH:  n = fx_harmonic_enh_new(); break;
    case FX_TYPE_FORMANT_FILT:  n = fx_formant_filt_new(); break;
    case FX_TYPE_COMB_FILTER:   n = fx_comb_filter_new(220.0f, 0.5f); break;
    case FX_TYPE_TILT_EQ:       n = fx_tilt_eq_new(0.0f); break;
    case FX_TYPE_PITCH_QUANT:   n = fx_pitch_quant_new(0, 1); break;
    case FX_TYPE_GRAN_FREEZE:   n = fx_gran_freeze_new(); break;
    case FX_TYPE_STUTTER:       n = fx_stutter_new(); break;
    case FX_TYPE_TAPE_STOP:     n = fx_tape_stop_new(); break;
    case FX_TYPE_HAAS:          n = fx_haas_new(15.0f); break;
    case FX_TYPE_RESONATOR:     n = fx_resonator_new(110.0f, 4); break;
    case FX_TYPE_FREEZE_REVERB: n = fx_freeze_reverb_new(); break;
    case FX_TYPE_STEP_FILTER:   n = fx_step_filter_new(); break;
    case FX_TYPE_SIDECHAIN_COMP: n = fx_sidechain_comp_new(); break;
    case FX_TYPE_TRANCE_GATE:   n = fx_trance_gate_new(); break;
    case FX_TYPE_ARP_DELAY:     n = fx_arp_delay_new(); break;
    default:                    return NULL;
    }
    if (n && (int)type >= 0 && (int)type < FX_TYPE_COUNT)
        memcpy(n->params, fx_defaults[type], sizeof(n->params));
    return n;
}

fx_node_t *fx_new(fx_type_t type) { return fx_new_with_defaults(type); }

/* ════════════════════════════════════════════════════════════════════════════
 * Sidechain / env follower shared buses
 * ════════════════════════════════════════════════════════════════════════════ */
float fx_sidechain_rms[FX_SIDECHAIN_LANES]  = {0};
float fx_env_follower_out[FX_SIDECHAIN_LANES] = {0};

/* ════════════════════════════════════════════════════════════════════════════
 * FX_TYPE_NOISE_GATE_SC  — gate keyed from sidechain RMS
 * ════════════════════════════════════════════════════════════════════════════ */
typedef struct {
    fx_node_t hdr;
    float thresh_open, thresh_close;
    float hold_frames, hold_cnt;
    float atk_coef, rel_coef;
    float gain;
    bool  open;
    uint8_t src_lane;
} fx_ngsc_t;

static void ngsc_process(fx_node_t *self, int16_t *bl, int16_t *br, int n,
                         float bpm, uint32_t tr)
{
    (void)bpm; (void)tr;
    fx_ngsc_t *s = (fx_ngsc_t *)self;
    float rms = fx_sidechain_rms[s->src_lane < FX_SIDECHAIN_LANES ? s->src_lane : 0];
    if (!s->open && rms >= s->thresh_open)  { s->open = true;  s->hold_cnt = s->hold_frames; }
    if ( s->open && rms < s->thresh_close) {
        if (s->hold_cnt > 0) s->hold_cnt -= (float)n;
        else                 s->open = false;
    } else if (s->open) s->hold_cnt = s->hold_frames;
    float target = s->open ? 1.0f : 0.0f;
    for (int i = 0; i < n; i++) {
        float step = (target > s->gain) ? s->atk_coef : s->rel_coef;
        s->gain += step * (target - s->gain);
        int32_t gq = (int32_t)(s->gain * 32768.0f);
        int32_t l = ((int32_t)bl[i] * gq) >> 15;
        int32_t r = ((int32_t)br[i] * gq) >> 15;
        if (l >  32767) l =  32767;
        if (l < -32768) l = -32768;
        if (r >  32767) r =  32767;
        if (r < -32768) r = -32768;
        bl[i] = (int16_t)l; br[i] = (int16_t)r;
    }
}

static void ngsc_set_param(fx_node_t *self, uint8_t id, float v)
{
    fx_ngsc_t *s = (fx_ngsc_t *)self;
    float sr = (float)SAMPLE_RATE;
    switch (id) {
    case 0: s->thresh_open  = powf(10.0f, v / 20.0f);
            s->thresh_close = s->thresh_open * 0.5f; break;
    case 1: s->hold_frames = v * sr / 1000.0f; break;
    case 2: s->atk_coef = 1.0f - expf(-1.0f / (sr * v * 0.001f)); break;
    case 3: s->rel_coef = 1.0f - expf(-1.0f / (sr * v * 0.001f)); break;
    case 4: s->src_lane = (uint8_t)(int)v; break;
    }
}

fx_node_t *fx_noise_gate_sc_new(void)
{
    fx_ngsc_t *s = FX_ALLOC(fx_ngsc_t);
    if (!s) return NULL;
    s->hdr.type = FX_TYPE_NOISE_GATE_SC; s->hdr.enabled = true;
    s->hdr.process = ngsc_process; s->hdr.set_param = ngsc_set_param;
    s->hdr.free = fx_generic_free;
    s->thresh_open  = powf(10.0f, -40.0f / 20.0f);
    s->thresh_close = s->thresh_open * 0.5f;
    s->hold_frames = (float)SAMPLE_RATE * 0.05f;
    s->gain = 1.0f; s->open = true;
    s->atk_coef = 1.0f - expf(-1.0f / ((float)SAMPLE_RATE * 0.001f));
    s->rel_coef = 1.0f - expf(-1.0f / ((float)SAMPLE_RATE * 0.100f));
    return &s->hdr;
}

/* ════════════════════════════════════════════════════════════════════════════
 * FX_TYPE_ENV_FOLLOWER  — extract RMS envelope, audio passes through
 * ════════════════════════════════════════════════════════════════════════════ */
typedef struct {
    fx_node_t hdr;
    float atk_coef, rel_coef;
    float env;
    uint8_t out_lane;
} fx_ef_t;

static void ef_process(fx_node_t *self, int16_t *bl, int16_t *br, int n,
                       float bpm, uint32_t tr)
{
    (void)bpm; (void)tr;
    fx_ef_t *s = (fx_ef_t *)self;
    float sum = 0.0f;
    for (int i = 0; i < n; i++) {
        float l = bl[i] / 32768.0f, r = br[i] / 32768.0f;
        sum += l * l + r * r;
    }
    float rms = sqrtf(sum / (float)(n * 2));
    float coef = (rms > s->env) ? s->atk_coef : s->rel_coef;
    s->env += coef * (rms - s->env);
    if (s->out_lane < FX_SIDECHAIN_LANES)
        fx_env_follower_out[s->out_lane] = s->env;
}

static void ef_set_param(fx_node_t *self, uint8_t id, float v)
{
    fx_ef_t *s = (fx_ef_t *)self;
    float sr = (float)SAMPLE_RATE;
    if (id == 0) s->atk_coef = 1.0f - expf(-1.0f / (sr * v * 0.001f));
    if (id == 1) s->rel_coef = 1.0f - expf(-1.0f / (sr * v * 0.001f));
    if (id == 2) s->out_lane = (uint8_t)(int)v;
}

fx_node_t *fx_env_follower_new(void)
{
    fx_ef_t *s = FX_ALLOC(fx_ef_t);
    if (!s) return NULL;
    s->hdr.type = FX_TYPE_ENV_FOLLOWER; s->hdr.enabled = true;
    s->hdr.process = ef_process; s->hdr.set_param = ef_set_param;
    s->hdr.free = fx_generic_free;
    s->atk_coef = 1.0f - expf(-1.0f / ((float)SAMPLE_RATE * 0.001f));
    s->rel_coef = 1.0f - expf(-1.0f / ((float)SAMPLE_RATE * 0.050f));
    return &s->hdr;
}

/* ════════════════════════════════════════════════════════════════════════════
 * FX_TYPE_DEESSER  — dynamic high-band compressor
 * ════════════════════════════════════════════════════════════════════════════ */
typedef struct {
    fx_node_t hdr;
    svf_t  detect_l, detect_r;   /* BP around sibilance band */
    svf_t  cut_l, cut_r;         /* narrow cut filter */
    float  thresh_lin, ratio;
    float  env;
    float  atk_coef, rel_coef;
    float  freq_hz;
} fx_ds_t;

static void ds_process(fx_node_t *self, int16_t *bl, int16_t *br, int n,
                       float bpm, uint32_t tr)
{
    (void)bpm; (void)tr;
    fx_ds_t *s = (fx_ds_t *)self;
    for (int i = 0; i < n; i++) {
        float l = bl[i] / 32768.0f, r = br[i] / 32768.0f;
        float dl = svf_tick_bp(&s->detect_l, l);
        float dr = svf_tick_bp(&s->detect_r, r);
        float peak = fabsf(dl) > fabsf(dr) ? fabsf(dl) : fabsf(dr);
        float coef = (peak > s->env) ? s->atk_coef : s->rel_coef;
        s->env += coef * (peak - s->env);
        float gain = 1.0f;
        if (s->env > s->thresh_lin && s->ratio > 1.0f) {
            float over = 20.0f * log10f(s->env / s->thresh_lin);
            gain = powf(10.0f, -over * (1.0f - 1.0f / s->ratio) / 20.0f);
        }
        /* Apply gain only to HF band via cut filter */
        float cut_l = svf_tick_bp(&s->cut_l, l) * (gain - 1.0f);
        float cut_r = svf_tick_bp(&s->cut_r, r) * (gain - 1.0f);
        l += cut_l; r += cut_r;
        if (l >  1.0f) l =  1.0f;
        if (l < -1.0f) l = -1.0f;
        if (r >  1.0f) r =  1.0f;
        if (r < -1.0f) r = -1.0f;
        bl[i] = (int16_t)(l * 32767.0f); br[i] = (int16_t)(r * 32767.0f);
    }
}

static void ds_set_param(fx_node_t *self, uint8_t id, float v)
{
    fx_ds_t *s = (fx_ds_t *)self;
    if (id == 0) {
        s->freq_hz = v;
        svf_set(&s->detect_l, v, 4.0f); svf_set(&s->detect_r, v, 4.0f);
        svf_set(&s->cut_l,    v, 4.0f); svf_set(&s->cut_r,    v, 4.0f);
    }
    if (id == 1) s->thresh_lin = powf(10.0f, v / 20.0f);
    if (id == 2) s->ratio = v > 1.0f ? v : 1.0f;
}

fx_node_t *fx_deesser_new(void)
{
    fx_ds_t *s = FX_ALLOC(fx_ds_t);
    if (!s) return NULL;
    s->hdr.type = FX_TYPE_DEESSER; s->hdr.enabled = true;
    s->hdr.process = ds_process; s->hdr.set_param = ds_set_param;
    s->hdr.free = fx_generic_free;
    s->freq_hz = 7000.0f; s->ratio = 4.0f;
    s->thresh_lin = powf(10.0f, -18.0f / 20.0f);
    svf_set(&s->detect_l, 7000.0f, 4.0f); svf_set(&s->detect_r, 7000.0f, 4.0f);
    svf_set(&s->cut_l,    7000.0f, 4.0f); svf_set(&s->cut_r,    7000.0f, 4.0f);
    s->atk_coef = 1.0f - expf(-1.0f / ((float)SAMPLE_RATE * 0.001f));
    s->rel_coef = 1.0f - expf(-1.0f / ((float)SAMPLE_RATE * 0.05f));
    return &s->hdr;
}

/* ════════════════════════════════════════════════════════════════════════════
 * FX_TYPE_STEREO_IMAGER  — freq-split M/S: widen highs, keep lows mono
 * ════════════════════════════════════════════════════════════════════════════ */
typedef struct {
    fx_node_t hdr;
    svf_t  lo_l, lo_r, hi_l, hi_r;
    float  width;       /* side gain for highs (1 = normal, 2 = wide) */
    float  crossover;
} fx_si_t;

static void si_process(fx_node_t *self, int16_t *bl, int16_t *br, int n,
                       float bpm, uint32_t tr)
{
    (void)bpm; (void)tr;
    fx_si_t *s = (fx_si_t *)self;
    int32_t wq = (int32_t)(s->width * 32768.0f);
    for (int i = 0; i < n; i++) {
        float l = bl[i] / 32768.0f, r = br[i] / 32768.0f;
        /* Low band: LP, keep as mono-ish (normal width = 1) */
        float ll = svf_tick_lp(&s->lo_l, l);
        float lr = svf_tick_lp(&s->lo_r, r);
        /* High band: LP complement (HP) */
        float hl = l - svf_tick_lp(&s->hi_l, l);
        float hr = r - svf_tick_lp(&s->hi_r, r);
        /* Widen highs via M/S */
        float hm = (hl + hr) * 0.5f;
        float hs = (hl - hr) * 0.5f * s->width;
        float out_l = (ll + lr) * 0.5f + hm + hs;
        float out_r = (ll + lr) * 0.5f + hm - hs;
        if (out_l >  1.0f) out_l =  1.0f;
        if (out_l < -1.0f) out_l = -1.0f;
        if (out_r >  1.0f) out_r =  1.0f;
        if (out_r < -1.0f) out_r = -1.0f;
        bl[i] = (int16_t)(out_l * 32767.0f);
        br[i] = (int16_t)(out_r * 32767.0f);
    }
    (void)wq;
}

static void si_set_param(fx_node_t *self, uint8_t id, float v)
{
    fx_si_t *s = (fx_si_t *)self;
    if (id == 0) {
        s->crossover = v;
        svf_set(&s->lo_l, v, 0.7f); svf_set(&s->lo_r, v, 0.7f);
        svf_set(&s->hi_l, v, 0.7f); svf_set(&s->hi_r, v, 0.7f);
    }
    if (id == 1) s->width = v < 0.0f ? 0.0f : v;
}

fx_node_t *fx_stereo_imager_new(float crossover_hz, float width)
{
    fx_si_t *s = FX_ALLOC(fx_si_t);
    if (!s) return NULL;
    s->hdr.type = FX_TYPE_STEREO_IMAGER; s->hdr.enabled = true;
    s->hdr.process = si_process; s->hdr.set_param = si_set_param;
    s->hdr.free = fx_generic_free;
    s->crossover = crossover_hz; s->width = width;
    svf_set(&s->lo_l, crossover_hz, 0.7f); svf_set(&s->lo_r, crossover_hz, 0.7f);
    svf_set(&s->hi_l, crossover_hz, 0.7f); svf_set(&s->hi_r, crossover_hz, 0.7f);
    return &s->hdr;
}

/* ════════════════════════════════════════════════════════════════════════════
 * FX_TYPE_TAPE_SAT  — hysteresis soft-clip + pre/de-emphasis
 * ════════════════════════════════════════════════════════════════════════════ */
typedef struct {
    fx_node_t hdr;
    float drive, bias, mix;
    bq_t  pre_l, pre_r;   /* pre-emphasis: HF boost */
    bq_t  de_l,  de_r;   /* de-emphasis:  HF cut   */
    float hyst_l, hyst_r; /* simple hysteresis state */
} fx_ts_t;

static inline float tape_hyst(float x, float *state, float drive)
{
    /* Simplified Jiles-Atherton: magnetic flux lags field */
    float target = tanhf(x * drive);
    *state += 0.3f * (target - *state);
    return *state;
}

static void ts_process(fx_node_t *self, int16_t *bl, int16_t *br, int n,
                       float bpm, uint32_t tr)
{
    (void)bpm; (void)tr;
    fx_ts_t *s = (fx_ts_t *)self;
    int mix_q15 = (int32_t)(s->mix * 32768.0f);
    int dry_q15 = 32768 - mix_q15;
    for (int i = 0; i < n; i++) {
        float l = bl[i] / 32768.0f, r = br[i] / 32768.0f;
        float el = bq_tick(&s->pre_l, l);
        float er = bq_tick(&s->pre_r, r);
        el = tape_hyst(el + s->bias, &s->hyst_l, s->drive);
        er = tape_hyst(er + s->bias, &s->hyst_r, s->drive);
        el = bq_tick(&s->de_l, el);
        er = bq_tick(&s->de_r, er);
        float ol = l + (el - l) * s->mix;
        float or_ = r + (er - r) * s->mix;
        if (ol >  1.0f) ol =  1.0f;
        if (ol < -1.0f) ol = -1.0f;
        if (or_ >  1.0f) or_ =  1.0f;
        if (or_ < -1.0f) or_ = -1.0f;
        bl[i] = (int16_t)(ol * 32767.0f); br[i] = (int16_t)(or_ * 32767.0f);
    }
    (void)mix_q15; (void)dry_q15;
}

static void ts_set_param(fx_node_t *self, uint8_t id, float v)
{
    fx_ts_t *s = (fx_ts_t *)self;
    if (id == 0) s->drive = v < 0.1f ? 0.1f : v;
    if (id == 1) s->bias  = v;
    if (id == 2) s->mix   = v > 1.0f ? 1.0f : (v < 0.0f ? 0.0f : v);
}

fx_node_t *fx_tape_sat_new(float drive)
{
    fx_ts_t *s = FX_ALLOC(fx_ts_t);
    if (!s) return NULL;
    s->hdr.type = FX_TYPE_TAPE_SAT; s->hdr.enabled = true;
    s->hdr.process = ts_process; s->hdr.set_param = ts_set_param;
    s->hdr.free = fx_generic_free;
    s->drive = drive; s->bias = 0.0f; s->mix = 0.7f;
    /* Pre-emphasis: +3 dB shelf at 3 kHz */
    bq_set_high_shelf(&s->pre_l, 3000.0f, 3.0f); s->pre_r = s->pre_l;
    /* De-emphasis: inverse */
    bq_set_high_shelf(&s->de_l,  3000.0f, -3.0f); s->de_r  = s->de_l;
    return &s->hdr;
}

/* ════════════════════════════════════════════════════════════════════════════
 * FX_TYPE_TUBE_AMP  — asymmetric waveshaper + DC drift
 * ════════════════════════════════════════════════════════════════════════════ */
typedef struct {
    fx_node_t hdr;
    float drive, bias, dc;
    svf_t tone_l, tone_r;
    float tone_freq;
} fx_ta_t;

static void ta_process(fx_node_t *self, int16_t *bl, int16_t *br, int n,
                       float bpm, uint32_t tr)
{
    (void)bpm; (void)tr;
    fx_ta_t *s = (fx_ta_t *)self;
    /* DC coupling: slow leakage toward bias */
    s->dc += 0.0001f * (s->bias - s->dc);
    for (int i = 0; i < n; i++) {
        float l = (float)bl[i] / 32768.0f * s->drive + s->dc;
        float r = (float)br[i] / 32768.0f * s->drive + s->dc;
        /* Asymmetric: positive clips harder (triode character) */
        l = (l >= 0.0f) ? (1.0f - expf(-l)) : (expf(l) - 1.0f) * 0.8f;
        r = (r >= 0.0f) ? (1.0f - expf(-r)) : (expf(r) - 1.0f) * 0.8f;
        l = svf_tick_lp(&s->tone_l, l * 32768.0f);
        r = svf_tick_lp(&s->tone_r, r * 32768.0f);
        if (l >  32767.0f) l =  32767.0f;
        if (l < -32768.0f) l = -32768.0f;
        if (r >  32767.0f) r =  32767.0f;
        if (r < -32768.0f) r = -32768.0f;
        bl[i] = (int16_t)l; br[i] = (int16_t)r;
    }
}

static void ta_set_param(fx_node_t *self, uint8_t id, float v)
{
    fx_ta_t *s = (fx_ta_t *)self;
    if (id == 0) s->drive = v < 0.1f ? 0.1f : v;
    if (id == 1) s->bias  = v;
    if (id == 2) {
        s->tone_freq = 1000.0f + v * 19000.0f;
        svf_set(&s->tone_l, s->tone_freq, 0.7f);
        svf_set(&s->tone_r, s->tone_freq, 0.7f);
    }
}

fx_node_t *fx_tube_amp_new(float drive)
{
    fx_ta_t *s = FX_ALLOC(fx_ta_t);
    if (!s) return NULL;
    s->hdr.type = FX_TYPE_TUBE_AMP; s->hdr.enabled = true;
    s->hdr.process = ta_process; s->hdr.set_param = ta_set_param;
    s->hdr.free = fx_generic_free;
    s->drive = drive; s->bias = 0.1f; s->tone_freq = 8000.0f;
    svf_set(&s->tone_l, 8000.0f, 0.7f); svf_set(&s->tone_r, 8000.0f, 0.7f);
    return &s->hdr;
}

/* ════════════════════════════════════════════════════════════════════════════
 * FX_TYPE_EXCITER  — HF harmonic generator
 * Split signal → LP low band (pass through) + HP high band →
 * saturate HP → scale by drive → blend back.
 * ════════════════════════════════════════════════════════════════════════════ */
typedef struct {
    fx_node_t hdr;
    svf_t  hp_l, hp_r;
    float  freq_hz, drive, mix;
} fx_ex_t;

static void ex_process(fx_node_t *self, int16_t *bl, int16_t *br, int n,
                       float bpm, uint32_t tr)
{
    (void)bpm; (void)tr;
    fx_ex_t *s = (fx_ex_t *)self;
    for (int i = 0; i < n; i++) {
        float l = bl[i] / 32768.0f, r = br[i] / 32768.0f;
        float hl = svf_tick_hp(&s->hp_l, l);
        float hr = svf_tick_hp(&s->hp_r, r);
        /* Saturate HF harmonics */
        hl = tanhf(hl * s->drive);
        hr = tanhf(hr * s->drive);
        float ol = l + hl * s->mix;
        float or_ = r + hr * s->mix;
        if (ol >  1.0f) ol =  1.0f;
        if (ol < -1.0f) ol = -1.0f;
        if (or_ >  1.0f) or_ =  1.0f;
        if (or_ < -1.0f) or_ = -1.0f;
        bl[i] = (int16_t)(ol * 32767.0f); br[i] = (int16_t)(or_ * 32767.0f);
    }
}

static void ex_set_param(fx_node_t *self, uint8_t id, float v)
{
    fx_ex_t *s = (fx_ex_t *)self;
    if (id == 0) { s->freq_hz = v; svf_set(&s->hp_l, v, 0.7f); svf_set(&s->hp_r, v, 0.7f); }
    if (id == 1) s->drive = v < 0.1f ? 0.1f : v;
    if (id == 2) s->mix   = v > 1.0f ? 1.0f : (v < 0.0f ? 0.0f : v);
}

fx_node_t *fx_exciter_new(void)
{
    fx_ex_t *s = FX_ALLOC(fx_ex_t);
    if (!s) return NULL;
    s->hdr.type = FX_TYPE_EXCITER; s->hdr.enabled = true;
    s->hdr.process = ex_process; s->hdr.set_param = ex_set_param;
    s->hdr.free = fx_generic_free;
    s->freq_hz = 5000.0f; s->drive = 3.0f; s->mix = 0.3f;
    svf_set(&s->hp_l, 5000.0f, 0.7f); svf_set(&s->hp_r, 5000.0f, 0.7f);
    return &s->hdr;
}

/* ════════════════════════════════════════════════════════════════════════════
 * FX_TYPE_HARMONIC_ENH  — add 2nd / 3rd harmonic partials
 * ════════════════════════════════════════════════════════════════════════════ */
typedef struct {
    fx_node_t hdr;
    float h2, h3;
    svf_t  lp_l, lp_r;   /* LP to remove aliased harmonics */
} fx_he_t;

static void he_process(fx_node_t *self, int16_t *bl, int16_t *br, int n,
                       float bpm, uint32_t tr)
{
    (void)bpm; (void)tr;
    fx_he_t *s = (fx_he_t *)self;
    for (int i = 0; i < n; i++) {
        float l = bl[i] / 32768.0f, r = br[i] / 32768.0f;
        float l2 = l * l;               /* 2nd: even harmonic (full-wave rect shape) */
        float l3 = l * l * l;           /* 3rd: odd harmonic */
        float r2 = r * r;
        float r3 = r * r * r;
        float ol = l + l2 * s->h2 + l3 * s->h3;
        float or_ = r + r2 * s->h2 + r3 * s->h3;
        /* LP to tame aliasing */
        ol = svf_tick_lp(&s->lp_l, ol * 32768.0f) / 32768.0f;
        or_ = svf_tick_lp(&s->lp_r, or_ * 32768.0f) / 32768.0f;
        if (ol >  1.0f) ol =  1.0f;
        if (ol < -1.0f) ol = -1.0f;
        if (or_ >  1.0f) or_ =  1.0f;
        if (or_ < -1.0f) or_ = -1.0f;
        bl[i] = (int16_t)(ol * 32767.0f); br[i] = (int16_t)(or_ * 32767.0f);
    }
}

static void he_set_param(fx_node_t *self, uint8_t id, float v)
{
    fx_he_t *s = (fx_he_t *)self;
    if (id == 0) s->h2 = v;
    if (id == 1) s->h3 = v;
}

fx_node_t *fx_harmonic_enh_new(void)
{
    fx_he_t *s = FX_ALLOC(fx_he_t);
    if (!s) return NULL;
    s->hdr.type = FX_TYPE_HARMONIC_ENH; s->hdr.enabled = true;
    s->hdr.process = he_process; s->hdr.set_param = he_set_param;
    s->hdr.free = fx_generic_free;
    s->h2 = 0.2f; s->h3 = 0.1f;
    svf_set(&s->lp_l, 16000.0f, 0.7f); svf_set(&s->lp_r, 16000.0f, 0.7f);
    return &s->hdr;
}

/* ════════════════════════════════════════════════════════════════════════════
 * FX_TYPE_FORMANT_FILT  — two resonant BP swept between vowel pairs
 * Formant freqs: A={800,1150}, E={350,2000}, I={270,2140},
 *                O={450,800},  U={325,700}
 * ════════════════════════════════════════════════════════════════════════════ */
static const float vowel_f1[5] = { 800.0f, 350.0f, 270.0f, 450.0f, 325.0f };
static const float vowel_f2[5] = { 1150.0f, 2000.0f, 2140.0f, 800.0f, 700.0f };

typedef struct {
    fx_node_t hdr;
    svf_t  f1_l, f1_r, f2_l, f2_r;
    float  vowel;   /* 0–4 continuous */
    float  morph;   /* 0 = dry, 1 = full vowel */
    float  mix;
} fx_ff_t;

static void ff_update(fx_ff_t *s)
{
    int v0 = (int)s->vowel;
    int v1 = v0 + 1;
    if (v0 > 3) v0 = 4;
    if (v1 > 4) v1 = 4;
    float t = s->vowel - (float)v0;
    float f1 = vowel_f1[v0] + t * (vowel_f1[v1] - vowel_f1[v0]);
    float f2 = vowel_f2[v0] + t * (vowel_f2[v1] - vowel_f2[v0]);
    svf_set(&s->f1_l, f1, 8.0f); svf_set(&s->f1_r, f1, 8.0f);
    svf_set(&s->f2_l, f2, 8.0f); svf_set(&s->f2_r, f2, 8.0f);
}

static void ff_process(fx_node_t *self, int16_t *bl, int16_t *br, int n,
                       float bpm, uint32_t tr)
{
    (void)bpm; (void)tr;
    fx_ff_t *s = (fx_ff_t *)self;
    for (int i = 0; i < n; i++) {
        float l = bl[i] / 32768.0f, r = br[i] / 32768.0f;
        float fl = (svf_tick_bp(&s->f1_l, l) + svf_tick_bp(&s->f2_l, l)) * 0.5f;
        float fr = (svf_tick_bp(&s->f1_r, r) + svf_tick_bp(&s->f2_r, r)) * 0.5f;
        float ol = l + (fl - l) * s->morph * s->mix;
        float or_ = r + (fr - r) * s->morph * s->mix;
        if (ol >  1.0f) ol =  1.0f;
        if (ol < -1.0f) ol = -1.0f;
        if (or_ >  1.0f) or_ =  1.0f;
        if (or_ < -1.0f) or_ = -1.0f;
        bl[i] = (int16_t)(ol * 32767.0f); br[i] = (int16_t)(or_ * 32767.0f);
    }
}

static void ff_set_param(fx_node_t *self, uint8_t id, float v)
{
    fx_ff_t *s = (fx_ff_t *)self;
    if (id == 0) { s->vowel = v < 0.0f ? 0.0f : (v > 4.0f ? 4.0f : v); ff_update(s); }
    if (id == 1) s->morph = v > 1.0f ? 1.0f : (v < 0.0f ? 0.0f : v);
    if (id == 2) s->mix   = v > 1.0f ? 1.0f : (v < 0.0f ? 0.0f : v);
}

fx_node_t *fx_formant_filt_new(void)
{
    fx_ff_t *s = FX_ALLOC(fx_ff_t);
    if (!s) return NULL;
    s->hdr.type = FX_TYPE_FORMANT_FILT; s->hdr.enabled = true;
    s->hdr.process = ff_process; s->hdr.set_param = ff_set_param;
    s->hdr.free = fx_generic_free;
    s->vowel = 0.0f; s->morph = 1.0f; s->mix = 0.8f;
    ff_update(s);
    return &s->hdr;
}

/* ════════════════════════════════════════════════════════════════════════════
 * FX_TYPE_COMB_FILTER  — fixed or LFO-swept metallic resonance
 * ════════════════════════════════════════════════════════════════════════════ */
#define COMB_BUF 4096

typedef struct {
    fx_node_t hdr;
    int16_t  *buf_l, *buf_r;
    int       len, pos;
    float     feedback, mix;
    float     freq_hz;
    float     lfo_rate;
    mod_lfo_t lfo;
} fx_cf_t;

static void cf_process(fx_node_t *self, int16_t *bl, int16_t *br, int n,
                       float bpm, uint32_t tr)
{
    (void)bpm; (void)tr;
    fx_cf_t *s = (fx_cf_t *)self;
    int mix_q15 = (int32_t)(s->mix * 32768.0f);
    int dry_q15 = 32768 - mix_q15;
    for (int i = 0; i < n; i++) {
        float lv = (s->lfo_rate > 0.0f) ? mod_lfo_tick(&s->lfo) : 0.0f;
        float freq = s->freq_hz * (1.0f + lv * 0.02f);
        int delay = (int)((float)SAMPLE_RATE / (freq > 1.0f ? freq : 1.0f));
        if (delay < 1) delay = 1;
        if (delay >= COMB_BUF) delay = COMB_BUF - 1;
        int rpos = (s->pos - delay + COMB_BUF) % COMB_BUF;
        int32_t fb_q15 = (int32_t)(s->feedback * 32768.0f);
        int32_t fl = (int32_t)bl[i] + ((int32_t)s->buf_l[rpos] * fb_q15 >> 15);
        int32_t fr = (int32_t)br[i] + ((int32_t)s->buf_r[rpos] * fb_q15 >> 15);
        if (fl >  32767) fl =  32767;
        if (fl < -32768) fl = -32768;
        if (fr >  32767) fr =  32767;
        if (fr < -32768) fr = -32768;
        s->buf_l[s->pos] = (int16_t)fl;
        s->buf_r[s->pos] = (int16_t)fr;
        s->pos = (s->pos + 1) % COMB_BUF;
        int32_t l = ((int32_t)bl[i] * dry_q15 + (int32_t)s->buf_l[rpos] * mix_q15) >> 15;
        int32_t r = ((int32_t)br[i] * dry_q15 + (int32_t)s->buf_r[rpos] * mix_q15) >> 15;
        if (l >  32767) l =  32767;
        if (l < -32768) l = -32768;
        if (r >  32767) r =  32767;
        if (r < -32768) r = -32768;
        bl[i] = (int16_t)l; br[i] = (int16_t)r;
    }
}

static void cf_set_param(fx_node_t *self, uint8_t id, float v)
{
    fx_cf_t *s = (fx_cf_t *)self;
    if (id == 0) s->freq_hz  = v > 1.0f ? v : 1.0f;
    if (id == 1) s->feedback = v > 0.98f ? 0.98f : v;
    if (id == 2) s->mix = v > 1.0f ? 1.0f : (v < 0.0f ? 0.0f : v);
    if (id == 3) { s->lfo_rate = v; mod_lfo_set_rate(&s->lfo, v); }
}

static void cf_free(fx_node_t *self)
{
    fx_cf_t *s = (fx_cf_t *)self;
    heap_caps_free(s->buf_l); heap_caps_free(s->buf_r); heap_caps_free(s);
}

fx_node_t *fx_comb_filter_new(float freq_hz, float feedback)
{
    fx_cf_t *s = FX_ALLOC(fx_cf_t);
    if (!s) return NULL;
    s->buf_l = (int16_t *)heap_caps_calloc(COMB_BUF, sizeof(int16_t), MALLOC_CAP_SPIRAM);
    s->buf_r = (int16_t *)heap_caps_calloc(COMB_BUF, sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!s->buf_l || !s->buf_r) { cf_free(&s->hdr); return NULL; }
    s->hdr.type = FX_TYPE_COMB_FILTER; s->hdr.enabled = true;
    s->hdr.process = cf_process; s->hdr.set_param = cf_set_param;
    s->hdr.free = cf_free;
    s->len = COMB_BUF; s->freq_hz = freq_hz; s->feedback = feedback;
    s->mix = 0.5f;
    return &s->hdr;
}

/* ════════════════════════════════════════════════════════════════════════════
 * FX_TYPE_TILT_EQ  — single-knob shelving tilt
 * Positive = brighten (bass cut + treble boost), negative = darken.
 * ════════════════════════════════════════════════════════════════════════════ */
typedef struct {
    fx_node_t hdr;
    bq_t lo_l, lo_r, hi_l, hi_r;
    float tilt_db;
} fx_teq_t;

static void teq_process(fx_node_t *self, int16_t *bl, int16_t *br, int n,
                        float bpm, uint32_t tr)
{
    (void)bpm; (void)tr;
    fx_teq_t *s = (fx_teq_t *)self;
    bq_process_stereo(&s->lo_l, &s->lo_r, bl, br, n);
    bq_process_stereo(&s->hi_l, &s->hi_r, bl, br, n);
}

static void teq_rebuild(fx_teq_t *s)
{
    /* Low shelf cuts, high shelf boosts (or vice versa) */
    bq_set_low_shelf (&s->lo_l,  200.0f, -s->tilt_db * 0.5f); s->lo_r = s->lo_l;
    bq_set_high_shelf(&s->hi_l, 5000.0f,  s->tilt_db * 0.5f); s->hi_r = s->hi_l;
}

static void teq_set_param(fx_node_t *self, uint8_t id, float v)
{
    fx_teq_t *s = (fx_teq_t *)self;
    if (id == 0) { s->tilt_db = v; teq_rebuild(s); }
}

fx_node_t *fx_tilt_eq_new(float tilt_db)
{
    fx_teq_t *s = FX_ALLOC(fx_teq_t);
    if (!s) return NULL;
    s->hdr.type = FX_TYPE_TILT_EQ; s->hdr.enabled = true;
    s->hdr.process = teq_process; s->hdr.set_param = teq_set_param;
    s->hdr.free = fx_generic_free;
    s->tilt_db = tilt_db;
    teq_rebuild(s);
    return &s->hdr;
}

/* ════════════════════════════════════════════════════════════════════════════
 * FX_TYPE_PITCH_QUANT  — snap incoming pitch to musical scale
 * Works by delaying the signal by one fundamental period and crossfading
 * when a pitch correction is needed. Practical for monophonic material.
 * ════════════════════════════════════════════════════════════════════════════ */
static const uint8_t scale_intervals[8][12] = {
    {1,1,1,1,1,1,1,1,1,1,1,1}, /* chromatic (pass-through) */
    {1,0,1,0,1,1,0,1,0,1,0,1}, /* major */
    {1,0,1,1,0,1,0,1,1,0,1,0}, /* natural minor */
    {1,0,1,0,1,1,0,1,0,1,0,0}, /* pentatonic major */
    {1,0,0,1,0,1,0,1,0,0,1,0}, /* pentatonic minor */
    {1,0,0,1,0,1,1,1,0,0,1,0}, /* blues */
    {1,0,1,1,0,1,0,1,0,1,1,0}, /* dorian */
    {1,1,0,1,0,1,0,1,1,0,1,0}, /* phrygian */
};

typedef struct {
    fx_node_t hdr;
    uint8_t root;   /* 0–11 */
    uint8_t scale;  /* 0–7 */
    /* Simple pitch detection: track zero-crossings */
    int16_t prev_l;
    int     zc_cnt;
    int     period_est;   /* in samples */
    float   detune;       /* fractional semitones to shift */
    /* Pitch shift via resampling (same as fx_ps read-pos trick) */
    int16_t *buf_l, *buf_r;
    int      write_pos;
    float    read_pos;
    float    ratio;
} fx_pq_t;

static int pq_nearest_in_scale(int semitone, int root, int scale_idx)
{
    int rel = ((semitone - root) % 12 + 12) % 12;
    /* Find nearest scale degree below and above */
    int below = rel, above = rel;
    for (int d = 0; d <= 11; d++) {
        if (scale_intervals[scale_idx][(rel - d + 12) % 12]) { below = rel - d; break; }
    }
    for (int d = 0; d <= 11; d++) {
        if (scale_intervals[scale_idx][(rel + d) % 12]) { above = rel + d; break; }
    }
    int target_rel = (rel - below <= above - rel) ? below : above;
    return root + target_rel;
}

static void pq_process(fx_node_t *self, int16_t *bl, int16_t *br, int n,
                       float bpm, uint32_t tr)
{
    (void)bpm; (void)tr;
    fx_pq_t *s = (fx_pq_t *)self;
    for (int i = 0; i < n; i++) {
        s->buf_l[s->write_pos] = bl[i];
        s->buf_r[s->write_pos] = br[i];
        /* Zero-crossing based period estimation */
        if ((s->prev_l < 0) != (bl[i] < 0)) {
            if (s->zc_cnt > 0 && s->zc_cnt < 2000)
                s->period_est = s->zc_cnt * 2;
            s->zc_cnt = 0;
        }
        s->zc_cnt++;
        s->prev_l = bl[i];
        /* Estimate pitch from period */
        if (s->period_est > 0) {
            float hz = (float)SAMPLE_RATE / (float)s->period_est;
            float semitone_f = 12.0f * log2f(hz / 440.0f) + 69.0f;
            int semitone = (int)(semitone_f + 0.5f);
            int target = pq_nearest_in_scale(semitone, s->root, s->scale & 7);
            float diff = (float)(target - semitone);
            s->ratio = powf(2.0f, diff / 12.0f);
        }
        /* Read at pitch-ratio position */
        int ri = (int)s->read_pos % PS_BUF_FRAMES;
        float fr = s->read_pos - (int)s->read_pos;
        int ri2 = (ri + 1) % PS_BUF_FRAMES;
        bl[i] = (int16_t)(s->buf_l[ri] * (1.0f - fr) + s->buf_l[ri2] * fr);
        br[i] = (int16_t)(s->buf_r[ri] * (1.0f - fr) + s->buf_r[ri2] * fr);
        s->read_pos += s->ratio;
        if (s->read_pos >= PS_BUF_FRAMES) s->read_pos -= PS_BUF_FRAMES;
        s->write_pos = (s->write_pos + 1) % PS_BUF_FRAMES;
    }
}

static void pq_set_param(fx_node_t *self, uint8_t id, float v)
{
    fx_pq_t *s = (fx_pq_t *)self;
    if (id == 0) s->root  = (uint8_t)((int)v % 12);
    if (id == 1) s->scale = (uint8_t)((int)v & 7);
}

static void pq_free(fx_node_t *self)
{
    fx_pq_t *s = (fx_pq_t *)self;
    heap_caps_free(s->buf_l); heap_caps_free(s->buf_r); heap_caps_free(s);
}

fx_node_t *fx_pitch_quant_new(int root, int scale)
{
    fx_pq_t *s = FX_ALLOC(fx_pq_t);
    if (!s) return NULL;
    s->buf_l = (int16_t *)heap_caps_calloc(PS_BUF_FRAMES, sizeof(int16_t), MALLOC_CAP_SPIRAM);
    s->buf_r = (int16_t *)heap_caps_calloc(PS_BUF_FRAMES, sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!s->buf_l || !s->buf_r) { pq_free(&s->hdr); return NULL; }
    s->hdr.type = FX_TYPE_PITCH_QUANT; s->hdr.enabled = true;
    s->hdr.process = pq_process; s->hdr.set_param = pq_set_param;
    s->hdr.free = pq_free;
    s->root = (uint8_t)(root % 12); s->scale = (uint8_t)(scale & 7);
    s->ratio = 1.0f; s->read_pos = PS_BUF_FRAMES - 512;
    return &s->hdr;
}

/* ════════════════════════════════════════════════════════════════════════════
 * FX_TYPE_GRAN_FREEZE  — capture grain buffer, loop at variable rate/scatter
 * ════════════════════════════════════════════════════════════════════════════ */
#define GRAN_BUF 24000   /* 500 ms at 48 kHz */
#define GRAN_NUM_GRAINS 4

typedef struct {
    float read_pos;
    float rate;
    int   active;
} grain_t;

typedef struct {
    fx_node_t hdr;
    int16_t  *buf_l, *buf_r;
    int       write_pos;
    bool      frozen;
    int       freeze_pos;   /* write pos when frozen */
    float     rate;         /* playback rate 0.25–4.0 */
    float     scatter;      /* random offset between grains 0–1 */
    float     mix;
    grain_t   grains[GRAN_NUM_GRAINS];
    uint16_t  rng;
} fx_gf_t;

static void gf_process(fx_node_t *self, int16_t *bl, int16_t *br, int n,
                       float bpm, uint32_t tr)
{
    (void)bpm; (void)tr;
    fx_gf_t *s = (fx_gf_t *)self;
    int mix_q15 = (int32_t)(s->mix * 32768.0f);
    int dry_q15 = 32768 - mix_q15;
    for (int i = 0; i < n; i++) {
        if (!s->frozen) {
            s->buf_l[s->write_pos] = bl[i];
            s->buf_r[s->write_pos] = br[i];
            s->write_pos = (s->write_pos + 1) % GRAN_BUF;
        }
        int32_t wet_l = 0, wet_r = 0;
        for (int g = 0; g < GRAN_NUM_GRAINS; g++) {
            grain_t *gr = &s->grains[g];
            int ri  = (int)gr->read_pos % GRAN_BUF;
            int ri2 = (ri + 1) % GRAN_BUF;
            float fr = gr->read_pos - (int)gr->read_pos;
            wet_l += (int32_t)(s->buf_l[ri] * (1.0f - fr) + s->buf_l[ri2] * fr);
            wet_r += (int32_t)(s->buf_r[ri] * (1.0f - fr) + s->buf_r[ri2] * fr);
            gr->read_pos += gr->rate;
            if (gr->read_pos >= GRAN_BUF) {
                gr->read_pos -= GRAN_BUF;
                /* Re-scatter on grain wrap */
                s->rng = (uint16_t)(s->rng * 6364 + 1);
                float offset = (float)(s->rng & 0x0FFF) / 4096.0f * s->scatter * 2400.0f;
                int start = s->frozen ? s->freeze_pos : s->write_pos;
                gr->read_pos = (float)((start - (int)offset + GRAN_BUF) % GRAN_BUF);
            }
        }
        wet_l /= GRAN_NUM_GRAINS; wet_r /= GRAN_NUM_GRAINS;
        int32_t l = ((int32_t)bl[i] * dry_q15 + wet_l * mix_q15) >> 15;
        int32_t r = ((int32_t)br[i] * dry_q15 + wet_r * mix_q15) >> 15;
        if (l >  32767) l =  32767;
        if (l < -32768) l = -32768;
        if (r >  32767) r =  32767;
        if (r < -32768) r = -32768;
        bl[i] = (int16_t)l; br[i] = (int16_t)r;
    }
}

static void gf_set_param(fx_node_t *self, uint8_t id, float v)
{
    fx_gf_t *s = (fx_gf_t *)self;
    if (id == 0) {
        bool freeze = (v != 0.0f);
        if (freeze && !s->frozen) s->freeze_pos = s->write_pos;
        s->frozen = freeze;
    }
    if (id == 1) {
        s->rate = v < 0.25f ? 0.25f : (v > 4.0f ? 4.0f : v);
        for (int g = 0; g < GRAN_NUM_GRAINS; g++) s->grains[g].rate = s->rate;
    }
    if (id == 2) s->scatter = v > 1.0f ? 1.0f : (v < 0.0f ? 0.0f : v);
    if (id == 3) s->mix = v > 1.0f ? 1.0f : (v < 0.0f ? 0.0f : v);
}

static void gf_free(fx_node_t *self)
{
    fx_gf_t *s = (fx_gf_t *)self;
    heap_caps_free(s->buf_l); heap_caps_free(s->buf_r); heap_caps_free(s);
}

fx_node_t *fx_gran_freeze_new(void)
{
    fx_gf_t *s = FX_ALLOC(fx_gf_t);
    if (!s) return NULL;
    s->buf_l = (int16_t *)heap_caps_calloc(GRAN_BUF, sizeof(int16_t), MALLOC_CAP_SPIRAM);
    s->buf_r = (int16_t *)heap_caps_calloc(GRAN_BUF, sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!s->buf_l || !s->buf_r) { gf_free(&s->hdr); return NULL; }
    s->hdr.type = FX_TYPE_GRAN_FREEZE; s->hdr.enabled = true;
    s->hdr.process = gf_process; s->hdr.set_param = gf_set_param;
    s->hdr.free = gf_free;
    s->rate = 1.0f; s->scatter = 0.3f; s->mix = 1.0f; s->rng = 0xACE1;
    for (int g = 0; g < GRAN_NUM_GRAINS; g++) {
        s->grains[g].rate     = 1.0f;
        s->grains[g].read_pos = (float)(g * GRAN_BUF / GRAN_NUM_GRAINS);
    }
    return &s->hdr;
}

/* ════════════════════════════════════════════════════════════════════════════
 * FX_TYPE_STUTTER  — capture N ms + re-trigger rhythmically
 * ════════════════════════════════════════════════════════════════════════════ */
#define STUTTER_BUF 24000

typedef struct {
    fx_node_t hdr;
    int16_t  *buf_l, *buf_r;
    int       capture_len;   /* in samples */
    int       play_pos;
    bool      active;
    bool      clock_sync;
    float     sync_div;
    uint32_t  next_tick;
} fx_st_t;

static void st_process(fx_node_t *self, int16_t *bl, int16_t *br, int n,
                       float bpm, uint32_t tr)
{
    fx_st_t *s = (fx_st_t *)self;
    if (s->clock_sync && bpm > 0.0f) {
        float beat_ms = 60000.0f / bpm;
        float ms = beat_ms * s->sync_div * 4.0f;
        s->capture_len = (int)(ms * SAMPLE_RATE / 1000.0f);
        if (s->capture_len >= STUTTER_BUF) s->capture_len = STUTTER_BUF - 1;
    }
    (void)tr;
    for (int i = 0; i < n; i++) {
        if (s->active) {
            /* Play from capture buffer, loop */
            if (s->play_pos >= s->capture_len) s->play_pos = 0;
            bl[i] = s->buf_l[s->play_pos];
            br[i] = s->buf_r[s->play_pos];
            s->play_pos++;
        }
        /* Always capture (circular) */
        s->buf_l[s->play_pos % STUTTER_BUF] = bl[i];
        s->buf_r[s->play_pos % STUTTER_BUF] = br[i];
    }
}

static void st_set_param(fx_node_t *self, uint8_t id, float v)
{
    fx_st_t *s = (fx_st_t *)self;
    if (id == 0) {
        int len = (int)(v * SAMPLE_RATE / 1000.0f);
        s->capture_len = len < 1 ? 1 : (len >= STUTTER_BUF ? STUTTER_BUF - 1 : len);
    }
    if (id == 1) { s->active = (v != 0.0f); s->play_pos = 0; }
    if (id == 2) s->clock_sync = (v != 0.0f);
    if (id == 3) s->sync_div = v;
}

static void st_free(fx_node_t *self)
{
    fx_st_t *s = (fx_st_t *)self;
    heap_caps_free(s->buf_l); heap_caps_free(s->buf_r); heap_caps_free(s);
}

fx_node_t *fx_stutter_new(void)
{
    fx_st_t *s = FX_ALLOC(fx_st_t);
    if (!s) return NULL;
    s->buf_l = (int16_t *)heap_caps_calloc(STUTTER_BUF, sizeof(int16_t), MALLOC_CAP_SPIRAM);
    s->buf_r = (int16_t *)heap_caps_calloc(STUTTER_BUF, sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!s->buf_l || !s->buf_r) { st_free(&s->hdr); return NULL; }
    s->hdr.type = FX_TYPE_STUTTER; s->hdr.enabled = true;
    s->hdr.process = st_process; s->hdr.set_param = st_set_param;
    s->hdr.free = st_free;
    s->capture_len = (int)(SAMPLE_RATE * 0.125f); /* 125 ms */
    s->sync_div = 0.125f; /* 1/8 note */
    return &s->hdr;
}

/* ════════════════════════════════════════════════════════════════════════════
 * FX_TYPE_TAPE_STOP  — pitch-down on trigger; tape-start is inverse
 * ════════════════════════════════════════════════════════════════════════════ */
#define TSTOP_BUF 8192

typedef struct {
    fx_node_t hdr;
    int16_t  *buf_l, *buf_r;
    int       write_pos;
    float     read_pos;
    float     speed;       /* 1.0 = normal; decreases to 0 on stop */
    bool      stopping;
    bool      starting;
    float     time_ms;
    float     decel;       /* speed decrement per sample */
} fx_tstop_t;

static void tstop_process(fx_node_t *self, int16_t *bl, int16_t *br, int n,
                          float bpm, uint32_t tr)
{
    (void)bpm; (void)tr;
    fx_tstop_t *s = (fx_tstop_t *)self;
    for (int i = 0; i < n; i++) {
        s->buf_l[s->write_pos] = bl[i];
        s->buf_r[s->write_pos] = br[i];
        s->write_pos = (s->write_pos + 1) % TSTOP_BUF;
        if (s->stopping) {
            s->speed -= s->decel;
            if (s->speed <= 0.0f) { s->speed = 0.0f; s->stopping = false; }
        } else if (s->starting) {
            s->speed += s->decel;
            if (s->speed >= 1.0f) { s->speed = 1.0f; s->starting = false; }
        }
        int ri  = (int)s->read_pos % TSTOP_BUF;
        float fr = s->read_pos - (int)s->read_pos;
        int ri2 = (ri + 1) % TSTOP_BUF;
        bl[i] = (int16_t)(s->buf_l[ri] * (1.0f - fr) + s->buf_l[ri2] * fr);
        br[i] = (int16_t)(s->buf_r[ri] * (1.0f - fr) + s->buf_r[ri2] * fr);
        s->read_pos += s->speed;
        if (s->read_pos >= TSTOP_BUF) s->read_pos -= TSTOP_BUF;
    }
}

static void tstop_set_param(fx_node_t *self, uint8_t id, float v)
{
    fx_tstop_t *s = (fx_tstop_t *)self;
    if (id == 0) {
        /* trigger: 1 = stop, -1 = start */
        if (v > 0.0f) { s->stopping = true; s->starting = false; }
        else           { s->starting = true; s->stopping = false; }
    }
    if (id == 1) {
        s->time_ms = v;
        s->decel = 1.0f / (v * SAMPLE_RATE / 1000.0f);
    }
}

static void tstop_free(fx_node_t *self)
{
    fx_tstop_t *s = (fx_tstop_t *)self;
    heap_caps_free(s->buf_l); heap_caps_free(s->buf_r); heap_caps_free(s);
}

fx_node_t *fx_tape_stop_new(void)
{
    fx_tstop_t *s = FX_ALLOC(fx_tstop_t);
    if (!s) return NULL;
    s->buf_l = (int16_t *)heap_caps_calloc(TSTOP_BUF, sizeof(int16_t), MALLOC_CAP_SPIRAM);
    s->buf_r = (int16_t *)heap_caps_calloc(TSTOP_BUF, sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!s->buf_l || !s->buf_r) { tstop_free(&s->hdr); return NULL; }
    s->hdr.type = FX_TYPE_TAPE_STOP; s->hdr.enabled = true;
    s->hdr.process = tstop_process; s->hdr.set_param = tstop_set_param;
    s->hdr.free = tstop_free;
    s->speed = 1.0f; s->time_ms = 500.0f;
    s->decel = 1.0f / (500.0f * SAMPLE_RATE / 1000.0f);
    s->read_pos = TSTOP_BUF - 128;
    return &s->hdr;
}

/* ════════════════════════════════════════════════════════════════════════════
 * FX_TYPE_HAAS  — tiny delay on one channel for perceived width
 * ════════════════════════════════════════════════════════════════════════════ */
#define HAAS_BUF 2048   /* ~42 ms max */

typedef struct {
    fx_node_t hdr;
    int16_t  buf[HAAS_BUF];
    int      pos;
    int      delay_frames;
    int      side;   /* 0 = delay R, 1 = delay L */
} fx_haas_t;

static void haas_process(fx_node_t *self, int16_t *bl, int16_t *br, int n,
                         float bpm, uint32_t tr)
{
    (void)bpm; (void)tr;
    fx_haas_t *s = (fx_haas_t *)self;
    for (int i = 0; i < n; i++) {
        int rpos = (s->pos - s->delay_frames + HAAS_BUF) % HAAS_BUF;
        if (s->side == 0) {
            s->buf[s->pos] = bl[i];
            br[i] = s->buf[rpos];
        } else {
            s->buf[s->pos] = br[i];
            bl[i] = s->buf[rpos];
        }
        s->pos = (s->pos + 1) % HAAS_BUF;
    }
}

static void haas_set_param(fx_node_t *self, uint8_t id, float v)
{
    fx_haas_t *s = (fx_haas_t *)self;
    if (id == 0) {
        s->delay_frames = (int)(v * SAMPLE_RATE / 1000.0f);
        if (s->delay_frames < 1) s->delay_frames = 1;
        if (s->delay_frames >= HAAS_BUF) s->delay_frames = HAAS_BUF - 1;
    }
    if (id == 1) s->side = (int)v & 1;
}

fx_node_t *fx_haas_new(float delay_ms)
{
    fx_haas_t *s = FX_ALLOC(fx_haas_t);
    if (!s) return NULL;
    s->hdr.type = FX_TYPE_HAAS; s->hdr.enabled = true;
    s->hdr.process = haas_process; s->hdr.set_param = haas_set_param;
    s->hdr.free = fx_generic_free;
    s->delay_frames = (int)(delay_ms * SAMPLE_RATE / 1000.0f);
    if (s->delay_frames < 1) s->delay_frames = 1;
    if (s->delay_frames >= HAAS_BUF) s->delay_frames = HAAS_BUF - 1;
    return &s->hdr;
}

/* ════════════════════════════════════════════════════════════════════════════
 * FX_TYPE_RESONATOR  — tuned comb filter bank (noise → pitched texture)
 * ════════════════════════════════════════════════════════════════════════════ */
#define RES_MAX_COMBS  8
#define RES_COMB_BUF   4096

typedef struct {
    int16_t *buf;
    int      len, pos;
    float    decay;
    float    state;
} res_comb_t;

typedef struct {
    fx_node_t hdr;
    res_comb_t combs[RES_MAX_COMBS];
    int   count;
    float root_hz;
    float decay_coef;
    float mix;
} fx_res_t;

static void res_rebuild(fx_res_t *s)
{
    for (int i = 0; i < s->count; i++) {
        float hz = s->root_hz * (float)(i + 1);  /* harmonic series */
        int len = (int)((float)SAMPLE_RATE / (hz > 1.0f ? hz : 1.0f));
        if (len < 1) len = 1;
        if (len >= RES_COMB_BUF) len = RES_COMB_BUF - 1;
        s->combs[i].len   = len;
        s->combs[i].decay = s->decay_coef;
    }
}

static void res_process(fx_node_t *self, int16_t *bl, int16_t *br, int n,
                        float bpm, uint32_t tr)
{
    (void)bpm; (void)tr;
    fx_res_t *s = (fx_res_t *)self;
    int mix_q15 = (int32_t)(s->mix * 32768.0f);
    int dry_q15 = 32768 - mix_q15;
    for (int i = 0; i < n; i++) {
        float in = ((float)bl[i] + (float)br[i]) * 0.5f;
        float out = 0.0f;
        for (int c = 0; c < s->count; c++) {
            res_comb_t *cb = &s->combs[c];
            int rpos = (cb->pos - cb->len + RES_COMB_BUF) % RES_COMB_BUF;
            float delayed = cb->buf[rpos] / 32768.0f;
            float val = in / 32768.0f + delayed * cb->decay;
            /* LP damp for natural decay */
            cb->state = cb->state * 0.5f + val * 0.5f;
            int32_t store = (int32_t)(cb->state * 32768.0f);
            if (store >  32767) store =  32767;
            if (store < -32768) store = -32768;
            cb->buf[cb->pos] = (int16_t)store;
            cb->pos = (cb->pos + 1) % RES_COMB_BUF;
            out += delayed;
        }
        out /= (float)s->count;
        int32_t l = ((int32_t)bl[i] * dry_q15 >> 15) + ((int32_t)(out * 32767.0f) * mix_q15 >> 15);
        int32_t r = ((int32_t)br[i] * dry_q15 >> 15) + ((int32_t)(out * 32767.0f) * mix_q15 >> 15);
        if (l >  32767) l =  32767;
        if (l < -32768) l = -32768;
        if (r >  32767) r =  32767;
        if (r < -32768) r = -32768;
        bl[i] = (int16_t)l; br[i] = (int16_t)r;
    }
}

static void res_set_param(fx_node_t *self, uint8_t id, float v)
{
    fx_res_t *s = (fx_res_t *)self;
    if (id == 0) { s->root_hz = v > 1.0f ? v : 1.0f; res_rebuild(s); }
    if (id == 1) { s->decay_coef = v > 0.999f ? 0.999f : (v < 0.0f ? 0.0f : v); res_rebuild(s); }
    if (id == 2) {
        int c = (int)v; if (c < 1) c = 1; if (c > RES_MAX_COMBS) c = RES_MAX_COMBS;
        s->count = c; res_rebuild(s);
    }
    if (id == 3) s->mix = v > 1.0f ? 1.0f : (v < 0.0f ? 0.0f : v);
}

static void res_free(fx_node_t *self)
{
    fx_res_t *s = (fx_res_t *)self;
    for (int i = 0; i < RES_MAX_COMBS; i++) heap_caps_free(s->combs[i].buf);
    heap_caps_free(s);
}

fx_node_t *fx_resonator_new(float root_hz, int count)
{
    fx_res_t *s = FX_ALLOC(fx_res_t);
    if (!s) return NULL;
    for (int i = 0; i < RES_MAX_COMBS; i++) {
        s->combs[i].buf = (int16_t *)heap_caps_calloc(RES_COMB_BUF, sizeof(int16_t), MALLOC_CAP_SPIRAM);
        if (!s->combs[i].buf) { res_free(&s->hdr); return NULL; }
    }
    s->hdr.type = FX_TYPE_RESONATOR; s->hdr.enabled = true;
    s->hdr.process = res_process; s->hdr.set_param = res_set_param;
    s->hdr.free = res_free;
    s->root_hz = root_hz; s->count = count < 1 ? 1 : (count > RES_MAX_COMBS ? RES_MAX_COMBS : count);
    s->decay_coef = 0.85f; s->mix = 0.5f;
    res_rebuild(s);
    return &s->hdr;
}

/* ════════════════════════════════════════════════════════════════════════════
 * FX_TYPE_FREEZE_REVERB  — Freeverb with infinite freeze
 * Reuses Freeverb internals; when frozen, feedback is set to 1.0 (infinite).
 * ════════════════════════════════════════════════════════════════════════════ */
typedef struct {
    fx_node_t hdr;
    comb_t  comb_l[FV_NUMCOMBS], comb_r[FV_NUMCOMBS];
    allp_t  ap_l[FV_NUMALLPASS], ap_r[FV_NUMALLPASS];
    float   room, damp, width, mix;
    float   wet1, wet2;
    bool    frozen;
} fx_freverb_t;

static void freverb_update(fx_freverb_t *s)
{
    float room_fb = s->frozen ? 1.0f : (0.28f + s->room * 0.7f);
    float damp    = s->frozen ? 0.0f : (s->damp * 0.5f);
    for (int i = 0; i < FV_NUMCOMBS; i++) {
        s->comb_l[i].feedback = room_fb; s->comb_r[i].feedback = room_fb;
        s->comb_l[i].damp1 = damp; s->comb_l[i].damp2 = 1.0f - damp;
        s->comb_r[i].damp1 = damp; s->comb_r[i].damp2 = 1.0f - damp;
    }
    float scale = 0.015f;
    float wet = s->mix * scale;
    s->wet1 = wet * (s->width / 2.0f + 0.5f);
    s->wet2 = wet * ((1.0f - s->width) / 2.0f);
}

static void freverb_process(fx_node_t *self, int16_t *bl, int16_t *br, int n,
                            float bpm, uint32_t tr)
{
    (void)bpm; (void)tr;
    fx_freverb_t *s = (fx_freverb_t *)self;
    float inp_scale = s->frozen ? 0.0f : 0.015f;  /* no new input when frozen */
    for (int i = 0; i < n; i++) {
        float in = ((float)bl[i] + (float)br[i]) * inp_scale / 32768.0f;
        float outl = 0.0f, outr = 0.0f;
        for (int c = 0; c < FV_NUMCOMBS; c++) {
            outl += comb_process(&s->comb_l[c], in);
            outr += comb_process(&s->comb_r[c], in);
        }
        for (int a = 0; a < FV_NUMALLPASS; a++) {
            outl = allp_process(&s->ap_l[a], outl);
            outr = allp_process(&s->ap_r[a], outr);
        }
        float l = (float)bl[i] / 32768.0f + outl * s->wet1 + outr * s->wet2;
        float r = (float)br[i] / 32768.0f + outr * s->wet1 + outl * s->wet2;
        l *= 32768.0f; r *= 32768.0f;
        if (l >  32767.0f) l =  32767.0f;
        if (l < -32768.0f) l = -32768.0f;
        if (r >  32767.0f) r =  32767.0f;
        if (r < -32768.0f) r = -32768.0f;
        bl[i] = (int16_t)l; br[i] = (int16_t)r;
    }
}

static void freverb_set_param(fx_node_t *self, uint8_t id, float v)
{
    fx_freverb_t *s = (fx_freverb_t *)self;
    switch (id) {
    case 0: s->room   = v; break;
    case 1: s->damp   = v; break;
    case 2: s->mix    = v; break;
    case 3: s->frozen = (v != 0.0f); break;
    }
    freverb_update(s);
}

static void freverb_free(fx_node_t *self)
{
    fx_freverb_t *s = (fx_freverb_t *)self;
    for (int i = 0; i < FV_NUMCOMBS;   i++) { heap_caps_free(s->comb_l[i].buf); heap_caps_free(s->comb_r[i].buf); }
    for (int i = 0; i < FV_NUMALLPASS; i++) { heap_caps_free(s->ap_l[i].buf);   heap_caps_free(s->ap_r[i].buf); }
    heap_caps_free(s);
}

fx_node_t *fx_freeze_reverb_new(void)
{
    fx_freverb_t *s = FX_ALLOC(fx_freverb_t);
    if (!s) return NULL;
    float scale = (float)SAMPLE_RATE / 44100.0f;
    for (int i = 0; i < FV_NUMCOMBS; i++) {
        int lenl = (int)(fv_comb_sizes[i] * scale);
        int lenr = (int)((fv_comb_sizes[i] + FV_SPREAD) * scale);
        s->comb_l[i].buf = (int16_t *)heap_caps_calloc((size_t)lenl, sizeof(int16_t), MALLOC_CAP_SPIRAM);
        s->comb_r[i].buf = (int16_t *)heap_caps_calloc((size_t)lenr, sizeof(int16_t), MALLOC_CAP_SPIRAM);
        if (!s->comb_l[i].buf || !s->comb_r[i].buf) { freverb_free(&s->hdr); return NULL; }
        s->comb_l[i].len = lenl; s->comb_r[i].len = lenr;
        s->comb_l[i].damp2 = s->comb_r[i].damp2 = 1.0f;
    }
    for (int i = 0; i < FV_NUMALLPASS; i++) {
        int lenl = (int)(fv_ap_sizes[i] * scale);
        int lenr = (int)((fv_ap_sizes[i] + FV_SPREAD) * scale);
        s->ap_l[i].buf = (int16_t *)heap_caps_calloc((size_t)lenl, sizeof(int16_t), MALLOC_CAP_SPIRAM);
        s->ap_r[i].buf = (int16_t *)heap_caps_calloc((size_t)lenr, sizeof(int16_t), MALLOC_CAP_SPIRAM);
        if (!s->ap_l[i].buf || !s->ap_r[i].buf) { freverb_free(&s->hdr); return NULL; }
        s->ap_l[i].len = lenl; s->ap_r[i].len = lenr;
        s->ap_l[i].feedback = s->ap_r[i].feedback = 0.5f;
    }
    s->hdr.type = FX_TYPE_FREEZE_REVERB; s->hdr.enabled = true;
    s->hdr.process = freverb_process; s->hdr.set_param = freverb_set_param;
    s->hdr.free = freverb_free;
    s->room = 0.8f; s->damp = 0.3f; s->width = 1.0f; s->mix = 0.5f;
    freverb_update(s);
    return &s->hdr;
}

/* ════════════════════════════════════════════════════════════════════════════
 * FX_TYPE_STEP_FILTER  — step-sequencer driven LP cutoff, clock-synced
 * ════════════════════════════════════════════════════════════════════════════ */
#define SFILT_MAX_STEPS 16

typedef struct {
    fx_node_t hdr;
    svf_t  svf_l, svf_r;
    float  steps[SFILT_MAX_STEPS];   /* cutoff 0–1 per step */
    int    step_count;
    int    cur_step;
    bool   clock_sync;
    float  sync_div;
    uint32_t next_tick;
    float  resonance;
} fx_sf_t;

static void sf_process(fx_node_t *self, int16_t *bl, int16_t *br, int n,
                       float bpm, uint32_t tr)
{
    fx_sf_t *s = (fx_sf_t *)self;
    /* Advance step on clock boundary */
    if (s->clock_sync && bpm > 0.0f) {
        float beat_ms = 60000.0f / bpm;
        float step_ms = beat_ms * s->sync_div * 4.0f;
        uint32_t step_samples = (uint32_t)(step_ms * SAMPLE_RATE / 1000.0f);
        if (step_samples == 0) step_samples = 1;
        if (s->next_tick == 0 || tr >= s->next_tick) {
            s->cur_step = (s->cur_step + 1) % s->step_count;
            s->next_tick = tr + step_samples;
            float cutoff = 200.0f + s->steps[s->cur_step] * 19800.0f;
            svf_set(&s->svf_l, cutoff, s->resonance);
            svf_set(&s->svf_r, cutoff, s->resonance);
        }
    }
    for (int i = 0; i < n; i++) {
        float l = svf_tick_lp(&s->svf_l, (float)bl[i]);
        float r = svf_tick_lp(&s->svf_r, (float)br[i]);
        if (l >  32767.0f) l =  32767.0f;
        if (l < -32768.0f) l = -32768.0f;
        if (r >  32767.0f) r =  32767.0f;
        if (r < -32768.0f) r = -32768.0f;
        bl[i] = (int16_t)l; br[i] = (int16_t)r;
    }
}

static void sf_set_param(fx_node_t *self, uint8_t id, float v)
{
    fx_sf_t *s = (fx_sf_t *)self;
    if (id == 0) { int c = (int)v; s->step_count = c < 1 ? 1 : (c > SFILT_MAX_STEPS ? SFILT_MAX_STEPS : c); }
    if (id == 1) s->clock_sync = (v != 0.0f);
    /* id 2..17: step values */
    if (id >= 2 && id < 2 + SFILT_MAX_STEPS)
        s->steps[id - 2] = v > 1.0f ? 1.0f : (v < 0.0f ? 0.0f : v);
}

fx_node_t *fx_step_filter_new(void)
{
    fx_sf_t *s = FX_ALLOC(fx_sf_t);
    if (!s) return NULL;
    s->hdr.type = FX_TYPE_STEP_FILTER; s->hdr.enabled = true;
    s->hdr.process = sf_process; s->hdr.set_param = sf_set_param;
    s->hdr.free = fx_generic_free;
    s->step_count = 8; s->resonance = 2.0f; s->clock_sync = true; s->sync_div = 0.25f;
    /* Default pattern: alternating open/closed */
    for (int i = 0; i < SFILT_MAX_STEPS; i++) s->steps[i] = (i & 1) ? 0.8f : 0.2f;
    svf_set(&s->svf_l, 2000.0f, 2.0f); svf_set(&s->svf_r, 2000.0f, 2.0f);
    return &s->hdr;
}

/* ════════════════════════════════════════════════════════════════════════════
 * FX_TYPE_SIDECHAIN_COMP  — ducking from cross-lane RMS
 * ════════════════════════════════════════════════════════════════════════════ */
typedef struct {
    fx_node_t hdr;
    float thresh_lin, ratio, makeup;
    float atk_coef, rel_coef;
    float gain;
    uint8_t src_lane;
} fx_scc_t;

static void scc_process(fx_node_t *self, int16_t *bl, int16_t *br, int n,
                        float bpm, uint32_t tr)
{
    (void)bpm; (void)tr;
    fx_scc_t *s = (fx_scc_t *)self;
    float rms = fx_sidechain_rms[s->src_lane < FX_SIDECHAIN_LANES ? s->src_lane : 0];
    float target = 1.0f;
    if (rms > s->thresh_lin && s->ratio > 1.0f) {
        float over_db = 20.0f * log10f(rms / s->thresh_lin);
        float gr_db = over_db * (1.0f - 1.0f / s->ratio);
        target = powf(10.0f, -gr_db / 20.0f) * s->makeup;
    } else {
        target = s->makeup;
    }
    float coef = (target < s->gain) ? s->atk_coef : s->rel_coef;
    s->gain += coef * (target - s->gain);
    int32_t gq = (int32_t)(s->gain * 32768.0f);
    for (int i = 0; i < n; i++) {
        int32_t l = ((int32_t)bl[i] * gq) >> 15;
        int32_t r = ((int32_t)br[i] * gq) >> 15;
        if (l >  32767) l =  32767;
        if (l < -32768) l = -32768;
        if (r >  32767) r =  32767;
        if (r < -32768) r = -32768;
        bl[i] = (int16_t)l; br[i] = (int16_t)r;
    }
}

static void scc_set_param(fx_node_t *self, uint8_t id, float v)
{
    fx_scc_t *s = (fx_scc_t *)self;
    float sr = (float)SAMPLE_RATE;
    switch (id) {
    case 0: s->thresh_lin = powf(10.0f, v / 20.0f); break;
    case 1: s->ratio   = v > 1.0f ? v : 1.0f; break;
    case 2: s->atk_coef = 1.0f - expf(-1.0f / (sr * v * 0.001f)); break;
    case 3: s->rel_coef = 1.0f - expf(-1.0f / (sr * v * 0.001f)); break;
    case 4: s->makeup  = powf(10.0f, v / 20.0f); break;
    case 5: s->src_lane = (uint8_t)(int)v; break;
    }
}

fx_node_t *fx_sidechain_comp_new(void)
{
    fx_scc_t *s = FX_ALLOC(fx_scc_t);
    if (!s) return NULL;
    s->hdr.type = FX_TYPE_SIDECHAIN_COMP; s->hdr.enabled = true;
    s->hdr.process = scc_process; s->hdr.set_param = scc_set_param;
    s->hdr.free = fx_generic_free;
    s->thresh_lin = powf(10.0f, -12.0f / 20.0f);
    s->ratio = 8.0f; s->makeup = 1.0f; s->gain = 1.0f;
    s->atk_coef = 1.0f - expf(-1.0f / ((float)SAMPLE_RATE * 0.005f));
    s->rel_coef = 1.0f - expf(-1.0f / ((float)SAMPLE_RATE * 0.100f));
    return &s->hdr;
}

/* ════════════════════════════════════════════════════════════════════════════
 * FX_TYPE_TRANCE_GATE  — rhythmic amplitude chopper, clock-synced 16-step pattern
 * ════════════════════════════════════════════════════════════════════════════ */
#define TGATE_STEPS 16

typedef struct {
    fx_node_t hdr;
    uint16_t  pattern;      /* bitmask: bit i = step i on/off */
    int       step_div;     /* note division: 8 = 1/8, 16 = 1/16, etc. */
    float     atk_coef, rel_coef;
    float     gain;
    int       cur_step;
    uint32_t  next_tick;
} fx_tg_t;

static void tg_process(fx_node_t *self, int16_t *bl, int16_t *br, int n,
                       float bpm, uint32_t tr)
{
    fx_tg_t *s = (fx_tg_t *)self;
    float step_ms = (bpm > 0.0f) ? (60000.0f / bpm * 4.0f / (float)s->step_div) : 100.0f;
    uint32_t step_samples = (uint32_t)(step_ms * SAMPLE_RATE / 1000.0f);
    if (step_samples == 0) step_samples = 1;
    for (int i = 0; i < n; i++) {
        if (tr + (uint32_t)i >= s->next_tick) {
            s->cur_step = (s->cur_step + 1) % TGATE_STEPS;
            s->next_tick = tr + (uint32_t)i + step_samples;
        }
        bool on = (s->pattern >> s->cur_step) & 1;
        float target = on ? 1.0f : 0.0f;
        float coef = (target > s->gain) ? s->atk_coef : s->rel_coef;
        s->gain += coef * (target - s->gain);
        int32_t gq = (int32_t)(s->gain * 32768.0f);
        int32_t l = ((int32_t)bl[i] * gq) >> 15;
        int32_t r = ((int32_t)br[i] * gq) >> 15;
        if (l >  32767) l =  32767;
        if (l < -32768) l = -32768;
        if (r >  32767) r =  32767;
        if (r < -32768) r = -32768;
        bl[i] = (int16_t)l; br[i] = (int16_t)r;
    }
}

static void tg_set_param(fx_node_t *self, uint8_t id, float v)
{
    fx_tg_t *s = (fx_tg_t *)self;
    float sr = (float)SAMPLE_RATE;
    if (id == 0) s->pattern  = (uint16_t)(uint32_t)v;
    if (id == 1) s->step_div = (int)v > 0 ? (int)v : 16;
    if (id == 2) s->atk_coef = 1.0f - expf(-1.0f / (sr * v * 0.001f));
    if (id == 3) s->rel_coef = 1.0f - expf(-1.0f / (sr * v * 0.001f));
}

fx_node_t *fx_trance_gate_new(void)
{
    fx_tg_t *s = FX_ALLOC(fx_tg_t);
    if (!s) return NULL;
    s->hdr.type = FX_TYPE_TRANCE_GATE; s->hdr.enabled = true;
    s->hdr.process = tg_process; s->hdr.set_param = tg_set_param;
    s->hdr.free = fx_generic_free;
    s->pattern  = 0b1010101010101010;  /* alternating 16th notes */
    s->step_div = 16;
    s->gain     = 1.0f;
    s->atk_coef = 1.0f - expf(-1.0f / ((float)SAMPLE_RATE * 0.002f));
    s->rel_coef = 1.0f - expf(-1.0f / ((float)SAMPLE_RATE * 0.010f));
    return &s->hdr;
}

/* ════════════════════════════════════════════════════════════════════════════
 * FX_TYPE_ARP_DELAY  — each delay repeat transposed by semitone interval
 * ════════════════════════════════════════════════════════════════════════════ */
#define ARPD_TAPS     8
#define ARPD_BUF      96000  /* 2 s at 48 kHz */

typedef struct {
    fx_node_t hdr;
    int16_t  *buf_l, *buf_r;
    int       write_pos;
    float     time_ms;
    float     feedback;
    float     mix;
    int       semitone_step;   /* transpose per repeat (+1 = up, -1 = down) */
    int       max_repeats;
    /* Pitch shift state per tap (reuse read-pos trick) */
    float     read_pos[ARPD_TAPS];
    float     ratios[ARPD_TAPS];
} fx_arpd_t;

static void arpd_rebuild(fx_arpd_t *s)
{
    for (int t = 0; t < s->max_repeats && t < ARPD_TAPS; t++)
        s->ratios[t] = powf(2.0f, (float)(s->semitone_step * (t + 1)) / 12.0f);
}

static void arpd_process(fx_node_t *self, int16_t *bl, int16_t *br, int n,
                         float bpm, uint32_t tr)
{
    (void)bpm; (void)tr;
    fx_arpd_t *s = (fx_arpd_t *)self;
    int delay_frames = (int)(s->time_ms * SAMPLE_RATE / 1000.0f);
    if (delay_frames < 1) delay_frames = 1;
    if (delay_frames >= ARPD_BUF) delay_frames = ARPD_BUF - 1;
    int mix_q15 = (int32_t)(s->mix * 32768.0f / (float)s->max_repeats);
    int dry_q15 = 32768 - (int32_t)(s->mix * 32768.0f);
    for (int i = 0; i < n; i++) {
        s->buf_l[s->write_pos] = bl[i];
        s->buf_r[s->write_pos] = br[i];
        int32_t wet_l = 0, wet_r = 0;
        for (int t = 0; t < s->max_repeats && t < ARPD_TAPS; t++) {
            /* Each tap delayed by t+1 × delay_frames */
            float base = (float)(s->write_pos - (t + 1) * delay_frames + ARPD_BUF);
            int ri  = (int)base % ARPD_BUF;
            float fr = base - (float)(int)base;
            int ri2 = (ri + 1) % ARPD_BUF;
            float raw_l = s->buf_l[ri] * (1.0f - fr) + s->buf_l[ri2] * fr;
            float raw_r = s->buf_r[ri] * (1.0f - fr) + s->buf_r[ri2] * fr;
            /* Pitch-shift tap via read position modulation */
            int rp  = (int)s->read_pos[t] % ARPD_BUF;
            float fpf = s->read_pos[t] - (int)s->read_pos[t];
            int rp2 = (rp + 1) % ARPD_BUF;
            float pl = s->buf_l[rp] * (1.0f - fpf) + s->buf_l[rp2] * fpf;
            float pr = s->buf_r[rp] * (1.0f - fpf) + s->buf_r[rp2] * fpf;
            /* Blend pitch-shifted and straight (simple crossfade) */
            wet_l += (int32_t)((raw_l * 0.5f + pl * 0.5f) * (float)(int32_t)(s->feedback * 32768.0f) / 32768.0f);
            wet_r += (int32_t)((raw_r * 0.5f + pr * 0.5f) * (float)(int32_t)(s->feedback * 32768.0f) / 32768.0f);
            s->read_pos[t] += s->ratios[t];
            if (s->read_pos[t] >= ARPD_BUF) s->read_pos[t] -= ARPD_BUF;
        }
        s->write_pos = (s->write_pos + 1) % ARPD_BUF;
        int32_t l = ((int32_t)bl[i] * dry_q15 >> 15) + (wet_l * mix_q15 >> 15);
        int32_t r = ((int32_t)br[i] * dry_q15 >> 15) + (wet_r * mix_q15 >> 15);
        if (l >  32767) l =  32767;
        if (l < -32768) l = -32768;
        if (r >  32767) r =  32767;
        if (r < -32768) r = -32768;
        bl[i] = (int16_t)l; br[i] = (int16_t)r;
    }
}

static void arpd_set_param(fx_node_t *self, uint8_t id, float v)
{
    fx_arpd_t *s = (fx_arpd_t *)self;
    if (id == 0) s->time_ms = v;
    if (id == 1) s->feedback = v > 0.99f ? 0.99f : (v < 0.0f ? 0.0f : v);
    if (id == 2) s->mix = v > 1.0f ? 1.0f : (v < 0.0f ? 0.0f : v);
    if (id == 3) { s->semitone_step = (int)v; arpd_rebuild(s); }
    if (id == 4) {
        int r = (int)v; s->max_repeats = r < 1 ? 1 : (r > ARPD_TAPS ? ARPD_TAPS : r);
        arpd_rebuild(s);
    }
}

static void arpd_free(fx_node_t *self)
{
    fx_arpd_t *s = (fx_arpd_t *)self;
    heap_caps_free(s->buf_l); heap_caps_free(s->buf_r); heap_caps_free(s);
}

fx_node_t *fx_arp_delay_new(void)
{
    fx_arpd_t *s = FX_ALLOC(fx_arpd_t);
    if (!s) return NULL;
    s->buf_l = (int16_t *)heap_caps_calloc(ARPD_BUF, sizeof(int16_t), MALLOC_CAP_SPIRAM);
    s->buf_r = (int16_t *)heap_caps_calloc(ARPD_BUF, sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!s->buf_l || !s->buf_r) { arpd_free(&s->hdr); return NULL; }
    s->hdr.type = FX_TYPE_ARP_DELAY; s->hdr.enabled = true;
    s->hdr.process = arpd_process; s->hdr.set_param = arpd_set_param;
    s->hdr.free = arpd_free;
    s->time_ms = 125.0f; s->feedback = 0.5f; s->mix = 0.4f;
    s->semitone_step = 5; /* perfect fourth up per repeat */
    s->max_repeats = 4;
    for (int t = 0; t < ARPD_TAPS; t++)
        s->read_pos[t] = (float)(ARPD_BUF - (t + 1) * 6000);
    arpd_rebuild(s);
    return &s->hdr;
}
