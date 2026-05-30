/*
 * arp.c — Arpeggiator (Phase 3)
 */

#include <string.h>
#include "arp.h"
#include "synth.h"
#include "audio.h"

/* ── Pseudo-random for ARP_RANDOM ───────────────────────────────────────── */
static uint16_t s_arp_rand = 0x1234;
static uint8_t arp_rand(uint8_t max)
{
    s_arp_rand = (uint16_t)(s_arp_rand * 6364 + 1);
    return (uint8_t)((s_arp_rand >> 8) % (max ? max : 1));
}

/* ── Init ────────────────────────────────────────────────────────────────── */
void arp_init(arp_t *a)
{
    memset(a, 0, sizeof(*a));
    a->mode         = ARP_UP;
    a->octave_range = 1;
    a->step_div     = 16;
    a->gate_pct     = 80;
    a->velocity_mode = ARP_VEL_ORIGINAL;
    a->fixed_vel    = 100;
    a->swing_pct    = 50;
    a->enabled      = false;
}

/* ── Note-on / Note-off ──────────────────────────────────────────────────── */
void arp_note_on(arp_t *a, synth_inst_t *synth, uint8_t note, uint8_t vel)
{
    if (!a->enabled) {
        if (synth) audio_note_on(synth, note, vel);
        return;
    }
    /* Latch: new note clears the latch set */
    if (a->latch && a->latch_active) {
        a->held_count    = 0;
        a->latch_active  = false;
    }
    /* Add to held set (avoid duplicates) */
    for (int i = 0; i < a->held_count; i++)
        if (a->held[i] == note) { a->held_vel[i] = vel; return; }
    if (a->held_count < 16) {
        a->held[a->held_count]     = note;
        a->held_vel[a->held_count] = vel;
        a->held_count++;
    }
    arp_build_seq(a);
}

void arp_note_off(arp_t *a, synth_inst_t *synth, uint8_t note)
{
    if (!a->enabled) {
        if (synth) audio_note_off(synth, note);
        return;
    }
    if (a->latch) { a->latch_active = true; return; }
    /* Remove from held set */
    int w = 0;
    for (int i = 0; i < a->held_count; i++) {
        if (a->held[i] == note) continue;
        a->held[w]     = a->held[i];
        a->held_vel[w] = a->held_vel[i];
        w++;
    }
    a->held_count = (uint8_t)w;
    arp_build_seq(a);
    if (a->held_count == 0 && a->note_active && synth && synth->note_off)
        synth->note_off(synth, a->cur_note);
}

/* ── Sort helpers ────────────────────────────────────────────────────────── */
static void sort_asc(uint8_t *notes, uint8_t *vels, int n)
{
    /* Insertion sort — n ≤ 16 */
    for (int i = 1; i < n; i++) {
        uint8_t key = notes[i], kv = vels[i];
        int j = i - 1;
        while (j >= 0 && notes[j] > key) {
            notes[j + 1] = notes[j];
            vels[j + 1]  = vels[j];
            j--;
        }
        notes[j + 1] = key;
        vels[j + 1]  = kv;
    }
}

/* ── Build sequence ──────────────────────────────────────────────────────── */
void arp_build_seq(arp_t *a)
{
    if (a->held_count == 0) { a->seq_len = 0; return; }

    /* Make sorted and insertion-order copies */
    uint8_t sorted[16], sorted_vel[16];
    memcpy(sorted,     a->held,     a->held_count);
    memcpy(sorted_vel, a->held_vel, a->held_count);
    sort_asc(sorted, sorted_vel, a->held_count);

    int len = 0;
    uint8_t range = a->octave_range < 1 ? 1 : (a->octave_range > 4 ? 4 : a->octave_range);

    switch (a->mode) {
    case ARP_UP:
        for (int oct = 0; oct < range; oct++)
            for (int i = 0; i < a->held_count && len < 32; i++) {
                a->seq[len]     = (uint8_t)(sorted[i] + oct * 12);
                a->seq_vel[len] = sorted_vel[i];
                len++;
            }
        break;
    case ARP_DOWN:
        for (int oct = range - 1; oct >= 0; oct--)
            for (int i = a->held_count - 1; i >= 0 && len < 32; i--) {
                a->seq[len]     = (uint8_t)(sorted[i] + oct * 12);
                a->seq_vel[len] = sorted_vel[i];
                len++;
            }
        break;
    case ARP_UP_DOWN:
        /* Up (exclude top), Down (exclude bottom) */
        for (int oct = 0; oct < range; oct++)
            for (int i = 0; i < a->held_count && len < 32; i++) {
                a->seq[len]     = (uint8_t)(sorted[i] + oct * 12);
                a->seq_vel[len] = sorted_vel[i];
                len++;
            }
        /* Remove last (top) */
        if (len > 1) len--;
        {   /* Down: skip first (bottom) */
            int start = len;
            for (int oct = range - 1; oct >= 0; oct--)
                for (int i = a->held_count - 1; i >= 0 && len < 32; i--) {
                    int note = sorted[i] + oct * 12;
                    /* Skip the top note (already added going up) */
                    if (oct == range - 1 && i == a->held_count - 1) continue;
                    /* Skip the bottom note (avoid double) */
                    if (oct == 0 && i == 0) continue;
                    a->seq[len]     = (uint8_t)note;
                    a->seq_vel[len] = sorted_vel[i];
                    len++;
                }
            (void)start;
        }
        break;
    case ARP_UP_DOWN_INCL:
        for (int oct = 0; oct < range; oct++)
            for (int i = 0; i < a->held_count && len < 32; i++) {
                a->seq[len]     = (uint8_t)(sorted[i] + oct * 12);
                a->seq_vel[len] = sorted_vel[i];
                len++;
            }
        for (int oct = range - 1; oct >= 0 && len < 32; oct--)
            for (int i = a->held_count - 1; i >= 0 && len < 32; i--) {
                a->seq[len]     = (uint8_t)(sorted[i] + oct * 12);
                a->seq_vel[len] = sorted_vel[i];
                len++;
            }
        break;
    case ARP_DOWN_UP:
        for (int oct = range - 1; oct >= 0; oct--)
            for (int i = a->held_count - 1; i >= 0 && len < 32; i--) {
                a->seq[len]     = (uint8_t)(sorted[i] + oct * 12);
                a->seq_vel[len] = sorted_vel[i];
                len++;
            }
        if (len > 1) len--;  /* exclude bottom for smooth repeat */
        for (int oct = 0; oct < range && len < 32; oct++)
            for (int i = 0; i < a->held_count && len < 32; i++) {
                if (oct == 0 && i == 0) continue;  /* skip bottom */
                a->seq[len]     = (uint8_t)(sorted[i] + oct * 12);
                a->seq_vel[len] = sorted_vel[i];
                len++;
            }
        break;
    case ARP_ORDER:
        /* Insertion order, with octave range */
        for (int oct = 0; oct < range; oct++)
            for (int i = 0; i < a->held_count && len < 32; i++) {
                a->seq[len]     = (uint8_t)(a->held[i] + oct * 12);
                a->seq_vel[len] = a->held_vel[i];
                len++;
            }
        break;
    case ARP_RANDOM:
        /* Fill sequence with random picks — will re-randomise each step */
        len = a->held_count * range;
        if (len > 32) len = 32;
        for (int i = 0; i < len; i++) {
            int idx = arp_rand(a->held_count);
            int oct = (int)arp_rand((uint8_t)range);
            a->seq[i]     = (uint8_t)(a->held[idx] + oct * 12);
            a->seq_vel[i] = a->held_vel[idx];
        }
        break;
    case ARP_CHORD:
        /* All notes fire simultaneously — encoded as a single step */
        for (int i = 0; i < a->held_count && len < 32; i++) {
            a->seq[len]     = a->held[i];
            a->seq_vel[len] = a->held_vel[i];
            len++;
        }
        break;
    case ARP_AS_PLAYED:
        /* Identical to ORDER but preserves velocities */
        for (int oct = 0; oct < range; oct++)
            for (int i = 0; i < a->held_count && len < 32; i++) {
                a->seq[len]     = (uint8_t)(a->held[i] + oct * 12);
                a->seq_vel[len] = a->held_vel[i];
                len++;
            }
        break;
    default:
        break;
    }
    a->seq_len = (uint8_t)len;
    if (a->step >= a->seq_len) a->step = 0;
}

/* ── Tick ─────────────────────────────────────────────────────────────────── */
void arp_tick(arp_t *a, synth_inst_t *synth,
              uint32_t master_tick, uint32_t tick_rate)
{
    if (!a->enabled || a->seq_len == 0 || !synth) return;

    /* Ticks per arp step = tick_rate * 4 / step_div
     * e.g. step_div=16 → tick_rate*4/16 = tick_rate/4 ticks per 1/16 note */
    uint32_t step_div = a->step_div ? a->step_div : 16;
    uint32_t ticks_per_step = (tick_rate * 4) / step_div;
    if (ticks_per_step == 0) ticks_per_step = 1;

    /* Gate off when note duration expires */
    if (a->note_active) {
        uint32_t gate_dur = ticks_per_step * a->gate_pct / 100;
        if (gate_dur == 0) gate_dur = 1;
        if (master_tick >= a->next_tick - ticks_per_step + gate_dur) {
            if (synth->note_off && a->mode != ARP_CHORD)
                synth->note_off(synth, a->cur_note);
            a->note_active = false;
        }
    }

    if (master_tick < a->next_tick) return;

    /* Advance step */
    if (a->seq_len == 0) return;

    /* For ARP_RANDOM: re-randomise next step each time */
    if (a->mode == ARP_RANDOM) {
        int idx = arp_rand(a->held_count);
        int oct = (int)arp_rand(a->octave_range ? a->octave_range : 1);
        a->cur_note = (uint8_t)(a->held[idx] + oct * 12);
        uint8_t vel = a->held_vel[idx];
        if (synth->note_on) synth->note_on(synth, a->cur_note, vel);
    } else if (a->mode == ARP_CHORD) {
        /* Fire all held notes */
        for (int i = 0; i < a->seq_len; i++)
            if (synth->note_on) synth->note_on(synth, a->seq[i], a->seq_vel[i]);
        a->cur_note = a->seq[0];
    } else {
        a->cur_note = a->seq[a->step];
        uint8_t vel = a->seq_vel[a->step];
        /* Velocity mode */
        if (a->velocity_mode == ARP_VEL_FIXED) {
            vel = a->fixed_vel;
        } else if (a->velocity_mode == ARP_VEL_ACCENT) {
            vel = a->even_step ? 127 : 64;
        }
        if (synth->note_on) synth->note_on(synth, a->cur_note, vel);
        a->step = (uint8_t)((a->step + 1) % a->seq_len);
    }
    a->note_active = true;
    a->even_step   = !a->even_step;

    /* Schedule next step with swing */
    uint32_t swing_offset = 0;
    if (a->even_step && a->swing_pct > 50) {
        swing_offset = ticks_per_step * (a->swing_pct - 50) / 50;
    }
    a->next_tick = master_tick + ticks_per_step + swing_offset;
}
