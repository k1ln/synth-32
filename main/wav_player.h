#pragma once
/*
 * Phase 1 — WAV playback engine
 * Streams 16-bit PCM WAVs from SD card into PSRAM ring buffers.
 * 16 lanes, each with an independent sd_stream_task feed.
 *
 * All heap allocations go to PSRAM (MALLOC_CAP_SPIRAM).
 * Audio hot-path uses integer math only — no floats, no malloc.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Constants ───────────────────────────────────────────────────────────── */
#define WAV_MAX_LANES        32
#define STREAM_BUF_FRAMES    512   /* stereo frames per ring — ~11ms at 48kHz */
#define WAV_SAMPLE_RATE      48000
#define WAV_MIX_FRAMES       64    /* frames per audio callback — must match AUDIO_BUF_FRAMES */
#define DECLICK_FRAMES       96    /* ~2ms fade-out before retrigger */

/* ── Playback modes ──────────────────────────────────────────────────────── */
typedef enum {
    WAV_MODE_ONE_SHOT = 0, /* play once, stop at EOF */
    WAV_MODE_LOOP,         /* loop between loop_start and loop_end */
    WAV_MODE_GATE,         /* play while lane is held, stop on release */
} wav_play_mode_t;

/* ── WAV file descriptor ─────────────────────────────────────────────────── */
typedef struct {
    FILE    *fp;
    uint32_t data_offset;  /* byte offset of PCM data in file */
    uint32_t data_bytes;   /* total PCM bytes */
    uint32_t pos;          /* current read position within PCM data */
    uint8_t  channels;     /* 1 or 2 */
    uint32_t sample_rate;  /* native SR (may differ from WAV_SAMPLE_RATE) */
    uint8_t  bits;         /* 16 only */
} wav_file_t;

/* ── Per-lane stream ring (lives in PSRAM) ───────────────────────────────── */
typedef struct {
    int16_t  buf[STREAM_BUF_FRAMES * 2]; /* stereo interleaved — streaming fallback */
    uint16_t write_idx;
    uint16_t read_idx;
    uint16_t fill;
    bool     active;

    /* Playback parameters */
    wav_play_mode_t play_mode;
    uint32_t loop_start;  /* byte offset of loop start in PCM data */
    uint32_t loop_end;    /* byte offset of loop end (0 = EOF) */

    /* Fixed-point resampler state (16.16) — used only for streaming path */
    uint32_t src_step;    /* (src_sr << 16) / WAV_SAMPLE_RATE */
    uint32_t src_phase;
    int16_t  last_l;
    int16_t  last_r;

    /* WAV file handle — owned by this lane (NULL when fully preloaded) */
    wav_file_t wav;

    /* Volume: 0..256 (Q8, 256 = unity) */
    uint8_t volume;

    /* Declick: pending retrigger applied by the audio drain after a short fade. */
    volatile bool    retrigger_pending;
    uint32_t         retrigger_pos;    /* new seek byte offset within PCM data */
    uint8_t          retrigger_vol;    /* volume to apply after retrigger */
    uint16_t         declick_frames;   /* counts down from DECLICK_FRAMES to 0 */

    /* PSRAM preload: when pcm_buf != NULL the entire file is resident in PSRAM.
     * The streaming ring and fseek/fread path are not used.
     * pcm_buf holds stereo interleaved int16 at WAV_SAMPLE_RATE; pcm_frames is
     * the total number of stereo frames; pcm_pos is the current read cursor. */
    int16_t  *pcm_buf;    /* heap_caps_malloc(MALLOC_CAP_SPIRAM), NULL if streaming */
    uint32_t  pcm_frames; /* total stereo frames in pcm_buf */
    uint32_t  pcm_pos;    /* read cursor: next frame index to output */
} wav_lane_t;

/* ── Public API ──────────────────────────────────────────────────────────── */

/* Call once after SD card is mounted. Allocates all lane rings in PSRAM
 * and starts the sd_stream_task on core 0. */
void wav_engine_init(void);

/* Claim the next free lane slot; returns -1 if the pool is exhausted. */
int wav_lane_alloc_slot(void);

/* Return a pointer to a pool slot by index (for drum rows). */
wav_lane_t *wav_lane_get(int slot);

/* Open a WAV file into an already-allocated pool slot.
 * Sets up the resampler; does NOT start playback (retrigger_ring does that). */
bool wav_lane_open(int slot, const char *path);

/* Load a WAV file onto a lane and start streaming.
 * lane: 0..WAV_MAX_LANES-1
 * path: absolute SD path, e.g. "/sdcard/sounds/kick.wav"
 * Returns true on success. */
bool wav_lane_play(int lane, const char *path, wav_play_mode_t mode,
                   uint32_t loop_start, uint32_t loop_end, uint8_t volume);

/* Stop a lane and close its file handle. */
void wav_lane_stop(int lane);

/* Set per-lane volume (0..255, 128 = unity-ish, 256 = full). */
void wav_lane_set_volume(int lane, uint8_t vol);

/* Called from the audio task (core 1) to mix all active lanes into dst.
 * dst: stereo interleaved int16, WAV_MIX_FRAMES frames.
 * The caller is responsible for clamping the final sum before I2S write. */
void wav_mix(int16_t *dst, int n_frames);

/* Returns the last-measured core 0 sd_stream_task CPU load (0-100%). */
uint8_t wav_get_stream_load_pct(void);

#ifdef __cplusplus
}
#endif
