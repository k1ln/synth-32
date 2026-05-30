#pragma once
/*
 * synth.h — Synth instrument vtable + factory declarations (Phase 3)
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct synth_inst_s synth_inst_t;

struct synth_inst_s {
    void (*render   )(synth_inst_t *self, int16_t *out_l, int16_t *out_r, int n_frames);
    void (*note_on  )(synth_inst_t *self, uint8_t note, uint8_t vel);
    void (*note_off )(synth_inst_t *self, uint8_t note);
    void (*set_param)(synth_inst_t *self, uint8_t param_id, float value);
    void (*free     )(synth_inst_t *self);
    uint8_t  type_id;
    uint8_t  voice_count;
    /* type-specific state allocated after this header in PSRAM */
};

/* ── Type IDs ────────────────────────────────────────────────────────────── */
#define SYNTH_TYPE_MONO_WT      0
#define SYNTH_TYPE_POLY_WT      1
#define SYNTH_TYPE_SUPERSAW     2
#define SYNTH_TYPE_FM2          3
#define SYNTH_TYPE_FM4          4
#define SYNTH_TYPE_SUBTRACTIVE  5
#define SYNTH_TYPE_KS           6
#define SYNTH_TYPE_BELL         7
#define SYNTH_TYPE_PAD          8
#define SYNTH_TYPE_NOISE        9
#define SYNTH_TYPE_BASS         10
#define SYNTH_TYPE_LEAD         11
#define SYNTH_TYPE_CHORD        12
#define SYNTH_TYPE_DRUM_BD      13
#define SYNTH_TYPE_DRUM_SD      14
#define SYNTH_TYPE_DRUM_HH      15
#define SYNTH_TYPE_ORGAN        16
#define SYNTH_TYPE_MORPH        17
#define SYNTH_TYPE_VOWEL        18
#define SYNTH_TYPE_BITCRUSH     19
#define SYNTH_TYPE_COUNT        20

/* ── Per-type factory functions (defined in synth_osc.c / synth_phys.c / synth_extra.c) ── */
synth_inst_t *synth_mono_wt_new(void);
synth_inst_t *synth_poly_wt_new(void);
synth_inst_t *synth_supersaw_new(void);
synth_inst_t *synth_fm2_new(void);
synth_inst_t *synth_fm4_new(void);
synth_inst_t *synth_subtractive_new(void);
synth_inst_t *synth_ks_new(void);
synth_inst_t *synth_bell_new(void);
synth_inst_t *synth_pad_new(void);
synth_inst_t *synth_noise_new(void);
synth_inst_t *synth_bass_new(void);
synth_inst_t *synth_lead_new(void);
synth_inst_t *synth_chord_new(void);
synth_inst_t *synth_drum_bd_new(void);
synth_inst_t *synth_drum_sd_new(void);
synth_inst_t *synth_drum_hh_new(void);
synth_inst_t *synth_organ_new(void);
synth_inst_t *synth_morph_new(void);
synth_inst_t *synth_vowel_new(void);
synth_inst_t *synth_bitcrush_new(void);

/* Dispatcher: allocate synth instance by type_id. Returns NULL if unknown. */
synth_inst_t *synth_new(uint8_t type_id);

/* Free a synth instance (calls inst->free if non-NULL). */
void synth_free(synth_inst_t *inst);

#ifdef __cplusplus
}
#endif
