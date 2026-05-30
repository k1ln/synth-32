/*
 * lfo.c — LFO engine + modulation matrix (Phase 3)
 */

#include <string.h>
#include <math.h>
#include "lfo.h"
#include "audio.h"   /* AUDIO_BUF_FRAMES, SAMPLE_RATE */
#include "synth_voice.h"  /* sv_wt[], SV_WAVE_*, sv_hz_to_step() */

void lane_lfo_init(lane_lfo_t *ll)
{
    memset(ll, 0, sizeof(*ll));
    for (int i = 0; i < NUM_LANE_LFOS; i++) {
        ll->lfo[i].shape    = LFO_SINE;
        ll->lfo[i].sync     = LFO_FREE;
        ll->lfo[i].rate_hz  = 1.0f;
        ll->lfo[i].depth    = 1.0f;
        ll->lfo[i].sh_lfsr  = (uint16_t)(0xACE1 + i);
    }
}

void lfo_set_rate(lfo_t *l, lfo_sync_t sync, float rate_hz,
                  float bpm, uint32_t tick_rate)
{
    l->sync    = sync;
    l->rate_hz = rate_hz;
    if (sync == LFO_FREE) {
        /* step = (rate_hz / SR) * 2^24 * AUDIO_BUF_FRAMES
         * (advance whole blocks at a time) */
        l->step = (uint32_t)(rate_hz * (float)AUDIO_BUF_FRAMES
                             / (float)SAMPLE_RATE * (1u << 24));
    } else {
        /* Clock-sync: convert note division to Hz then same formula */
        static const float divs[] = {4.0f, 8.0f, 16.0f, 32.0f};
        int div_idx = (int)sync - 1;
        if (div_idx < 0) div_idx = 0;
        if (div_idx > 3) div_idx = 3;
        float div = divs[div_idx];
        float sync_hz = (bpm / 60.0f) * (div / 4.0f);  /* 1/div note per beat */
        l->step = (uint32_t)(sync_hz * (float)AUDIO_BUF_FRAMES
                             / (float)SAMPLE_RATE * (1u << 24));
    }
}

/* ── Advance one LFO by one block ────────────────────────────────────────── */
static float lfo_advance(lfo_t *l)
{
    if (!l->enabled) return 0.0f;

    uint32_t prev_phase = l->phase;
    l->phase += l->step;

    float out = 0.0f;
    uint8_t idx = (uint8_t)(l->phase >> 24);

    switch (l->shape) {
    case LFO_SINE:
        out = (float)sv_wt[SV_WAVE_SINE][idx] / 32768.0f;
        break;
    case LFO_TRIANGLE:
        out = (float)sv_wt[SV_WAVE_TRIANGLE][idx] / 32768.0f;
        break;
    case LFO_SQUARE:
        out = (l->phase & 0x80000000u) ? 1.0f : -1.0f;
        break;
    case LFO_SAW:
        out = (float)sv_wt[SV_WAVE_SAW][idx] / 32768.0f;
        break;
    case LFO_SAMPLE_HOLD: {
        /* Trigger on phase wraparound (rising zero-cross) */
        bool was_high = (prev_phase & 0x80000000u) != 0;
        bool is_high  = (l->phase   & 0x80000000u) != 0;
        if (!was_high && is_high) {
            l->sh_lfsr = (uint16_t)(l->sh_lfsr >> 1) ^ ((l->sh_lfsr & 1) ? 0xB400 : 0);
        }
        out = (float)(int16_t)l->sh_lfsr / 32768.0f;
        break;
    }
    default:
        break;
    }
    l->output = out * l->depth;
    return l->output;
}

/* ── Lane LFO tick ───────────────────────────────────────────────────────── */
void lane_lfo_tick(lane_lfo_t *ll, uint32_t block_size)
{
    (void)block_size;

    /* Advance each LFO */
    float lfo_out[NUM_LANE_LFOS];
    for (int i = 0; i < NUM_LANE_LFOS; i++)
        lfo_out[i] = lfo_advance(&ll->lfo[i]);

    /* Source values */
    static uint16_t rand_lfsr = 0x1234;
    rand_lfsr = (uint16_t)(rand_lfsr >> 1) ^ ((rand_lfsr & 1) ? 0xB400 : 0);
    float src_val[MOD_SRC_COUNT] = {
        lfo_out[0], lfo_out[1], lfo_out[2], lfo_out[3],
        ll->velocity_input,
        ll->note_input,
        (float)(int16_t)rand_lfsr / 32768.0f,
        ll->env_input,
    };

    /* Reset destinations */
    memset(ll->mod_out, 0, sizeof(ll->mod_out));

    /* Accumulate routes */
    for (int i = 0; i < ll->route_count; i++) {
        const mod_route_t *r = &ll->routes[i];
        if (!r->enabled) continue;
        if (r->src  >= MOD_SRC_COUNT)  continue;
        if (r->dest >= MOD_DEST_COUNT) continue;
        ll->mod_out[r->dest] += src_val[r->src] * r->depth;
    }
}

bool lane_lfo_add_route(lane_lfo_t *ll, mod_src_t src,
                        mod_dest_t dest, float depth)
{
    if (ll->route_count >= MOD_MATRIX_SIZE) return false;
    mod_route_t *r = &ll->routes[ll->route_count++];
    r->src     = src;
    r->dest    = dest;
    r->depth   = depth;
    r->enabled = true;
    return true;
}
