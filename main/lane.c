/*
 * lane.c — Lane model + song container
 */

#include <string.h>
#include <stdlib.h>
#include "esp_random.h"
#include "lane.h"
#include "clock.h"
#include "drum_seq.h"
#include "piano_roll.h"
#include "arp.h"
#include "lfo.h"
#include "fx.h"
#include "wav_player.h"

/* Reset playback state on loop restart (LIVE or SONG mode). */
void lane_restart(lane_t *lane)
{
    if (lane->type == LANE_TYPE_DRUM && lane->drum_seq)
        drum_seq_reset(lane->drum_seq, lane->lane_tick);
    else if (lane->type == LANE_TYPE_DRUMSYNTH && lane->dsyn)
        dsyn_reset(lane->dsyn, lane->lane_tick);
    else if (lane->type == LANE_TYPE_SYNTH && lane->piano_roll)
        piano_roll_reset(lane->piano_roll);
    else if (lane->type == LANE_TYPE_WAV && lane->wav_lane_slot >= 0) {
        /* Retrigger: seek back to start and re-arm the ring */
        wav_lane_t *wl = wav_lane_get(lane->wav_lane_slot);
        if (wl && wl->wav.fp) {
            wl->active = false;
            wl->read_idx = wl->write_idx = wl->fill = 0;
            wl->src_phase = 0;
            fseek(wl->wav.fp, (long)wl->wav.data_offset, SEEK_SET);
            wl->wav.pos = 0;
            wl->active = true;
        }
    }
}

void lane_reset(lane_t *lane)
{
    memset(lane, 0, sizeof(*lane));
    lane->type          = LANE_TYPE_WAV;
    lane->active        = false;
    lane->mute          = false;
    lane->volume        = 1.0f;
    lane->pan           = 0.0f;
    lane->wav_lane_slot = -1;
    arp_init(&lane->arp);
    lane_lfo_init(&lane->lfo);
    lane_adsr_init(&lane->adsr, 0.0f, 0.0f, 1.0f, 0.0f); /* default: gate-through */
    for (int i = 0; i < FX_MAX_PER_LANE; i++) lane->fx[i] = NULL;
    /* Synth param defaults */
    lane->synth_params[2]  = 10.0f;      /* ATK ms */
    lane->synth_params[3]  = 100.0f;     /* DCY ms */
    lane->synth_params[4]  = 0.8f;       /* SUS */
    lane->synth_params[5]  = 200.0f;     /* REL ms */
    lane->synth_params[9]  = 8000.0f;    /* filter freq Hz */
    lane->synth_params[10] = 0.3f;       /* filter res */
    lane->synth_params[7]  = 2.0f;       /* FM ratio */
    lane->synth_params[8]  = 1.0f;       /* FM depth */
    lane->synth_params[20] = 10.0f;      /* extra ATK ms */
    lane->synth_params[21] = 100.0f;     /* extra DCY ms */
    lane->synth_params[22] = 0.8f;       /* extra SUS */
    lane->synth_params[23] = 200.0f;     /* extra REL ms */
}

void lane_init_all(song_t *song)
{
    clock_init(&song->clock);
    song->playback_mode       = PLAYBACK_LIVE;
    song->active_scene        = -1;
    song->autosave_interval   = 8;   /* every 8 bars */
    song->send_return_level   = 0.5f;
    memset(song->name, 0, sizeof(song->name));
    memset(song->scenes, 0, sizeof(song->scenes));
    memset(&song->arrangement, 0, sizeof(song->arrangement));
    for (int i = 0; i < NUM_LANES; i++)
        lane_reset(&song->lanes[i]);
}

void lanes_tick(song_t *song, uint32_t tick_delta, uint32_t master_tick)
{
    if (tick_delta == 0) return;

    uint32_t bar_ticks = CLOCK_BAR_TICKS(&song->clock);
    bool song_mode = (song->playback_mode == PLAYBACK_SONG);

    for (int i = 0; i < NUM_LANES; i++) {
        lane_t *lane = &song->lanes[i];
        if (!lane->active) continue;

        /* Groove: add swing/humanise offset to tick advance */
        uint32_t effective_delta = tick_delta;
        if (lane->groove.enabled) {
            if (lane->groove.humanise > 0) {
                int32_t jitter = (int32_t)(esp_random() % (uint32_t)(lane->groove.humanise * 2 + 1))
                                 - lane->groove.humanise;
                effective_delta = (uint32_t)((int32_t)effective_delta + jitter);
            }
        }
        lane->lane_tick += effective_delta;

        if (lane->loop_len_ticks == 0) continue;

        if (lane->lane_tick >= lane->loop_len_ticks) {
            if (!song_mode) {
                lane->lane_tick       = lane->lane_tick % lane->loop_len_ticks;
                lane->pending_restart = false;
                lane_restart(lane);
                arrangement_advance(song);
            } else {
                lane->pending_restart = true;
            }
        }

        if (song_mode && lane->pending_restart && bar_ticks > 0 &&
            (master_tick % bar_ticks == 0)) {
            lane->pending_restart = false;
            lane->lane_tick       = 0;
            lane_restart(lane);
            arrangement_advance(song);
        }
    }
}

/* ── Scene API ───────────────────────────────────────────────────────────── */

void scene_capture(song_t *song, int idx, const char *name)
{
    if (idx < 0 || idx >= SCENE_MAX) return;
    scene_t *sc = &song->scenes[idx];
    sc->active = true;
    if (name) {
        strncpy(sc->name, name, SCENE_NAME_LEN - 1);
        sc->name[SCENE_NAME_LEN - 1] = '\0';
    }
    for (int i = 0; i < NUM_LANES; i++) {
        sc->lane_mute[i]   = song->lanes[i].mute;
        sc->lane_solo[i]   = song->lanes[i].solo;
        sc->lane_volume[i] = song->lanes[i].volume;
    }
    song->dirty = true;
}

void scene_recall(song_t *song, int idx)
{
    if (idx < 0 || idx >= SCENE_MAX) return;
    const scene_t *sc = &song->scenes[idx];
    if (!sc->active) return;
    for (int i = 0; i < NUM_LANES; i++) {
        song->lanes[i].mute   = sc->lane_mute[i];
        song->lanes[i].solo   = sc->lane_solo[i];
        song->lanes[i].volume = sc->lane_volume[i];
    }
    song->active_scene = idx;
}

/* ── Arrangement API ─────────────────────────────────────────────────────── */

void arrangement_advance(song_t *song)
{
    arrangement_t *arr = &song->arrangement;
    if (!arr->enabled || arr->count == 0) return;

    arr->current_repeat++;
    uint8_t rep = arr->steps[arr->current_step].repeat;
    if (rep == 0) rep = 1;

    if (arr->current_repeat >= rep) {
        arr->current_repeat = 0;
        arr->current_step = (uint8_t)((arr->current_step + 1) % arr->count);
        scene_recall(song, arr->steps[arr->current_step].scene_idx);
    }
}
