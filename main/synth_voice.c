/*
 * synth_voice.c — Shared synth building blocks (Phase 3)
 */

#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include "synth_voice.h"
#include "esp_log.h"

static const char *TAG = "synth_voice";

/* ── Wavetables ──────────────────────────────────────────────────────────── */
int16_t sv_wt[SV_WAVE_COUNT][SV_WT_SIZE];

void sv_wt_init(void)
{
    for (int i = 0; i < SV_WT_SIZE; i++) {
        float t = (float)i / SV_WT_SIZE;
        sv_wt[SV_WAVE_SINE][i]     = (int16_t)(SV_WT_AMP * sinf(2.0f * (float)M_PI * t));
        sv_wt[SV_WAVE_SAW][i]      = (int16_t)(-SV_WT_AMP + (int)(2 * SV_WT_AMP * t));
        sv_wt[SV_WAVE_SQUARE][i]   = (i < SV_WT_SIZE / 2) ? SV_WT_AMP : -SV_WT_AMP;
        if      (t < 0.25f) sv_wt[SV_WAVE_TRIANGLE][i] = (int16_t)( SV_WT_AMP * 4.0f * t);
        else if (t < 0.75f) sv_wt[SV_WAVE_TRIANGLE][i] = (int16_t)( SV_WT_AMP * (2.0f - 4.0f * t));
        else                sv_wt[SV_WAVE_TRIANGLE][i] = (int16_t)( SV_WT_AMP * (4.0f * t - 4.0f));
    }
}

/* ── Note frequency ──────────────────────────────────────────────────────── */
float sv_note_to_hz(uint8_t note)
{
    /* A4 = MIDI 69 = 440 Hz */
    return 440.0f * powf(2.0f, ((float)(int)note - 69.0f) / 12.0f);
}

/* ── ADSR ────────────────────────────────────────────────────────────────── */
void sv_adsr_set(sv_adsr_t *a, float atk_ms, float dcy_ms,
                 float sus, float rel_ms)
{
    a->atk = (uint32_t)(atk_ms * SAMPLE_RATE / 1000.0f);
    a->dcy = (uint32_t)(dcy_ms * SAMPLE_RATE / 1000.0f);
    a->sus = sus < 0.0f ? 0.0f : (sus > 1.0f ? 1.0f : sus);
    a->rel = (uint32_t)(rel_ms * SAMPLE_RATE / 1000.0f);
}

void sv_adsr_gate_on(sv_adsr_t *a)
{
    a->rel_lvl = a->level;   /* avoid click: start from current level */
    a->cnt     = 0;
    a->stage   = SV_ST_ATTACK;
}

void sv_adsr_gate_off(sv_adsr_t *a)
{
    if (a->stage == SV_ST_IDLE) return;
    a->rel_lvl = a->level;
    a->cnt     = 0;
    a->stage   = SV_ST_RELEASE;
}

/* ── Voice pool ──────────────────────────────────────────────────────────── */
int sv_voice_steal(sv_voice_t *pool, int pool_size, uint8_t note)
{
    /* First: same note already active (retrigger) */
    for (int i = 0; i < pool_size; i++)
        if (pool[i].active && pool[i].note == note) return i;
    /* Second: idle voice */
    for (int i = 0; i < pool_size; i++)
        if (!pool[i].active) return i;
    /* Third: oldest releasing voice */
    for (int i = 0; i < pool_size; i++)
        if (pool[i].adsr.stage == SV_ST_RELEASE) return i;
    return 0;   /* steal first */
}

void sv_voice_note_on(sv_voice_t *v, uint8_t note, uint8_t vel,
                      uint32_t step, sv_adsr_t *tmpl)
{
    bool same_note = v->active && (v->note == note);
    v->note  = note;
    v->vel   = vel ? vel : 1;
    v->step  = step;
    /* Preserve phase when retriggering the same note to avoid click.
     * Reset to 0 only for a new note so the waveform starts cleanly. */
    if (!same_note) {
        v->phase = 0;
        v->lfsr  = 0xACE1;
    }
    if (tmpl) v->adsr = *tmpl;
    sv_adsr_gate_on(&v->adsr);
    v->active = true;
}

void sv_voice_note_off(sv_voice_t *v)
{
    sv_adsr_gate_off(&v->adsr);
}

/* ── SVF (State Variable Filter) ─────────────────────────────────────────── */
void sv_svf_set(sv_svf_t *f, float hz, float q)
{
    f->f = 2.0f * sinf((float)M_PI * hz / (float)SAMPLE_RATE);
    f->q = 1.0f / (q > 0.5f ? q : 0.5f);
    /* Clamp to avoid instability */
    if (f->f > 0.99f) f->f = 0.99f;
    if (f->f < 0.001f) f->f = 0.001f;
}

/* ── Custom wavetable import ─────────────────────────────────────────────── */

bool sv_wt_load_custom(sv_wave_t slot, const char *wav_path)
{
    if (slot < SV_WAVE_CUSTOM1 || slot >= SV_WAVE_COUNT) return false;
    FILE *fp = fopen(wav_path, "rb");
    if (!fp) { ESP_LOGE(TAG, "sv_wt_load_custom: cannot open %s", wav_path); return false; }

    /* Read minimal WAV header: RIFF/WAVE/fmt /data */
    uint8_t hdr[44];
    if (fread(hdr, 1, 44, fp) < 44) { fclose(fp); return false; }
    /* Validate */
    if (hdr[0]!='R'||hdr[1]!='I'||hdr[2]!='F'||hdr[3]!='F') { fclose(fp); return false; }
    uint16_t channels  = (uint16_t)(hdr[22] | (hdr[23] << 8));
    uint32_t src_rate  = (uint32_t)(hdr[24] | (hdr[25]<<8) | (hdr[26]<<16) | (hdr[27]<<24));
    uint16_t bits      = (uint16_t)(hdr[34] | (hdr[35] << 8));
    if (bits != 16 || channels < 1 || src_rate == 0) { fclose(fp); return false; }

    /* Find data chunk (scan past non-data chunks) */
    long data_pos = 12;
    fseek(fp, data_pos, SEEK_SET);
    uint32_t data_bytes = 0;
    for (int iter = 0; iter < 8; iter++) {
        uint8_t tag[4]; uint32_t csz;
        if (fread(tag, 1, 4, fp) < 4) break;
        if (fread(&csz, 4, 1, fp) < 1) break;
        if (tag[0]=='d'&&tag[1]=='a'&&tag[2]=='t'&&tag[3]=='a') { data_bytes = csz; break; }
        fseek(fp, (long)csz, SEEK_CUR);
    }
    if (data_bytes == 0) { fclose(fp); return false; }

    /* data_start = current position (immediately after "data"+size fields) */
    long data_start = ftell(fp);
    uint32_t src_frames = data_bytes / (channels * 2);
    if (src_frames == 0) { fclose(fp); return false; }

    /* Resample src_frames → SV_WT_SIZE using linear interpolation */
    for (int i = 0; i < SV_WT_SIZE; i++) {
        float    src_pos = (float)i * (float)src_frames / (float)SV_WT_SIZE;
        uint32_t idx0    = (uint32_t)src_pos;
        uint32_t idx1    = (idx0 + 1 < src_frames) ? idx0 + 1 : idx0;
        float    frac    = src_pos - (float)idx0;

        int16_t s0 = 0, s1 = 0;
        fseek(fp, data_start + (long)(idx0 * channels * 2), SEEK_SET);
        fread(&s0, 2, 1, fp);
        fseek(fp, data_start + (long)(idx1 * channels * 2), SEEK_SET);
        fread(&s1, 2, 1, fp);

        int32_t mixed = (int32_t)((float)s0 * (1.0f - frac) + (float)s1 * frac);
        sv_wt[slot][i] = (int16_t)(mixed * SV_WT_AMP / 32767);
    }
    fclose(fp);
    ESP_LOGI(TAG, "loaded custom waveform slot %d from %s", (int)slot, wav_path);
    return true;
}
