/*
 * audio.c — I2C bus recovery, I2S / ES8311 codec init, wavetables, voice pool,
 *           ADSR envelope, audio_task (core 1, priority 20).
 */

#include <math.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "bsp/esp-bsp.h"
#include "bsp/esp32_p4_platform.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "es8311_codec.h"
#include "audio.h"
#include "lane.h"
#include "drum_seq.h"
#include "piano_roll.h"
#include "arp.h"
#include "lfo.h"
#include "fx.h"
#include "synth_voice.h"   /* sv_soft_clip — master-bus soft limiter */
#include "ws_audio.h"
#include "settings.h"
#include "render_export.h"

static const char *TAG = "audio";

/* ── Board pin / port map ────────────────────────────────────────────────── */
#define PIN_I2C_SDA    BSP_I2C_SDA
#define PIN_I2C_SCL    BSP_I2C_SCL
#define I2C_PORT       BSP_I2C_NUM

#define I2S_MCK_IO     BSP_I2S_MCLK
#define I2S_BCK_IO     BSP_I2S_SCLK
#define I2S_WS_IO      BSP_I2S_LCLK
#define I2S_DO_IO      BSP_I2S_DOUT
#define I2S_DI_IO      BSP_I2S_DSIN
#define GPIO_PA        BSP_POWER_AMP_IO
#define I2S_PORT       CONFIG_BSP_I2S_NUM
#define MCLK_MULTIPLE  256

/* ── PCM5102 external DAC — second I2S master output ─────────────────────────
 * Wiring: VIN→5V, GND→GND, BCK→GPIO37, LRCK→GPIO28, DIN→GPIO21, SCK→GND.
 * SCK grounded ⇒ the PCM5102 runs its internal PLL, so no MCLK is routed.
 * The onboard ES8311 codec uses I2S port CONFIG_BSP_I2S_NUM (=1); the PCM5102
 * gets its own port 0 and mirrors the same stereo buffer.                    */
#define PCM_I2S_PORT   I2S_NUM_0
#define PCM_BCK_IO     GPIO_NUM_37    /* BCK  — bit clock           */
#define PCM_WS_IO      GPIO_NUM_28    /* LRCK — word/left-right clk */
#define PCM_DO_IO      GPIO_NUM_21    /* DIN  — serial data         */

/* ── Wavetable storage ───────────────────────────────────────────────────── */
int16_t        wt_square  [WAVETABLE_SIZE];
int16_t        wt_sawtooth[WAVETABLE_SIZE];
int16_t        wt_sine    [WAVETABLE_SIZE];
int16_t        wt_triangle[WAVETABLE_SIZE];
const int16_t *volatile s_wavetable = wt_square;

/* ── Voice pool ──────────────────────────────────────────────────────────── */
voice_t s_voices[NUM_VOICES];

volatile uint32_t s_attack_samp  = 2400;
volatile uint32_t s_decay_samp   = 4800;
volatile uint32_t s_release_samp = 9600;

/* ── Note event queue (SPSC: UI task writes, audio task reads) ───────────── */
static note_evt_t  s_note_q[NOTE_Q_SIZE];
static volatile uint32_t s_note_q_head = 0;   /* written by audio task  */
static volatile uint32_t s_note_q_tail = 0;   /* written by UI task     */
static volatile bool     s_all_notes_off = false; /* set by UI, cleared by audio */

void audio_note_on(synth_inst_t *synth, uint8_t note, uint8_t vel)
{
    uint32_t tail = s_note_q_tail;
    uint32_t next = (tail + 1) & (NOTE_Q_SIZE - 1);
    if (next == s_note_q_head) return;  /* queue full — drop */
    s_note_q[tail].synth = synth;
    s_note_q[tail].note  = note;
    s_note_q[tail].vel   = vel ? vel : 1;
    /* Store tail last so the audio task sees a complete entry */
    __atomic_store_n(&s_note_q_tail, next, __ATOMIC_RELEASE);
}

void audio_note_off(synth_inst_t *synth, uint8_t note)
{
    uint32_t tail = s_note_q_tail;
    uint32_t next = (tail + 1) & (NOTE_Q_SIZE - 1);
    if (next == s_note_q_head) return;
    s_note_q[tail].synth = synth;
    s_note_q[tail].note  = note;
    s_note_q[tail].vel   = 0;
    __atomic_store_n(&s_note_q_tail, next, __ATOMIC_RELEASE);
}

void audio_all_notes_off(void)
{
    __atomic_store_n(&s_all_notes_off, true, __ATOMIC_RELEASE);
}

static void drain_note_queue(void)
{
    /* All-notes-off takes priority — applied before any queued events */
    if (__atomic_load_n(&s_all_notes_off, __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&s_all_notes_off, false, __ATOMIC_RELAXED);
        for (int li = 0; li < NUM_LANES; li++) {
            lane_t *lane = &g_song.lanes[li];
            if (!lane->active || lane->type != LANE_TYPE_SYNTH) continue;
            synth_inst_t *synth = lane->synth;
            if (!synth || !synth->note_off) continue;
            for (int note = 0; note < 128; note++)
                synth->note_off(synth, (uint8_t)note);
        }
        for (int i = 0; i < NUM_VOICES; i++) {
            s_voices[i].note_on = false;
            s_voices[i].st      = ST_IDLE;
        }
        g_live_held_count = 0;
        /* Flush the queue — any pending note_ons after a stop are stale */
        s_note_q_head = __atomic_load_n(&s_note_q_tail, __ATOMIC_ACQUIRE);
        return;
    }

    uint32_t tail = __atomic_load_n(&s_note_q_tail, __ATOMIC_ACQUIRE);
    while (s_note_q_head != tail) {
        note_evt_t *e = &s_note_q[s_note_q_head];
        if (e->synth) {
            if (e->vel > 0) {
                if (e->synth->note_on)  e->synth->note_on (e->synth, e->note, e->vel);
            } else {
                if (e->synth->note_off) e->synth->note_off(e->synth, e->note);
            }
        }
        s_note_q_head = (s_note_q_head + 1) & (NOTE_Q_SIZE - 1);
    }
}

/* ── I2S / codec handles ─────────────────────────────────────────────────── */
static i2s_chan_handle_t      s_tx     = NULL;  /* ES8311 codec (port 1) */
static i2s_chan_handle_t      s_tx_pcm = NULL;  /* PCM5102 DAC   (port 0) */
esp_codec_dev_handle_t        s_spk    = NULL;

static int16_t s_audio_buf[AUDIO_BUF_FRAMES * 2];

/* ── Wavetable init ──────────────────────────────────────────────────────── */
void wavetable_init(void)
{
    for (int i = 0; i < WAVETABLE_SIZE; i++) {
        float t = (float)i / WAVETABLE_SIZE;
        wt_square[i]   = (i < WAVETABLE_SIZE / 2) ? WT_AMP : -WT_AMP;
        wt_sawtooth[i] = (int16_t)(-WT_AMP + (int)(2 * WT_AMP * t));
        wt_sine[i]     = (int16_t)(WT_AMP * sinf(2.0f * (float)M_PI * t));
        if      (t < 0.25f) wt_triangle[i] = (int16_t)( WT_AMP * 4.0f * t);
        else if (t < 0.75f) wt_triangle[i] = (int16_t)( WT_AMP * (2.0f - 4.0f * t));
        else                wt_triangle[i] = (int16_t)( WT_AMP * (4.0f * t - 4.0f));
    }
}

/* ── Voice helpers ───────────────────────────────────────────────────────── */
uint32_t freq_to_step(float hz)
{
    return (uint32_t)(hz * (float)WAVETABLE_SIZE / (float)SAMPLE_RATE * (1u << 24));
}

int alloc_voice(int key_id, uint32_t phase_step)
{
    int best = -1;
    float best_progress = -1.0f;
    for (int i = 0; i < NUM_VOICES; i++) {
        if (s_voices[i].st == ST_IDLE) { best = i; break; }
        if (s_voices[i].st == ST_RELEASE) {
            float p = (float)s_voices[i].env_cnt;
            if (p > best_progress) { best_progress = p; best = i; }
        }
    }
    if (best < 0) best = 0;
    s_voices[best].key_id     = key_id;
    s_voices[best].phase_step = phase_step;
    s_voices[best].note_on    = true;
    return best;
}

/* ── Per-block int32 accumulator buses (static — no heap in audio hot path) ── */
static int32_t s_lane_l[AUDIO_BUF_FRAMES];
static int32_t s_lane_r[AUDIO_BUF_FRAMES];
static int32_t s_master_l[AUDIO_BUF_FRAMES];
static int32_t s_master_r[AUDIO_BUF_FRAMES];
static int32_t s_send_l[AUDIO_BUF_FRAMES];   /* shared send bus accumulator  */
static int32_t s_send_r[AUDIO_BUF_FRAMES];
/* Scratch int16 buffers for ADSR + FX chain (operates on int16) */
static int16_t s_fx_l[AUDIO_BUF_FRAMES];
static int16_t s_fx_r[AUDIO_BUF_FRAMES];

/* Sound-browser preview slot — mixed into master bus when >= 0. */
static volatile int s_preview_slot = -1;

void audio_set_preview_slot(int slot) { s_preview_slot = slot; }

/* Stem export: one FILE* per lane, opened when stem_export_active is set */
#define STEM_EXPORT_MAX_LANES NUM_LANES
static FILE *s_stem_fp[STEM_EXPORT_MAX_LANES];
static uint32_t s_stem_frames[STEM_EXPORT_MAX_LANES];

/* ── Stem export helpers ──────────────────────────────────────────────────── */
static void stem_export_lane_write(int li, const int16_t *l_buf, const int16_t *r_buf)
{
    if (!s_stem_fp[li]) return;
    /* Interleave and write */
    static int16_t interleaved[AUDIO_BUF_FRAMES * 2];
    for (int f = 0; f < AUDIO_BUF_FRAMES; f++) {
        interleaved[f * 2 + 0] = l_buf[f];
        interleaved[f * 2 + 1] = r_buf[f];
    }
    fwrite(interleaved, sizeof(int16_t) * 2, AUDIO_BUF_FRAMES, s_stem_fp[li]);
    s_stem_frames[li] += AUDIO_BUF_FRAMES;
}

static void wav_patch_header(FILE *fp, uint32_t frames)
{
    uint32_t data_size = frames * 2 * sizeof(int16_t);
    uint32_t riff_size = 36 + data_size;
    fseek(fp, 4, SEEK_SET); fwrite(&riff_size, 4, 1, fp);
    fseek(fp, 40, SEEK_SET); fwrite(&data_size, 4, 1, fp);
    fclose(fp);
}

void stem_export_start(const char *dir)
{
    memset(s_stem_fp, 0, sizeof(s_stem_fp));
    memset(s_stem_frames, 0, sizeof(s_stem_frames));
    for (int li = 0; li < NUM_LANES; li++) {
        if (!g_song.lanes[li].active) continue;
        char path[160];
        const char *lname = g_song.lanes[li].type == LANE_TYPE_DRUM      ? "drum"  :
                            g_song.lanes[li].type == LANE_TYPE_DRUMSYNTH ? "dsyn"  :
                            g_song.lanes[li].type == LANE_TYPE_SYNTH     ? "synth" : "wav";
        snprintf(path, sizeof(path), "%s/lane%02d_%s.wav", dir, li, lname);
        FILE *fp = fopen(path, "wb");
        if (!fp) continue;
        /* Write placeholder header matching render_export.c layout */
        uint32_t zero = 0; uint16_t ch = 2; uint32_t sr = 48000;
        uint16_t bits = 16; uint16_t ba = 4; uint32_t br = 192000; uint16_t af = 1;
        uint32_t fsz = 16;
        fwrite("RIFF",1,4,fp); fwrite(&zero,4,1,fp); fwrite("WAVE",1,4,fp);
        fwrite("fmt ",1,4,fp); fwrite(&fsz,4,1,fp); fwrite(&af,2,1,fp);
        fwrite(&ch,2,1,fp); fwrite(&sr,4,1,fp); fwrite(&br,4,1,fp);
        fwrite(&ba,2,1,fp); fwrite(&bits,2,1,fp);
        fwrite("data",1,4,fp); fwrite(&zero,4,1,fp);
        s_stem_fp[li] = fp;
    }
    g_song.stem_export_active = true;
}

void stem_export_stop(void)
{
    for (int li = 0; li < NUM_LANES; li++) {
        if (s_stem_fp[li]) {
            wav_patch_header(s_stem_fp[li], s_stem_frames[li]);
            s_stem_fp[li] = NULL;
        }
    }
    g_song.stem_export_active = false;
}

void audio_panic(void)
{
    /* Schedule all-notes-off on the audio thread (safe cross-core).
     * The audio task will silence all synth voices and flush the note queue. */
    audio_all_notes_off();
}

/* ── Metronome click generator ───────────────────────────────────────────── */
static uint32_t s_metro_tick_last = 0;
static uint32_t s_metro_click_samp = 0; /* samples remaining in current click */
#define METRO_CLICK_SAMPS  480  /* 10 ms at 48 kHz */

static void metronome_mix(int32_t *out_l, int32_t *out_r, int n_frames,
                          uint32_t master_tick, uint32_t tick_delta, const seq_clock_t *clk)
{
    if (!g_song.metronome_enabled) return;
    uint32_t beat_ticks = clk->tick_rate;
    int32_t  vol        = (int32_t)(g_song.metronome_volume * 16384.0f);

    uint32_t prev_tick = master_tick - tick_delta;
    for (int f = 0; f < n_frames; f++) {
        /* Compute tick at this sample (linear interpolation across block) */
        uint32_t sample_tick = prev_tick + (uint32_t)((uint64_t)tick_delta * f / n_frames);
        uint32_t beat_phase  = sample_tick % beat_ticks;
        bool on_beat = (beat_phase == 0) || (s_metro_click_samp > 0);
        if (beat_phase == 0 && sample_tick != s_metro_tick_last) {
            s_metro_click_samp = METRO_CLICK_SAMPS;
            s_metro_tick_last  = sample_tick;
        }
        if (s_metro_click_samp > 0) {
            /* Short decaying sine burst */
            float t   = 1.0f - (float)s_metro_click_samp / METRO_CLICK_SAMPS;
            int16_t s = (int16_t)((float)vol * (1.0f - t) * (((s_metro_click_samp & 16) ? 1 : -1)));
            out_l[f] += s; out_r[f] += s;
            s_metro_click_samp--;
        }
        (void)on_beat;
    }
}

/* ── Render the legacy wavetable voice pool into a lane bus ──────────────── */
/*
 * Used by LANE_TYPE_SYNTH lanes until Phase 3 synth instruments land.
 * Pulls from the global s_voices[] pool shared with the UI piano keyboard.
 */
static void render_voice_pool(int32_t *bus_l, int32_t *bus_r, int n_frames)
{
    uint32_t atk = s_attack_samp, dcy = s_decay_samp, rel = s_release_samp;
    const int16_t *wt = s_wavetable;

    /* Snapshot note-on edges once per block */
    for (int v = 0; v < NUM_VOICES; v++) {
        voice_t *vp = &s_voices[v];
        bool note = vp->note_on;
        if (note && !vp->prev_note) {
            vp->rel_amp = vp->env_amp; vp->st = ST_ATTACK;  vp->env_cnt = 0;
        } else if (!note && vp->prev_note && vp->st != ST_IDLE) {
            vp->rel_amp = vp->env_amp; vp->st = ST_RELEASE; vp->env_cnt = 0;
        }
        vp->prev_note = note;
    }

    for (int i = 0; i < n_frames; i++) {
        int32_t mix = 0;
        for (int v = 0; v < NUM_VOICES; v++) {
            voice_t *vp = &s_voices[v];
            switch (vp->st) {
            case ST_ATTACK:
                vp->env_amp = atk ? (float)vp->env_cnt / (float)atk : 1.0f;
                if (vp->env_cnt >= atk) { vp->env_amp = 1.0f; vp->st = ST_DECAY; vp->env_cnt = 0; }
                break;
            case ST_DECAY:
                vp->env_amp = 1.0f - (1.0f - SUSTAIN_LEVEL) * (float)vp->env_cnt / (float)(dcy ? dcy : 1);
                if (vp->env_cnt >= dcy) { vp->env_amp = SUSTAIN_LEVEL; vp->st = ST_SUSTAIN; }
                break;
            case ST_SUSTAIN:
                vp->env_amp = SUSTAIN_LEVEL;
                break;
            case ST_RELEASE:
                vp->env_amp = vp->rel_amp * (1.0f - (float)vp->env_cnt / (float)(rel ? rel : 1));
                if (vp->env_cnt >= rel) { vp->env_amp = 0.0f; vp->st = ST_IDLE; vp->key_id = -1; }
                break;
            default:
                vp->env_amp = 0.0f;
                break;
            }
            vp->env_cnt++;
            if (vp->st != ST_IDLE)
                mix += (int32_t)(wt[(vp->phase_acc >> 24) & 0xFF] * vp->env_amp);
            vp->phase_acc += vp->phase_step;
        }
        bus_l[i] += mix;
        bus_r[i] += mix;
    }
}

/* ── CPU load tracking (audio task) ─────────────────────────────────────── */
/* callback_us = microseconds of DSP work per block (excludes i2s_channel_write
 * blocking time).  budget_us = block period at 48kHz.  load_pct reported 1/s. */
static volatile uint32_t s_audio_load_pct  = 0;   /* 0-100, last measured */
static volatile uint32_t s_audio_xrun_cnt  = 0;   /* ring starvation count */
uint32_t audio_get_load_pct(void)  { return s_audio_load_pct; }
uint32_t audio_get_xrun_cnt(void)  { return s_audio_xrun_cnt; }

/* ── Audio task (core 1, priority 20) — Phase 2.4 full lane mixer ────────── */
static void audio_task(void *arg)
{
    const uint32_t budget_us = (uint32_t)(1000000ULL * AUDIO_BUF_FRAMES / SAMPLE_RATE);
    uint64_t acc_work_us   = 0;
    uint64_t acc_period_us = 0;
    int64_t  last_report   = esp_timer_get_time();

    while (1) {
        int64_t t_start = esp_timer_get_time();

        /* ── 0. Apply pending note events from the UI task ─────────────── */
        drain_note_queue();

        /* ── 1. Advance master clock + all lane ticks (Phase 2.1 / 2.3) ── */
        uint32_t prev_master = g_song.clock.tick_count;
        uint32_t tick_delta  = clock_advance(&g_song.clock);
        uint32_t master_tick = g_song.clock.tick_count;
        lanes_tick(&g_song, tick_delta, master_tick);

        /* ── 2. Clear master + send bus ─────────────────────────────────── */
        memset(s_master_l, 0, sizeof(s_master_l));
        memset(s_master_r, 0, sizeof(s_master_r));
        memset(s_send_l,   0, sizeof(s_send_l));
        memset(s_send_r,   0, sizeof(s_send_r));

        /* ── 3. Per-lane mix ─────────────────────────────────────────────── */

        /* Solo pre-pass: if any active lane has solo=true, only soloed lanes
         * contribute to the mix (non-soloed lanes are silenced this block). */
        bool any_solo = false;
        for (int li = 0; li < NUM_LANES; li++) {
            if (g_song.lanes[li].active && g_song.lanes[li].solo) {
                any_solo = true;
                break;
            }
        }

        for (int li = 0; li < NUM_LANES; li++) {
            lane_t *lane = &g_song.lanes[li];
            if (!lane->active) continue;

            /* Clear lane bus */
            memset(s_lane_l, 0, sizeof(s_lane_l));
            memset(s_lane_r, 0, sizeof(s_lane_r));

            /* Solo mode: silence lanes that are not soloed */
            bool effectively_muted = lane->mute || (any_solo && !lane->solo);
            if (!effectively_muted) {
                /* ── a. WAV lane: drain PSRAM buffer or stream ring ─────── */
                if (lane->type == LANE_TYPE_WAV && lane->wav_lane) {
                    wav_lane_t *wl = lane->wav_lane;
                    if (wl->active) {
                        if (wl->pcm_buf) {
                            /* Preloaded PSRAM path */
                            for (int f = 0; f < AUDIO_BUF_FRAMES; f++) {
                                if (wl->pcm_pos >= wl->pcm_frames) {
                                    if (wl->play_mode == WAV_MODE_LOOP) wl->pcm_pos = 0;
                                    else { wl->active = false; break; }
                                }
                                int16_t l = wl->pcm_buf[wl->pcm_pos * 2 + 0];
                                int16_t r = wl->pcm_buf[wl->pcm_pos * 2 + 1];
                                wl->pcm_pos++;
                                int32_t vol = (int32_t)wl->volume;
                                s_lane_l[f] += ((int32_t)l * vol) >> 8;
                                s_lane_r[f] += ((int32_t)r * vol) >> 8;
                            }
                        } else {
                            /* Streaming ring path */
                            bool starved = false;
                            for (int f = 0; f < AUDIO_BUF_FRAMES; f++) {
                                if (wl->fill == 0) { starved = true; break; }
                                int16_t l = wl->buf[wl->read_idx * 2 + 0];
                                int16_t r = wl->buf[wl->read_idx * 2 + 1];
                                wl->read_idx = (uint16_t)((wl->read_idx + 1) & (STREAM_BUF_FRAMES - 1));
                                wl->fill--;
                                int32_t vol = (int32_t)wl->volume;
                                s_lane_l[f] += ((int32_t)l * vol) >> 8;
                                s_lane_r[f] += ((int32_t)r * vol) >> 8;
                            }
                            if (starved) s_audio_xrun_cnt++;
                        }
                    }
                }

                /* ── b. Drum lane: step trigger + ring drain ─────────────── */
                else if (lane->type == LANE_TYPE_DRUM && lane->drum_seq) {
                    drum_seq_tick(lane->drum_seq, lane->lane_tick, tick_delta,
                                  s_lane_l, s_lane_r, AUDIO_BUF_FRAMES);
                }

                /* ── b2. Drum-synth lane: step trigger + analog voice render ── */
                else if (lane->type == LANE_TYPE_DRUMSYNTH && lane->dsyn) {
                    dsyn_tick(lane->dsyn, lane->lane_tick, tick_delta,
                              s_lane_l, s_lane_r, AUDIO_BUF_FRAMES);
                }

                /* ── c. Synth lane: LFO → arp/piano-roll → render ───────────── */
                else if (lane->type == LANE_TYPE_SYNTH) {
                    /* LFO block tick: updates lane->lfo.mod_out[] */
                    lane_lfo_tick(&lane->lfo, AUDIO_BUF_FRAMES);

                    if (lane->synth) {
                        uint32_t prev_lane_tick = lane->lane_tick - tick_delta;

                        /* Note repeat: retrigger held note at clock-locked rate */
                        if (lane->note_repeat && lane->note_repeat_note > 0 &&
                            lane->note_repeat_div > 0) {
                            uint32_t rep_ticks = g_song.clock.tick_rate * 4 / lane->note_repeat_div;
                            if (rep_ticks > 0 && master_tick >= lane->note_repeat_next_tick) {
                                lane->synth->note_on(lane->synth, lane->note_repeat_note, 100);
                                lane->note_repeat_next_tick = master_tick + rep_ticks;
                            }
                        }

                        if (lane->arp.enabled) {
                            /* Arp intercepts note events and drives the synth */
                            arp_tick(&lane->arp, lane->synth, lane->lane_tick,
                                     g_song.clock.tick_rate);
                        } else if (lane->piano_roll) {
                            piano_roll_tick(lane->piano_roll, prev_lane_tick,
                                            lane->lane_tick, lane->loop_len_ticks,
                                            lane->synth);
                        }
                    }

                    /* Render synth voices (Phase 3 vtable or legacy voice pool) */
                    if (lane->synth && lane->synth->render) {
                        static int16_t s_synth_l[AUDIO_BUF_FRAMES];
                        static int16_t s_synth_r[AUDIO_BUF_FRAMES];
                        memset(s_synth_l, 0, sizeof(s_synth_l));
                        memset(s_synth_r, 0, sizeof(s_synth_r));
                        lane->synth->render(lane->synth, s_synth_l, s_synth_r, AUDIO_BUF_FRAMES);
                        /* Apply LFO amplitude modulation (additive offset in [-1,+1]) */
                        float amp_mod = 1.0f + lane_lfo_get(&lane->lfo, MOD_DEST_AMPLITUDE);
                        if (amp_mod < 0.0f) amp_mod = 0.0f;
                        int32_t amp_q8 = (int32_t)(amp_mod * 256.0f);
                        for (int f = 0; f < AUDIO_BUF_FRAMES; f++) {
                            s_lane_l[f] += (s_synth_l[f] * amp_q8) >> 8;
                            s_lane_r[f] += (s_synth_r[f] * amp_q8) >> 8;
                        }
                    } else {
                        render_voice_pool(s_lane_l, s_lane_r, AUDIO_BUF_FRAMES);
                    }
                }

                /* ── d. Clip int32 lane bus → int16 for ADSR + FX ───────────── */
                for (int f = 0; f < AUDIO_BUF_FRAMES; f++) {
                    int32_t l = s_lane_l[f], r = s_lane_r[f];
                    if (l >  32767) l =  32767;
                    if (l < -32768) l = -32768;
                    if (r >  32767) r =  32767;
                    if (r < -32768) r = -32768;
                    s_fx_l[f] = (int16_t)l; s_fx_r[f] = (int16_t)r;
                }

                /* ── e. Publish per-lane RMS to sidechain bus ────────────── */
                {
                    float sum = 0.0f;
                    for (int f = 0; f < AUDIO_BUF_FRAMES; f++) {
                        float l = s_fx_l[f] / 32768.0f, r = s_fx_r[f] / 32768.0f;
                        sum += l * l + r * r;
                    }
                    if (li < FX_SIDECHAIN_LANES)
                        fx_sidechain_rms[li] = sqrtf(sum / (float)(AUDIO_BUF_FRAMES * 2));
                }

                /* ── f. Lane ADSR envelope ───────────────────────────────── */
                lane_adsr_process(&lane->adsr, s_fx_l, s_fx_r, AUDIO_BUF_FRAMES);

                /* ── g. Per-lane FX chain ────────────────────────────────── */
                fx_chain_process(lane->fx, lane->fx_count,
                                 s_fx_l, s_fx_r, AUDIO_BUF_FRAMES,
                                 g_song.clock.bpm, g_song.clock.tick_rate);

                /* ── h. Volume + pan, promote int16 back to int32 bus ───── */
                {
                    int32_t vol   = (int32_t)(lane->volume * 256.0f);
                    int32_t pan_l = (int32_t)((1.0f - (lane->pan > 0.0f ? lane->pan : 0.0f)) * 256.0f);
                    int32_t pan_r = (int32_t)((1.0f + (lane->pan < 0.0f ? lane->pan : 0.0f)) * 256.0f);
                    for (int f = 0; f < AUDIO_BUF_FRAMES; f++) {
                        s_lane_l[f] = ((int32_t)s_fx_l[f] * vol * pan_l) >> 16;
                        s_lane_r[f] = ((int32_t)s_fx_r[f] * vol * pan_r) >> 16;
                    }
                }

                /* ── i. Send bus feed ─────────────────────────────────────── */
                if (lane->send_level > 0.001f) {
                    int32_t sv = (int32_t)(lane->send_level * 256.0f);
                    for (int f = 0; f < AUDIO_BUF_FRAMES; f++) {
                        s_send_l[f] += (s_lane_l[f] * sv) >> 8;
                        s_send_r[f] += (s_lane_r[f] * sv) >> 8;
                    }
                }

                /* ── j. Stem export (pre-master) ──────────────────────────── */
                if (g_song.stem_export_active) {
                    static int16_t s_stem_l16[AUDIO_BUF_FRAMES];
                    static int16_t s_stem_r16[AUDIO_BUF_FRAMES];
                    for (int f = 0; f < AUDIO_BUF_FRAMES; f++) {
                        int32_t lv = s_lane_l[f], rv = s_lane_r[f];
                        if (lv >  32767) lv =  32767;
                        if (lv < -32768) lv = -32768;
                        if (rv >  32767) rv =  32767;
                        if (rv < -32768) rv = -32768;
                        s_stem_l16[f] = (int16_t)lv;
                        s_stem_r16[f] = (int16_t)rv;
                    }
                    stem_export_lane_write(li, s_stem_l16, s_stem_r16);
                }
            }
            /* muted/non-soloed lane contributes zero; clock still advanced above */

            /* ── Accumulate lane bus → master bus ────────────────────────── */
            for (int f = 0; f < AUDIO_BUF_FRAMES; f++) {
                s_master_l[f] += s_lane_l[f];
                s_master_r[f] += s_lane_r[f];
            }
        }

        /* ── 3b. Process send bus FX + mix return into master ────────────── */
        if (g_song.send_fx_count > 0 && g_song.send_return_level > 0.001f) {
            for (int f = 0; f < AUDIO_BUF_FRAMES; f++) {
                int32_t lv = s_send_l[f], rv = s_send_r[f];
                if (lv >  32767) lv =  32767;
                if (lv < -32768) lv = -32768;
                if (rv >  32767) rv =  32767;
                if (rv < -32768) rv = -32768;
                s_fx_l[f] = (int16_t)lv; s_fx_r[f] = (int16_t)rv;
            }
            fx_chain_process(g_song.send_fx, g_song.send_fx_count,
                             s_fx_l, s_fx_r, AUDIO_BUF_FRAMES,
                             g_song.clock.bpm, g_song.clock.tick_rate);
            int32_t ret_v = (int32_t)(g_song.send_return_level * 256.0f);
            for (int f = 0; f < AUDIO_BUF_FRAMES; f++) {
                s_master_l[f] += ((int32_t)s_fx_l[f] * ret_v) >> 8;
                s_master_r[f] += ((int32_t)s_fx_r[f] * ret_v) >> 8;
            }
        }

        /* ── 3b2. Sound-browser preview slot (bypasses lane FX) ──────────── */
        {
            int psl = s_preview_slot;
            if (psl >= 0) {
                wav_lane_t *pl = wav_lane_get(psl);
                if (pl && pl->active) {
                    if (pl->pcm_buf) {
                        for (int f = 0; f < AUDIO_BUF_FRAMES; f++) {
                            if (pl->pcm_pos >= pl->pcm_frames) { pl->active = false; break; }
                            int16_t l = pl->pcm_buf[pl->pcm_pos * 2 + 0];
                            int16_t r = pl->pcm_buf[pl->pcm_pos * 2 + 1];
                            pl->pcm_pos++;
                            int32_t vol = (int32_t)pl->volume;
                            s_master_l[f] += ((int32_t)l * vol) >> 8;
                            s_master_r[f] += ((int32_t)r * vol) >> 8;
                        }
                    } else {
                        for (int f = 0; f < AUDIO_BUF_FRAMES; f++) {
                            if (pl->fill == 0) break;
                            int16_t l = pl->buf[pl->read_idx * 2 + 0];
                            int16_t r = pl->buf[pl->read_idx * 2 + 1];
                            pl->read_idx = (uint16_t)((pl->read_idx + 1) & (STREAM_BUF_FRAMES - 1));
                            pl->fill--;
                            int32_t vol = (int32_t)pl->volume;
                            s_master_l[f] += ((int32_t)l * vol) >> 8;
                            s_master_r[f] += ((int32_t)r * vol) >> 8;
                        }
                    }
                }
            }
        }

        /* ── 3c. Metronome click ─────────────────────────────────────────── */
        metronome_mix(s_master_l, s_master_r, AUDIO_BUF_FRAMES,
                      master_tick, tick_delta, &g_song.clock);

        /* ── 4. Master FX + vol/pan + clip → int16 → I2S ──────────────── */
        {
            /* Soft-limit master bus into int16 L/R scratch — gentle tanh knee
             * on peaks (e.g. several lanes / a big chord summing hot) instead
             * of a harsh hard clip. */
            for (int f = 0; f < AUDIO_BUF_FRAMES; f++) {
                s_fx_l[f] = (int16_t)sv_soft_clip(s_master_l[f]);
                s_fx_r[f] = (int16_t)sv_soft_clip(s_master_r[f]);
            }
            /* Apply master FX chain in-place on L/R scratch */
            if (g_song.master_fx_count > 0)
                fx_chain_process(g_song.master_fx, g_song.master_fx_count,
                                 s_fx_l, s_fx_r, AUDIO_BUF_FRAMES,
                                 g_song.clock.bpm, g_song.clock.tick_rate);
            /* Master vol/pan + interleave to audio buffer */
            float mvol = g_settings.master_volume;
            float mpan = g_settings.master_pan;
            float pan_l = (mpan < 0.0f) ? 1.0f : (1.0f - mpan);
            float pan_r = (mpan > 0.0f) ? 1.0f : (1.0f + mpan);
            int32_t vol_l = (int32_t)(mvol * pan_l * 65536.0f);
            int32_t vol_r = (int32_t)(mvol * pan_r * 65536.0f);
            for (int f = 0; f < AUDIO_BUF_FRAMES; f++) {
                int32_t l = (s_fx_l[f] * vol_l) >> 16;
                int32_t r = (s_fx_r[f] * vol_r) >> 16;
                if (l >  32767) l =  32767;
                if (l < -32768) l = -32768;
                if (r >  32767) r =  32767;
                if (r < -32768) r = -32768;
                s_audio_buf[f * 2 + 0] = (int16_t)l;
                s_audio_buf[f * 2 + 1] = (int16_t)r;
            }
        }

        (void)prev_master;

        /* Measure DSP work time before blocking on I2S write */
        int64_t t_work_end = esp_timer_get_time();
        acc_work_us   += (uint64_t)(t_work_end - t_start);
        acc_period_us += budget_us;

        size_t written;
        i2s_channel_write(s_tx, s_audio_buf, sizeof(s_audio_buf), &written, portMAX_DELAY);
        if (s_tx_pcm) {
            size_t w_pcm;
            i2s_channel_write(s_tx_pcm, s_audio_buf, sizeof(s_audio_buf), &w_pcm, portMAX_DELAY);
        }
        ws_audio_push(s_audio_buf, AUDIO_BUF_FRAMES);
        if (render_export_active())
            render_export_write(s_audio_buf, AUDIO_BUF_FRAMES);

        /* Update CPU load counter once per second — no UART/log on audio core */
        int64_t now = esp_timer_get_time();
        if (now - last_report >= 1000000LL) {
            s_audio_load_pct = (acc_period_us > 0)
                               ? (uint32_t)(acc_work_us * 100ULL / acc_period_us)
                               : 0;
            s_audio_xrun_cnt = 0;
            acc_work_us   = 0;
            acc_period_us = 0;
            last_report   = now;
        }
    }
}

/* ── I2C bus recovery + init ─────────────────────────────────────────────── */
void i2c_init(void)
{
    /* Recover a stuck bus before handing it to the BSP */
    gpio_config_t in = {
        .pin_bit_mask = (1ULL << PIN_I2C_SDA) | (1ULL << PIN_I2C_SCL),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&in);
    vTaskDelay(pdMS_TO_TICKS(2));
    if (!gpio_get_level(PIN_I2C_SDA) || !gpio_get_level(PIN_I2C_SCL)) {
        ESP_LOGW(TAG, "I2C bus stuck — recovering with 9 SCL pulses");
        gpio_set_direction(PIN_I2C_SCL, GPIO_MODE_OUTPUT_OD);
        gpio_set_direction(PIN_I2C_SDA, GPIO_MODE_OUTPUT_OD);
        gpio_set_level(PIN_I2C_SDA, 1);
        for (int i = 0; i < 9; i++) {
            gpio_set_level(PIN_I2C_SCL, 0); esp_rom_delay_us(5);
            gpio_set_level(PIN_I2C_SCL, 1); esp_rom_delay_us(5);
        }
        gpio_set_level(PIN_I2C_SDA, 0); esp_rom_delay_us(5);
        gpio_set_level(PIN_I2C_SCL, 1); esp_rom_delay_us(5);
        gpio_set_level(PIN_I2C_SDA, 1); esp_rom_delay_us(5);
    }
    ESP_ERROR_CHECK(bsp_i2c_init());
}

/* ── Audio init ──────────────────────────────────────────────────────────── */
void audio_init(void)
{
    gpio_config_t pa_cfg = {};
    pa_cfg.pin_bit_mask = 1ULL << GPIO_PA;
    pa_cfg.mode = GPIO_MODE_OUTPUT;
    gpio_config(&pa_cfg);
    gpio_set_level(GPIO_PA, 1);

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.auto_clear      = true;
    chan_cfg.dma_desc_num    = 4;            /* 4 × 64 frames = 256 frames ~5.3ms total */
    chan_cfg.dma_frame_num   = AUDIO_BUF_FRAMES; /* one DMA descriptor = one audio block */
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &s_tx, NULL));

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_MCK_IO, .bclk = I2S_BCK_IO, .ws = I2S_WS_IO,
            .dout = I2S_DO_IO,  .din  = I2S_DI_IO,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    std_cfg.clk_cfg.mclk_multiple = (i2s_mclk_multiple_t)MCLK_MULTIPLE;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx));

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port       = I2C_PORT,
        .addr       = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = bsp_i2c_get_handle(),
    };
    const audio_codec_ctrl_if_t *i2c_ctrl = audio_codec_new_i2c_ctrl(&i2c_cfg);
    assert(i2c_ctrl);

    audio_codec_i2s_cfg_t i2s_cfg = {};
    i2s_cfg.port      = I2S_PORT;
    i2s_cfg.tx_handle = s_tx;
    i2s_cfg.rx_handle = NULL;
    const audio_codec_data_if_t *i2s_data = audio_codec_new_i2s_data(&i2s_cfg);
    assert(i2s_data);

    es8311_codec_cfg_t es_cfg = {};
    es_cfg.ctrl_if     = i2c_ctrl;
    es_cfg.gpio_if     = audio_codec_new_gpio();
    es_cfg.codec_mode  = (esp_codec_dec_work_mode_t)ESP_CODEC_DEV_TYPE_OUT;
    es_cfg.pa_pin      = GPIO_PA;
    es_cfg.pa_reverted = false;
    es_cfg.master_mode = false;
    es_cfg.use_mclk    = true;
    es_cfg.hw_gain.pa_voltage        = 5.0f;
    es_cfg.hw_gain.codec_dac_voltage = 3.3f;
    const audio_codec_if_t *es_dev = es8311_codec_new(&es_cfg);
    assert(es_dev);

    esp_codec_dev_cfg_t dev_cfg = {};
    dev_cfg.dev_type = ESP_CODEC_DEV_TYPE_OUT;
    dev_cfg.codec_if = es_dev;
    dev_cfg.data_if  = i2s_data;
    s_spk = esp_codec_dev_new(&dev_cfg);
    assert(s_spk);

    esp_codec_dev_sample_info_t fs = {};
    fs.sample_rate     = SAMPLE_RATE;
    fs.channel         = 2;
    fs.bits_per_sample = 16;
    ESP_ERROR_CHECK(esp_codec_dev_open(s_spk, &fs));
    ESP_ERROR_CHECK(esp_codec_dev_set_out_vol(s_spk, 100));

    /* ── PCM5102 external DAC on a second I2S master ─────────────────────────
     * Same role/clock/format as the codec so both DACs stay sample-locked.
     * No control bus (PCM5102 is hardware-configured); just feed it I2S.    */
    i2s_chan_config_t pcm_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(PCM_I2S_PORT, I2S_ROLE_MASTER);
    pcm_chan_cfg.auto_clear    = true;
    pcm_chan_cfg.dma_desc_num  = 4;
    pcm_chan_cfg.dma_frame_num = AUDIO_BUF_FRAMES;
    ESP_ERROR_CHECK(i2s_new_channel(&pcm_chan_cfg, &s_tx_pcm, NULL));

    i2s_std_config_t pcm_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,            /* SCK→GND: PCM5102 internal PLL */
            .bclk = PCM_BCK_IO, .ws = PCM_WS_IO,
            .dout = PCM_DO_IO,  .din = I2S_GPIO_UNUSED,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx_pcm, &pcm_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx_pcm));

    for (int i = 0; i < NUM_VOICES; i++) { s_voices[i].st = ST_IDLE; s_voices[i].key_id = -1; }
    ws_audio_init();
    xTaskCreatePinnedToCore(audio_task, "audio", 8192, NULL, 20, NULL, 1);
}
