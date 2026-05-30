#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_codec_dev.h"
#include "synth.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Wavetable / voice constants ─────────────────────────────────────────── */
#define WAVETABLE_SIZE   256
#define SAMPLE_RATE      48000
#define AUDIO_BUF_FRAMES 64
#define WT_AMP           28000
#define NUM_VOICES       8
#define SUSTAIN_LEVEL    0.7f

/* ── ADSR envelope state ─────────────────────────────────────────────────── */
typedef enum { ST_IDLE, ST_ATTACK, ST_DECAY, ST_SUSTAIN, ST_RELEASE } adsr_st_t;

typedef struct {
    volatile uint32_t phase_step;
    volatile bool     note_on;
    volatile int      key_id;
    uint32_t  phase_acc;
    adsr_st_t st;
    float     env_amp;
    float     rel_amp;
    uint32_t  env_cnt;
    bool      prev_note;
} voice_t;

/* ── Polyphonic live-key gain normalisation (written by ui.c, read by audio.c) ── */
/* Number of keys currently held in LIVE mode (0 = none / silent). */
extern volatile int g_live_held_count;
/* g_song.lanes[] index of the live lane, or -1 when not in LIVE mode. */
extern volatile int g_live_lane_idx;

/* ── Globals exposed to ui.c ─────────────────────────────────────────────── */
extern int16_t        wt_square[WAVETABLE_SIZE];
extern int16_t        wt_sawtooth[WAVETABLE_SIZE];
extern int16_t        wt_sine[WAVETABLE_SIZE];
extern int16_t        wt_triangle[WAVETABLE_SIZE];
extern const int16_t *volatile s_wavetable;

extern voice_t s_voices[NUM_VOICES];

extern volatile uint32_t s_attack_samp;
extern volatile uint32_t s_decay_samp;
extern volatile uint32_t s_release_samp;

extern esp_codec_dev_handle_t s_spk;

/* ── Note event queue ────────────────────────────────────────────────────── */
/* UI task (core 0) posts events here; audio task (core 1) drains them at   */
/* the start of each block. Eliminates the data-race click on live playing. */
#define NOTE_Q_SIZE  32   /* must be power of two */

typedef struct {
    synth_inst_t *synth;   /* target synth instance */
    uint8_t       note;
    uint8_t       vel;     /* 0 = note_off, >0 = note_on */
} note_evt_t;

/* Post a note_on or note_off from any task. Thread-safe (SPSC). */
void audio_note_on (synth_inst_t *synth, uint8_t note, uint8_t vel);
void audio_note_off(synth_inst_t *synth, uint8_t note);

/* Request an immediate all-notes-off on the audio thread (safe from any task). */
void audio_all_notes_off(void);

/* ── API ─────────────────────────────────────────────────────────────────── */
void     i2c_init(void);
void     audio_init(void);          /* calls i2c_init, starts audio_task */
void     wavetable_init(void);
uint32_t freq_to_step(float hz);
int      alloc_voice(int key_id, uint32_t phase_step);

/* Send note_off to every active synth voice and reset the legacy voice pool.
 * Call from the UI task when the user hits STOP. */
void     audio_panic(void);

/* Stem export: record each active lane to its own WAV file in dir/ */
void     stem_export_start(const char *dir);
void     stem_export_stop(void);

/* Sound-browser preview: mix the given wav pool slot into the master bus.
 * Pass -1 to disable. Safe to call from UI task. */
void     audio_set_preview_slot(int slot);

#ifdef __cplusplus
}
#endif
