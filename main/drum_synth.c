/*
 * drum_synth.c — Analog drum synthesizer lane (TR-808 style)
 *
 * See drum_synth.h for the model.  All synthesis is float at audio rate; with
 * at most DSYN_MAX_ROWS voices and only a handful ringing at once this is well
 * within the ESP32 audio block budget (the subtractive/physical synths already
 * run float render paths).  Voices decay exponentially and self-deactivate, so
 * an idle lane costs almost nothing.
 */

#include <string.h>
#include <math.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "audio.h"          /* SAMPLE_RATE */
#include "drum_synth.h"

static const char *TAG = "drum_synth";

#define SR        ((float)SAMPLE_RATE)
#define OUT_SCALE 26000.0f          /* mono [-1,1] → int16 with mix headroom */

/* The classic 808 metallic cluster: six mutually-inharmonic square oscillators
 * shared by the hats and the cymbal. */
static const float k_cluster[DSYN_NUM_OSC] =
    { 205.3f, 304.4f, 369.6f, 522.7f, 540.0f, 800.0f };

/* ── Small DSP helpers ───────────────────────────────────────────────────── */

static inline float lerp(float a, float b, float t)
{
    if (t < 0.0f) t = 0.0f; else if (t > 1.0f) t = 1.0f;
    return a + (b - a) * t;
}

/* Per-sample exponential decay coefficient that reaches ~ -60 dB in `sec`. */
static inline float decay_coef(float sec)
{
    if (sec < 0.001f) sec = 0.001f;
    return expf(-6.9078f / (sec * SR));   /* ln(1000) ≈ 6.9078 → -60 dB */
}

/* Sine from the shared wavetable, phase in [0,1) → [-1,1]. */
static inline float dsyn_sin(float ph01)
{
    uint8_t i = (uint8_t)((int)(ph01 * (float)SV_WT_SIZE)) & (SV_WT_SIZE - 1);
    return sv_wt[SV_WAVE_SINE][i] * (1.0f / 32768.0f);
}

/* Band-limited-ish square from phase. */
static inline float dsyn_sq(float ph01)
{
    return ph01 < 0.5f ? 1.0f : -1.0f;
}

/* xorshift32 white noise → [-1,1]. */
static inline float dsyn_noise(dsyn_state_t *s)
{
    uint32_t x = s->lfsr;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    s->lfsr = x;
    return (float)(int32_t)x * (1.0f / 2147483648.0f);
}

static inline float wrap1(float p) { return p - (float)(int)p; }

/* ── Allocation / defaults ───────────────────────────────────────────────── */

const char *dsyn_voice_name(dsyn_voice_t t)
{
    switch (t) {
    case DSYN_BD: return "BD"; case DSYN_SD: return "SD";
    case DSYN_LT: return "LT"; case DSYN_MT: return "MT"; case DSYN_HT: return "HT";
    case DSYN_LC: return "LC"; case DSYN_MC: return "MC"; case DSYN_HC: return "HC";
    case DSYN_RS: return "RS"; case DSYN_CP: return "CP"; case DSYN_CB: return "CB";
    case DSYN_CY: return "CY"; case DSYN_OH: return "OH"; case DSYN_CH: return "CH";
    case DSYN_MA: return "MA"; case DSYN_CL: return "CL";
    default:      return "??";
    }
}

void dsyn_row_set_voice(dsyn_t *ds, int ri, dsyn_voice_t type)
{
    if (!ds || ri < 0 || ri >= DSYN_MAX_ROWS) return;
    if (type >= DSYN_VOICE_COUNT) type = DSYN_BD;

    dsyn_params_t *p = &ds->params[ri];
    /* Sensible default knob set — a usable 808 voice out of the box. */
    p->type  = type;
    p->tune  = 0.5f;
    p->decay = 0.5f;
    p->tone  = 0.5f;
    p->snap  = 0.5f;
    p->drive = 0.0f;
    p->level = 0.85f;
    p->pan   = 0.0f;

    switch (type) {
    case DSYN_BD: p->decay = 0.55f; p->tune = 0.30f; break;
    case DSYN_SD: p->decay = 0.40f; p->snap = 0.55f; break;
    case DSYN_CH: p->decay = 0.15f; p->tone = 0.60f; break;
    case DSYN_OH: p->decay = 0.45f; p->tone = 0.60f; break;
    case DSYN_CY: p->decay = 0.75f; p->tone = 0.55f; break;
    case DSYN_CP: p->decay = 0.40f; break;
    case DSYN_CB: p->decay = 0.45f; break;
    default: break;
    }
}

dsyn_t *dsyn_alloc(void)
{
    dsyn_t *ds = heap_caps_calloc(1, sizeof(dsyn_t), MALLOC_CAP_SPIRAM);
    if (!ds) { ESP_LOGE(TAG, "dsyn alloc failed"); return NULL; }

    ds->step_count  = 16;
    ds->accent_gain = 1.5f;

    /* Seed a default 4-piece kit: BD / SD / CH / OH. */
    static const dsyn_voice_t kit[4] = { DSYN_BD, DSYN_SD, DSYN_CH, DSYN_OH };
    for (int r = 0; r < 4; r++) {
        dsyn_row_set_voice(ds, r, kit[r]);
        ds->state[r].lfsr = 0x1234567u + (uint32_t)r * 0x9e3779b9u;
    }
    /* Basic four-on-the-floor-ish seed so a fresh lane makes sound. */
    for (int s = 0; s < 16; s += 4) ds->steps[0][s].velocity = 100;   /* BD  */
    ds->steps[1][4].velocity = 100; ds->steps[1][12].velocity = 100;  /* SD  */
    for (int s = 0; s < 16; s += 2) ds->steps[2][s].velocity = 80;     /* CH  */
    ds->steps[3][2].velocity = 70;  ds->steps[3][10].velocity = 70;    /* OH  */

    for (int r = 0; r < DSYN_MAX_ROWS; r++) {
        if (ds->state[r].lfsr == 0) ds->state[r].lfsr = 0x2545F491u + (uint32_t)r;
    }
    ds->row_count = 4;
    return ds;
}

void dsyn_update_timing(dsyn_t *ds, uint32_t loop_len_ticks)
{
    if (!ds || ds->step_count == 0) return;
    ds->ticks_per_step = loop_len_ticks / ds->step_count;
}

void dsyn_reset(dsyn_t *ds, uint32_t lane_tick_now)
{
    if (!ds) return;
    uint8_t  sc  = ds->step_count ? ds->step_count : 16;
    uint32_t tps = ds->ticks_per_step;
    for (int r = 0; r < DSYN_MAX_ROWS; r++) {
        ds->cur_step[r]       = sc - 1;
        ds->last_step_tick[r] = lane_tick_now - tps;
    }
}

/* ── Voice trigger ───────────────────────────────────────────────────────── */

static void voice_trigger(dsyn_state_t *s, const dsyn_params_t *p, float vellevel)
{
    /* Preserve the running noise seed across hits. */
    uint32_t seed = s->lfsr ? s->lfsr : 0x2545F491u;
    memset(s, 0, sizeof(*s));
    s->lfsr = seed;

    s->active = true;
    s->vel    = vellevel;
    s->pan_l  = 1.0f - (p->pan > 0.0f ? p->pan : 0.0f);
    s->pan_r  = 1.0f + (p->pan < 0.0f ? p->pan : 0.0f);

    s->amp = 1.0f; s->amp2 = 1.0f; s->pitch_env = 1.0f;
    s->pitch_coef = 1.0f;          /* default: no pitch sweep                  */

    const float tune  = p->tune, decay = p->decay, tone = p->tone, snap = p->snap;

    switch (p->type) {
    case DSYN_BD: {
        s->base_freq  = lerp(35.0f, 90.0f, tune);
        s->sweep_hz   = s->base_freq * (2.0f + 6.0f * snap);
        s->pitch_coef = decay_coef(0.030f + 0.04f * snap);
        s->amp_coef   = decay_coef(lerp(0.08f, 1.20f, decay));
        s->amp2_coef  = decay_coef(0.004f);                    /* click attack */
        break;
    }
    case DSYN_SD: {
        s->freq[0]   = lerp(160.0f, 260.0f, tune);
        s->freq[1]   = s->freq[0] * 1.78f;
        s->amp2_coef = decay_coef(lerp(0.05f, 0.16f, decay));  /* body         */
        s->amp_coef  = decay_coef(lerp(0.06f, 0.28f, snap));   /* noise tail   */
        sv_svf_set(&s->f1, lerp(900.0f, 5000.0f, tone), 1.2f); /* noise HP-ish */
        break;
    }
    case DSYN_LT: case DSYN_MT: case DSYN_HT:
    case DSYN_LC: case DSYN_MC: case DSYN_HC: {
        float base, dmin, dmax, swp;
        switch (p->type) {
        case DSYN_LT: base = lerp(70.0f, 110.0f, tune);  dmin=0.15f; dmax=0.60f; swp=0.8f; break;
        case DSYN_MT: base = lerp(110.0f,170.0f, tune);  dmin=0.13f; dmax=0.50f; swp=0.8f; break;
        case DSYN_HT: base = lerp(160.0f,250.0f, tune);  dmin=0.11f; dmax=0.42f; swp=0.8f; break;
        case DSYN_LC: base = lerp(150.0f,220.0f, tune);  dmin=0.08f; dmax=0.28f; swp=0.5f; break;
        case DSYN_MC: base = lerp(220.0f,320.0f, tune);  dmin=0.07f; dmax=0.24f; swp=0.5f; break;
        default:      base = lerp(330.0f,480.0f, tune);  dmin=0.06f; dmax=0.20f; swp=0.5f; break;
        }
        s->base_freq  = base;
        s->sweep_hz   = base * (swp * (0.5f + snap));
        s->pitch_coef = decay_coef(0.05f);
        s->amp_coef   = decay_coef(lerp(dmin, dmax, decay));
        s->amp2_coef  = decay_coef(0.003f);
        break;
    }
    case DSYN_RS: {
        s->freq[0]   = 1700.0f * lerp(0.85f, 1.20f, tune);
        s->freq[1]   = 520.0f  * lerp(0.85f, 1.20f, tune);
        s->amp_coef  = decay_coef(lerp(0.020f, 0.045f, decay));
        s->amp2_coef = decay_coef(0.006f);                     /* noise click  */
        break;
    }
    case DSYN_CP: {
        sv_svf_set(&s->f1, lerp(800.0f, 1600.0f, tone), 2.0f); /* band-pass    */
        s->amp_coef  = decay_coef(0.012f);                     /* burst stage  */
        s->amp2_coef = decay_coef(lerp(0.10f, 0.45f, decay));  /* reverb tail  */
        s->burst     = 3;
        s->burst_t   = 0.010f;
        break;
    }
    case DSYN_CB: {
        float sc = lerp(0.85f, 1.20f, tune);
        s->freq[0]  = 540.0f * sc;
        s->freq[1]  = 800.0f * sc;
        sv_svf_set(&s->f1, 2640.0f, 1.5f);                     /* band-pass    */
        s->amp_coef = decay_coef(lerp(0.10f, 0.55f, decay));
        break;
    }
    case DSYN_CY: case DSYN_OH: case DSYN_CH: {
        float sc = lerp(0.80f, 1.25f, tune);
        for (int i = 0; i < DSYN_NUM_OSC; i++) s->freq[i] = k_cluster[i] * sc;
        sv_svf_set(&s->f1, lerp(4000.0f, 9000.0f, tone), 0.7f);  /* high-pass  */
        sv_svf_set(&s->f2, lerp(6000.0f, 10000.0f, tone), 1.0f); /* band-pass  */
        float dmin, dmax;
        if      (p->type == DSYN_CY) { dmin = 0.40f; dmax = 1.80f; }
        else if (p->type == DSYN_OH) { dmin = 0.18f; dmax = 0.65f; }
        else                         { dmin = 0.02f; dmax = 0.14f; } /* CH     */
        s->amp_coef = decay_coef(lerp(dmin, dmax, decay));
        break;
    }
    case DSYN_MA: {
        sv_svf_set(&s->f1, lerp(4000.0f, 8000.0f, tone), 0.8f);  /* high-pass  */
        s->amp_coef = decay_coef(lerp(0.015f, 0.050f, decay));
        break;
    }
    case DSYN_CL: {
        s->freq[0]  = 2500.0f * lerp(0.85f, 1.20f, tune);
        s->amp_coef = decay_coef(lerp(0.020f, 0.060f, decay));
        break;
    }
    default:
        s->amp_coef = decay_coef(0.2f);
        break;
    }
}

/* ── Per-sample voice render (mono, pre-pan; envelopes advance here) ──────── */

static inline float render_voice(dsyn_state_t *s, const dsyn_params_t *p)
{
    float out = 0.0f;

    switch (p->type) {
    case DSYN_BD: {
        float f = s->base_freq + s->sweep_hz * s->pitch_env;
        s->ph[0] = wrap1(s->ph[0] + f / SR);
        float body  = dsyn_sin(s->ph[0]) * s->amp;
        float click = dsyn_noise(s) * s->amp2 * 0.6f * p->tone;
        out = body + click;
        s->pitch_env *= s->pitch_coef;
        s->amp  *= s->amp_coef;
        s->amp2 *= s->amp2_coef;
        break;
    }
    case DSYN_SD: {
        s->ph[0] = wrap1(s->ph[0] + s->freq[0] / SR);
        s->ph[1] = wrap1(s->ph[1] + s->freq[1] / SR);
        float body = (dsyn_sin(s->ph[0]) + dsyn_sin(s->ph[1]) * 0.7f) * s->amp2;
        sv_svf_tick(&s->f1, dsyn_noise(s));
        float noise = s->f1.hp * s->amp;
        float bodyg = 1.0f - 0.5f * p->snap;
        out = body * bodyg * 0.8f + noise * (0.5f + 0.6f * p->snap);
        s->amp  *= s->amp_coef;
        s->amp2 *= s->amp2_coef;
        break;
    }
    case DSYN_LT: case DSYN_MT: case DSYN_HT:
    case DSYN_LC: case DSYN_MC: case DSYN_HC: {
        float f = s->base_freq + s->sweep_hz * s->pitch_env;
        s->ph[0] = wrap1(s->ph[0] + f / SR);
        out = dsyn_sin(s->ph[0]) * s->amp;
        s->pitch_env *= s->pitch_coef;
        s->amp *= s->amp_coef;
        break;
    }
    case DSYN_RS: {
        s->ph[0] = wrap1(s->ph[0] + s->freq[0] / SR);
        s->ph[1] = wrap1(s->ph[1] + s->freq[1] / SR);
        float tone = (dsyn_sin(s->ph[0]) * 0.6f + dsyn_sin(s->ph[1]) * 0.4f) * s->amp;
        float clk  = dsyn_noise(s) * s->amp2 * 0.5f;
        out = tone + clk;
        s->amp  *= s->amp_coef;
        s->amp2 *= s->amp2_coef;
        break;
    }
    case DSYN_CP: {
        /* Quick retriggered bursts then a longer band-passed tail. */
        if (s->burst > 0) {
            s->burst_t -= 1.0f / SR;
            if (s->burst_t <= 0.0f) { s->amp = 1.0f; s->burst--; s->burst_t = 0.010f; }
        }
        sv_svf_tick(&s->f1, dsyn_noise(s));
        out = s->f1.bp * (s->amp + s->amp2 * 0.7f);
        s->amp  *= s->amp_coef;
        s->amp2 *= s->amp2_coef;
        break;
    }
    case DSYN_CB: {
        s->ph[0] = wrap1(s->ph[0] + s->freq[0] / SR);
        s->ph[1] = wrap1(s->ph[1] + s->freq[1] / SR);
        float sq = (dsyn_sq(s->ph[0]) + dsyn_sq(s->ph[1])) * 0.5f;
        sv_svf_tick(&s->f1, sq);
        out = s->f1.bp * s->amp;
        s->amp *= s->amp_coef;
        break;
    }
    case DSYN_CY: case DSYN_OH: case DSYN_CH: {
        float sum = 0.0f;
        for (int i = 0; i < DSYN_NUM_OSC; i++) {
            s->ph[i] = wrap1(s->ph[i] + s->freq[i] / SR);
            sum += dsyn_sq(s->ph[i]);
        }
        sum *= (1.0f / DSYN_NUM_OSC);
        sv_svf_tick(&s->f1, sum);          /* high-pass  */
        sv_svf_tick(&s->f2, s->f1.hp);     /* band-pass  */
        out = (s->f1.hp * 0.7f + s->f2.bp * 0.5f) * s->amp;
        s->amp *= s->amp_coef;
        break;
    }
    case DSYN_MA: {
        sv_svf_tick(&s->f1, dsyn_noise(s));
        out = s->f1.hp * s->amp;
        s->amp *= s->amp_coef;
        break;
    }
    case DSYN_CL: {
        s->ph[0] = wrap1(s->ph[0] + s->freq[0] / SR);
        out = dsyn_sin(s->ph[0]) * s->amp;
        s->amp *= s->amp_coef;
        break;
    }
    default:
        s->amp *= s->amp_coef;
        break;
    }

    /* Optional drive (tanh saturation). */
    if (p->drive > 0.001f) {
        float g = 1.0f + p->drive * 6.0f;
        out = tanhf(out * g);
    }

    /* Voice runs out of energy → free the slot. */
    if (s->amp < 0.0004f && s->amp2 < 0.0004f) s->active = false;

    return out * s->vel;
}

/* ── Live / sequencer trigger ────────────────────────────────────────────── */

static void fire_row(dsyn_t *ds, int ri, uint8_t velocity, bool accent)
{
    dsyn_params_t *p = &ds->params[ri];
    float vel  = (float)(velocity ? velocity : 100) / 127.0f;
    float gain = vel * p->level * (accent ? ds->accent_gain : 1.0f);
    if (gain > 1.6f) gain = 1.6f;
    voice_trigger(&ds->state[ri], p, gain);
}

void dsyn_trigger_row(dsyn_t *ds, int ri, uint8_t velocity)
{
    if (!ds || ri < 0 || ri >= ds->row_count) return;
    fire_row(ds, ri, velocity, false);
}

/* ── Audio-task tick ─────────────────────────────────────────────────────── */

void dsyn_tick(dsyn_t *ds, uint32_t lane_tick, uint32_t tick_delta,
               int32_t *out_l, int32_t *out_r, int n_frames)
{
    if (!ds) return;
    (void)tick_delta;

    uint32_t tps        = ds->ticks_per_step;
    bool     seq_running = (ds->step_count != 0 && tps != 0);

    /* ── Step advance / trigger ──────────────────────────────────────────── */
    if (seq_running) {
        for (int r = 0; r < ds->row_count; r++) {
            uint32_t since = lane_tick - ds->last_step_tick[r];
            uint32_t elapsed = since / tps;
            for (uint32_t e = 0; e < elapsed; e++) {
                ds->cur_step[r] = (uint8_t)((ds->cur_step[r] + 1) % ds->step_count);
                ds->last_step_tick[r] += tps;

                drum_step_t *st = &ds->steps[r][ds->cur_step[r]];
                if (st->velocity == 0) continue;

                uint8_t prob = st->probability ? st->probability : 100;
                if (prob < 100 && (esp_random() % 100) >= prob) continue;

                fire_row(ds, r, st->velocity, st->accent);
            }
        }
    }

    /* ── Render every active voice into the bus ──────────────────────────── */
    for (int r = 0; r < ds->row_count; r++) {
        dsyn_state_t  *s = &ds->state[r];
        if (!s->active) continue;
        dsyn_params_t *p = &ds->params[r];

        float gl = s->pan_l * OUT_SCALE;
        float gr = s->pan_r * OUT_SCALE;
        for (int f = 0; f < n_frames; f++) {
            float m = render_voice(s, p);
            out_l[f] += (int32_t)(m * gl);
            out_r[f] += (int32_t)(m * gr);
            if (!s->active) break;     /* tail ended mid-block                 */
        }
    }
}
