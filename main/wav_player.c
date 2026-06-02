/*
 * wav_player.c — Phase 1 WAV streaming engine
 *
 * Architecture:
 *   sd_stream_task  (core 0, priority 10) — refills PSRAM ring buffers from SD
 *   wav_mix()       (core 1, audio task)  — reads rings, integer-lerp resample, mix
 *
 * Ring synchronisation: fill/read_idx/write_idx are updated with single-word
 * writes which are atomic on RISC-V. We rely on this rather than a mutex to
 * keep the audio hot-path lock-free.
 *
 * Resampler: linear interpolation, fixed-point 16.16.
 *   out[n] = last + (next - last) * phase >> 16
 *   phase advances by src_step = (src_sr << 16) / 48000 per output frame.
 */

#include "wav_player.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "esp_timer.h"

static const char *TAG = "wav";

/* Lane rings placed in PSRAM at link time — no runtime heap_caps_calloc needed. */
static EXT_RAM_BSS_ATTR wav_lane_t s_lane_storage[WAV_MAX_LANES];
static wav_lane_t *s_lanes[WAV_MAX_LANES];

/* Simple slot allocator — slots 0..WAV_MAX_LANES-1 assigned monotonically.
 * Drum rows claim slots at assign-time; WAV lanes claim at lane-create time.
 * Slots are never freed (PSRAM is plentiful and the pool covers all needs). */
static int s_next_slot = 0;

/* ── WAV header parser ───────────────────────────────────────────────────── */

/* Returns false on parse failure. Only reads the first 44 bytes (RIFF/fmt/data).
 * Works for canonical PCM WAVs; skips extra chunks to find "data". */
static bool wav_open_file(wav_file_t *w, const char *path)
{
    w->fp = fopen(path, "rb");
    if (!w->fp) {
        ESP_LOGE(TAG, "cannot open %s", path);
        return false;
    }

    uint8_t hdr[44];
    if (fread(hdr, 1, 44, w->fp) != 44) goto fail;

    /* RIFF */
    if (hdr[0]!='R'||hdr[1]!='I'||hdr[2]!='F'||hdr[3]!='F') goto fail;
    if (hdr[8]!='W'||hdr[9]!='A'||hdr[10]!='V'||hdr[11]!='E') goto fail;
    /* fmt  */
    if (hdr[12]!='f'||hdr[13]!='m'||hdr[14]!='t'||hdr[15]!=' ') goto fail;

    uint16_t audio_fmt  = (uint16_t)(hdr[20] | (hdr[21]<<8));
    uint16_t channels   = (uint16_t)(hdr[22] | (hdr[23]<<8));
    uint32_t sample_rate= (uint32_t)(hdr[24]|(hdr[25]<<8)|(hdr[26]<<16)|(hdr[27]<<24));
    uint16_t bits       = (uint16_t)(hdr[34] | (hdr[35]<<8));

    if (audio_fmt != 1) { ESP_LOGE(TAG, "non-PCM WAV"); goto fail; }
    if (bits != 16)     { ESP_LOGE(TAG, "non-16bit WAV (%d)", bits); goto fail; }
    if (channels < 1 || channels > 2) goto fail;

    w->channels    = (uint8_t)channels;
    w->sample_rate = sample_rate;
    w->bits        = 16;

    /* Scan for "data" chunk — may not be at offset 36 */
    fseek(w->fp, 36, SEEK_SET);
    char id[4];
    uint32_t chunk_size;
    int limit = 16; /* max chunks to scan */
    while (limit-- > 0) {
        if (fread(id, 1, 4, w->fp) != 4) goto fail;
        if (fread(&chunk_size, 4, 1, w->fp) != 1) goto fail;
        if (id[0]=='d'&&id[1]=='a'&&id[2]=='t'&&id[3]=='a') {
            w->data_offset = (uint32_t)ftell(w->fp);
            w->data_bytes  = chunk_size;
            w->pos         = 0;
            ESP_LOGI(TAG, "opened %s: %uHz %uch %u bytes", path,
                     (unsigned)sample_rate, (unsigned)channels,
                     (unsigned)chunk_size);
            return true;
        }
        fseek(w->fp, (long)chunk_size, SEEK_CUR);
    }
fail:
    if (w->fp) { fclose(w->fp); w->fp = NULL; }
    return false;
}

/* Read n_frames of native PCM into out (stereo interleaved int16).
 * Handles mono→stereo duplication. Returns frames actually read. */
static int wav_read_native(wav_file_t *w, int16_t *out, int n_frames)
{
    if (!w->fp || w->pos >= w->data_bytes) return 0;

    uint32_t bytes_per_frame = w->channels * 2u;
    uint32_t remaining_frames = (w->data_bytes - w->pos) / bytes_per_frame;
    if ((uint32_t)n_frames > remaining_frames) n_frames = (int)remaining_frames;
    if (n_frames <= 0) return 0;

    if (w->channels == 2) {
        size_t bytes = (size_t)n_frames * 4;
        if (fread(out, 1, bytes, w->fp) != bytes) n_frames = 0;
        w->pos += (uint32_t)(n_frames * 4);
    } else {
        /* mono: read n_frames int16s, duplicate to stereo */
        size_t bytes = (size_t)n_frames * 2;
        int16_t tmp[bytes / 2];
        size_t got = fread(tmp, 2, (size_t)n_frames, w->fp);
        w->pos += (uint32_t)(got * 2);
        for (size_t i = 0; i < got; i++) {
            out[i*2 + 0] = tmp[i];
            out[i*2 + 1] = tmp[i];
        }
        n_frames = (int)got;
    }
    return n_frames;
}

/* ── Ring helpers ────────────────────────────────────────────────────────── */

/* Space available in the ring for writing (frames). */
static inline uint16_t ring_space(const wav_lane_t *ln)
{
    return STREAM_BUF_FRAMES - ln->fill;
}

/* Push one stereo frame into the ring; caller must ensure space > 0. */
static inline void ring_push(wav_lane_t *ln, int16_t l, int16_t r)
{
    ln->buf[ln->write_idx * 2 + 0] = l;
    ln->buf[ln->write_idx * 2 + 1] = r;
    ln->write_idx = (uint16_t)((ln->write_idx + 1) & (STREAM_BUF_FRAMES - 1));
    ln->fill++;
}

/* Pop one stereo frame; caller must ensure fill > 0. */
static inline void ring_pop(wav_lane_t *ln, int16_t *l, int16_t *r)
{
    *l = ln->buf[ln->read_idx * 2 + 0];
    *r = ln->buf[ln->read_idx * 2 + 1];
    ln->read_idx = (uint16_t)((ln->read_idx + 1) & (STREAM_BUF_FRAMES - 1));
    ln->fill--;
}

/* ── SD streaming task ───────────────────────────────────────────────────── */
/* Runs on core 0. Wakes every 2 ms, refills any hungry ring. */

#define STREAM_TASK_PERIOD_MS  4
#define REFILL_THRESHOLD       (STREAM_BUF_FRAMES / 2)

/* Scratch buffer for native-rate reads: max one ring-worth of stereo frames */
static int16_t s_read_scratch[STREAM_BUF_FRAMES * 2];

/* Core 0 CPU load tracking */
static volatile uint8_t s_stream_load_pct = 0;

uint8_t wav_get_stream_load_pct(void) { return s_stream_load_pct; }

static void sd_stream_task(void *arg)
{
    (void)arg;
    uint32_t log_ticks = 0;
    uint64_t work_us_acc = 0;
    uint32_t period_us = (uint32_t)STREAM_TASK_PERIOD_MS * 1000u;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(STREAM_TASK_PERIOD_MS));
        int64_t t0 = esp_timer_get_time();

        for (int i = 0; i < WAV_MAX_LANES; i++) {
            wav_lane_t *ln = s_lanes[i];
            if (!ln || !ln->active) continue;
            if (ln->pcm_buf) continue;  /* fully preloaded — no streaming needed */
            if (ring_space(ln) < REFILL_THRESHOLD) continue;

            uint16_t space = ring_space(ln);
            /* How many native frames do we need to produce `space` output frames?
             * src_step = (src_sr << 16) / 48000
             * Each output frame consumes src_step/65536 native frames.
             * Request a safe upper bound: space * src_step / 65536 + 2 */
            uint32_t native_needed = ((uint32_t)space * ln->src_step >> 16) + 2;
            if (native_needed > STREAM_BUF_FRAMES) native_needed = STREAM_BUF_FRAMES;

            /* Read native frames into scratch */
            int got = wav_read_native(&ln->wav, s_read_scratch, (int)native_needed);

            /* EOF handling */
            if (got < (int)native_needed) {
                if (ln->play_mode == WAV_MODE_LOOP) {
                    /* Seek back to loop_start */
                    uint32_t seek_pos = ln->wav.data_offset + ln->loop_start;
                    fseek(ln->wav.fp, (long)seek_pos, SEEK_SET);
                    ln->wav.pos = ln->loop_start;
                    /* Fill remainder from loop start */
                    int remain = (int)native_needed - got;
                    int extra = wav_read_native(&ln->wav, s_read_scratch + got * 2, remain);
                    got += extra;
                } else if (got == 0) {
                    if (ln->retrigger_pending) {
                        /* One-shot hit EOF but a fresh hit is queued. Keep the
                         * lane active and drop the declick so wav_mix fires the
                         * queued re-hit immediately instead of losing it. */
                        ln->declick_frames = 0;
                    } else {
                        ln->active = false;
                    }
                    continue;
                }
            }

            /* Resample + push into ring using linear interpolation.
             * All arithmetic in int32 — no floats. */
            uint32_t phase = ln->src_phase;
            uint32_t step  = ln->src_step;
            int16_t ll = ln->last_l, lr = ln->last_r;
            int src_idx = 0;

            for (uint16_t f = 0; f < space && src_idx < got; f++) {
                /* Advance phase */
                while (phase >= 0x10000u && src_idx < got) {
                    ll = s_read_scratch[src_idx * 2 + 0];
                    lr = s_read_scratch[src_idx * 2 + 1];
                    src_idx++;
                    phase -= 0x10000u;
                }
                if (src_idx >= got) break;

                int16_t nl = s_read_scratch[src_idx * 2 + 0];
                int16_t nr = s_read_scratch[src_idx * 2 + 1];

                /* Linear interpolate: out = ll + (nl - ll) * phase / 65536 */
                int32_t out_l = ll + (((int32_t)(nl - ll) * (int32_t)phase) >> 16);
                int32_t out_r = lr + (((int32_t)(nr - lr) * (int32_t)phase) >> 16);

                ring_push(ln, (int16_t)out_l, (int16_t)out_r);
                phase += step;
            }

            ln->src_phase = phase;
            ln->last_l    = ll;
            ln->last_r    = lr;
        }

        /* Measure work time this iteration */
        int64_t t1 = esp_timer_get_time();
        work_us_acc += (uint64_t)(t1 - t0);
        log_ticks++;

        /* Log once per second (~1000ms / STREAM_TASK_PERIOD_MS iterations) */
        if (log_ticks >= (1000u / STREAM_TASK_PERIOD_MS)) {
            uint64_t budget_us = (uint64_t)log_ticks * period_us;
            s_stream_load_pct = (uint8_t)(work_us_acc * 100u / (budget_us ? budget_us : 1u));
            work_us_acc = 0;
            log_ticks   = 0;
        }
    }
}

/* ── Engine init ─────────────────────────────────────────────────────────── */

/* Wire up the lane pointer table. Idempotent and free of any task/SD work, so
 * it can run very early — before the boot song_load preloads drum samples,
 * which needs s_lanes[] populated to hand out slots. (Previously this lived in
 * wav_engine_init, called after song_load, so boot-time drum preloads silently
 * failed: wav_lane_get() returned NULL → empty buffers → no drum playback.) */
void wav_pool_init(void)
{
    for (int i = 0; i < WAV_MAX_LANES; i++) {
        s_lanes[i] = &s_lane_storage[i];
    }
}

/* Release every slot back to the allocator: stop playback, free preload
 * buffers, and rewind the monotonic slot counter. Called by song_load before
 * building the new song so reloading doesn't leak slots until the pool is
 * exhausted (lane_reset only memsets lanes, it never frees their wav slots). */
void wav_pool_reset(void)
{
    for (int i = 0; i < WAV_MAX_LANES; i++) {
        wav_lane_t *ln = s_lanes[i];
        if (!ln) continue;
        ln->active = false;
        if (ln->wav.fp) { fclose(ln->wav.fp); ln->wav.fp = NULL; }
        if (ln->pcm_buf) {
            heap_caps_free(ln->pcm_buf);
            ln->pcm_buf    = NULL;
            ln->pcm_frames = 0;
            ln->pcm_pos    = 0;
        }
    }
    s_next_slot = 0;
}

void wav_engine_init(void)
{
    wav_pool_init();
    xTaskCreatePinnedToCore(sd_stream_task, "sd_stream", 8192, NULL, 10, NULL, 0);
    ESP_LOGI(TAG, "WAV engine ready, %d lanes in PSRAM", WAV_MAX_LANES);
}

/* ── Pool accessor ───────────────────────────────────────────────────────── */

/* Claim the next free lane slot; returns -1 if the pool is exhausted. */
int wav_lane_alloc_slot(void)
{
    if (s_next_slot >= WAV_MAX_LANES) {
        ESP_LOGE(TAG, "wav lane pool exhausted");
        return -1;
    }
    return s_next_slot++;
}

/* Return a pointer to a pool slot by index, or NULL if out of range. */
wav_lane_t *wav_lane_get(int slot)
{
    if (slot < 0 || slot >= WAV_MAX_LANES) return NULL;
    return s_lanes[slot];
}

/* Maximum preload size: 8 MB stereo PCM (covers ~85s at 48kHz — any real sample).
 * Above this limit we fall back to the streaming path. */
#define PRELOAD_MAX_BYTES  (8 * 1024 * 1024)

/* Open a WAV file into a pool slot and fully preload its PCM into PSRAM.
 * After this call wav.fp is closed (NULL). Falls back to streaming if PSRAM
 * allocation fails or file exceeds PRELOAD_MAX_BYTES. */
bool wav_lane_open(int slot, const char *path)
{
    wav_lane_t *ln = wav_lane_get(slot);
    if (!ln) return false;

    ln->active = false;
    if (ln->wav.fp) { fclose(ln->wav.fp); ln->wav.fp = NULL; }

    /* Free any previous preload buffer */
    if (ln->pcm_buf) {
        heap_caps_free(ln->pcm_buf);
        ln->pcm_buf    = NULL;
        ln->pcm_frames = 0;
        ln->pcm_pos    = 0;
    }

    memset(&ln->wav, 0, sizeof(ln->wav));
    if (!wav_open_file(&ln->wav, path)) return false;

    ln->play_mode  = WAV_MODE_ONE_SHOT;
    ln->loop_start = 0;
    ln->loop_end   = ln->wav.data_bytes;
    ln->volume     = 200;

    /* Compute resampler ratio */
    uint32_t src_step;
    if (ln->wav.sample_rate == WAV_SAMPLE_RATE) {
        src_step = 0x10000u;
    } else {
        src_step = (uint32_t)((uint64_t)ln->wav.sample_rate << 16) / WAV_SAMPLE_RATE;
    }
    ln->src_step  = src_step;
    ln->src_phase = 0;
    ln->last_l    = 0;
    ln->last_r    = 0;

    /* ── PSRAM preload ─────────────────────────────────────────────────── */
    /* Estimate output frames after resampling */
    uint32_t src_frames   = ln->wav.data_bytes / (ln->wav.channels * 2u);
    uint32_t out_frames   = (uint32_t)((uint64_t)src_frames * WAV_SAMPLE_RATE / ln->wav.sample_rate) + 2;
    uint32_t out_bytes    = out_frames * 2u * sizeof(int16_t); /* stereo int16 */

    if (out_bytes <= PRELOAD_MAX_BYTES) {
        int16_t *buf = heap_caps_malloc(out_bytes, MALLOC_CAP_SPIRAM);
        if (buf) {
            /* Read and resample the entire file into buf */
            int16_t native[STREAM_BUF_FRAMES * 2];
            uint32_t phase    = 0;
            int16_t  ll = 0, lr = 0;
            uint32_t out_pos  = 0;

            while (out_pos < out_frames) {
                /* Read a chunk of native frames */
                int need = STREAM_BUF_FRAMES;
                int got  = wav_read_native(&ln->wav, native, need);
                if (got <= 0) break;

                int src_idx = 0;
                while (src_idx < got && out_pos < out_frames) {
                    while (phase >= 0x10000u && src_idx < got) {
                        ll = native[src_idx * 2 + 0];
                        lr = native[src_idx * 2 + 1];
                        src_idx++;
                        phase -= 0x10000u;
                    }
                    if (src_idx >= got) break;
                    int16_t nl = native[src_idx * 2 + 0];
                    int16_t nr = native[src_idx * 2 + 1];
                    buf[out_pos * 2 + 0] = (int16_t)(ll + (((int32_t)(nl - ll) * (int32_t)phase) >> 16));
                    buf[out_pos * 2 + 1] = (int16_t)(lr + (((int32_t)(nr - lr) * (int32_t)phase) >> 16));
                    out_pos++;
                    phase += src_step;
                }
            }

            ln->pcm_buf    = buf;
            ln->pcm_frames = out_pos;
            ln->pcm_pos    = 0;

            /* File no longer needed — close to free the file descriptor */
            fclose(ln->wav.fp);
            ln->wav.fp = NULL;

            ESP_LOGI(TAG, "preloaded %s: %lu frames (%lu bytes) in PSRAM",
                     path, (unsigned long)out_pos, (unsigned long)(out_pos * 4u));
        } else {
            ESP_LOGW(TAG, "PSRAM alloc failed for %s (%lu bytes), streaming", path, (unsigned long)out_bytes);
        }
    } else {
        ESP_LOGW(TAG, "%s too large for preload (%lu bytes), streaming", path, (unsigned long)out_bytes);
    }

    ln->write_idx         = 0;
    ln->read_idx          = 0;
    ln->fill              = 0;
    ln->retrigger_pending = false;
    ln->declick_frames    = 0;
    /* Do NOT set active — retrigger_ring activates when step fires */
    return true;
}

/* ── Lane control ────────────────────────────────────────────────────────── */

bool wav_lane_play(int lane, const char *path, wav_play_mode_t mode,
                   uint32_t loop_start, uint32_t loop_end, uint8_t volume)
{
    if (lane < 0 || lane >= WAV_MAX_LANES) return false;
    wav_lane_t *ln = s_lanes[lane];
    if (!ln) return false;

    /* Stop any existing playback */
    if (ln->active) {
        ln->active = false;
        if (ln->wav.fp) { fclose(ln->wav.fp); ln->wav.fp = NULL; }
    }
    if (ln->pcm_buf) { heap_caps_free(ln->pcm_buf); ln->pcm_buf = NULL; ln->pcm_frames = 0; ln->pcm_pos = 0; }

    memset(&ln->wav, 0, sizeof(ln->wav));
    if (!wav_open_file(&ln->wav, path)) return false;

    ln->play_mode  = mode;
    ln->loop_start = loop_start;
    ln->loop_end   = (loop_end == 0) ? ln->wav.data_bytes : loop_end;
    ln->volume     = volume;

    /* Precompute resampler ratio: (src_sr << 16) / 48000 */
    if (ln->wav.sample_rate == WAV_SAMPLE_RATE) {
        ln->src_step = 0x10000u; /* 1.0 — no resampling needed */
    } else {
        ln->src_step = (uint32_t)((uint64_t)ln->wav.sample_rate << 16) / WAV_SAMPLE_RATE;
    }
    ln->src_phase         = 0;
    ln->last_l            = 0;
    ln->last_r            = 0;
    ln->write_idx         = 0;
    ln->read_idx          = 0;
    ln->fill              = 0;
    ln->retrigger_pending = false;
    ln->declick_frames    = 0;
    /* pcm_buf already cleared above; streaming path keeps it NULL */
    ln->pcm_pos           = 0;

    ln->active = true;
    return true;
}

void wav_lane_stop(int lane)
{
    if (lane < 0 || lane >= WAV_MAX_LANES) return;
    wav_lane_t *ln = s_lanes[lane];
    if (!ln) return;
    ln->active = false;
    if (ln->wav.fp) { fclose(ln->wav.fp); ln->wav.fp = NULL; }
    if (ln->pcm_buf) { heap_caps_free(ln->pcm_buf); ln->pcm_buf = NULL; ln->pcm_frames = 0; ln->pcm_pos = 0; }
}

void wav_lane_set_volume(int lane, uint8_t vol)
{
    if (lane < 0 || lane >= WAV_MAX_LANES) return;
    wav_lane_t *ln = s_lanes[lane];
    if (ln) ln->volume = vol;
}

/* ── Audio mix (called from core-1 audio task) ───────────────────────────── */

void wav_mix(int16_t *dst, int n_frames)
{
    /* Accumulate into int32 to avoid overflow during mixing */
    static int32_t acc_l[WAV_MIX_FRAMES];
    static int32_t acc_r[WAV_MIX_FRAMES];

    memset(acc_l, 0, (size_t)n_frames * sizeof(int32_t));
    memset(acc_r, 0, (size_t)n_frames * sizeof(int32_t));

    for (int i = 0; i < WAV_MAX_LANES; i++) {
        wav_lane_t *ln = s_lanes[i];
        if (!ln || !ln->active) continue;

        if (ln->pcm_buf) {
            /* ── Preloaded PSRAM path ────────────────────────────────── */
            for (int f = 0; f < n_frames; f++) {
                /* Commit pending retrigger once declick fade reaches zero */
                if (ln->retrigger_pending && ln->declick_frames == 0) {
                    ln->pcm_pos           = ln->retrigger_pos;
                    ln->volume            = ln->retrigger_vol;
                    ln->retrigger_pending = false;
                    /* pcm_pos was already set to the frame offset, keep active */
                }
                if (ln->pcm_pos >= ln->pcm_frames) {
                    if (ln->retrigger_pending) {
                        /* A fresh hit is queued and the one-shot just ran out.
                         * Fire it now instead of deactivating — otherwise the
                         * lane goes inactive and the re-trigger is dropped, so
                         * rapid re-hits near the sample tail feel like they
                         * "wait" for the old sample to finish. */
                        ln->pcm_pos           = ln->retrigger_pos;
                        ln->volume            = ln->retrigger_vol;
                        ln->retrigger_pending = false;
                        ln->declick_frames    = 0;
                    } else if (ln->play_mode == WAV_MODE_LOOP) {
                        ln->pcm_pos = 0;
                    } else {
                        ln->active = false;
                        break;
                    }
                }
                int16_t l = ln->pcm_buf[ln->pcm_pos * 2 + 0];
                int16_t r = ln->pcm_buf[ln->pcm_pos * 2 + 1];
                ln->pcm_pos++;
                int32_t vol = (int32_t)ln->volume;
                if (ln->declick_frames > 0) {
                    vol = vol * ln->declick_frames / DECLICK_FRAMES;
                    ln->declick_frames--;
                }
                acc_l[f] += ((int32_t)l * vol) >> 8;
                acc_r[f] += ((int32_t)r * vol) >> 8;
            }
        } else {
            /* ── Streaming ring path (fallback) ─────────────────────── */
            for (int f = 0; f < n_frames; f++) {
                if (ln->retrigger_pending && ln->declick_frames == 0) {
                    ln->active    = false;
                    ln->read_idx  = 0;
                    ln->write_idx = 0;
                    ln->fill      = 0;
                    ln->src_phase = 0;
                    ln->last_l    = 0;
                    ln->last_r    = 0;
                    uint32_t seek_to = ln->wav.data_offset + ln->retrigger_pos;
                    fseek(ln->wav.fp, (long)seek_to, SEEK_SET);
                    ln->wav.pos           = ln->retrigger_pos;
                    ln->volume            = ln->retrigger_vol;
                    ln->retrigger_pending = false;
                    ln->active            = true;
                    break;
                }
                if (ln->fill == 0) break;
                int16_t l, r;
                ring_pop(ln, &l, &r);
                int32_t vol = (int32_t)ln->volume;
                if (ln->declick_frames > 0) {
                    vol = vol * ln->declick_frames / DECLICK_FRAMES;
                    ln->declick_frames--;
                }
                acc_l[f] += ((int32_t)l * vol) >> 8;
                acc_r[f] += ((int32_t)r * vol) >> 8;
            }
        }
    }

    /* Clamp and interleave into output buffer */
    for (int f = 0; f < n_frames; f++) {
        int32_t l = acc_l[f], r = acc_r[f];
        if (l >  32767) l =  32767;
        if (l < -32768) l = -32768;
        if (r >  32767) r =  32767;
        if (r < -32768) r = -32768;
        dst[f * 2 + 0] = (int16_t)l;
        dst[f * 2 + 1] = (int16_t)r;
    }
}
