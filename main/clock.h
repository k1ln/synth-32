#pragma once
/*
 * clock.h — Master sequencer clock (Phase 2.1)
 *
 * Advances in fixed-point inside the audio task every AUDIO_BUF_FRAMES samples.
 * Tick formula: delta = (AUDIO_BUF_FRAMES * tick_rate * bpm) / (60 * SAMPLE_RATE)
 * At 120 BPM / 96 PPQN / 512-frame buffer → ~1.966 ticks/block → Q16 fraction
 * accumulates cleanly without drift.
 */

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CLOCK_DEFAULT_BPM      120.0f
#define CLOCK_DEFAULT_PPQN     96
#define CLOCK_DEFAULT_BEATS    4        /* beats per bar (time sig numerator) */
#define CLOCK_BPM_MIN          20.0f
#define CLOCK_BPM_MAX          300.0f
#define CLOCK_TAP_HISTORY      4        /* tap intervals averaged for tap tempo */

/* bar_ticks = ppqn * beats_per_bar */
#define CLOCK_BAR_TICKS(c)  ((c)->tick_rate * (c)->beats_per_bar)

typedef struct {
    float    bpm;              /* 20.0 – 300.0                                   */
    uint32_t tick_rate;        /* ticks per beat — default 96 PPQN               */
    uint32_t beats_per_bar;    /* time sig numerator — default 4                  */
    uint32_t tick_count;       /* absolute ticks since play start                 */

    /* Fixed-point accumulator (Q16) for sub-tick audio-block remainder */
    uint32_t frac_acc;         /* 16.16: lower 16 bits are fractional ticks       */

    volatile bool running;     /* start/stop — writable from any task             */

    /* Tap tempo ring buffer — timestamps in ms from esp_timer_get_time() / 1000 */
    uint32_t tap_times[CLOCK_TAP_HISTORY];
    int      tap_idx;
    int      tap_count;
} seq_clock_t;

/* Initialise to defaults. Does not start the clock. */
void     clock_init(seq_clock_t *c);

/* Start / stop (idempotent). Stop resets tick_count and frac_acc. */
void     clock_start(seq_clock_t *c);
void     clock_stop(seq_clock_t *c);

/* Set BPM, clamped to [CLOCK_BPM_MIN, CLOCK_BPM_MAX]. Thread-safe for single write. */
void     clock_set_bpm(seq_clock_t *c, float bpm);

/* Record a tap event (call on each tap). Updates BPM automatically once ≥2 taps.
 * Returns updated BPM, or 0 if not enough taps yet. */
float    clock_tap(seq_clock_t *c);

/* Advance the clock by one audio block (AUDIO_BUF_FRAMES samples).
 * Returns the integer tick delta for this block (0 or 1 typically, rarely 2).
 * Must only be called from the audio task. */
uint32_t clock_advance(seq_clock_t *c);

#ifdef __cplusplus
}
#endif
