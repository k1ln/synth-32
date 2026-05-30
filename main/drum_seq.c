/*
 * drum_seq.c — Drum step sequencer (Phase 2.5)
 */

#include <string.h>
#include <stdlib.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "drum_seq.h"

static const char *TAG = "drum_seq";

/* ── Allocation ──────────────────────────────────────────────────────────── */

drum_seq_t *drum_seq_alloc(void)
{
    drum_seq_t *seq = heap_caps_calloc(1, sizeof(drum_seq_t), MALLOC_CAP_SPIRAM);
    if (!seq) { ESP_LOGE(TAG, "drum_seq alloc failed"); return NULL; }
    seq->step_count  = 16;
    seq->accent_gain = 1.5f;

    /* Pre-populate with 4 blank rows so the grid is immediately assignable
     * without the user needing to tap +ROW first. */
    int initial_rows = 4;
    if (initial_rows > DRUM_MAX_ROWS) initial_rows = DRUM_MAX_ROWS;
    for (int r = 0; r < initial_rows; r++) {
        seq->rows[r].volume     = 1.0f;
        seq->rows[r].step_count = seq->step_count;
    }
    seq->row_count = (uint8_t)initial_rows;
    return seq;
}

bool drum_seq_alloc_rings(drum_seq_t *seq, wav_lane_t *pool,
                          int pool_start, int pool_count)
{
    int slot = pool_start;
    for (int r = 0; r < seq->row_count; r++) {
        drum_row_t *row = &seq->rows[r];
        if (slot >= pool_start + pool_count) {
            ESP_LOGE(TAG, "no pool slots for row %d", r);
            return false;
        }
        row->ring      = &pool[slot];
        row->lane_slot = slot;
        slot++;

        /* Multi-hit rings */
        for (int h = 0; h < row->multi_hit_count && h < DRUM_MULTI_HIT_MAX; h++) {
            if (slot >= pool_start + pool_count) {
                ESP_LOGE(TAG, "no pool slot for multi-hit row %d hit %d", r, h);
                return false;
            }
            row->multi_ring[h] = &pool[slot];
            slot++;
        }
    }
    return true;
}

void drum_seq_update_timing(drum_seq_t *seq, uint32_t loop_len_ticks)
{
    if (seq->step_count == 0) return;
    seq->ticks_per_step = loop_len_ticks / seq->step_count;
}

void drum_seq_reset(drum_seq_t *seq, uint32_t lane_tick_now)
{
    uint8_t sc = seq->step_count ? seq->step_count : 16;
    /* Set to one-before-first so the first tick increment lands on step 0 */
    seq->current_step   = sc - 1;
    seq->last_step_tick = lane_tick_now;
    seq->bar_count      = 0;
    for (int r = 0; r < seq->row_count; r++) {
        uint8_t rs = seq->rows[r].step_count ? seq->rows[r].step_count : sc;
        seq->rows[r].current_step   = rs - 1;
        seq->rows[r].last_step_tick = lane_tick_now;
        seq->rows[r].pass_count     = 0;
    }
    memset(seq->mute_group_last, 0xFF, sizeof(seq->mute_group_last));
}

bool drum_seq_remove_row(drum_seq_t *seq, int ri)
{
    if (!seq || ri < 0 || ri >= seq->row_count) return false;

    drum_row_t *row = &seq->rows[ri];

    /* Stop and free the primary ring's PSRAM PCM buffer */
    if (row->ring) {
        wav_lane_stop(row->lane_slot);
        if (row->ring->pcm_buf) {
            heap_caps_free(row->ring->pcm_buf);
            row->ring->pcm_buf    = NULL;
            row->ring->pcm_frames = 0;
            row->ring->pcm_pos    = 0;
        }
    }

    /* Stop multi-hit rings */
    for (int h = 0; h < row->multi_hit_count && h < DRUM_MULTI_HIT_MAX; h++) {
        if (row->multi_ring[h]) {
            wav_lane_stop(row->multi_ring[h] - row->ring + row->lane_slot);
            if (row->multi_ring[h]->pcm_buf) {
                heap_caps_free(row->multi_ring[h]->pcm_buf);
                row->multi_ring[h]->pcm_buf    = NULL;
                row->multi_ring[h]->pcm_frames = 0;
            }
        }
    }

    /* Shift subsequent rows down by one */
    int rows_to_move = seq->row_count - ri - 1;
    if (rows_to_move > 0)
        memmove(&seq->rows[ri], &seq->rows[ri + 1],
                rows_to_move * sizeof(drum_row_t));

    seq->row_count--;
    memset(&seq->rows[seq->row_count], 0, sizeof(drum_row_t));
    return true;
}

/* ── Euclidean / Bjorklund ───────────────────────────────────────────────── */

void drum_seq_euclidean(drum_row_t *row, uint8_t hits, uint8_t steps, uint8_t velocity)
{
    if (steps == 0 || steps > DRUM_MAX_STEPS) return;
    if (hits > steps) hits = steps;

    /* Bjorklund algorithm via Bresenham-style distribution */
    int pattern[DRUM_MAX_STEPS] = {0};
    if (hits == 0) {
        for (int i = 0; i < steps; i++) row->steps[i].velocity = 0;
        row->step_count = steps;
        return;
    }

    int bucket = 0;
    for (int i = 0; i < steps; i++) {
        bucket += hits;
        if (bucket >= steps) { bucket -= steps; pattern[i] = 1; }
        else                 { pattern[i] = 0; }
    }
    for (int i = 0; i < steps; i++) {
        row->steps[i].velocity   = pattern[i] ? velocity : 0;
        row->steps[i].probability = 100;
        row->steps[i].ratchet     = 1;
        row->steps[i].condition   = STEP_COND_ALWAYS;
    }
    row->step_count = steps;
}

/* ── Sample loading ──────────────────────────────────────────────────────── */

bool drum_row_load_wav(drum_row_t *row, const char *sdcard_path)
{
    /* Allocate a lane slot if this row doesn't have one yet */
    if (!row->ring) {
        int slot = wav_lane_alloc_slot();
        if (slot < 0) {
            ESP_LOGE(TAG, "drum_row_load_wav: no lane slots left");
            return false;
        }
        row->ring      = wav_lane_get(slot);
        row->lane_slot = slot;
        if (!row->ring) return false;
    }

    /* Open the WAV file into the ring's lane slot */
    bool ok = wav_lane_open(row->lane_slot, sdcard_path);
    if (!ok) ESP_LOGE(TAG, "drum_row_load_wav: failed to open %s", sdcard_path);
    return ok;
}

/* ── Row trigger ─────────────────────────────────────────────────────────── */

/*
 * Schedule a retrigger: record the new start position and volume so the audio
 * drain applies a short declick fade then resets the ring on the audio thread.
 * This avoids the hard flush-to-zero click that would occur if we reset here.
 */
static void retrigger_ring(wav_lane_t *ring, uint32_t start_offset_bytes)
{
    if (!ring) return;

    if (ring->pcm_buf) {
        /* Preloaded PSRAM path: convert byte offset to frame index */
        uint32_t frame_offset = start_offset_bytes / 4u; /* 4 bytes per stereo frame */
        if (frame_offset >= ring->pcm_frames) frame_offset = 0;

        if (!ring->active) {
            /* Idle: start immediately. ring->volume was set by trigger_row
             * just before this call; do not overwrite it from retrigger_vol
             * (which is uninitialised on the first hit and is what made
             * fresh drum rows silent). */
            ring->pcm_pos = frame_offset;
            ring->active  = true;
        } else {
            /* Playing: schedule declick then retrigger */
            ring->retrigger_pos     = frame_offset;
            ring->retrigger_vol     = ring->volume;
            ring->declick_frames    = DECLICK_FRAMES;
            ring->retrigger_pending = true;
        }
        return;
    }

    /* Streaming path */
    if (!ring->wav.fp) return;
    ring->retrigger_pos     = start_offset_bytes;
    ring->retrigger_vol     = ring->volume;
    ring->declick_frames    = ring->active ? DECLICK_FRAMES : 0;
    ring->retrigger_pending = true;
    if (!ring->active) {
        ring->active    = false;
        ring->read_idx  = 0;
        ring->write_idx = 0;
        ring->fill      = 0;
        ring->src_phase = 0;
        ring->last_l    = 0;
        ring->last_r    = 0;
        uint32_t seek_to = ring->wav.data_offset + start_offset_bytes;
        fseek(ring->wav.fp, (long)seek_to, SEEK_SET);
        ring->wav.pos           = start_offset_bytes;
        ring->retrigger_pending = false;
        ring->active            = true;
    }
}

static void trigger_row(drum_row_t *row, uint8_t velocity, bool accent, float accent_gain)
{
    if (!row->ring) return;

    float vel_scale = (float)velocity / 127.0f;
    float gain      = vel_scale * row->volume * (accent ? accent_gain : 1.0f);

    /* For preloaded lanes start_offset maps to a frame index in pcm_buf;
     * for streaming lanes it maps to a byte offset in the raw PCM data. */
    uint32_t start_ref;
    if (row->ring->pcm_buf) {
        start_ref = (uint32_t)(row->start_offset * (float)row->ring->pcm_frames);
        start_ref &= ~0u; /* already frame-aligned */
        /* retrigger_ring PSRAM path expects byte offset it converts via /4;
         * pass as frame * 4 so the /4 in retrigger_ring yields the right frame. */
        start_ref = start_ref * 4u;
    } else {
        start_ref = (uint32_t)(row->start_offset * (float)row->ring->wav.data_bytes);
        start_ref &= ~3u; /* align to stereo frame (4 bytes) */
    }

    /* Apply volume as Q8 to ring */
    row->ring->volume = (uint8_t)(gain * 255.0f < 255.0f ? gain * 255.0f : 255.0f);
    retrigger_ring(row->ring, start_ref);

    /* Start multi-hit chain if applicable */
    row->multi_idx = 0;
    row->multi_next_tick = 0;   /* caller will set absolute tick below */
}

/* ── Condition check ─────────────────────────────────────────────────────── */

static bool step_condition_ok(const drum_step_t *step, uint8_t pass, uint8_t phrase_len)
{
    switch (step->condition) {
    case STEP_COND_ALWAYS:   return true;
    case STEP_COND_FILL:     return (phrase_len > 0) && ((pass % phrase_len) == (phrase_len - 1));
    case STEP_COND_NOT_FILL: return (phrase_len > 0) && ((pass % phrase_len) != (phrase_len - 1));
    case STEP_COND_EVERY2:   return (pass % 2) == 0;
    case STEP_COND_EVERY3:   return (pass % 3) == 0;
    case STEP_COND_EVERY4:   return (pass % 4) == 0;
    default: return true;
    }
}

/* ── Main tick function ──────────────────────────────────────────────────── */

void drum_seq_tick(drum_seq_t *seq, uint32_t lane_tick, uint32_t tick_delta,
                   int32_t *out_l, int32_t *out_r, int n_frames)
{
    if (!seq || seq->step_count == 0 || seq->ticks_per_step == 0) return;

    uint32_t tps       = seq->ticks_per_step;
    uint8_t  phrase    = seq->phrase_len ? seq->phrase_len : 4;

    bool any_solo = false;
    for (int r = 0; r < seq->row_count; r++)
        if (seq->rows[r].solo) { any_solo = true; break; }

    /* ── Per-row step advance (supports polyrhythm via row->step_count) ─── */
    for (int r = 0; r < seq->row_count; r++) {
        drum_row_t *row = &seq->rows[r];
        if (row->mute) continue;
        if (any_solo && !row->solo) continue;

        uint8_t  row_steps = row->step_count ? row->step_count : seq->step_count;
        /* Per-row ticks_per_step derived from global tps scaled by ratio */
        uint32_t row_tps   = (row->step_count && row->step_count != seq->step_count)
                             ? (tps * seq->step_count / row_steps)
                             : tps;
        if (row_tps == 0) row_tps = tps;

        uint32_t ticks_since   = lane_tick - row->last_step_tick;
        uint32_t steps_elapsed = ticks_since / row_tps;

        for (uint32_t s = 0; s < steps_elapsed; s++) {
            uint8_t prev = row->current_step;
            row->current_step = (uint8_t)((row->current_step + 1) % row_steps);
            /* Track passes (loop wraps) */
            if (row->current_step <= prev) row->pass_count++;
            row->last_step_tick += row_tps;

            drum_step_t *step = &row->steps[row->current_step];
            if (step->velocity == 0) continue;

            /* Probability gate */
            uint8_t prob = step->probability ? step->probability : 100;
            if (prob < 100) {
                uint32_t rnd = esp_random() % 100;
                if (rnd >= prob) continue;
            }

            /* Step condition */
            if (!step_condition_ok(step, row->pass_count, phrase)) continue;

            /* Mute group exclusion: cut previous row in same group */
            if (row->mute_group > 0 && row->mute_group <= 8) {
                uint8_t g  = row->mute_group - 1;
                uint8_t pr = seq->mute_group_last[g];
                if (pr != 0xFF && pr != (uint8_t)r && pr < seq->row_count) {
                    wav_lane_t *cut = seq->rows[pr].ring;
                    if (cut) cut->active = false;
                }
                seq->mute_group_last[g] = (uint8_t)r;
            }

            /* Ratchet: subdivide step into N equal hits */
            uint8_t ratchet = step->ratchet ? step->ratchet : 1;
            if (ratchet < 1) ratchet = 1;
            if (ratchet > 4) ratchet = 4;

            trigger_row(row, step->velocity, step->accent, seq->accent_gain);
            row->multi_next_tick = row->last_step_tick;

            /* Schedule additional ratchet hits via multi_gap_ms repurposed approach */
            if (ratchet > 1) {
                /* Store ratchet sub-hit timing: gap = row_tps / ratchet in ticks */
                row->multi_hit_count = ratchet;
                row->multi_idx = 1; /* first hit already triggered */
                uint32_t gap_ticks = row_tps / ratchet;
                for (int h = 1; h < ratchet && h < DRUM_MULTI_HIT_MAX; h++) {
                    row->multi_gap_ms[h] = (uint16_t)(gap_ticks * 32 / (tps ? tps : 1));
                }
                row->multi_next_tick = row->last_step_tick + gap_ticks;
            }
        }
    }

    /* Update global step counter (used for UI display) */
    {
        uint32_t ticks_since   = lane_tick - seq->last_step_tick;
        uint32_t steps_elapsed = ticks_since / tps;
        for (uint32_t s = 0; s < steps_elapsed; s++) {
            uint8_t prev = seq->current_step;
            seq->current_step = (uint8_t)((seq->current_step + 1) % seq->step_count);
            seq->last_step_tick += tps;
            if (seq->current_step <= prev) seq->bar_count++;
        }
    }

    /* ── Multi-hit chain advance ────────────────────────────────────────── */
    for (int r = 0; r < seq->row_count; r++) {
        drum_row_t *row = &seq->rows[r];
        if (row->multi_hit_count < 2) continue;
        if (row->multi_idx >= row->multi_hit_count) continue;

        /* Convert multi_gap_ms to ticks: approx tps * gap_ms / step_duration_ms.
         * At 120 BPM / 96 PPQN / 16 steps: step = 24 ticks = 31.25 ms.
         * Use fixed approximation: 1 ms ≈ tps * 4 / 125 at 120 BPM 1/16.
         * Better: store as ms, convert lazily using ~0.032 ticks/ms at 120/16 — use float. */
        if (lane_tick >= row->multi_next_tick) {
            uint8_t idx = row->multi_idx;
            wav_lane_t *mring = row->multi_ring[idx];
            if (mring && mring->wav.fp) {
                mring->volume = row->ring->volume;
                retrigger_ring(mring, 0);
            }
            row->multi_idx++;
            if (row->multi_idx < row->multi_hit_count) {
                /* Schedule next hit: convert gap_ms to ticks.
                 * ticks_per_ms ≈ tps / step_duration_ms (approximate) */
                uint16_t gap_ms = row->multi_gap_ms[row->multi_idx];
                /* Safe approx: assume ~32 ms per step at nominal BPM */
                uint32_t gap_ticks = (uint32_t)gap_ms * tps / 32;
                row->multi_next_tick += (gap_ticks ? gap_ticks : 1);
            }
        }
    }

    /* ── Drain active rings into output bus ─────────────────────────────── */
    bool any_solo_drain = false;
    for (int r = 0; r < seq->row_count; r++)
        if (seq->rows[r].solo) { any_solo_drain = true; break; }

    for (int r = 0; r < seq->row_count; r++) {
        drum_row_t *row = &seq->rows[r];
        if (row->mute) continue;
        if (any_solo_drain && !row->solo) continue;

        /* Pan law: equal-power approximation via linear split */
        int32_t pan_l = (int32_t)((1.0f - (row->pan > 0.0f ? row->pan : 0.0f)) * 256.0f);
        int32_t pan_r = (int32_t)((1.0f + (row->pan < 0.0f ? row->pan : 0.0f)) * 256.0f);

        /* ── Primary ring drain ──────────────────────────────────────────── */
        wav_lane_t *ring = row->ring;
        if (ring && ring->active) {
            if (ring->pcm_buf) {
                /* Preloaded PSRAM path */
                for (int f = 0; f < n_frames; f++) {
                    if (ring->retrigger_pending && ring->declick_frames == 0) {
                        ring->pcm_pos           = ring->retrigger_pos;
                        ring->volume            = ring->retrigger_vol;
                        ring->retrigger_pending = false;
                    }
                    if (ring->pcm_pos >= ring->pcm_frames) {
                        ring->active = false;
                        break;
                    }
                    int16_t l      = ring->pcm_buf[ring->pcm_pos * 2 + 0];
                    int16_t r_samp = ring->pcm_buf[ring->pcm_pos * 2 + 1];
                    ring->pcm_pos++;
                    int32_t sv = (int32_t)ring->volume;
                    if (ring->declick_frames > 0) {
                        sv = sv * ring->declick_frames / DECLICK_FRAMES;
                        ring->declick_frames--;
                    }
                    out_l[f] += ((int32_t)l * sv * pan_l) >> 16;
                    out_r[f] += ((int32_t)r_samp * sv * pan_r) >> 16;
                }
            } else {
                /* Streaming ring path */
                for (int f = 0; f < n_frames; f++) {
                    if (ring->retrigger_pending && ring->declick_frames == 0) {
                        ring->active    = false;
                        ring->read_idx  = 0;
                        ring->write_idx = 0;
                        ring->fill      = 0;
                        ring->src_phase = 0;
                        ring->last_l    = 0;
                        ring->last_r    = 0;
                        uint32_t seek_to = ring->wav.data_offset + ring->retrigger_pos;
                        fseek(ring->wav.fp, (long)seek_to, SEEK_SET);
                        ring->wav.pos           = ring->retrigger_pos;
                        ring->volume            = ring->retrigger_vol;
                        ring->retrigger_pending = false;
                        ring->active            = true;
                        break;
                    }
                    if (ring->fill == 0) break;
                    int16_t l      = ring->buf[ring->read_idx * 2 + 0];
                    int16_t r_samp = ring->buf[ring->read_idx * 2 + 1];
                    ring->read_idx = (uint16_t)((ring->read_idx + 1) & (STREAM_BUF_FRAMES - 1));
                    ring->fill--;
                    int32_t sv = (int32_t)ring->volume;
                    if (ring->declick_frames > 0) {
                        sv = sv * ring->declick_frames / DECLICK_FRAMES;
                        ring->declick_frames--;
                    }
                    out_l[f] += ((int32_t)l * sv * pan_l) >> 16;
                    out_r[f] += ((int32_t)r_samp * sv * pan_r) >> 16;
                }
            }
        }

        /* ── Multi-hit rings ─────────────────────────────────────────────── */
        for (int h = 0; h < row->multi_hit_count && h < DRUM_MULTI_HIT_MAX; h++) {
            wav_lane_t *mring = row->multi_ring[h];
            if (!mring || !mring->active) continue;
            if (mring->pcm_buf) {
                for (int f = 0; f < n_frames; f++) {
                    if (mring->pcm_pos >= mring->pcm_frames) { mring->active = false; break; }
                    int16_t l  = mring->pcm_buf[mring->pcm_pos * 2 + 0];
                    int16_t rs = mring->pcm_buf[mring->pcm_pos * 2 + 1];
                    mring->pcm_pos++;
                    int32_t sv = (int32_t)mring->volume;
                    out_l[f] += ((int32_t)l * sv * pan_l) >> 16;
                    out_r[f] += ((int32_t)rs * sv * pan_r) >> 16;
                }
            } else {
                for (int f = 0; f < n_frames; f++) {
                    if (mring->fill == 0) break;
                    int16_t l  = mring->buf[mring->read_idx * 2 + 0];
                    int16_t rs = mring->buf[mring->read_idx * 2 + 1];
                    mring->read_idx = (uint16_t)((mring->read_idx + 1) & (STREAM_BUF_FRAMES - 1));
                    mring->fill--;
                    int32_t sv = (int32_t)mring->volume;
                    out_l[f] += ((int32_t)l * sv * pan_l) >> 16;
                    out_r[f] += ((int32_t)rs * sv * pan_r) >> 16;
                }
            }
        }
    }
}
