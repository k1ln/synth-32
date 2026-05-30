#pragma once
/*
 * fx.h — Per-lane effect chain + lane ADSR (M4)
 *
 * Each fx_node_t is a vtable pointer to a heap-allocated effect in PSRAM.
 * fx_chain_process() runs the chain in order, in place on int16 L/R buffers.
 *
 * Lane ADSR (lane_adsr_t) applies a smooth amplitude envelope to the lane
 * bus — independent of synth voice ADSRs. Driven by lane play/stop events.
 *
 * All render paths: integer or fixed-point, no heap allocation in hot path.
 * Delay/reverb buffers allocated in PSRAM at fx_new() time.
 */

#include <stdint.h>
#include <stdbool.h>
#include "audio.h"   /* SAMPLE_RATE, AUDIO_BUF_FRAMES */

#ifdef __cplusplus
extern "C" {
#endif

/* ── FX type IDs ─────────────────────────────────────────────────────────── */
typedef enum {
    FX_TYPE_NONE = 0,
    FX_TYPE_FILTER,         /* LP/HP/BP/Notch SVF                              */
    FX_TYPE_EQ3,            /* 3-band: low-shelf + mid-peak + hi-shelf         */
    FX_TYPE_EQ5,            /* 5 fully parametric biquad bands                 */
    FX_TYPE_COMPRESSOR,     /* RMS + gain computer, block-rate                 */
    FX_TYPE_LIMITER,        /* brick-wall lookahead 2 ms                       */
    FX_TYPE_GATE,           /* RMS threshold + hold                            */
    FX_TYPE_TRANSIENT,      /* fast/slow env follower difference               */
    FX_TYPE_DISTORTION,     /* drive + tone, 4 modes                          */
    FX_TYPE_OVERDRIVE,      /* asymmetric soft-clip                            */
    FX_TYPE_WAVEFOLDER,     /* Buchla fold-back                                */
    FX_TYPE_BITCRUSH,       /* bit-depth + SR divider                         */
    FX_TYPE_DELAY,          /* stereo ping-pong delay, PSRAM buffer            */
    FX_TYPE_REVERB,         /* Freeverb: 8 comb + 4 allpass                   */
    FX_TYPE_CHORUS,         /* 3-tap LFO delay                                */
    FX_TYPE_FLANGER,        /* short LFO delay + feedback                      */
    FX_TYPE_PHASER,         /* 4-stage all-pass LFO                           */
    FX_TYPE_TREMOLO,        /* LFO amplitude mod                               */
    FX_TYPE_VIBRATO,        /* LFO pitch via delay line                        */
    FX_TYPE_RING_MOD,       /* × sine carrier                                  */
    FX_TYPE_PITCH_SHIFT,    /* granular ±24 semitones                          */
    FX_TYPE_AUTO_PAN,       /* LFO L↔R sweep                                  */
    FX_TYPE_STEREO_WIDTH,   /* mid/side width                                  */
    /* ── New types ──────────────────────────────────────────────────────── */
    FX_TYPE_NOISE_GATE_SC,  /* gate keyed from sidechain RMS (cross-lane)      */
    FX_TYPE_ENV_FOLLOWER,   /* extract amplitude → mod_out (not audio path)    */
    FX_TYPE_DEESSER,        /* dynamic HP in presence band                     */
    FX_TYPE_STEREO_IMAGER,  /* freq-split M/S: widen highs, keep lows mono     */
    FX_TYPE_TAPE_SAT,       /* hysteresis soft-clip + pre/de-emphasis EQ       */
    FX_TYPE_TUBE_AMP,       /* asymmetric waveshaper + DC drift                */
    FX_TYPE_EXCITER,        /* HF harmonic generator                           */
    FX_TYPE_HARMONIC_ENH,   /* add 2nd/3rd harmonic partials                   */
    FX_TYPE_FORMANT_FILT,   /* two resonant BP swept as vowel pairs            */
    FX_TYPE_COMB_FILTER,    /* fixed or LFO-swept comb                         */
    FX_TYPE_TILT_EQ,        /* single-knob shelving tilt                       */
    FX_TYPE_PITCH_QUANT,    /* snap pitch to musical scale in real time        */
    FX_TYPE_GRAN_FREEZE,    /* capture grain buffer + loop at variable rate    */
    FX_TYPE_STUTTER,        /* capture N ms + re-trigger rhythmically          */
    FX_TYPE_TAPE_STOP,      /* pitch-down slowdown on command                  */
    FX_TYPE_HAAS,           /* tiny delay on one channel for width             */
    FX_TYPE_RESONATOR,      /* tuned comb bank: noise → pitched texture        */
    FX_TYPE_FREEZE_REVERB,  /* reverb with infinite-feedback freeze gate       */
    FX_TYPE_STEP_FILTER,    /* step-sequencer driven cutoff, clock-synced      */
    FX_TYPE_SIDECHAIN_COMP, /* ducking from cross-lane RMS                     */
    FX_TYPE_TRANCE_GATE,    /* rhythmic amp chop, clock-synced pattern         */
    FX_TYPE_ARP_DELAY,      /* each repeat transposed by semitone interval     */
    FX_TYPE_COUNT
} fx_type_t;

/* Filter mode sub-types */
typedef enum { FILT_LP = 0, FILT_HP, FILT_BP, FILT_NOTCH } filt_mode_t;

/* Distortion mode sub-types */
typedef enum { DIST_SOFT = 0, DIST_HARD, DIST_TUBE, DIST_FUZZ } dist_mode_t;

/* ── FX vtable ───────────────────────────────────────────────────────────── */
typedef struct fx_node_s fx_node_t;

struct fx_node_s {
    fx_type_t type;
    bool      enabled;
    /* Process n_frames of stereo int16 audio in place.
     * bpm and tick_rate passed for clock-sync effects (delay). */
    void (*process)(fx_node_t *self,
                    int16_t *buf_l, int16_t *buf_r, int n_frames,
                    float bpm, uint32_t tick_rate);
    /* Set a named parameter (param_id values defined per type below). */
    void (*set_param)(fx_node_t *self, uint8_t param_id, float value);
    void (*free)(fx_node_t *self);
    /* Mirror of last value passed to set_param for param_id < 8.
     * Read by UI/WS to render slider values; never read by DSP. */
    float params[8];
    /* type-specific state follows inline (allocated as one PSRAM block) */
};

/* ── Lane ADSR ───────────────────────────────────────────────────────────── */
typedef enum {
    LADSR_IDLE = 0,
    LADSR_ATTACK,
    LADSR_DECAY,
    LADSR_SUSTAIN,
    LADSR_RELEASE,
} ladsr_stage_t;

typedef struct {
    ladsr_stage_t stage;
    float         level;     /* current output 0.0–1.0                         */
    float         atk_rate;  /* per-sample increment in attack                 */
    float         dcy_rate;  /* per-sample decrement in decay                  */
    float         sus_level; /* sustain level 0.0–1.0                         */
    float         rel_rate;  /* per-sample decrement in release                */
    /* serialisable params (ms) */
    float         atk_ms, dcy_ms, sus, rel_ms;
} lane_adsr_t;

/* ── API ─────────────────────────────────────────────────────────────────── */

/* Lane ADSR */
void lane_adsr_init(lane_adsr_t *a, float atk_ms, float dcy_ms,
                    float sus, float rel_ms);
/* Update params + derived rates in place (live edit; preserves stage/level). */
void lane_adsr_set_params(lane_adsr_t *a, float atk_ms, float dcy_ms,
                          float sus, float rel_ms);
void lane_adsr_gate_on(lane_adsr_t *a);
void lane_adsr_gate_off(lane_adsr_t *a);
/* Apply envelope to int16 L/R buffers in place; returns peak |level|. */
float lane_adsr_process(lane_adsr_t *a, int16_t *buf_l, int16_t *buf_r,
                        int n_frames);

/* FX chain — operates on a fixed-size fx_node_t* array in lane_t */
#define FX_MAX_PER_LANE 8

void fx_chain_process(fx_node_t **chain, int count,
                      int16_t *buf_l, int16_t *buf_r, int n_frames,
                      float bpm, uint32_t tick_rate);

void fx_chain_set_param(fx_node_t **chain, int count, int slot,
                        uint8_t param_id, float value);

/* Free all nodes in chain, NULL out pointers. */
void fx_chain_free(fx_node_t **chain, int *count);

/* Insert node at slot, shifting later nodes right. Returns false if full. */
bool fx_chain_insert(fx_node_t **chain, int *count, int slot, fx_node_t *node);

/* Remove and free node at slot, shifting later nodes left. */
void fx_chain_remove(fx_node_t **chain, int *count, int slot);

/* Move node from src_slot to dst_slot (swap). */
void fx_chain_move(fx_node_t **chain, int count, int src_slot, int dst_slot);

/* ── Factory functions ───────────────────────────────────────────────────── */
/* Param IDs are common across types where named the same. */

/* FX_TYPE_FILTER  params: 0=cutoff_hz, 1=resonance, 2=mode(filt_mode_t) */
fx_node_t *fx_filter_new(filt_mode_t mode, float cutoff_hz, float resonance);

/* FX_TYPE_EQ3  params: 0=low_gain_db, 1=mid_gain_db, 2=hi_gain_db,
 *                      3=mid_freq_hz, 4=mid_q */
fx_node_t *fx_eq3_new(void);

/* FX_TYPE_EQ5  params: band*5 + {gain_db, freq_hz, q, type(0=peak,1=ls,2=hs), en} */
fx_node_t *fx_eq5_new(void);

/* FX_TYPE_COMPRESSOR  params: 0=thresh_db, 1=ratio, 2=atk_ms, 3=rel_ms,
 *                             4=makeup_db */
fx_node_t *fx_compressor_new(void);

/* FX_TYPE_LIMITER  params: 0=thresh_db, 1=lookahead_ms */
fx_node_t *fx_limiter_new(void);

/* FX_TYPE_GATE  params: 0=thresh_db, 1=hold_ms, 2=atk_ms, 3=rel_ms */
fx_node_t *fx_gate_new(void);

/* FX_TYPE_TRANSIENT  params: 0=attack_gain, 1=sustain_gain */
fx_node_t *fx_transient_new(void);

/* FX_TYPE_DISTORTION  params: 0=drive, 1=tone, 2=mode(dist_mode_t) */
fx_node_t *fx_distortion_new(dist_mode_t mode, float drive);

/* FX_TYPE_OVERDRIVE  params: 0=drive, 1=tone */
fx_node_t *fx_overdrive_new(float drive);

/* FX_TYPE_WAVEFOLDER  params: 0=fold, 1=gain */
fx_node_t *fx_wavefolder_new(void);

/* FX_TYPE_BITCRUSH  params: 0=bits(1–16), 1=sr_div(1–32) */
fx_node_t *fx_bitcrush_new(int bits, int sr_div);

/* FX_TYPE_DELAY  params: 0=time_ms (or 0=sync_div when clock_sync=1),
 *                        1=feedback, 2=mix, 3=ping_pong(0/1),
 *                        4=clock_sync(0/1) */
fx_node_t *fx_delay_new(float time_ms, float feedback, float mix);

/* FX_TYPE_REVERB  params: 0=room, 1=damp, 2=width, 3=mix */
fx_node_t *fx_reverb_new(float room, float damp, float mix);

/* FX_TYPE_CHORUS  params: 0=rate_hz, 1=depth_ms, 2=mix */
fx_node_t *fx_chorus_new(void);

/* FX_TYPE_FLANGER  params: 0=rate_hz, 1=depth_ms, 2=feedback, 3=mix */
fx_node_t *fx_flanger_new(void);

/* FX_TYPE_PHASER  params: 0=rate_hz, 1=depth, 2=feedback, 3=mix */
fx_node_t *fx_phaser_new(void);

/* FX_TYPE_TREMOLO  params: 0=rate_hz, 1=depth */
fx_node_t *fx_tremolo_new(void);

/* FX_TYPE_VIBRATO  params: 0=rate_hz, 1=depth_ms */
fx_node_t *fx_vibrato_new(void);

/* FX_TYPE_RING_MOD  params: 0=carrier_hz, 1=mix */
fx_node_t *fx_ring_mod_new(float carrier_hz);

/* FX_TYPE_PITCH_SHIFT  params: 0=semitones(-24..+24) */
fx_node_t *fx_pitch_shift_new(float semitones);

/* FX_TYPE_AUTO_PAN  params: 0=rate_hz, 1=depth */
fx_node_t *fx_auto_pan_new(float rate_hz);

/* FX_TYPE_STEREO_WIDTH  params: 0=width(0=mono,1=normal,2=wide) */
fx_node_t *fx_stereo_width_new(float width);

/* ── New factory functions ───────────────────────────────────────────────── */

/* FX_TYPE_NOISE_GATE_SC  params: 0=thresh_db, 1=hold_ms, 2=atk_ms, 3=rel_ms
 * Reads from fx_sidechain_rms[] indexed by lane_id (set by audio task). */
fx_node_t *fx_noise_gate_sc_new(void);

/* FX_TYPE_ENV_FOLLOWER  params: 0=atk_ms, 1=rel_ms
 * Writes to fx_env_follower_out[lane_id]; audio passes through unchanged. */
fx_node_t *fx_env_follower_new(void);

/* FX_TYPE_DEESSER  params: 0=freq_hz, 1=thresh_db, 2=ratio */
fx_node_t *fx_deesser_new(void);

/* FX_TYPE_STEREO_IMAGER  params: 0=crossover_hz, 1=width */
fx_node_t *fx_stereo_imager_new(float crossover_hz, float width);

/* FX_TYPE_TAPE_SAT  params: 0=drive, 1=bias, 2=mix */
fx_node_t *fx_tape_sat_new(float drive);

/* FX_TYPE_TUBE_AMP  params: 0=drive, 1=bias, 2=tone */
fx_node_t *fx_tube_amp_new(float drive);

/* FX_TYPE_EXCITER  params: 0=freq_hz, 1=drive, 2=mix */
fx_node_t *fx_exciter_new(void);

/* FX_TYPE_HARMONIC_ENH  params: 0=h2_level, 1=h3_level */
fx_node_t *fx_harmonic_enh_new(void);

/* FX_TYPE_FORMANT_FILT  params: 0=vowel(0–4: A E I O U), 1=morph, 2=mix */
fx_node_t *fx_formant_filt_new(void);

/* FX_TYPE_COMB_FILTER  params: 0=freq_hz, 1=feedback, 2=mix, 3=lfo_rate */
fx_node_t *fx_comb_filter_new(float freq_hz, float feedback);

/* FX_TYPE_TILT_EQ  params: 0=tilt_db (-12..+12, positive = brighten) */
fx_node_t *fx_tilt_eq_new(float tilt_db);

/* FX_TYPE_PITCH_QUANT  params: 0=root(0–11), 1=scale(0=chrom,1=major,2=minor,
 *                              3=pent_maj,4=pent_min,5=blues,6=dorian,7=phryg)
 * Works on pitched monophonic input (shifts freq via short delay). */
fx_node_t *fx_pitch_quant_new(int root, int scale);

/* FX_TYPE_GRAN_FREEZE  params: 0=freeze(0/1), 1=rate, 2=scatter, 3=mix */
fx_node_t *fx_gran_freeze_new(void);

/* FX_TYPE_STUTTER  params: 0=length_ms, 1=trigger(0/1), 2=clock_sync(0/1),
 *                          3=sync_div */
fx_node_t *fx_stutter_new(void);

/* FX_TYPE_TAPE_STOP  params: 0=trigger(0/1), 1=time_ms */
fx_node_t *fx_tape_stop_new(void);

/* FX_TYPE_HAAS  params: 0=delay_ms(1–35), 1=side(0=L,1=R) */
fx_node_t *fx_haas_new(float delay_ms);

/* FX_TYPE_RESONATOR  params: 0=root_hz, 1=decay, 2=count(2–8), 3=mix */
fx_node_t *fx_resonator_new(float root_hz, int count);

/* FX_TYPE_FREEZE_REVERB  params: 0=room, 1=damp, 2=mix, 3=freeze(0/1) */
fx_node_t *fx_freeze_reverb_new(void);

/* FX_TYPE_STEP_FILTER  params: 0=step_count(4–16), 1=clock_sync,
 *                              2..17=step values (cutoff 0–1 per step) */
fx_node_t *fx_step_filter_new(void);

/* FX_TYPE_SIDECHAIN_COMP  params: 0=thresh_db, 1=ratio, 2=atk_ms, 3=rel_ms,
 *                                 4=makeup_db, 5=src_lane(0–15) */
fx_node_t *fx_sidechain_comp_new(void);

/* FX_TYPE_TRANCE_GATE  params: 0=pattern(uint16 bitmask), 1=step_div,
 *                              2=atk_ms, 3=rel_ms */
fx_node_t *fx_trance_gate_new(void);

/* FX_TYPE_ARP_DELAY  params: 0=time_ms, 1=feedback, 2=mix,
 *                            3=semitone_step, 4=max_repeats */
fx_node_t *fx_arp_delay_new(void);

/* Sidechain bus — audio task writes per-lane RMS here before fx_chain_process.
 * FX_TYPE_NOISE_GATE_SC and FX_TYPE_SIDECHAIN_COMP read from this. */
#define FX_SIDECHAIN_LANES 16
extern float fx_sidechain_rms[FX_SIDECHAIN_LANES];

/* Envelope follower output bus — written by FX_TYPE_ENV_FOLLOWER. */
extern float fx_env_follower_out[FX_SIDECHAIN_LANES];

/* Generic: allocate by type with default parameters. */
fx_node_t *fx_new(fx_type_t type);

#ifdef __cplusplus
}
#endif
