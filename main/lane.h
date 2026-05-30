#pragma once
/*
 * lane.h — Lane model + song-level container (Phase 2.2 / 2.3)
 *
 * 16 independent lanes, each typed as WAV one-shot, drum sequencer, or synth.
 * The audio task owns bus_l / bus_r; all other fields are UI-writable with the
 * caveats noted (volatile bool fields are single-byte atomic on RISC-V).
 *
 * Phase-2.5 (drum_seq_t) and Phase-2.6 / Phase-3 (piano_roll_t, synth_inst_t)
 * are forward-declared here; their full definitions live in their own headers.
 */

#include <stdint.h>
#include <stdbool.h>
#include "wav_player.h"
#include "clock.h"
#include "synth.h"
#include "drum_seq.h"
#include "piano_roll.h"
#include "arp.h"
#include "lfo.h"
#include "fx.h"

/* audio.h is NOT included here to avoid circular dependency.
 * clock.c and lane.c include audio.h directly for AUDIO_BUF_FRAMES / SAMPLE_RATE. */

#ifdef __cplusplus
extern "C" {
#endif

/* FX_MAX_PER_LANE defined in fx.h — 4 slots per lane */

/* ── Lane count ─────────────────────────────────────────────────────────── */
#define NUM_LANES 64

/* ── Groove / swing per lane ─────────────────────────────────────────────── */
typedef struct {
    uint8_t  swing_pct;     /* 50 = straight, 51–75 = swing on even steps     */
    int8_t   humanise;      /* 0 = off; 1–20 = random tick offset ± this many */
    bool     enabled;
} groove_t;

/* ── Scene snapshot ──────────────────────────────────────────────────────── */
#define SCENE_MAX        8    /* A–H */
#define SCENE_NAME_LEN   16

typedef struct {
    bool    active;                  /* true if this slot is populated        */
    char    name[SCENE_NAME_LEN];
    bool    lane_mute[NUM_LANES];    /* mute state at snapshot time           */
    bool    lane_solo[NUM_LANES];
    float   lane_volume[NUM_LANES];
} scene_t;

/* ── Arrangement step ────────────────────────────────────────────────────── */
#define ARRANGEMENT_MAX_STEPS  32

typedef struct {
    uint8_t scene_idx;   /* index into song_t.scenes[]                       */
    uint8_t repeat;      /* 1–16 times                                       */
} arrangement_step_t;

typedef struct {
    arrangement_step_t steps[ARRANGEMENT_MAX_STEPS];
    uint8_t            count;          /* number of steps in chain            */
    uint8_t            current_step;   /* runtime: current position           */
    uint8_t            current_repeat; /* runtime: repeat counter             */
    bool               enabled;        /* false = ignore arrangement, free loop */
} arrangement_t;

/* ── Lane type ───────────────────────────────────────────────────────────── */
typedef enum {
    LANE_TYPE_WAV   = 0,   /* streams a WAV file from SD, one-shot / loop   */
    LANE_TYPE_DRUM,        /* step sequencer with per-row samples            */
    LANE_TYPE_SYNTH,       /* procedural synth voice + piano roll            */
} lane_type_t;

/* ── Lane ────────────────────────────────────────────────────────────────── */
typedef struct {
    lane_type_t      type;
    volatile bool    active;    /* false = lane is empty / unused              */
    volatile bool    mute;      /* true = bus zeroed; clock + SD still run     */
    volatile bool    solo;      /* true = only soloed lanes mix to master bus  */
    float            volume;    /* 0.0 – 1.0                                   */
    float            pan;       /* -1.0 (L) … +1.0 (R)                        */
    char             name[32];  /* user-editable label; "" = use type default  */

    /* ── Clock / loop ──────────────────────────────────────────────────── */
    uint32_t         loop_len_ticks;   /* 0 = play once; else loop every N ticks */
    uint32_t         lane_tick;        /* current tick within loop (audio-task owned) */
    bool             pending_restart;  /* SONG mode: restart queued, fires on bar boundary */

    /* ── WAV source (LANE_TYPE_WAV) ──────────────────────────────────── */
    char             wav_path[128];    /* path relative to /sdcard/sounds/      */
    wav_lane_t      *wav_lane;         /* points into wav_engine's lane array   */
    int              wav_lane_slot;    /* index in wav_engine pool (-1 = unassigned) */

    /* ── Drum sequencer (LANE_TYPE_DRUM) ─────────────────────────────── */
    drum_seq_t      *drum_seq;

    /* ── Synth + piano roll (LANE_TYPE_SYNTH) ────────────────────────── */
    synth_inst_t    *synth;
    piano_roll_t    *piano_roll;
    arp_t            arp;
    lane_lfo_t       lfo;
    /* Cached synth param values — indexed by param ID (0-12, 20-23) */
    float            synth_params[24];

    /* ── Groove / swing ─────────────────────────────────────────────────── */
    groove_t         groove;

    /* ── Per-lane ADSR (lane amplitude envelope) ─────────────────────── */
    lane_adsr_t      adsr;

    /* ── Per-lane effect chain ────────────────────────────────────────── */
    fx_node_t       *fx[FX_MAX_PER_LANE];  /* heap-allocated vtable nodes */
    int              fx_count;

    /* ── Note repeat ─────────────────────────────────────────────────── */
    bool     note_repeat;         /* true = retrigger held note at rate_div   */
    uint8_t  note_repeat_div;     /* 4=1/4, 8=1/8, 16=1/16, 32=1/32          */
    uint8_t  note_repeat_note;    /* the note being repeated                  */
    uint32_t note_repeat_next_tick; /* next retrigger tick (audio-task owned) */

    /* ── Loop record / overdub ───────────────────────────────────────── */
    bool     loop_rec_armed;      /* arm next transport start for recording   */
    bool     loop_rec_active;     /* currently recording                      */
    bool     loop_rec_overdub;    /* true = overdub on existing notes         */

    /* ── Send bus level ─────────────────────────────────────────────── */
    float    send_level;          /* 0–1: how much of this lane feeds send bus */

    /* ── Mixer bus (written exclusively by audio task, core 1) ───────── */
    int32_t  bus_l;
    int32_t  bus_r;
} lane_t;

/* ── Performance macro (XY pad) ─────────────────────────────────────────── */
#define MACRO_MAX  4

typedef struct {
    mod_dest_t dest;
    uint8_t    lane_idx;   /* lane the destination applies to; 0xFF = master  */
    float      min_val;
    float      max_val;
} macro_binding_t;

typedef struct {
    macro_binding_t x_bind[MACRO_MAX];
    macro_binding_t y_bind[MACRO_MAX];
    int             x_count;
    int             y_count;
    float           x;     /* current X value 0–1 */
    float           y;     /* current Y value 0–1 */
} macro_pad_t;

/* ── Playback mode ───────────────────────────────────────────────────────── */
typedef enum {
    PLAYBACK_LIVE = 0,   /* immediate loop restart, finger-drum feel          */
    PLAYBACK_SONG,       /* loop restarts snap to next bar boundary           */
} playback_mode_t;

/* ── Song container ──────────────────────────────────────────────────────── */
typedef struct song_s {
    lane_t           lanes[NUM_LANES];
    seq_clock_t      clock;
    playback_mode_t  playback_mode;
    char             name[64];        /* song name shown in top bar           */
    bool             dirty;           /* unsaved changes pending              */
    fx_node_t       *master_fx[FX_MAX_PER_LANE];
    int              master_fx_count;

    /* ── Scenes & arrangement ──────────────────────────────────────────── */
    scene_t          scenes[SCENE_MAX];
    int              active_scene;    /* -1 = none; 0–7 = scene index        */
    arrangement_t    arrangement;

    /* ── Auto-save ─────────────────────────────────────────────────────── */
    uint32_t         autosave_bar;    /* master bar count at last autosave    */
    uint8_t          autosave_interval; /* bars between autosaves; 0 = off   */

    /* ── Send bus ──────────────────────────────────────────────────────── */
    fx_node_t       *send_fx[FX_MAX_PER_LANE]; /* shared reverb/delay chain  */
    int              send_fx_count;
    float            send_return_level; /* master return level 0–1           */

    /* ── Macro XY pad ──────────────────────────────────────────────────── */
    macro_pad_t      macro;

    /* ── Metronome ─────────────────────────────────────────────────────── */
    bool             metronome_enabled;
    float            metronome_volume;  /* 0–1 */

    /* ── Stem export ───────────────────────────────────────────────────── */
    bool             stem_export_active;
} song_t;

/* Global song instance — defined in synth-32.c, shared across audio + UI tasks */
extern song_t g_song;

/* ── API ─────────────────────────────────────────────────────────────────── */

/* Initialise all lanes to inactive defaults. Must be called before audio task. */
void lane_init_all(song_t *song);

/* Reset a single lane to empty/inactive state. */
void lane_reset(lane_t *lane);

/* Called on loop restart: resets drum_seq cursor / piano_roll cursor. */
void lane_restart(lane_t *lane);

/* Called from audio task: advance every active lane by `tick_delta` ticks.
 * Handles loop restart (LIVE or SONG mode), mute, and playback mode logic.
 * `master_tick` is song->clock.tick_count after the current clock_advance(). */
void lanes_tick(song_t *song, uint32_t tick_delta, uint32_t master_tick);

/* ── Scene API ───────────────────────────────────────────────────────────── */
/* Capture current lane mute/solo/volume into scene slot idx. */
void scene_capture(song_t *song, int idx, const char *name);
/* Recall scene: apply mute/solo/volume from scene slot idx. */
void scene_recall(song_t *song, int idx);

/* ── Arrangement API ─────────────────────────────────────────────────────── */
/* Called from audio task each time a lane loop wraps (bar boundary).
 * Advances arrangement position and recalls next scene when needed. */
void arrangement_advance(song_t *song);

/* Apply volume + pan from a lane's float params to a stereo int32 bus pair.
 * Inline helper used by the audio task for each lane accumulation step. */
static inline void lane_apply_volume_pan(const lane_t *lane,
                                         int32_t *bus_l, int32_t *bus_r)
{
    /* Volume scale: float 0.0–1.0 → Q8 0..256 */
    int32_t vol = (int32_t)(lane->volume * 256.0f);
    int32_t pan_l = (int32_t)((1.0f - (lane->pan > 0 ? lane->pan : 0)) * 256.0f);
    int32_t pan_r = (int32_t)((1.0f + (lane->pan < 0 ? lane->pan : 0)) * 256.0f);
    *bus_l = (*bus_l * vol * pan_l) >> 16;
    *bus_r = (*bus_r * vol * pan_r) >> 16;
}

#ifdef __cplusplus
}
#endif
