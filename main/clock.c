/*
 * clock.c — Master sequencer clock (Phase 2.1)
 */

#include <string.h>
#include "esp_timer.h"
#include "clock.h"
#include "audio.h"   /* AUDIO_BUF_FRAMES, SAMPLE_RATE */

void clock_init(seq_clock_t *c)
{
    memset(c, 0, sizeof(*c));
    c->bpm          = CLOCK_DEFAULT_BPM;
    c->tick_rate    = CLOCK_DEFAULT_PPQN;
    c->beats_per_bar = CLOCK_DEFAULT_BEATS;
    c->running      = false;
}

void clock_start(seq_clock_t *c)
{
    if (!c->running) {
        c->tick_count = 0;
        c->frac_acc   = 0;
        c->running    = true;
    }
}

void clock_stop(seq_clock_t *c)
{
    c->running    = false;
    c->tick_count = 0;
    c->frac_acc   = 0;
}

void clock_set_bpm(seq_clock_t *c, float bpm)
{
    if (bpm < CLOCK_BPM_MIN) bpm = CLOCK_BPM_MIN;
    if (bpm > CLOCK_BPM_MAX) bpm = CLOCK_BPM_MAX;
    c->bpm = bpm;
}

float clock_tap(seq_clock_t *c)
{
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);

    /* Discard taps >3 s apart — user paused, start fresh */
    if (c->tap_count > 0) {
        int prev = (c->tap_idx + CLOCK_TAP_HISTORY - 1) % CLOCK_TAP_HISTORY;
        if (now_ms - c->tap_times[prev] > 3000) {
            c->tap_count = 0;
            c->tap_idx   = 0;
        }
    }

    c->tap_times[c->tap_idx] = now_ms;
    c->tap_idx = (c->tap_idx + 1) % CLOCK_TAP_HISTORY;
    if (c->tap_count < CLOCK_TAP_HISTORY) c->tap_count++;

    if (c->tap_count < 2) return 0.0f;

    /* Average the last (tap_count-1) intervals */
    int n = c->tap_count - 1;
    int start = (c->tap_idx - c->tap_count + CLOCK_TAP_HISTORY) % CLOCK_TAP_HISTORY;
    uint32_t oldest = c->tap_times[start];
    int end   = (c->tap_idx - 1 + CLOCK_TAP_HISTORY) % CLOCK_TAP_HISTORY;
    uint32_t newest = c->tap_times[end];
    float avg_interval_ms = (float)(newest - oldest) / (float)n;

    float new_bpm = 60000.0f / avg_interval_ms;
    clock_set_bpm(c, new_bpm);
    return c->bpm;
}

uint32_t clock_advance(seq_clock_t *c)
{
    if (!c->running) return 0;

    /*
     * Exact ticks per block = (AUDIO_BUF_FRAMES * tick_rate * bpm) / (60 * SAMPLE_RATE)
     *
     * Use Q16 fixed-point: multiply numerator by 65536 before dividing, keeping
     * the fractional part in frac_acc across blocks so there is no cumulative drift.
     *
     *   numerator_q16 = (uint64_t)AUDIO_BUF_FRAMES * tick_rate * (bpm * 65536) / 65536
     *                 = AUDIO_BUF_FRAMES * tick_rate * bpm (in Q16 scale)
     *
     * Simplified: compute (AUDIO_BUF_FRAMES * tick_rate * bpm_fp) / (60 * SAMPLE_RATE)
     * where bpm_fp = (uint64_t)(bpm * 65536) gives Q16 result directly.
     */
    uint64_t bpm_fp    = (uint64_t)(c->bpm * 65536.0f + 0.5f);   /* Q16 */
    uint64_t num       = (uint64_t)AUDIO_BUF_FRAMES * c->tick_rate * bpm_fp;
    uint64_t den       = (uint64_t)60 * SAMPLE_RATE;
    uint64_t ticks_q16 = num / den;   /* Q16: upper bits = whole ticks, lower 16 = frac */

    /* Accumulate fractional part; promote to whole tick when it overflows */
    c->frac_acc += (uint32_t)(ticks_q16 & 0xFFFF);
    uint32_t whole = (uint32_t)(ticks_q16 >> 16) + (c->frac_acc >> 16);
    c->frac_acc  &= 0xFFFF;

    c->tick_count += whole;
    return whole;
}
