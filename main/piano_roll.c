/*
 * piano_roll.c — Piano roll note storage + playback (Phase 2.6)
 */

#include <string.h>
#include <stdlib.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "piano_roll.h"

static const char *TAG = "piano_roll";

/* ── Allocation ──────────────────────────────────────────────────────────── */

piano_roll_t *piano_roll_alloc(void)
{
    piano_roll_t *pr = heap_caps_calloc(1, sizeof(piano_roll_t), MALLOC_CAP_SPIRAM);
    if (!pr) { ESP_LOGE(TAG, "piano_roll alloc failed"); return NULL; }
    pr->step_resolution = 16;
    pr->view_ticks_wide = 384;   /* 1 bar @ 4/4 96PPQN */
    return pr;
}

/* ── Edit API ────────────────────────────────────────────────────────────── */

bool piano_roll_add_note(piano_roll_t *pr, uint32_t tick_start, uint32_t tick_len,
                         uint8_t note, uint8_t velocity)
{
    if (pr->note_count >= PIANO_ROLL_MAX_NOTES) return false;
    pr_note_t *n = &pr->notes[pr->note_count++];
    n->tick_start = tick_start;
    n->tick_len   = tick_len;
    n->note       = note;
    n->velocity   = velocity;
    piano_roll_sort(pr);
    return true;
}

void piano_roll_remove_note(piano_roll_t *pr, uint32_t tick_start, uint8_t note)
{
    int w = 0;
    for (int i = 0; i < pr->note_count; i++) {
        if (pr->notes[i].tick_start == tick_start && pr->notes[i].note == note)
            continue;
        pr->notes[w++] = pr->notes[i];
    }
    pr->note_count = (uint16_t)w;
}

static int note_cmp(const void *a, const void *b)
{
    const pr_note_t *na = (const pr_note_t *)a;
    const pr_note_t *nb = (const pr_note_t *)b;
    if (na->tick_start < nb->tick_start) return -1;
    if (na->tick_start > nb->tick_start) return  1;
    return 0;
}

void piano_roll_sort(piano_roll_t *pr)
{
    qsort(pr->notes, pr->note_count, sizeof(pr_note_t), note_cmp);
}

void piano_roll_reset(piano_roll_t *pr)
{
    (void)pr;   /* no cursor state — tick scan is stateless */
}

/* ── Audio-task tick ─────────────────────────────────────────────────────── */

/* Fire events in a single half-open interval [a, b) */
static void pr_scan(piano_roll_t *pr, uint32_t a, uint32_t b, synth_inst_t *synth)
{
    for (int i = 0; i < pr->note_count; i++) {
        const pr_note_t *n = &pr->notes[i];
        if (n->tick_start >= b) break;   /* sorted: no more note-ons possible */
        if (n->tick_start >= a && n->tick_start < b) {
            if (synth && synth->note_on)
                synth->note_on(synth, n->note, n->velocity);
        }
        if (n->tick_len > 0) {
            uint32_t off_tick = n->tick_start + n->tick_len;
            if (off_tick >= a && off_tick < b) {
                if (synth && synth->note_off)
                    synth->note_off(synth, n->note);
            }
        }
    }
}

void piano_roll_tick(piano_roll_t *pr, uint32_t prev_tick, uint32_t lane_tick,
                     uint32_t loop_len, synth_inst_t *synth)
{
    if (!pr || pr->note_count == 0) return;

    /* Detect loop wrap: lanes_tick() already wrapped lane_tick, so prev_tick
     * was computed as (wrapped_tick - delta) which underflows to a large number.
     * In that case split into [prev_tick, loop_len) + [0, lane_tick). */
    if (loop_len > 0 && prev_tick > lane_tick) {
        pr_scan(pr, prev_tick, loop_len, synth);
        pr_scan(pr, 0, lane_tick, synth);
    } else {
        pr_scan(pr, prev_tick, lane_tick, synth);
    }
}
