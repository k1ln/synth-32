#pragma once
/*
 * arp.h — Per-synth-lane arpeggiator (Phase 3)
 *
 * Intercepts held notes and steps through them (or a derived pattern) in
 * time with the master clock. Operates between piano_roll / live note-on
 * events and the synth voice engine.
 *
 * arp_tick() is called from the audio task once per block.
 */

#include <stdint.h>
#include <stdbool.h>
#include "synth.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Mode ────────────────────────────────────────────────────────────────── */
typedef enum {
    ARP_UP = 0,
    ARP_DOWN,
    ARP_UP_DOWN,          /* no repeat at extremes                           */
    ARP_UP_DOWN_INCL,     /* repeats top and bottom note                     */
    ARP_DOWN_UP,
    ARP_ORDER,            /* note-on insertion order                         */
    ARP_RANDOM,
    ARP_CHORD,            /* all held notes fire simultaneously               */
    ARP_AS_PLAYED,        /* exact note-on sequence from piano roll           */
    ARP_MODE_COUNT
} arp_mode_t;

/* ── Velocity mode ───────────────────────────────────────────────────────── */
typedef enum {
    ARP_VEL_ORIGINAL = 0, /* use original note velocity                      */
    ARP_VEL_ACCENT,       /* alternate full / half velocity                  */
    ARP_VEL_FIXED,        /* constant velocity                               */
} arp_vel_mode_t;

/* ── Arpeggiator state ───────────────────────────────────────────────────── */
typedef struct {
    bool           enabled;
    arp_mode_t     mode;
    uint8_t        octave_range;   /* 1–4 */
    uint8_t        step_div;       /* 4=1/4, 8=1/8, 16=1/16, 32=1/32        */
    bool           latch;          /* keep notes after release               */
    uint8_t        gate_pct;       /* note duration % of step (10–100)       */
    arp_vel_mode_t velocity_mode;
    uint8_t        fixed_vel;      /* velocity when velocity_mode == FIXED   */
    bool           retrigger;      /* re-gate ADSR on each step              */
    uint8_t        swing_pct;      /* 50 = straight, 51–75 = swing           */

    /* Held-note set */
    uint8_t        held[16];       /* MIDI notes in insertion order          */
    uint8_t        held_vel[16];   /* corresponding velocities               */
    uint8_t        held_count;
    bool           latch_active;   /* latch is currently holding             */

    /* Computed sequence */
    uint8_t        seq[32];        /* note numbers (with octave spread)      */
    uint8_t        seq_vel[32];    /* velocities for each step               */
    uint8_t        seq_len;

    /* Runtime */
    uint8_t        step;           /* current position in seq[]              */
    uint32_t       next_tick;      /* master tick of next arp step           */
    bool           note_active;    /* current step note is on                */
    uint8_t        cur_note;       /* note currently gated on                */
    bool           even_step;      /* for swing: tracks even/odd step        */
} arp_t;

/* ── API ─────────────────────────────────────────────────────────────────── */

/* Initialise to sensible defaults (disabled, UP mode, 1/16). */
void arp_init(arp_t *a);

/* Notify arp of an incoming note-on event.
 * If arp is disabled the note is forwarded directly to synth->note_on. */
void arp_note_on(arp_t *a, synth_inst_t *synth, uint8_t note, uint8_t vel);

/* Notify arp of a note-off event. */
void arp_note_off(arp_t *a, synth_inst_t *synth, uint8_t note);

/* Recompute the step sequence from held notes + mode + octave range.
 * Must be called after every held-set change. */
void arp_build_seq(arp_t *a);

/* Called from audio task every block.
 * master_tick: current master tick count
 * tick_rate:   PPQN (ticks per beat)
 * synth:       target voice engine */
void arp_tick(arp_t *a, synth_inst_t *synth,
              uint32_t master_tick, uint32_t tick_rate);

#ifdef __cplusplus
}
#endif
