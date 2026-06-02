'use strict';

// ─────────────────────────────────────────────────────────────────────────────
// Navigation
// ─────────────────────────────────────────────────────────────────────────────
function nav(id, btn) {
  if (document.body.classList.contains('laptop')) return; // panes handle their own sections
  document.querySelectorAll('section').forEach(s => s.classList.remove('active'));
  document.getElementById('s-' + id).classList.add('active');
  document.querySelectorAll('.nav-tab').forEach(b => b.classList.remove('active'));
  btn.classList.add('active');
  if (id === 'drum') renderDrumGrid();
  if (id === 'dsyn') renderDsyn();
  if (id === 'roll') renderPianoRoll();
}

// ─────────────────────────────────────────────────────────────────────────────
// Laptop / multi-pane mode
// ─────────────────────────────────────────────────────────────────────────────
const LP_PANELS = [
  { id: 'song',     label: 'SONG' },
  { id: 'drum',     label: 'DRUM GRID' },
  { id: 'dsyn',     label: '808 SYNTH' },
  { id: 'roll',     label: 'PIANO ROLL' },
  { id: 'master',   label: 'MASTER' },
  { id: 'fx',       label: 'FX CHAIN' },
  { id: 'live',     label: 'LIVE PLAY' },
  { id: 'cutter',   label: 'SAMPLE CUT' },
  { id: 'upload',   label: 'UPLOAD' },
  { id: 'songs',    label: 'SONGS' },
  { id: 'arr',      label: 'ARRANGEMENT' },
  { id: 'wt',       label: 'WAVETABLE' },
  { id: 'settings', label: 'SETTINGS' },
  { id: 'synth',    label: 'SYNTH' },
  { id: 'wav',      label: 'WAV DETAIL' },
];

const LP_STORAGE_KEY = 'synth32_lp_layout';
let lpActive = false;
let lpCols   = 2;
let lpPanes  = []; // array of section ids currently shown

const LP_DEFAULT = { cols: 2, panes: ['song', 'drum'] };

function laptopToggle() {
  lpActive = !lpActive;
  localStorage.setItem(LP_STORAGE_KEY + '_active', lpActive ? '1' : '0');
  document.body.classList.toggle('laptop', lpActive);
  document.getElementById('lp-toggle-btn').classList.toggle('on', lpActive);
  if (lpActive) {
    laptopLoad();
    laptopRender();
  } else {
    laptopDestroy();
    // restore normal single-section view to first tab
    document.querySelectorAll('section').forEach(s => s.classList.remove('active'));
    document.getElementById('s-song').classList.add('active');
    document.querySelectorAll('.nav-tab').forEach((b,i) => b.classList.toggle('active', i===0));
  }
}

function laptopLoad() {
  try {
    const saved = JSON.parse(localStorage.getItem(LP_STORAGE_KEY) || 'null');
    if (saved && Array.isArray(saved.panes)) {
      lpCols  = saved.cols  || 2;
      lpPanes = saved.panes.slice();
      return;
    }
  } catch(_) {}
  lpCols  = LP_DEFAULT.cols;
  lpPanes = LP_DEFAULT.panes.slice();
}

function laptopSaveLayout() {
  localStorage.setItem(LP_STORAGE_KEY, JSON.stringify({ cols: lpCols, panes: lpPanes }));
  const btn = document.querySelector('.lp-save-btn');
  if (btn) { const t = btn.textContent; btn.textContent = 'SAVED ✓'; setTimeout(() => btn.textContent = t, 1200); }
}

function laptopResetLayout() {
  localStorage.removeItem(LP_STORAGE_KEY);
  lpCols  = LP_DEFAULT.cols;
  lpPanes = LP_DEFAULT.panes.slice();
  laptopRender();
}

function laptopSetCols(n) {
  lpCols = n;
  document.documentElement.style.setProperty('--lp-cols', n);
}

function laptopAddPane() {
  // pick first panel not already visible
  const next = LP_PANELS.find(p => !lpPanes.includes(p.id));
  lpPanes.push(next ? next.id : LP_PANELS[0].id);
  laptopRender();
}

function laptopRemovePane(idx) {
  if (lpPanes.length <= 1) return;
  lpPanes.splice(idx, 1);
  laptopRender();
}

function laptopPaneChange(idx, id) {
  lpPanes[idx] = id;
  laptopRender();
}

function laptopRender() {
  // set CSS column var
  document.documentElement.style.setProperty('--lp-cols', lpCols);

  // sync cols select
  const colSel = document.getElementById('lp-cols-sel');
  if (colSel) colSel.value = String(lpCols);

  const main = document.querySelector('main');

  // remove existing panes
  main.querySelectorAll('.lp-pane').forEach(p => p.remove());

  // move all sections back to <main> (they may have been moved into panes)
  document.querySelectorAll('.lp-pane-body section').forEach(s => main.appendChild(s));

  lpPanes.forEach((sectionId, idx) => {
    const panel = LP_PANELS.find(p => p.id === sectionId) || LP_PANELS[0];

    const pane = document.createElement('div');
    pane.className = 'lp-pane';

    // header
    const hdr = document.createElement('div');
    hdr.className = 'lp-pane-hdr';

    const sel = document.createElement('select');
    sel.className = 'lp-pane-sel';
    LP_PANELS.forEach(p => {
      const opt = document.createElement('option');
      opt.value = p.id;
      opt.textContent = p.label;
      if (p.id === sectionId) opt.selected = true;
      sel.appendChild(opt);
    });
    sel.addEventListener('change', () => laptopPaneChange(idx, sel.value));

    const closeBtn = document.createElement('button');
    closeBtn.className = 'lp-pane-close';
    closeBtn.textContent = '✕';
    closeBtn.title = 'Remove pane';
    closeBtn.addEventListener('click', () => laptopRemovePane(idx));

    hdr.appendChild(sel);
    hdr.appendChild(closeBtn);

    // body — move the actual section element inside
    const body = document.createElement('div');
    body.className = 'lp-pane-body';
    const sectionEl = document.getElementById('s-' + panel.id);
    if (sectionEl) body.appendChild(sectionEl);

    pane.appendChild(hdr);
    pane.appendChild(body);
    main.appendChild(pane);

    // trigger any needed renders for this panel
    if (panel.id === 'drum')    renderDrumGrid();
    if (panel.id === 'dsyn')    renderDsyn();
    if (panel.id === 'roll')    renderPianoRoll();
    if (panel.id === 'live')    renderLive();
    if (panel.id === 'songs')   requestSongList();
    if (panel.id === 'cutter')  loadCutDirs();
    if (panel.id === 'upload')  loadDirs();
    if (panel.id === 'settings') loadWifiConfig();
  });
}

function laptopDestroy() {
  const main = document.querySelector('main');
  // move sections back to <main> before removing panes
  document.querySelectorAll('.lp-pane-body section').forEach(s => main.appendChild(s));
  main.querySelectorAll('.lp-pane').forEach(p => p.remove());
  document.documentElement.style.removeProperty('--lp-cols');
}

// Restore laptop mode on page load if it was active
document.addEventListener('DOMContentLoaded', () => {
  if (localStorage.getItem(LP_STORAGE_KEY + '_active') === '1') {
    lpActive = true;
    document.body.classList.add('laptop');
    document.getElementById('lp-toggle-btn').classList.add('on');
    laptopLoad();
    laptopRender();
  }
});

// ─────────────────────────────────────────────────────────────────────────────
// State mirror
// ─────────────────────────────────────────────────────────────────────────────
const NUM_LANES = 16;
const state = {
  running: false,
  bpm: 120,
  ppqn: 96,
  beats: 4,
  master_vol: 0.8,
  master_pan: 0.0,
  lanes: [],         // lane objects from WS_MSG_LANE_UPDATE
  drum: {},          // lane_idx -> { rows: [ { steps:[], step_count } ] }
  dsyn: {},          // lane_idx -> { step_count, row_count, rows:[{voice,knobs[7],steps[64],accents[64]}] }
  notes: {},         // lane_idx -> pr_note_t[]
  playhead: 0,       // master tick
  step: new Uint8Array(NUM_LANES).fill(0xFF), // per-lane current_step
  meter_l: 0,
  meter_r: 0,
  fx: {},           // lane_idx -> [{slot, type, enabled, params:[f32×8]}]
  arp: {},          // lane_idx -> arp state { enabled, mode, octave, div, gate, velMode, fixedVel, latch, retrig, swing }
  playback_mode: 0, // 0=LIVE, 1=SONG
  songName: '',
  songDirty: false,
};
let selected_lane = -1;  // lane selected for drum/roll/adsr views

// ─────────────────────────────────────────────────────────────────────────────
// WebSocket
// ─────────────────────────────────────────────────────────────────────────────
const CMD = {
  SET_LANE:         0x80,
  SET_DRUM_STEP:    0x81,
  ADD_NOTE:         0x82,
  DEL_NOTE:         0x83,
  SET_FX:           0x84,
  SET_ADSR:         0x85,
  SET_CLOCK:        0x86,
  TRANSPORT:        0x87,
  SET_MASTER:       0x88,
  SAVE_SONG:        0x89,
  LOAD_SONG:        0x8A,
  NEW_SONG:         0x8B,
  LIST_SONGS:       0x8C,
  SET_SETTINGS:     0x8D,
  ADD_LANE:         0x8E,
  DEL_LANE:         0x8F,
  DRUM_ROW_ASSIGN:  0x90,
  SET_SYNTH:        0x91,
  NOTE_ON:          0x92,
  NOTE_OFF:         0x93,
  DRUM_TRIGGER:     0x94,
  TOGGLE_MUTE:      0x95,
  TOGGLE_SOLO:      0x96,
  RENAME_LANE:      0x97,
  DUPLICATE_LANE:   0x98,
  SET_ARP:          0x99,
  SET_DRUM_ROW:     0x9A,
  SCENE_CAPTURE:    0x9B,
  SCENE_RECALL:     0x9C,
  SET_GROOVE:       0x9D,
  SET_METRONOME:    0x9E,
  STEM_EXPORT:      0x9F,
  SAVE_VERSIONED:   0xA0,
  SAVE_TEMPLATE:    0xA1,
  LOAD_TEMPLATE:    0xA2,
  SET_ARRANGEMENT:  0xA3,
  DRUM_EUCLIDEAN:   0xA4,
  SET_NOTE_REPEAT:  0xA5,
  SET_SEND_LEVEL:   0xA6,
  WT_IMPORT:        0xA7,
  SET_DSYN:         0xA8,
  AUDIO_STREAM:     0xF0,
};
const MSG = {
  FULL_STATE:    0x01,
  LANE_UPDATE:   0x02,
  DRUM_STEP:     0x03,
  NOTE_EVENT:    0x04,
  FX_UPDATE:     0x05,
  ADSR_UPDATE:   0x06,
  CLOCK_UPDATE:  0x07,
  TRANSPORT:     0x08,
  MASTER_UPDATE: 0x09,
  SONG_LIST:     0x0A,
  METER:         0x0C,
  PLAYHEAD:      0x0D,
  SYNTH_PARAMS:  0x0E,
  AUDIO_DROPPED: 0x0F,
  SETTINGS:      0x0B,
  DSYN_ROW:      0x10,
};

let ws = null;
const dot = document.getElementById('ws-dot');

function wsConnect() {
  ws = new WebSocket('ws://' + location.host + '/ws');
  ws.binaryType = 'arraybuffer';
  ws.onopen  = () => { dot.className = 'ws-dot ok'; };
  ws.onclose = () => { dot.className = 'ws-dot err'; setTimeout(wsConnect, 3000); };
  ws.onerror = () => { dot.className = 'ws-dot err'; };
  ws.onmessage = e => handleMsg(new DataView(e.data), e.data.byteLength);
}
wsConnect();

function wsSend(buf) {
  if (ws && ws.readyState === 1) ws.send(buf);
}

// ─────────────────────────────────────────────────────────────────────────────
// Message decoder
// ─────────────────────────────────────────────────────────────────────────────
function handleMsg(dv, len) {
  if (len < 1) return;
  const type = dv.getUint8(0);
  switch (type) {
    case MSG.FULL_STATE: decodeFullState(dv, len); renderSong(); renderTopBar(); break;
    case MSG.LANE_UPDATE: decodeLane(dv, len); renderSong(); break;
    case MSG.DRUM_STEP:   decodeDrumStep(dv, len); renderDrumGrid(); break;
    case MSG.DSYN_ROW:    decodeDsynRow(dv, len); renderDsyn(); break;
    case MSG.FX_UPDATE:   decodeFxChain(dv, len); renderFxChain(); break;
    case MSG.NOTE_EVENT:  decodeNoteEvent(dv, len); if (document.getElementById('s-roll').classList.contains('active')) renderPianoRoll(); break;
    case MSG.CLOCK_UPDATE: decodeClock(dv, len); break;
    case MSG.TRANSPORT:    decodeTransport(dv); break;
    case MSG.MASTER_UPDATE: decodeMaster(dv, len); break;
    case MSG.ADSR_UPDATE:   decodeAdsrUpdate(dv, len); break;
    case MSG.METER:         decodeMeter(dv, len); break;
    case MSG.PLAYHEAD:      decodePlayhead(dv, len); renderDrumPlayhead(); break;
    case MSG.SONG_LIST:     decodeSongList(dv, len); break;
    case MSG.SETTINGS:      decodeSettings(dv, len); break;
    case MSG.SYNTH_PARAMS:  decodeSynthParams(dv, len); break;
    case MSG.AUDIO_DROPPED: handleAudioDropped(); break;
    default: handleAudioFrame(type, dv, len); break;
  }
}

function decodeLane(dv, len) {
  if (len < 6) return;
  const li = dv.getUint8(1);
  if (li >= NUM_LANES) return;
  while (state.lanes.length <= li) state.lanes.push(null);
  const wav_path = len >= 18 + 128 ? readStr(dv, 18, 128) : '';
  // Lane name: firmware stores it in bytes 146-176 (after wav_path)
  const name_off = 18 + 128;
  const name_raw = len >= name_off + 32 ? readStr(dv, name_off, 32) : '';
  // Fallback name: basename of wav_path for WAV lanes, "Lane N" otherwise
  const fallback = wav_path
    ? wav_path.split('/').pop().replace(/\.wav$/i, '').slice(0, 16)
    : `Lane ${li + 1}`;
  state.lanes[li] = {
    idx:      li,
    active:   dv.getUint8(2) !== 0,
    type:     dv.getUint8(3),      // 0=WAV 1=DRUM 2=SYNTH
    mute:     dv.getUint8(4) !== 0,
    solo:     dv.getUint8(5) !== 0,
    volume:   len >= 10 ? dv.getFloat32(6,  true) : 1.0,
    pan:      len >= 14 ? dv.getFloat32(10, true) : 0.0,
    loop_len: len >= 18 ? dv.getUint32(14, true)  : 0,
    wav_path,
    name:     name_raw || fallback,
  };
}

function decodeDrumStep(dv, len) {
  if (len < 4) return;
  const li = dv.getUint8(1), ri = dv.getUint8(2), sc = dv.getUint8(3);
  if (!state.drum[li]) state.drum[li] = { rows: [] };
  while (state.drum[li].rows.length <= ri) state.drum[li].rows.push({ steps: [], step_count: 16, mute: false, volume: 1.0 });
  const row = state.drum[li].rows[ri];
  row.step_count = sc;
  row.steps = [];
  const nsteps = Math.min(sc, len - 4);
  for (let s = 0; s < nsteps; s++) row.steps.push(dv.getUint8(4 + s));
  // Extended: mute + volume after steps
  const ext = 4 + 64;
  if (len >= ext + 5) {
    row.mute   = dv.getUint8(ext) !== 0;
    row.volume = dv.getFloat32(ext + 1, true);
  }
}

function decodeClock(dv, len) {
  if (len < 13) return;
  state.bpm   = dv.getFloat32(1, true);
  state.ppqn  = dv.getUint32(5, true);
  state.beats = dv.getUint32(9, true);
  document.getElementById('bpm-disp').textContent = state.bpm.toFixed(1);
  document.getElementById('bpm-input').value = Math.round(state.bpm);
  highlightPpqn(state.ppqn);
  highlightTimeSig(state.beats);
}

function decodeTransport(dv) {
  state.running = dv.getUint8(1) !== 0;
  const btn = document.getElementById('play-btn');
  btn.textContent = state.running ? '■' : '▶';
  btn.classList.toggle('active', state.running);
}

function decodeMaster(dv, len) {
  if (len < 5) return;
  state.master_vol = dv.getFloat32(1, true);
  if (len >= 9) state.master_pan = dv.getFloat32(5, true);
  document.getElementById('master-fill').style.width = Math.round(state.master_vol * 100) + '%';
  const vr = document.getElementById('master-vol-range');
  if (vr) { vr.value = Math.round(state.master_vol * 100); document.getElementById('master-vol-val').textContent = Math.round(state.master_vol * 100) + '%'; }
  const pr = document.getElementById('master-pan-range');
  if (pr) { pr.value = Math.round(state.master_pan * 100); document.getElementById('master-pan-val').textContent = Math.round(state.master_pan * 100); }
}

function linToDb(v) {
  if (v <= 0) return -Infinity;
  const db = 20 * Math.log10(v);
  return db;
}

function decodeMeter(dv, len) {
  if (len < 9) return;
  state.meter_l = Math.min(1, dv.getFloat32(1, true));
  state.meter_r = Math.min(1, dv.getFloat32(5, true));
  document.getElementById('meter-l').style.height = Math.round(state.meter_l * 100) + '%';
  document.getElementById('meter-r').style.height = Math.round(state.meter_r * 100) + '%';
  // Update levels bars and dB readout in master section
  const lBar = document.getElementById('lvl-l-bar');
  const rBar = document.getElementById('lvl-r-bar');
  const lDb  = document.getElementById('lvl-l-db');
  const rDb  = document.getElementById('lvl-r-db');
  if (lBar) lBar.style.width = Math.round(state.meter_l * 100) + '%';
  if (rBar) rBar.style.width = Math.round(state.meter_r * 100) + '%';
  if (lDb) {
    const db = linToDb(state.meter_l);
    lDb.textContent = isFinite(db) ? db.toFixed(1) + ' dB' : '−∞ dB';
  }
  if (rDb) {
    const db = linToDb(state.meter_r);
    rDb.textContent = isFinite(db) ? db.toFixed(1) + ' dB' : '−∞ dB';
  }
}

const _prevStep = new Uint8Array(NUM_LANES).fill(0xFF);

function decodePlayhead(dv, len) {
  if (len < 5) return;
  state.playhead = dv.getUint32(1, true);
  for (let i = 0; i < NUM_LANES && 5 + i < len; i++) {
    const newStep = dv.getUint8(5 + i);
    if (newStep !== _prevStep[i]) {
      _prevStep[i] = newStep;
      flashDrumPadsForLane(i, newStep);
    }
    state.step[i] = newStep;
  }
}

function flashDrumPadsForLane(li, stepIdx) {
  // Flash live-pad elements for rows that have a hit at stepIdx
  const seq = state.drum[li];
  if (!seq) return;
  seq.rows.forEach((row, ri) => {
    const vel = row.steps && row.steps[stepIdx];
    if (!vel) return;
    const pad = document.querySelector(`.live-pad[data-li="${li}"][data-ri="${ri}"]`);
    if (!pad) return;
    pad.classList.add('active');
    setTimeout(() => pad.classList.remove('active'), 80);
  });
}

function decodeNoteEvent(dv, len) {
  // NOTE_EVENT: [type, lane, u16 note_count, (u32 tick_start, u32 tick_len, u8 note, u8 vel) × N]
  if (len < 3) return;
  const li = dv.getUint8(1);
  if (li >= NUM_LANES) return;
  const count = dv.getUint16(2, true);
  const notes = [];
  let off = 4;
  for (let i = 0; i < count && off + 10 <= len; i++) {
    notes.push({
      tick_start: dv.getUint32(off,     true),
      tick_len:   dv.getUint32(off + 4, true),
      note:       dv.getUint8(off + 8),
      velocity:   dv.getUint8(off + 9),
    });
    off += 10;
  }
  state.notes[li] = notes;
}

function decodeAdsrUpdate(dv, len) {
  if (len < 18) return;
  const li = dv.getUint8(1);
  if (li >= NUM_LANES) return;
  if (li !== selected_lane) return;
  document.getElementById('adr-a').value = dv.getFloat32(2,  true);
  document.getElementById('adr-d').value = dv.getFloat32(6,  true);
  document.getElementById('adr-s').value = Math.round(dv.getFloat32(10, true) * 100);
  document.getElementById('adr-r').value = dv.getFloat32(14, true);
  document.getElementById('adv-a').textContent = document.getElementById('adr-a').value + 'ms';
  document.getElementById('adv-d').textContent = document.getElementById('adr-d').value + 'ms';
  document.getElementById('adv-s').textContent = document.getElementById('adr-s').value + '%';
  document.getElementById('adv-r').textContent = document.getElementById('adr-r').value + 'ms';
  renderAdsrCurve();
}

// state.songs: array of song name strings
let songList = [];

function decodeSongList(dv, len) {
  const raw = new Uint8Array(dv.buffer, 1, len - 1);
  const text = new TextDecoder().decode(raw);
  songList = text.split('\n').map(s => s.trim()).filter(Boolean);
  renderSongList();
}

function decodeSynthParams(dv, len) {
  if (len < 3) return;
  const li = dv.getUint8(1);
  if (li >= NUM_LANES) return;
  const typeId = dv.getUint8(2);
  const params = [];
  for (let i = 0; i < 8 && 3 + i * 5 + 4 <= len; i++) {
    params.push({ id: dv.getUint8(3 + i * 5), val: dv.getFloat32(4 + i * 5, true) });
  }
  state.synthParams = state.synthParams || {};
  state.synthParams[li] = { typeId, params };
  if (synthLaneIdx === li) renderSynthParams();
}

function readStr(dv, off, maxLen) {
  let s = '';
  for (let i = 0; i < maxLen; i++) {
    const c = dv.getUint8(off + i);
    if (!c) break;
    s += String.fromCharCode(c);
  }
  return s;
}

function decodeSettings(dv, len) {
  // byte 1: playback_mode (0=LIVE, 1=SONG)
  // bytes 2-5: default_bpm f32
  if (len >= 2) {
    state.playback_mode = dv.getUint8(1);
    updatePlaybackModeUI();
  }
  if (len >= 6) {
    const defBpm = dv.getFloat32(2, true);
    const inp = document.getElementById('default-bpm-input');
    if (inp) inp.value = Math.round(defBpm);
  }
}

function updatePlaybackModeUI() {
  const live = document.getElementById('pb-mode-live');
  const song = document.getElementById('pb-mode-song');
  if (!live || !song) return;
  const isLive = (state.playback_mode === 0);
  live.classList.toggle('lime', isLive);
  song.classList.toggle('lime', !isLive);
}

function sendPlaybackMode(mode) {
  state.playback_mode = mode;
  updatePlaybackModeUI();
  wsSend(new Uint8Array([CMD.SET_SETTINGS, 0x03, mode]).buffer);
}

function decodeFullState(dv, len) {
  // FULL_STATE: [type, u8 dirty, name[64], ...]
  if (len < 2) return;
  state.songDirty = dv.getUint8(1) !== 0;
  if (len >= 2 + 64) {
    const name = readStr(dv, 2, 64);
    if (name) state.songName = name;
  }
}

function renderTopBar() {
  const el = document.getElementById('song-name-disp');
  if (!el) return;
  el.textContent = (state.songName || 'untitled') + (state.songDirty ? ' *' : '');
}

// ─────────────────────────────────────────────────────────────────────────────
// SONG VIEW render
// ─────────────────────────────────────────────────────────────────────────────
const TYPE_NAMES = ['WAV', 'DRUM', 'SYNTH', '808'];
const TYPE_KEYS  = ['wav', 'drum', 'synth', 'dsyn'];

function laneBarsLabel(loop_len) {
  if (!loop_len) return '—';
  const bar_t = (state.ppqn || 96) * (state.beats || 4);
  const bars = loop_len / bar_t;
  if (bars === Math.round(bars)) return bars + (bars === 1 ? ' bar' : ' bars');
  return (loop_len / (state.ppqn || 96)).toFixed(1) + 'b';
}

function laneName(li) {
  const l = state.lanes[li];
  if (!l) return `Lane ${li + 1}`;
  return l.name || `Lane ${li + 1}`;
}

function renderSong() {
  const list = document.getElementById('lane-list');
  const active = state.lanes.filter(l => l && l.active);
  const anySolo = active.some(l => l.solo);
  list.innerHTML = '';
  active.forEach(l => {
    const tk = TYPE_KEYS[l.type] || 'wav';
    const tn = TYPE_NAMES[l.type] || '?';
    const vol_pct = Math.round(l.volume * 100);
    const pan_pct = Math.round((l.pan + 1) * 50);  // -1..1 → 0..100%
    const dimmed = l.mute || (anySolo && !l.solo);
    const loopLabel = laneBarsLabel(l.loop_len);
    const row = document.createElement('div');
    row.className = 'lane-row' + (dimmed ? ' muted' : '');
    row.innerHTML =
      `<div class="lane-bar ${tk}"></div>` +
      `<span class="lane-name">${String(l.idx + 1).padStart(2,'0')}</span>` +
      `<div class="lane-name-block"><span class="lane-label">${esc(l.name)}</span><span class="lane-chip ${tk}">${tn}</span></div>` +
      `<div class="lane-loop-len">${loopLabel}</div>` +
      `<div style="display:flex;flex-direction:column;gap:3px;flex:1;min-width:0">` +
      `<div class="lane-fader"><div class="lane-fader-fill ${tk}" style="width:${vol_pct}%"></div></div>` +
      `<div class="lane-fader lane-pan-fader"><div class="lane-fader-fill" style="width:${pan_pct}%;background:var(--cyan)"></div><div style="position:absolute;left:50%;top:0;width:1px;height:100%;background:rgba(255,255,255,0.2)"></div></div>` +
      `</div>` +
      `<div class="lane-solo${l.solo ? ' active' : ''}" onclick="event.stopPropagation();sendToggleSolo(${l.idx})">S</div>` +
      `<div class="lane-mute${l.mute ? ' muted' : ''}" onclick="event.stopPropagation();sendToggleMute(${l.idx})">${l.mute ? '✕' : '⊘'}</div>` +
      `<div class="lane-solo" onclick="event.stopPropagation();openFxEditor(${l.idx})" style="font-size:13px">⋮</div>`;
    row.addEventListener('click', () => selectLane(l.idx));
    row.addEventListener('contextmenu', e => { e.preventDefault(); openCtxMenu(e, l.idx); });
    let longPressTimer = null;
    row.addEventListener('pointerdown', e => {
      longPressTimer = setTimeout(() => { longPressTimer = null; openCtxMenu(e, l.idx); }, 600);
    });
    row.addEventListener('pointerup',   () => { if (longPressTimer) clearTimeout(longPressTimer); });
    row.addEventListener('pointermove', () => { if (longPressTimer) { clearTimeout(longPressTimer); longPressTimer = null; } });

    // Lane volume fader drag
    const fader = row.querySelector('.lane-fader');
    if (fader) {
      let faderDragX = null, faderDragVol = null;
      fader.addEventListener('pointerdown', e => {
        e.stopPropagation();
        if (longPressTimer) { clearTimeout(longPressTimer); longPressTimer = null; }
        fader.setPointerCapture(e.pointerId);
        faderDragX = e.clientX;
        faderDragVol = l.volume;
      });
      fader.addEventListener('pointermove', e => {
        if (faderDragX === null) return;
        const dx = (e.clientX - faderDragX) / fader.offsetWidth;
        const newVol = Math.max(0, Math.min(1, faderDragVol + dx));
        l.volume = newVol;
        fader.querySelector('.lane-fader-fill').style.width = Math.round(newVol * 100) + '%';
      });
      fader.addEventListener('pointerup', () => {
        if (faderDragX === null) return;
        faderDragX = null;
        sendLaneVolume(l.idx, l.volume);
      });
    }

    // Lane pan fader drag (double-tap resets to centre)
    const panFader = row.querySelector('.lane-pan-fader');
    if (panFader) {
      let panDragX = null, panDragVal = null, panLastTap = 0;
      panFader.addEventListener('pointerdown', e => {
        e.stopPropagation();
        if (longPressTimer) { clearTimeout(longPressTimer); longPressTimer = null; }
        const now = Date.now();
        if (now - panLastTap < 350) {
          l.pan = 0;
          panFader.querySelector('.lane-fader-fill').style.width = '50%';
          sendLanePan(l.idx, 0);
          panDragX = null; return;
        }
        panLastTap = now;
        panFader.setPointerCapture(e.pointerId);
        panDragX = e.clientX;
        panDragVal = l.pan;
      });
      panFader.addEventListener('pointermove', e => {
        if (panDragX === null) return;
        const dx = (e.clientX - panDragX) / panFader.offsetWidth * 2;
        const newPan = Math.max(-1, Math.min(1, panDragVal + dx));
        l.pan = newPan;
        panFader.querySelector('.lane-fader-fill').style.width = Math.round((newPan + 1) * 50) + '%';
      });
      panFader.addEventListener('pointerup', () => {
        if (panDragX === null) return;
        panDragX = null;
        sendLanePan(l.idx, l.pan);
      });
    }

    list.appendChild(row);
  });

  // Add lane row (always shown at bottom)
  if (active.length < 16) {
    const addRow = document.createElement('div');
    addRow.className = 'lane-row add-lane-row';
    addRow.innerHTML = '<div style="flex:1;text-align:center;font-family:var(--f-mono);font-size:12px;color:var(--t2);letter-spacing:.1em">+ ADD LANE</div>';
    addRow.onclick = () => openAddLanePicker();
    list.appendChild(addRow);
  }
}

// Add lane type picker
let addLanePickerOpen = false;
function openAddLanePicker() {
  const existing = document.getElementById('add-lane-picker');
  if (existing) { existing.remove(); return; }
  const picker = document.createElement('div');
  picker.id = 'add-lane-picker';
  picker.className = 'fx-picker open';
  picker.innerHTML =
    '<div id="add-lane-picker-inner" class="" style="background:var(--bg-2);border-radius:16px 16px 0 0;padding:0 0 24px 0;max-height:60vh;overflow-y:auto">' +
    '<div class="sh">ADD LANE</div>' +
    ['DRUM', 'SYNTH', 'WAV'].map((name, i) =>
      `<div class="fx-pick-item" onclick="sendAddLane(${i});document.getElementById('add-lane-picker').remove()">${name}</div>`
    ).join('') +
    '</div>';
  picker.addEventListener('pointerdown', e => { if (e.target === picker) picker.remove(); });
  document.body.appendChild(picker);
}

function sendAddLane(type) {
  wsSend(new Uint8Array([CMD.ADD_LANE, type]).buffer);
}

function selectLane(li) {
  selected_lane = li;
  const l = state.lanes[li];
  if (!l) return;
  const nm = laneName(li);
  if (l.type === 0) {  // WAV
    openWavDetail(li);
  } else if (l.type === 1) {  // DRUM
    document.getElementById('drum-lane-name').textContent = nm;
    nav('drum', document.querySelectorAll('.nav-tab')[1]);
    renderDrumGrid();
  } else if (l.type === 3) {  // DRUMSYNTH (808)
    document.getElementById('dsyn-lane-name').textContent = nm;
    dsynSelRow = 0;
    nav('dsyn', document.querySelectorAll('.nav-tab')[1]);
    renderDsyn();
  } else if (l.type === 2) {  // SYNTH
    document.getElementById('roll-lane-name').textContent = nm;
    nav('roll', document.querySelectorAll('.nav-tab')[2]);
    // Sync loop length selector
    const sel = document.getElementById('roll-loop-sel');
    if (sel && l.loop_len) {
      const bar_t = (state.ppqn || 96) * (state.beats || 4);
      const bars  = Math.round(l.loop_len / bar_t);
      if (bars >= 1) sel.value = bars;
    }
    renderPianoRoll();
  }
  // Update ADSR title when lane changes
  const adsrTitle = document.getElementById('adsr-title');
  if (adsrTitle) adsrTitle.textContent = `ADSR — ${nm}`;
}

function openWavDetail(li) {
  const l = state.lanes[li];
  document.getElementById('wav-lane-name').textContent = laneName(li);
  document.getElementById('wav-path-disp').textContent = l.wav_path || '— no file —';
  document.getElementById('wav-loop-disp').textContent = laneBarsLabel(l.loop_len);
  const volEl = document.getElementById('wav-vol');
  if (volEl) { volEl.value = Math.round(l.volume * 100); document.getElementById('wav-vol-val').textContent = Math.round(l.volume * 100) + '%'; }
  const panEl = document.getElementById('wav-pan');
  if (panEl) { panEl.value = Math.round(l.pan * 100); document.getElementById('wav-pan-val').textContent = Math.round(l.pan * 100); }
  document.querySelectorAll('section').forEach(s => s.classList.remove('active'));
  document.getElementById('s-wav').classList.add('active');
  document.querySelectorAll('.nav-tab').forEach(b => b.classList.remove('active'));
}

function sendLanePan(li, pan) {
  // SET_LANE mask=0x04: [cmd, lane, mask, f32_vol(ignored), f32_pan]
  const buf = new ArrayBuffer(11);
  const dv  = new DataView(buf);
  dv.setUint8(0, CMD.SET_LANE);
  dv.setUint8(1, li);
  dv.setUint8(2, 0x04);
  const l = state.lanes[li];
  dv.setFloat32(3, l ? l.volume : 1.0, true);
  dv.setFloat32(7, pan, true);
  wsSend(buf);
}

async function openWavFilePicker() {
  if (selected_lane < 0) return;
  const picker = document.createElement('div');
  picker.className = 'fx-picker open';
  picker.innerHTML =
    '<div style="background:var(--bg-2);border-radius:16px 16px 0 0;max-height:70vh;overflow-y:auto">' +
    '<div class="sh">CHOOSE FILE — Lane ' + (selected_lane + 1) + '</div>' +
    '<div id="wfp-dir-row" style="display:flex;gap:8px;padding:8px 16px;border-bottom:1px solid var(--line)">' +
    '<select id="wfp-dir" style="flex:1" onchange="loadWavPickerFiles()"><option value="sounds">sounds</option></select>' +
    '</div>' +
    '<div id="wfp-files" style="padding:8px 16px;font-family:var(--f-mono);font-size:12px;color:var(--t3)">Loading…</div>' +
    '</div>';
  picker.addEventListener('pointerdown', e => { if (e.target === picker) picker.remove(); });
  document.body.appendChild(picker);
  try {
    const r = await fetch('/sd/ls?path=sounds');
    const j = await r.json();
    const sel = document.getElementById('wfp-dir');
    (j.dirs || []).forEach(d => {
      const o = document.createElement('option');
      o.value = 'sounds/' + d; o.textContent = d;
      sel.appendChild(o);
    });
  } catch(e) {}
  loadWavPickerFiles();
}

async function loadWavPickerFiles() {
  const sel = document.getElementById('wfp-dir');
  const dir = sel ? sel.value : 'sounds';
  const container = document.getElementById('wfp-files');
  if (!container) return;
  container.textContent = 'Loading…';
  try {
    const r = await fetch('/sd/ls?path=' + encodeURIComponent(dir));
    const j = await r.json();
    const files = (j.files || []).filter(f => /\.wav$/i.test(f));
    container.innerHTML = '';
    files.forEach(f => {
      const div = document.createElement('div');
      div.className = 'fx-pick-item';
      div.textContent = f;
      div.onclick = () => {
        const path = dir + '/' + f;
        // SET_LANE mask=0x10 (wav_path)
        const enc = new TextEncoder().encode(path);
        const buf = new Uint8Array(3 + enc.length);
        buf[0] = CMD.SET_LANE; buf[1] = selected_lane; buf[2] = 0x10;
        buf.set(enc, 3);
        wsSend(buf.buffer);
        if (state.lanes[selected_lane]) state.lanes[selected_lane].wav_path = path;
        document.getElementById('wav-path-disp').textContent = path;
        document.querySelector('.fx-picker.open')?.remove();
      };
      container.appendChild(div);
    });
    if (!files.length) container.textContent = 'No WAV files';
  } catch(e) { container.textContent = 'Error loading'; }
}

// ─────────────────────────────────────────────────────────────────────────────
// DRUM GRID canvas
// ─────────────────────────────────────────────────────────────────────────────
let drumRowOffset = 0;
const DRUM_LABEL_W = 80;
const DRUM_CELL_H  = 60;
const DRUM_CELL_W  = 60;
const DRUM_BEAT_H  = 28;
const DRUM_ROWS_VIS = 6;

function drumScroll(dir) { drumRowOffset = Math.max(0, drumRowOffset + dir); renderDrumGrid(); }

function renderDrumGrid() {
  if (selected_lane < 0 || !state.drum[selected_lane]) return;
  const seq = state.drum[selected_lane];
  const rows = seq.rows;
  const nrows = rows.length;
  if (!nrows) return;
  const steps = rows[0].step_count || 16;
  const vis = Math.min(DRUM_ROWS_VIS, nrows);

  document.getElementById('drum-rows-vis-label').textContent = `rows ${drumRowOffset + 1}–${Math.min(drumRowOffset + vis, nrows)} of ${nrows}`;
  const stepsSel = document.getElementById('drum-steps-sel');
  if (stepsSel && stepsSel.value !== String(steps)) stepsSel.value = steps;
  const barsSel = document.getElementById('drum-bars-sel');
  if (barsSel) {
    const bar_t = (state.ppqn || 96) * (state.beats || 4);
    const l = state.lanes[selected_lane];
    if (l && l.loop_len && bar_t) {
      const bars = Math.round(l.loop_len / bar_t);
      if (barsSel.value !== String(bars)) barsSel.value = bars;
    }
  }

  const canvas = document.getElementById('drum-canvas');
  const W = DRUM_LABEL_W + steps * (DRUM_CELL_W + 2) + 2;
  const H = DRUM_BEAT_H + vis * (DRUM_CELL_H + 2);
  canvas.width = W; canvas.height = H;
  canvas.style.width  = W + 'px';
  canvas.style.height = H + 'px';

  const ctx = canvas.getContext('2d');
  ctx.fillStyle = '#0E0E11';
  ctx.fillRect(0, 0, W, H);

  // Beat header
  ctx.fillStyle = '#15151A';
  ctx.fillRect(DRUM_LABEL_W, 0, W - DRUM_LABEL_W, DRUM_BEAT_H);
  for (let s = 0; s < steps; s++) {
    const x = DRUM_LABEL_W + s * (DRUM_CELL_W + 2) + 2;
    if (s % 4 === 0) {
      ctx.fillStyle = '#74747D';
      ctx.font = '11px monospace';
      ctx.fillText(String(s / 4 + 1), x + 4, DRUM_BEAT_H - 6);
    }
  }

  for (let vi = 0; vi < vis; vi++) {
    const ri = vi + drumRowOffset;
    if (ri >= nrows) break;
    const row = rows[ri];
    const y = DRUM_BEAT_H + vi * (DRUM_CELL_H + 2);

    // Row label (tap left = assign WAV, tap right = mute toggle)
    const MUTE_ZONE = 20;
    ctx.fillStyle = row.mute ? '#2A1A1A' : '#1C1C22';
    ctx.fillRect(0, y, DRUM_LABEL_W, DRUM_CELL_H);
    // Mute button zone (right edge)
    ctx.fillStyle = row.mute ? '#FF4444' : '#2C2C34';
    ctx.beginPath();
    ctx.roundRect(DRUM_LABEL_W - MUTE_ZONE - 2, y + 4, MUTE_ZONE, DRUM_CELL_H - 8, 3);
    ctx.fill();
    ctx.fillStyle = row.mute ? '#FFF' : '#74747D';
    ctx.font = '9px monospace';
    ctx.fillText('M', DRUM_LABEL_W - MUTE_ZONE + 3, y + DRUM_CELL_H / 2 + 4);
    // Name + assign arrow
    ctx.fillStyle = row.mute ? '#555' : '#B8B8C0';
    ctx.font = '11px monospace';
    const rowLabel = row.wav_path ? row.wav_path.split('/').pop().replace(/\.wav$/i,'').slice(0,8) : `R${ri+1}`;
    ctx.fillText(rowLabel, 4, y + DRUM_CELL_H / 2 + 4);
    ctx.fillStyle = '#C7F546';
    ctx.font = '10px monospace';
    ctx.fillText('▲', DRUM_LABEL_W - MUTE_ZONE - 14, y + DRUM_CELL_H / 2 + 4);

    for (let s = 0; s < row.step_count; s++) {
      const vel   = row.steps[s] || 0;
      const accnt = row.accents && row.accents[s];
      const x     = DRUM_LABEL_W + s * (DRUM_CELL_W + 2) + 2;
      const isPlay = state.step[selected_lane] === s;
      let bg;
      if (vel > 0 && accnt) bg = '#FFB547';
      else if (vel > 0)     bg = '#C7F546';
      else if (isPlay)      bg = 'rgba(255,255,255,0.12)';
      else                  bg = '#1C1C22';
      ctx.fillStyle = bg;
      ctx.beginPath();
      ctx.roundRect(x, y + 2, DRUM_CELL_W - 2, DRUM_CELL_H - 4, 4);
      ctx.fill();
    }
  }
}

function renderDrumPlayhead() {
  if (document.getElementById('s-drum').classList.contains('active'))
    renderDrumGrid();
  if (document.getElementById('s-roll').classList.contains('active'))
    renderPianoRoll();
  dsynUpdatePlayhead();
}

// ─────────────────────────────────────────────────────────────────────────────
// 808 DRUM SYNTH editor
// ─────────────────────────────────────────────────────────────────────────────
const VOICE_NAMES = ['BD','SD','LT','MT','HT','LC','MC','HC','RS','CP','CB','CY','OH','CH','MA','CL'];
const VOICE_LONG  = ['Bass Drum','Snare','Lo Tom','Mid Tom','Hi Tom','Lo Conga','Mid Conga',
                     'Hi Conga','Rim Shot','Clap','Cowbell','Cymbal','Open Hat','Closed Hat',
                     'Maracas','Claves'];
const DSYN_KNOBS  = ['tune','decay','tone','snap','drive','level','pan'];
let dsynSelRow = 0;

function decodeDsynRow(dv, len) {
  if (len < 162) return;
  const li = dv.getUint8(1), ri = dv.getUint8(2);
  const voice = dv.getUint8(3), sc = dv.getUint8(4), rc = dv.getUint8(5);
  if (!state.dsyn[li]) state.dsyn[li] = { step_count: 16, row_count: 0, rows: [] };
  const seq = state.dsyn[li];
  seq.step_count = sc; seq.row_count = rc;
  if (seq.rows.length > rc) seq.rows.length = rc;
  while (seq.rows.length <= ri)
    seq.rows.push({ voice: 0, knobs: [0,0,0,0,0,0,0],
                    steps: new Array(64).fill(0), accents: new Array(64).fill(0) });
  const row = seq.rows[ri];
  row.voice = voice;
  for (let k = 0; k < 7; k++) row.knobs[k] = dv.getFloat32(6 + k * 4, true);
  for (let s = 0; s < 64; s++) { row.steps[s] = dv.getUint8(34 + s); row.accents[s] = dv.getUint8(98 + s); }
}

function renderDsyn() {
  if (selected_lane < 0) return;
  const seq = state.dsyn[selected_lane];
  const root = document.getElementById('dsyn-root');
  if (!seq || !root) return;
  const ssel = document.getElementById('dsyn-steps-sel');
  if (ssel && ssel.value !== String(seq.step_count)) ssel.value = seq.step_count;
  if (dsynSelRow >= seq.rows.length) dsynSelRow = 0;

  let html = '<div class="dsyn-grid">';
  seq.rows.forEach((row, ri) => {
    html += `<div class="dsyn-row${ri === dsynSelRow ? ' sel' : ''}">`;
    html += `<div class="dsyn-rowlabel" onclick="dsynSelectRow(${ri})">` +
            `<button class="dsyn-play" onclick="event.stopPropagation();sendDsynTrigger(${ri})">▶</button>` +
            `<span class="dsyn-vname">${VOICE_NAMES[row.voice] || '?'}</span></div>`;
    html += '<div class="dsyn-steps">';
    for (let s = 0; s < seq.step_count; s++) {
      const vel = row.steps[s] || 0, acc = row.accents[s];
      const cls = vel > 0 ? (acc ? 'on acc' : 'on') : '';
      const beat = (s % 4 === 0) ? ' beat' : '';
      const play = state.step[selected_lane] === s ? ' play' : '';
      html += `<div class="dsyn-cell ${cls}${beat}${play}" data-si="${s}" onclick="dsynCellTap(${ri},${s})"></div>`;
    }
    html += '</div></div>';
  });
  html += '</div>';

  const row = seq.rows[dsynSelRow];
  if (row) {
    html += '<div class="dsyn-knobs">';
    html += `<div class="dsyn-voicesel"><label>VOICE ${dsynSelRow + 1}</label>` +
            `<select onchange="sendDsynVoice(${dsynSelRow}, +this.value)">` +
            VOICE_LONG.map((nm, i) =>
              `<option value="${i}"${i === row.voice ? ' selected' : ''}>${VOICE_NAMES[i]} — ${nm}</option>`).join('') +
            '</select></div>';
    DSYN_KNOBS.forEach((kn, ki) => {
      const isPan = ki === 6;
      const min = isPan ? -100 : 0;
      const val = Math.round(row.knobs[ki] * 100);
      html += `<div class="dsyn-knob"><label>${kn.toUpperCase()}</label>` +
              `<input type="range" min="${min}" max="100" value="${val}" ` +
              `oninput="sendDsynKnob(${dsynSelRow},${ki},this.value/100,this)">` +
              `<span class="dsyn-kv">${val}</span></div>`;
    });
    html += `<div class="dsyn-rowbtns">` +
            `<button class="btn" onclick="sendDsynClearRow(${dsynSelRow})">CLEAR</button>` +
            `<button class="btn" onclick="sendDsynDelRow(${dsynSelRow})">DEL VOICE</button></div>`;
    html += '</div>';
  }
  root.innerHTML = html;
}

function dsynUpdatePlayhead() {
  if (selected_lane < 0 || !state.dsyn[selected_lane]) return;
  const root = document.getElementById('dsyn-root');
  if (!root || !root.firstChild) return;
  root.querySelectorAll('.dsyn-cell.play').forEach(c => c.classList.remove('play'));
  const cur = state.step[selected_lane];
  if (cur == null || cur >= 64) return;
  root.querySelectorAll(`.dsyn-cell[data-si="${cur}"]`).forEach(c => c.classList.add('play'));
}

function dsynSelectRow(ri) { dsynSelRow = ri; renderDsyn(); }

function dsynCellTap(ri, si) {
  const seq = state.dsyn[selected_lane];
  if (!seq || !seq.rows[ri]) return;
  const row = seq.rows[ri];
  const vel = row.steps[si] || 0, acc = row.accents[si];
  let nv, na;                       // cycle: off → on → accent → off
  if (vel === 0)      { nv = 100; na = 0; }
  else if (!acc)      { nv = 100; na = 1; }
  else                { nv = 0;   na = 0; }
  sendDsynStep(ri, si, nv, na);
}

// ── Encoders (op codes match cmd_set_dsyn in ws_cmd.c) ──
function sendDsynStep(ri, si, vel, acc) {
  if (selected_lane < 0) return;
  wsSend(new Uint8Array([CMD.SET_DSYN, selected_lane, 0, ri, si, vel, acc ? 1 : 0]).buffer);
}
function sendDsynKnob(ri, ki, val, el) {
  if (el && el.nextElementSibling) el.nextElementSibling.textContent = Math.round(val * 100);
  if (selected_lane < 0) return;
  const buf = new ArrayBuffer(9), dv = new DataView(buf);
  dv.setUint8(0, CMD.SET_DSYN); dv.setUint8(1, selected_lane); dv.setUint8(2, 1);
  dv.setUint8(3, ri); dv.setUint8(4, ki); dv.setFloat32(5, val, true);
  wsSend(buf);
}
function sendDsynVoice(ri, voice) {
  if (selected_lane < 0) return;
  wsSend(new Uint8Array([CMD.SET_DSYN, selected_lane, 2, ri, voice]).buffer);
}
function openDsynAddRow() {
  if (selected_lane < 0) return;
  wsSend(new Uint8Array([CMD.SET_DSYN, selected_lane, 3, 13]).buffer); // default = closed hat
}
function sendDsynDelRow(ri) {
  if (selected_lane < 0) return;
  wsSend(new Uint8Array([CMD.SET_DSYN, selected_lane, 4, ri]).buffer);
}
function sendDsynStepCount(sc) {
  if (selected_lane < 0) return;
  wsSend(new Uint8Array([CMD.SET_DSYN, selected_lane, 5, sc]).buffer);
}
function sendDsynTrigger(ri) {
  if (selected_lane < 0) return;
  wsSend(new Uint8Array([CMD.SET_DSYN, selected_lane, 6, ri, 110]).buffer);
}
function sendDsynClearRow(ri) {
  if (selected_lane < 0) return;
  wsSend(new Uint8Array([CMD.SET_DSYN, selected_lane, 7, ri]).buffer);
}
function sendDsynAccent(g) {
  if (selected_lane < 0) return;
  const buf = new ArrayBuffer(7), dv = new DataView(buf);
  dv.setUint8(0, CMD.SET_DSYN); dv.setUint8(1, selected_lane); dv.setUint8(2, 8);
  dv.setFloat32(3, g, true);
  wsSend(buf);
}

// Drum grid tap (double-tap=accent, long-press=velocity slider)
(function() {
  let lastTap = { ri: -1, si: -1, t: 0 };
  let longTimer = null;

  function drumHit(e) {
    const canvas = e.target;
    const rect   = canvas.getBoundingClientRect();
    const x = e.clientX - rect.left, y = e.clientY - rect.top;
    if (y < DRUM_BEAT_H) return null;
    if (selected_lane < 0 || !state.drum[selected_lane]) return null;
    const seq = state.drum[selected_lane];
    const vi  = Math.floor((y - DRUM_BEAT_H) / (DRUM_CELL_H + 2));
    const ri  = vi + drumRowOffset;
    if (ri >= seq.rows.length) return null;
    const MUTE_ZONE_W = 22;
    if (x < DRUM_LABEL_W) {
      if (x >= DRUM_LABEL_W - MUTE_ZONE_W) return { mute: true, ri };
      return { label: true, ri };
    }
    const steps = seq.rows[ri].step_count || 16;
    const si    = Math.floor((x - DRUM_LABEL_W) / (DRUM_CELL_W + 2));
    if (si < 0 || si >= steps) return null;
    return { ri, si };
  }

  document.getElementById('drum-canvas').addEventListener('pointerdown', e => {
    const hit = drumHit(e);
    if (!hit) return;

    if (hit.mute) {
      const seq = state.drum[selected_lane];
      const row = seq.rows[hit.ri];
      row.mute = !row.mute;
      renderDrumGrid();
      const buf = new Uint8Array([CMD.SET_DRUM_ROW, selected_lane, hit.ri, 0x01, row.mute ? 1 : 0]);
      wsSend(buf.buffer);
      return;
    }
    if (hit.label) {
      openDrumRowFilePicker(selected_lane, hit.ri);
      return;
    }

    const { ri, si } = hit;
    const seq = state.drum[selected_lane];
    const row = seq.rows[ri];
    const now = Date.now();
    const isDoubleTap = lastTap.ri === ri && lastTap.si === si && (now - lastTap.t) < 400;
    lastTap = { ri, si, t: now };

    if (isDoubleTap) {
      // Double-tap: toggle accent (only if step is on)
      if ((row.steps[si] || 0) > 0) {
        if (!row.accents) row.accents = {};
        row.accents[si] = !row.accents[si];
        const accent = row.accents[si] ? 1 : 0;
        renderDrumGrid();
        wsSend(new Uint8Array([CMD.SET_DRUM_STEP, selected_lane, ri, si, row.steps[si], accent]).buffer);
      }
      if (longTimer) { clearTimeout(longTimer); longTimer = null; }
      return;
    }

    // Long-press: velocity slider overlay
    longTimer = setTimeout(() => {
      longTimer = null;
      openDrumVelocityOverlay(ri, si, row.steps[si] || 100);
    }, 500);
  });

  document.getElementById('drum-canvas').addEventListener('pointerup', e => {
    if (longTimer) {
      clearTimeout(longTimer); longTimer = null;
      const hit = drumHit(e);
      if (!hit || hit.label) return;
      const { ri, si } = hit;
      const seq = state.drum[selected_lane];
      const row = seq.rows[ri];
      const vel = (row.steps[si] || 0) > 0 ? 0 : 100;
      row.steps[si] = vel;
      renderDrumGrid();
      wsSend(new Uint8Array([CMD.SET_DRUM_STEP, selected_lane, ri, si, vel, 0]).buffer);
    }
  });

  document.getElementById('drum-canvas').addEventListener('pointermove', e => {
    if (longTimer) { clearTimeout(longTimer); longTimer = null; }
  });
})();

function openDrumVelocityOverlay(ri, si, curVel) {
  const existing = document.getElementById('drum-vel-overlay');
  if (existing) existing.remove();
  const overlay = document.createElement('div');
  overlay.id = 'drum-vel-overlay';
  overlay.style.cssText = 'position:fixed;bottom:80px;left:50%;transform:translateX(-50%);background:var(--bg-3);border:1px solid var(--line-2);border-radius:12px;padding:16px 20px;z-index:300;display:flex;flex-direction:column;gap:10px;min-width:220px;box-shadow:0 8px 32px rgba(0,0,0,0.6)';
  overlay.innerHTML =
    `<div style="font-family:var(--f-mono);font-size:11px;color:var(--t2);letter-spacing:.1em">VELOCITY — R${ri+1} step ${si+1}</div>` +
    `<div style="display:flex;align-items:center;gap:10px">` +
    `<input type="range" id="drum-vel-range" min="1" max="127" value="${curVel}" style="flex:1" oninput="document.getElementById('drum-vel-num').textContent=this.value">` +
    `<span style="font-family:var(--f-mono);font-size:16px;color:var(--lime);min-width:32px" id="drum-vel-num">${curVel}</span>` +
    `</div>` +
    `<div style="display:flex;gap:8px">` +
    `<button class="btn lime" style="flex:1" onclick="applyDrumVelocity(${ri},${si})">SET</button>` +
    `<button class="btn" style="flex:1" onclick="document.getElementById('drum-vel-overlay').remove()">CANCEL</button>` +
    `</div>`;
  document.body.appendChild(overlay);
}

function applyDrumVelocity(ri, si) {
  const vel = parseInt(document.getElementById('drum-vel-range').value);
  document.getElementById('drum-vel-overlay')?.remove();
  if (selected_lane < 0 || !state.drum[selected_lane]) return;
  const row = state.drum[selected_lane].rows[ri];
  if (!row) return;
  row.steps[si] = vel;
  const accent = (row.accents && row.accents[si]) ? 1 : 0;
  renderDrumGrid();
  wsSend(new Uint8Array([CMD.SET_DRUM_STEP, selected_lane, ri, si, vel, accent]).buffer);
}

// Drum row WAV file picker
async function openDrumRowFilePicker(li, ri) {
  const picker = document.createElement('div');
  picker.className = 'fx-picker open';
  picker.innerHTML =
    '<div style="background:var(--bg-2);border-radius:16px 16px 0 0;max-height:70vh;overflow-y:auto">' +
    '<div class="sh">ASSIGN SAMPLE — Row ' + (ri+1) + '</div>' +
    '<div id="drp-dir-row" style="display:flex;gap:8px;padding:8px 16px;border-bottom:1px solid var(--line)">' +
    '<select id="drp-dir" style="flex:1" onchange="loadDrumPickerFiles()"><option value="sounds">sounds</option></select>' +
    '</div>' +
    '<div id="drp-files" style="padding:8px 16px;font-family:var(--f-mono);font-size:12px;color:var(--t3)">Loading…</div>' +
    '</div>';
  picker.addEventListener('pointerdown', e => { if (e.target === picker) picker.remove(); });
  document.body.appendChild(picker);

  // Load dirs
  try {
    const r = await fetch('/sd/ls?path=sounds');
    const j = await r.json();
    const sel = document.getElementById('drp-dir');
    (j.dirs || []).forEach(d => {
      const o = document.createElement('option');
      o.value = 'sounds/' + d; o.textContent = d;
      sel.appendChild(o);
    });
  } catch(e) {}

  window._drpLane = li;
  window._drpRow  = ri;
  loadDrumPickerFiles();
}

async function loadDrumPickerFiles() {
  const sel = document.getElementById('drp-dir');
  const dir = sel ? sel.value : 'sounds';
  const container = document.getElementById('drp-files');
  if (!container) return;
  container.textContent = 'Loading…';
  try {
    const r = await fetch('/sd/ls?path=' + encodeURIComponent(dir));
    const j = await r.json();
    const files = (j.files || []).filter(f => /\.wav$/i.test(f));
    container.innerHTML = '';
    files.forEach(f => {
      const div = document.createElement('div');
      div.className = 'fx-pick-item';
      div.textContent = f;
      div.onclick = () => {
        const path = dir + '/' + f;
        sendDrumRowAssign(window._drpLane, window._drpRow, path);
        document.querySelector('.fx-picker.open')?.remove();
      };
      container.appendChild(div);
    });
    if (!files.length) container.textContent = 'No WAV files';
  } catch(e) { container.textContent = 'Error loading'; }
}

function sendDrumRowAssign(li, ri, path) {
  const enc = new TextEncoder().encode(path);
  const buf = new Uint8Array(3 + enc.length);
  buf[0] = CMD.DRUM_ROW_ASSIGN;
  buf[1] = li;
  buf[2] = ri;
  buf.set(enc, 3);
  wsSend(buf.buffer);
  // Optimistic update
  if (state.drum[li] && state.drum[li].rows[ri])
    state.drum[li].rows[ri].wav_path = path;
  renderDrumGrid();
}

function sendDrumStepCount(count) {
  if (selected_lane < 0) return;
  // SET_LANE mask=0x20 (drum step_count, u8). The firmware rescales every row's
  // pattern so existing hits keep their position in time (stretch), so mirror
  // that here by remapping the client-side rows the same way.
  if (state.drum[selected_lane]) {
    state.drum[selected_lane].rows.forEach(row => {
      const oldSc = row.step_count || 16;
      const oldSteps = row.steps || [];
      const newSteps = new Array(count).fill(0);
      for (let v = 0; v < oldSc; v++) {
        if (!oldSteps[v]) continue;
        let dst = Math.floor(v * count / oldSc);
        if (dst >= count) dst = count - 1;
        newSteps[dst] = oldSteps[v];
      }
      row.steps = newSteps;
      row.step_count = count;
    });
  }
  const buf = new ArrayBuffer(4);
  const dv  = new DataView(buf);
  dv.setUint8(0, CMD.SET_LANE);
  dv.setUint8(1, selected_lane);
  dv.setUint8(2, 0x20);
  dv.setUint8(3, count & 0xFF);
  wsSend(buf);
  renderDrumGrid();
}

function sendDrumBars(bars) {
  if (selected_lane < 0) return;
  const bar_t = (state.ppqn || 96) * (state.beats || 4);
  sendDrumLoopLen(bars * bar_t);
}

function sendDrumLoopLen(loop_len) {
  if (selected_lane < 0) return;
  // SET_LANE mask=0x08 (loop_len_ticks): [cmd, lane, mask=0x08, u32 loop_len]
  const buf = new ArrayBuffer(7);
  const dv  = new DataView(buf);
  dv.setUint8(0, CMD.SET_LANE);
  dv.setUint8(1, selected_lane);
  dv.setUint8(2, 0x08);
  dv.setUint32(3, loop_len, true);
  wsSend(buf);
  if (state.lanes[selected_lane]) state.lanes[selected_lane].loop_len = loop_len;
}

function sendDrumAddRow() {
  if (selected_lane < 0) return;
  // ADD empty row: DRUM_ROW_ASSIGN with empty path signals "add row" if ri >= row_count
  if (!state.drum[selected_lane]) state.drum[selected_lane] = { rows: [] };
  const ri = state.drum[selected_lane].rows.length;
  // Push placeholder row locally
  state.drum[selected_lane].rows.push({ steps: new Array(16).fill(0), step_count: 16, wav_path: '' });
  renderDrumGrid();
  // Send assign with empty path (server creates empty row)
  const buf = new Uint8Array([CMD.DRUM_ROW_ASSIGN, selected_lane, ri]);
  wsSend(buf.buffer);
}

// ─────────────────────────────────────────────────────────────────────────────
// PIANO ROLL canvas
// ─────────────────────────────────────────────────────────────────────────────
const PR_KEY_W   = 40;
const PR_ROW_H   = 18;
const PR_NOTE_LO = 24;   // lowest MIDI note rendered (C1)
const PR_NOTE_HI = 108;  // highest MIDI note rendered (C8)
const PR_ROWS    = PR_NOTE_HI - PR_NOTE_LO + 1;
const PR_HEAD_H  = 24;
let rollPxPerTick = 0;   // 0 = auto; horizontal zoom in pixels per tick
let rollSnapDiv   = 16;  // snap divisor: 4/8/16/32/64, 0=free
const BK_FLAGS = [false,true,false,true,false,false,true,false,true,false,true,false];

function rollOctShift(dir) {
  const wrap = document.getElementById('roll-wrap');
  if (!wrap) return;
  wrap.scrollTop = Math.max(0, wrap.scrollTop - dir * 12 * PR_ROW_H);
}

function rollSetSnap(div) {
  rollSnapDiv = div;
  renderPianoRoll();
}

function sendRollLoopLen(bars) {
  if (selected_lane < 0) return;
  const bar_t = (state.ppqn || 96) * (state.beats || 4);
  const loop_len = bars * bar_t;
  const buf = new ArrayBuffer(7);
  const dv  = new DataView(buf);
  dv.setUint8(0, CMD.SET_LANE);
  dv.setUint8(1, selected_lane);
  dv.setUint8(2, 0x08);
  dv.setUint32(3, loop_len, true);
  wsSend(buf);
  if (state.lanes[selected_lane]) state.lanes[selected_lane].loop_len = loop_len;
}

function rollDefaultPxPerTick() {
  // Default: 2 bars span the viewport width
  const bar_t = (state.ppqn || 96) * (state.beats || 4);
  const wrap  = document.getElementById('roll-wrap');
  const vw    = (wrap ? wrap.clientWidth : window.innerWidth) - PR_KEY_W;
  return Math.max(0.02, vw / (bar_t * 2));
}

function rollZoom(factor) {
  // factor < 1 = zoom in (more px per tick), factor > 1 = zoom out
  if (!rollPxPerTick) rollPxPerTick = rollDefaultPxPerTick();
  rollPxPerTick = rollPxPerTick / factor;
  // Clamp: at least 0.01 px/tick (very zoomed out), at most 4 px/tick (very zoomed in)
  rollPxPerTick = Math.max(0.01, Math.min(4, rollPxPerTick));
  renderPianoRoll();
}

function rollScroll(dir) {
  const wrap = document.getElementById('roll-wrap');
  if (!wrap) return;
  wrap.scrollLeft = Math.max(0, wrap.scrollLeft + dir * Math.round(wrap.clientWidth / 2));
}

function rollLoopLenTicks() {
  const bar_t = (state.ppqn || 96) * (state.beats || 4);
  if (selected_lane >= 0 && state.lanes[selected_lane] && state.lanes[selected_lane].loop_len) {
    return state.lanes[selected_lane].loop_len;
  }
  return bar_t * 4;
}

function renderPianoRoll() {
  if (selected_lane < 0) return;
  const notes  = state.notes[selected_lane] || [];
  const canvas = document.getElementById('roll-canvas');
  const wrap   = document.getElementById('roll-wrap');
  // (Re)initialize horizontal zoom once the wrapper has real width
  if ((!rollPxPerTick || rollPxPerTick <= 0.02) && wrap && wrap.clientWidth > PR_KEY_W) {
    rollPxPerTick = rollDefaultPxPerTick();
  }
  if (!rollPxPerTick) rollPxPerTick = 0.5;

  const loop_t = rollLoopLenTicks();
  const bar_t  = (state.ppqn || 96) * (state.beats || 4);
  const W = PR_KEY_W + Math.max(200, Math.round(loop_t * rollPxPerTick));
  const H = PR_HEAD_H + PR_ROWS * PR_ROW_H;
  canvas.width = W; canvas.height = H;
  canvas.style.width = W + 'px'; canvas.style.height = H + 'px';

  const roll_w = W - PR_KEY_W;
  const ctx    = canvas.getContext('2d');

  // Header background
  ctx.fillStyle = '#0E0E11';
  ctx.fillRect(0, 0, W, PR_HEAD_H);

  // Row backgrounds: white-key rows light, black-key rows dark
  for (let r = 0; r < PR_ROWS; r++) {
    const note = PR_NOTE_HI - r;
    const semi = ((note % 12) + 12) % 12;
    const isBlack = BK_FLAGS[semi];
    const y = PR_HEAD_H + r * PR_ROW_H;

    // Key column (mini-keyboard on the left)
    ctx.fillStyle = isBlack ? '#1A1A1F' : '#E8E8EC';
    ctx.fillRect(0, y, PR_KEY_W - 1, PR_ROW_H);
    // Octave label on C
    if (semi === 0) {
      ctx.fillStyle = '#1A1A1F';
      ctx.font = '9px monospace';
      ctx.fillText('C' + (Math.floor(note / 12) - 1), 4, y + PR_ROW_H - 4);
    }

    // Row background in the grid area
    ctx.fillStyle = isBlack ? '#101015' : '#23232B';
    ctx.fillRect(PR_KEY_W, y, roll_w, PR_ROW_H);

    // Subtle row separator line
    ctx.fillStyle = 'rgba(0,0,0,0.25)';
    ctx.fillRect(PR_KEY_W, y + PR_ROW_H - 1, roll_w, 1);
  }

  // Bar lines + beat lines
  const beat_t = state.ppqn || 96;
  for (let tick = 0; tick <= loop_t; tick += beat_t) {
    const x = PR_KEY_W + tick * rollPxPerTick;
    const isBar = (tick % bar_t) === 0;
    ctx.fillStyle = isBar ? '#3A3A48' : 'rgba(58,58,72,0.4)';
    ctx.fillRect(x, PR_HEAD_H, isBar ? 1 : 1, H - PR_HEAD_H);
    if (isBar) {
      const bar = Math.floor(tick / bar_t) + 1;
      ctx.fillStyle = '#74747D';
      ctx.font = '10px monospace';
      ctx.fillText(bar, x + 3, 18);
    }
  }

  // Playhead
  const ph_tick = state.playhead % (loop_t || 1);
  const ph_x = PR_KEY_W + ph_tick * rollPxPerTick;
  ctx.fillStyle = '#C7F546';
  ctx.fillRect(ph_x, 0, 2, H);

  // Notes
  const NOTE_NAMES = ['C','C#','D','D#','E','F','F#','G','G#','A','A#','B'];
  const VEL_BAR_H = 3;
  for (const n of notes) {
    const r = PR_NOTE_HI - n.note;
    if (r < 0 || r >= PR_ROWS) continue;
    const nx = PR_KEY_W + n.tick_start * rollPxPerTick;
    const nw = Math.max(4, n.tick_len * rollPxPerTick);
    const ny = PR_HEAD_H + r * PR_ROW_H + 1;
    const nh = PR_ROW_H - 2;
    const alpha = 0.5 + (n.velocity / 127) * 0.5;
    ctx.fillStyle = `rgba(199,245,70,${alpha})`;
    ctx.beginPath();
    ctx.roundRect(nx, ny, nw, nh, 3);
    ctx.fill();
    ctx.fillStyle = '#C7F546';
    ctx.fillRect(nx, ny, 3, nh);
    const velW = Math.max(1, (n.velocity / 127) * (nw - 3));
    ctx.fillStyle = 'rgba(255,255,255,0.35)';
    ctx.fillRect(nx + 3, ny + nh - VEL_BAR_H, velW, VEL_BAR_H);
    if (nw > 16 && nh > 10) {
      const octave = Math.floor(n.note / 12) - 1;
      const noteName = NOTE_NAMES[n.note % 12] + octave;
      ctx.fillStyle = 'rgba(8,8,10,0.75)';
      ctx.font = '9px monospace';
      ctx.fillText(noteName, nx + 5, ny + nh - VEL_BAR_H - 3);
    }
  }

  // bar·beat·tick readout for playhead position
  const ph_bar  = Math.floor(ph_tick / bar_t) + 1;
  const ph_beat = Math.floor((ph_tick % bar_t) / (state.ppqn || 96)) + 1;
  const ph_t    = ph_tick % (state.ppqn || 96);
  document.getElementById('roll-pos').textContent = `▸ ${ph_bar}.${ph_beat}.${ph_t}`;
  const snapLabel = rollSnapDiv > 0 ? `1/${rollSnapDiv}` : 'free';
  document.getElementById('roll-snap-sel').title = `snap: ${snapLabel}`;
}

// Piano roll pointer interaction
(function() {
  const canvas = document.getElementById('roll-canvas');
  let pressTimer = null;
  let auditNote  = -1;  // note currently being auditioned via key column

  // Drag state: {note, mode:'move'|'resize', origStart, origLen, startTick}
  let drag = null;

  function xToTick(cx) {
    return (cx - PR_KEY_W) / (rollPxPerTick || rollDefaultPxPerTick());
  }

  function snapTick(tick) {
    const grid = rollSnapDiv > 0 ? state.ppqn * 4 / rollSnapDiv : 1;
    return rollSnapDiv > 0 ? Math.round(tick / grid) * grid : Math.round(tick);
  }

  function noteAtPointer(cx, cy) {
    if (cy < PR_HEAD_H) return null;
    const tick = xToTick(cx);
    const row  = Math.floor((cy - PR_HEAD_H) / PR_ROW_H);
    const note = PR_NOTE_HI - row;
    const notes = state.notes[selected_lane] || [];
    for (let i = notes.length - 1; i >= 0; i--) {
      const n = notes[i];
      if (n.note !== note) continue;
      if (tick < n.tick_start || tick >= n.tick_start + n.tick_len) continue;
      const nx_end = PR_KEY_W + (n.tick_start + n.tick_len) * rollPxPerTick;
      const isResize = (nx_end - cx) < 8;
      return { n, mode: isResize ? 'resize' : 'move' };
    }
    return null;
  }

  canvas.addEventListener('pointerdown', e => {
    if (selected_lane < 0) return;
    const rect = canvas.getBoundingClientRect();
    const cx   = e.clientX - rect.left;
    const cy   = e.clientY - rect.top;
    if (cy < PR_HEAD_H) return;

    const row  = Math.floor((cy - PR_HEAD_H) / PR_ROW_H);
    const note = PR_NOTE_HI - row;
    if (note < 0 || note > 127) return;

    // Key column: audition note
    if (cx < PR_KEY_W) {
      auditNote = note;
      wsSend(new Uint8Array([CMD.NOTE_ON, selected_lane, note, 100]).buffer);
      return;
    }

    const tick = xToTick(cx);
    const hit  = noteAtPointer(cx, cy);

    if (hit) {
      canvas.setPointerCapture(e.pointerId);
      drag = {
        note:      hit.n,
        mode:      hit.mode,
        origStart: hit.n.tick_start,
        origLen:   hit.n.tick_len,
        startTick: tick,
      };
      pressTimer = setTimeout(() => {
        pressTimer = null;
        if (drag && drag.mode === 'move') {
          drag = null;
          deleteRollNote(hit.n);
        }
      }, 400);
    } else {
      const snap_tick = snapTick(tick);
      const grid = rollSnapDiv > 0 ? state.ppqn * 4 / rollSnapDiv : state.ppqn;
      const tick_len = grid * 4;
      const vel = 100;
      if (!state.notes[selected_lane]) state.notes[selected_lane] = [];
      state.notes[selected_lane].push({ tick_start: snap_tick, tick_len, note, velocity: vel });
      renderPianoRoll();
      const buf = new ArrayBuffer(12);
      const dv  = new DataView(buf);
      dv.setUint8(0, CMD.ADD_NOTE);
      dv.setUint8(1, selected_lane);
      dv.setUint32(2, snap_tick, true);
      dv.setUint32(6, tick_len, true);
      dv.setUint8(10, note);
      dv.setUint8(11, vel);
      wsSend(buf);
    }
  });

  canvas.addEventListener('pointermove', e => {
    if (pressTimer) { clearTimeout(pressTimer); pressTimer = null; }
    if (!drag) return;
    const rect = canvas.getBoundingClientRect();
    const cx   = e.clientX - rect.left;
    const tick = xToTick(cx);
    const dt   = tick - drag.startTick;
    const grid = rollSnapDiv > 0 ? state.ppqn * 4 / rollSnapDiv : 1;

    if (drag.mode === 'move') {
      const newStart = Math.max(0, snapTick(drag.origStart + dt));
      drag.note.tick_start = newStart;
    } else {
      const newLen = Math.max(grid, snapTick(drag.origLen + dt));
      drag.note.tick_len = newLen;
    }
    renderPianoRoll();
  });

  canvas.addEventListener('pointerup', e => {
    if (pressTimer) { clearTimeout(pressTimer); pressTimer = null; }
    if (drag) {
      // Send updated note: DEL old + ADD new
      const n = drag.note;
      // Send DEL for original position, then ADD for new
      const delBuf = new ArrayBuffer(7);
      const delDv  = new DataView(delBuf);
      delDv.setUint8(0, CMD.DEL_NOTE);
      delDv.setUint8(1, selected_lane);
      delDv.setUint32(2, drag.origStart, true);
      delDv.setUint8(6, n.note);
      wsSend(delBuf);
      const addBuf = new ArrayBuffer(12);
      const addDv  = new DataView(addBuf);
      addDv.setUint8(0, CMD.ADD_NOTE);
      addDv.setUint8(1, selected_lane);
      addDv.setUint32(2, n.tick_start, true);
      addDv.setUint32(6, n.tick_len, true);
      addDv.setUint8(10, n.note);
      addDv.setUint8(11, n.velocity);
      wsSend(addBuf);
      drag = null;
    }
    if (auditNote >= 0 && selected_lane >= 0) {
      wsSend(new Uint8Array([CMD.NOTE_OFF, selected_lane, auditNote]).buffer);
      auditNote = -1;
    }
  });

  canvas.addEventListener('pointerleave', e => {
    if (drag) {
      // cancel drag — restore original positions
      drag.note.tick_start = drag.origStart;
      drag.note.tick_len   = drag.origLen;
      drag = null;
      renderPianoRoll();
    }
    if (auditNote >= 0 && selected_lane >= 0) {
      wsSend(new Uint8Array([CMD.NOTE_OFF, selected_lane, auditNote]).buffer);
      auditNote = -1;
    }
  });
})();

function deleteRollNote(n) {
  if (selected_lane < 0) return;
  const notes = state.notes[selected_lane];
  if (!notes) return;
  const idx = notes.indexOf(n);
  if (idx >= 0) notes.splice(idx, 1);
  renderPianoRoll();
  // Send DEL_NOTE: [cmd, lane, u32 tick_start, note]
  const buf = new ArrayBuffer(7);
  const dv  = new DataView(buf);
  dv.setUint8(0, CMD.DEL_NOTE);
  dv.setUint8(1, selected_lane);
  dv.setUint32(2, n.tick_start, true);
  dv.setUint8(6, n.note);
  wsSend(buf);
}

// ─────────────────────────────────────────────────────────────────────────────
// Transport + clock commands
// ─────────────────────────────────────────────────────────────────────────────
function sendTransport() {
  const action = state.running ? 0 : 1;
  wsSend(new Uint8Array([CMD.TRANSPORT, action]).buffer);
}

function sendTap() {
  wsSend(new Uint8Array([CMD.TRANSPORT, 2]).buffer);
}

function sendSetClock() {
  const bpm   = parseFloat(document.getElementById('bpm-input').value) || 120;
  const ppqn  = state.ppqn || 96;
  const beats = state.beats || 4;
  const buf   = new ArrayBuffer(1 + 12);
  const dv    = new DataView(buf);
  dv.setUint8(0, CMD.SET_CLOCK);
  dv.setFloat32(1, bpm, true);
  dv.setUint32(5, ppqn, true);
  dv.setUint32(9, beats, true);
  wsSend(buf);
}

function setPpqn(v) {
  state.ppqn = v;
  highlightPpqn(v);
  sendSetClock();
}

function highlightPpqn(v) {
  document.querySelectorAll('#ppqn-seg .btn').forEach(b => {
    b.classList.toggle('lime', parseInt(b.dataset.v) === v);
  });
}

function sendToggleMute(li) {
  wsSend(new Uint8Array([CMD.TOGGLE_MUTE, li]).buffer);
}

function sendToggleSolo(li) {
  wsSend(new Uint8Array([CMD.TOGGLE_SOLO, li]).buffer);
}

function sendLaneVolume(li, vol) {
  // SET_LANE: [cmd, lane, mask=0x02, f32 volume]
  const buf = new ArrayBuffer(7);
  const dv  = new DataView(buf);
  dv.setUint8(0, CMD.SET_LANE);
  dv.setUint8(1, li);
  dv.setUint8(2, 0x02);
  dv.setFloat32(3, vol, true);
  wsSend(buf);
}

function sendMasterVolume(vol) {
  const buf = new ArrayBuffer(9);
  const dv  = new DataView(buf);
  dv.setUint8(0, CMD.SET_MASTER);
  dv.setFloat32(1, vol, true);
  dv.setFloat32(5, state.master_pan, true);
  wsSend(buf);
}

function sendMasterPan(pan) {
  const buf = new ArrayBuffer(9);
  const dv  = new DataView(buf);
  dv.setUint8(0, CMD.SET_MASTER);
  dv.setFloat32(1, state.master_vol, true);
  dv.setFloat32(5, pan, true);
  wsSend(buf);
}

function onMasterVolRange(v) {
  const vol = v / 100;
  state.master_vol = vol;
  document.getElementById('master-vol-val').textContent = v + '%';
  document.getElementById('master-fill').style.width = v + '%';
  sendMasterVolume(vol);
}

function onMasterPanRange(v) {
  const pan = v / 100;
  state.master_pan = pan;
  document.getElementById('master-pan-val').textContent = v;
  sendMasterPan(pan);
}

function setTimeSig(beats) {
  state.beats = beats;
  highlightTimeSig(beats);
  sendSetClock();
}

function highlightTimeSig(beats) {
  document.querySelectorAll('#timesig-seg .btn').forEach(b => {
    b.classList.toggle('lime', parseInt(b.dataset.ts) === beats);
  });
}

// Master fader drag (in SONG view)
(function() {
  const fader = document.querySelector('.master-fader');
  if (!fader) return;
  const fill  = document.getElementById('master-fill');
  let dragX = null, dragVol = null;
  fader.addEventListener('pointerdown', e => {
    fader.setPointerCapture(e.pointerId);
    dragX   = e.clientX;
    dragVol = state.master_vol;
  });
  fader.addEventListener('pointermove', e => {
    if (dragX === null) return;
    const dx  = (e.clientX - dragX) / fader.offsetWidth;
    const vol = Math.max(0, Math.min(1, dragVol + dx));
    state.master_vol = vol;
    fill.style.width = Math.round(vol * 100) + '%';
  });
  fader.addEventListener('pointerup', () => {
    if (dragX === null) return;
    dragX = null;
    sendMasterVolume(state.master_vol);
  });
})();

// ─────────────────────────────────────────────────────────────────────────────
// FX chain editor
// ─────────────────────────────────────────────────────────────────────────────

// fx_type_t enum names matching fx.h (index = FX_TYPE_*)
const FX_TYPE_NAMES = [
  'NONE',
  'FILTER', 'EQ3', 'EQ5',
  'COMPRESSOR', 'LIMITER', 'GATE', 'TRANSIENT',
  'DISTORTION', 'OVERDRIVE', 'WAVEFOLDER', 'BITCRUSH',
  'DELAY', 'REVERB', 'CHORUS', 'FLANGER', 'PHASER',
  'TREMOLO', 'VIBRATO', 'RING MOD', 'PITCH SHIFT',
  'AUTO PAN', 'STEREO WIDTH',
  'NOISE GATE SC', 'ENV FOLLOWER', 'DE-ESSER', 'STEREO IMAGER',
  'TAPE SAT', 'TUBE AMP', 'EXCITER', 'HARMONIC ENH',
  'FORMANT FILT', 'COMB FILTER', 'TILT EQ', 'PITCH QUANT',
  'GRAN FREEZE', 'STUTTER', 'TAPE STOP', 'HAAS',
  'RESONATOR', 'FREEZE REVERB', 'STEP FILTER',
  'SIDECHAIN COMP', 'TRANCE GATE', 'ARP DELAY',
];

// Enumerated (discrete) params rendered as a dropdown instead of a slider.
// Key is "type:paramIndex"; value is the list of option labels (value = index).
const FX_PARAM_ENUMS = {
  '1:2': ['Lowpass', 'Highpass', 'Bandpass', 'Notch'],   // FILTER mode
};

// Per-type param descriptors: [{label, min, max, step, decimals}]
// Only first 8 params serialised; unlisted params show generic sliders.
const FX_PARAMS = {
  1:  [['CUTOFF Hz',0,20000,1,0],['RESONANCE',0,4,0.01,2],['MODE',0,3,1,0]],
  2:  [['LOW dB',-18,18,0.1,1],['MID dB',-18,18,0.1,1],['HI dB',-18,18,0.1,1],['MID Hz',200,8000,1,0],['MID Q',0.1,8,0.1,1]],
  3:  [['BAND',0,4,1,0],['GAIN dB',-18,18,0.1,1],['FREQ Hz',20,20000,1,0],['Q',0.1,8,0.1,1],['TYPE pk/ls/hs',0,2,1,0],['EN',0,1,1,0]],
  4:  [['THRESH dB',-60,0,0.5,1],['RATIO',1,20,0.1,1],['ATK ms',1,300,1,0],['REL ms',10,2000,1,0],['MAKEUP dB',0,24,0.5,1]],
  5:  [['THRESH dB',-30,0,0.5,1],['LOOKAHEAD ms',0,10,0.1,1]],
  6:  [['THRESH dB',-80,0,0.5,1],['HOLD ms',1,500,1,0],['ATK ms',1,100,1,0],['REL ms',10,1000,1,0]],
  7:  [['ATTACK GAIN',0,4,0.01,2],['SUSTAIN GAIN',0,4,0.01,2]],
  8:  [['DRIVE',0,16,0.05,2],['TONE',0,1,0.01,2],['MODE soft/hard/tube/fuzz',0,3,1,0]],
  9:  [['DRIVE',0,16,0.05,2],['TONE',0,1,0.01,2]],
  10: [['FOLD',0,1,0.01,2],['GAIN',0,4,0.01,2]],
  11: [['BITS',1,16,1,0],['SR DIV',1,32,1,0]],
  12: [['TIME ms',1,2000,1,0],['FEEDBACK',0,0.99,0.01,2],['MIX',0,1,0.01,2],['PING PONG',0,1,1,0],['CLOCK SYNC',0,1,1,0]],
  13: [['ROOM',0,1,0.01,2],['DAMP',0,1,0.01,2],['WIDTH',0,1,0.01,2],['MIX',0,1,0.01,2]],
  14: [['RATE Hz',0.1,10,0.1,1],['DEPTH ms',0.1,20,0.1,1],['MIX',0,1,0.01,2]],
  15: [['RATE Hz',0.1,10,0.1,1],['DEPTH ms',0.1,10,0.1,1],['FEEDBACK',0,0.99,0.01,2],['MIX',0,1,0.01,2]],
  16: [['RATE Hz',0.1,10,0.1,1],['DEPTH',0,1,0.01,2],['FEEDBACK',0,0.99,0.01,2],['MIX',0,1,0.01,2]],
  17: [['RATE Hz',0.1,20,0.1,1],['DEPTH',0,1,0.01,2]],
  18: [['RATE Hz',0.1,20,0.1,1],['DEPTH ms',0.1,10,0.1,1]],
  19: [['CARRIER Hz',20,4000,1,0],['MIX',0,1,0.01,2]],
  20: [['SEMITONES',-24,24,1,0]],
  21: [['RATE Hz',0.1,10,0.1,1],['DEPTH',0,1,0.01,2]],
  22: [['WIDTH',0,2,0.01,2]],
  23: [['THRESH dB',-80,0,0.5,1],['HOLD ms',1,500,1,0],['ATK ms',1,100,1,0],['REL ms',10,1000,1,0],['SRC LANE',0,15,1,0]],
  24: [['ATK ms',1,500,1,0],['REL ms',10,2000,1,0]],
  25: [['FREQ Hz',2000,12000,10,0],['THRESH dB',-40,0,0.5,1],['RATIO',1,20,0.1,1]],
  26: [['CROSSOVER Hz',100,2000,1,0],['WIDTH',0,2,0.01,2]],
  27: [['DRIVE',0,4,0.01,2],['BIAS',-1,1,0.01,2],['MIX',0,1,0.01,2]],
  28: [['DRIVE',0,4,0.01,2],['BIAS',-1,1,0.01,2],['TONE',0,1,0.01,2]],
  29: [['FREQ Hz',2000,12000,10,0],['DRIVE',0,4,0.01,2],['MIX',0,1,0.01,2]],
  30: [['H2 LEVEL',0,1,0.01,2],['H3 LEVEL',0,1,0.01,2]],
  31: [['VOWEL A/E/I/O/U',0,4,1,0],['MORPH',0,1,0.01,2],['MIX',0,1,0.01,2]],
  32: [['FREQ Hz',20,2000,1,0],['FEEDBACK',0,0.99,0.01,2],['MIX',0,1,0.01,2],['LFO RATE',0,10,0.1,1]],
  33: [['TILT dB',-12,12,0.1,1]],
  34: [['ROOT 0-11',0,11,1,0],['SCALE 0-7',0,7,1,0]],
  35: [['FREEZE',0,1,1,0],['RATE',0.25,4,0.01,2],['SCATTER',0,1,0.01,2],['MIX',0,1,0.01,2]],
  36: [['LENGTH ms',10,500,1,0],['TRIGGER',0,1,1,0],['CLOCK SYNC',0,1,1,0],['SYNC DIV',1,16,1,0]],
  37: [['TRIGGER',0,1,1,0],['TIME ms',50,4000,10,0]],
  38: [['DELAY ms',1,35,0.1,1],['SIDE L/R',0,1,1,0]],
  39: [['ROOT Hz',40,2000,1,0],['DECAY',0,1,0.01,2],['COUNT',2,8,1,0],['MIX',0,1,0.01,2]],
  40: [['ROOM',0,1,0.01,2],['DAMP',0,1,0.01,2],['MIX',0,1,0.01,2],['FREEZE',0,1,1,0]],
  41: [['STEP COUNT',4,16,1,0],['CLOCK SYNC',0,1,1,0],['STEP 1',0,1,0.01,2],['STEP 2',0,1,0.01,2],['STEP 3',0,1,0.01,2],['STEP 4',0,1,0.01,2]],
  42: [['THRESH dB',-60,0,0.5,1],['RATIO',1,20,0.1,1],['ATK ms',1,300,1,0],['REL ms',10,2000,1,0],['MAKEUP dB',0,24,0.5,1],['SRC LANE',0,15,1,0]],
  43: [['PATTERN',0,65535,1,0],['STEP DIV',1,16,1,0],['ATK ms',1,100,1,0],['REL ms',1,200,1,0]],
  44: [['TIME ms',50,2000,1,0],['FEEDBACK',0,0.99,0.01,2],['MIX',0,1,0.01,2],['SEMI STEP',-12,12,1,0],['MAX REPEATS',1,16,1,0]],
};

let fxLaneIdx = -1;    // lane currently shown in FX editor
let fxExpanded = -1;   // slot whose params are expanded (-1 = none)
let fxPickSlot = -1;   // pending insert slot for picker
let fxPickReplace = false; // true = picker swaps the slot's type instead of inserting

function decodeFxChain(dv, len) {
  if (len < 3) return;
  const li = dv.getUint8(1);
  if (li !== 255 && li !== 254 && li >= NUM_LANES) return;
  const count = dv.getUint8(2);
  const FX_SLOT_BYTES = 1 + 1 + 1 + 32;
  const slots = [];
  let off = 3;
  for (let s = 0; s < count; s++) {
    if (off + FX_SLOT_BYTES > len) break;
    const slot    = dv.getUint8(off);
    const type    = dv.getUint8(off + 1);
    const enabled = dv.getUint8(off + 2) !== 0;
    const params  = [];
    for (let i = 0; i < 8; i++) params.push(dv.getFloat32(off + 3 + i * 4, true));
    slots.push({ slot, type, enabled, params });
    off += FX_SLOT_BYTES;
  }
  state.fx[li] = slots;
  if (li === 255) {
    const el = document.getElementById('master-fx-count');
    if (el) el.textContent = slots.length + ' effect' + (slots.length === 1 ? '' : 's');
  }
  if (fxLaneIdx === li) renderFxChain();
}

function openFxEditor(li) {
  fxLaneIdx = li;
  fxExpanded = -1;
  document.getElementById('fx-lane-name').textContent =
    li === 255 ? 'MASTER' : li === 254 ? 'SEND' : laneName(li);
  nav('fx', document.querySelectorAll('.nav-tab')[4]);
  renderFxChain();
}

function renderFxChain() {
  const list   = document.getElementById('fx-chain-list');
  const empty  = document.getElementById('fx-empty');
  const slots  = (fxLaneIdx >= 0 && state.fx[fxLaneIdx]) ? state.fx[fxLaneIdx] : [];
  list.innerHTML = '';
  empty.style.display = slots.length ? 'none' : '';

  slots.forEach((fx, i) => {
    const name = FX_TYPE_NAMES[fx.type] || ('FX ' + fx.type);

    // Slot header row
    const row = document.createElement('div');
    row.className = 'fx-slot-row';
    row.innerHTML =
      `<span class="fx-slot-idx">${fx.slot + 1}</span>` +
      `<span class="fx-slot-name" style="cursor:pointer" onclick="fxToggleExpand(${fx.slot})">${name}</span>` +
      `<button class="fx-en-btn" onclick="fxChangeSlot(${fx.slot})">CHANGE</button>` +
      `<button class="fx-en-btn${fx.enabled ? ' on' : ''}" onclick="fxSetEnabled(${fx.slot},${!fx.enabled})">${fx.enabled ? 'ON' : 'OFF'}</button>` +
      `<button class="fx-rm-btn" onclick="fxRemove(${fx.slot})">DELETE</button>`;
    list.appendChild(row);

    // Param panel (expanded only)
    if (fx.slot === fxExpanded) {
      const panel = document.createElement('div');
      panel.className = 'fx-params';
      const defs = FX_PARAMS[fx.type] || [];
      const nParams = Math.max(defs.length, fx.params.filter(v => v !== 0).length > 0 ? defs.length || 4 : defs.length);
      const showN = defs.length || 0;
      if (showN === 0) {
        panel.innerHTML = '<span style="font-family:var(--f-mono);font-size:11px;color:var(--t3)">No parameters</span>';
      } else {
        defs.forEach(([label, mn, mx, step, dec], pi) => {
          const val = fx.params[pi] !== undefined ? fx.params[pi] : 0;
          const pr = document.createElement('div');
          pr.className = 'fx-param-row';
          const enumLabels = FX_PARAM_ENUMS[fx.type + ':' + pi];
          if (enumLabels) {
            const sel = Math.round(val);
            const opts = enumLabels.map((t, vi) =>
              `<option value="${vi}"${vi === sel ? ' selected' : ''}>${t}</option>`).join('');
            pr.innerHTML =
              `<span class="fx-param-label">${label}</span>` +
              `<select class="fx-param-sel" onchange="fxParamSelect(${fx.slot},${pi},this.value)">${opts}</select>`;
          } else {
            pr.innerHTML =
              `<span class="fx-param-label">${label}</span>` +
              `<input type="range" min="${mn}" max="${mx}" step="${step}" value="${val}"` +
              ` oninput="fxParamInput(${fx.slot},${pi},this.value,${dec},this)">` +
              `<span class="fx-param-val" id="fxpv-${fx.slot}-${pi}">${(+val).toFixed(dec)}</span>`;
          }
          panel.appendChild(pr);
        });
      }
      list.appendChild(panel);
    }
  });
}

function fxToggleExpand(slot) {
  fxExpanded = (fxExpanded === slot) ? -1 : slot;
  renderFxChain();
}

function fxSetEnabled(slot, en) {
  if (fxLaneIdx < 0) return;
  const chain = state.fx[fxLaneIdx] || [];
  const fx = chain.find(f => f.slot === slot);
  if (!fx) return;
  fx.enabled = en;
  renderFxChain();
  // sub_cmd=0: [cmd, lane, sub=0, slot, enabled, params×8]
  const params = fx.params.slice(0, 8);
  const buf = new ArrayBuffer(5 + 32);
  const dv  = new DataView(buf);
  dv.setUint8(0, CMD.SET_FX);
  dv.setUint8(1, fxLaneIdx);
  dv.setUint8(2, 0);
  dv.setUint8(3, slot);
  dv.setUint8(4, en ? 1 : 0);
  for (let i = 0; i < 8; i++) dv.setFloat32(5 + i * 4, params[i] || 0, true);
  wsSend(buf);
}

function fxParamInput(slot, paramIdx, rawVal, dec, inputEl) {
  const val = parseFloat(rawVal);
  const label = document.getElementById('fxpv-' + slot + '-' + paramIdx);
  if (label) label.textContent = val.toFixed(dec);
  // Update local state
  const chain = state.fx[fxLaneIdx] || [];
  const fx = chain.find(f => f.slot === slot);
  if (fx) fx.params[paramIdx] = val;
  fxSendParams(slot);
}

// Discrete/enumerated param change (dropdown) — e.g. filter mode.
function fxParamSelect(slot, paramIdx, rawVal) {
  const val = parseFloat(rawVal);
  const chain = state.fx[fxLaneIdx] || [];
  const fx = chain.find(f => f.slot === slot);
  if (fx) fx.params[paramIdx] = val;
  fxSendParams(slot);
}

function fxSendParams(slot) {
  if (fxLaneIdx < 0) return;
  const chain = state.fx[fxLaneIdx] || [];
  const fx = chain.find(f => f.slot === slot);
  if (!fx) return;
  const buf = new ArrayBuffer(5 + 32);
  const dv  = new DataView(buf);
  dv.setUint8(0, CMD.SET_FX);
  dv.setUint8(1, fxLaneIdx);
  dv.setUint8(2, 0);
  dv.setUint8(3, slot);
  dv.setUint8(4, fx.enabled ? 1 : 0);
  for (let i = 0; i < 8; i++) dv.setFloat32(5 + i * 4, fx.params[i] || 0, true);
  wsSend(buf);
}

function fxRemove(slot) {
  if (fxLaneIdx < 0) return;
  if (fxExpanded === slot) fxExpanded = -1;
  // Optimistic: remove locally
  const chain = state.fx[fxLaneIdx] || [];
  const idx = chain.findIndex(f => f.slot === slot);
  if (idx >= 0) chain.splice(idx, 1);
  // Re-number slots
  chain.forEach((f, i) => { f.slot = i; });
  renderFxChain();
  wsSend(new Uint8Array([CMD.SET_FX, fxLaneIdx, 2, slot]).buffer); // sub=remove
}

function fxAddSlot() {
  if (fxLaneIdx < 0) return;
  const chain = state.fx[fxLaneIdx] || [];
  fxPickSlot = chain.length; // append
  fxPickReplace = false;
  openFxPicker();
}

// Re-open the picker to swap the effect type at an existing slot
function fxChangeSlot(slot) {
  if (fxLaneIdx < 0) return;
  fxPickSlot = slot;
  fxPickReplace = true;
  openFxPicker();
}

// FX type picker bottom sheet
function openFxPicker() {
  const title = document.querySelector('#fx-picker-inner .sh');
  if (title) title.textContent = fxPickReplace ? 'CHANGE EFFECT' : 'CHOOSE EFFECT';
  const list = document.getElementById('fx-pick-list');
  list.innerHTML = '';
  FX_TYPE_NAMES
    .map((name, typeId) => ({ name, typeId }))
    .filter(o => o.typeId !== 0)            // skip NONE
    .sort((a, b) => a.name.localeCompare(b.name))
    .forEach(({ name, typeId }) => {
      const div = document.createElement('div');
      div.className = 'fx-pick-item';
      div.textContent = name;
      div.onclick = () => {
        closeFxPicker();
        if (fxPickReplace) fxReplace(fxPickSlot, typeId);
        else               fxInsert(fxPickSlot, typeId);
      };
      list.appendChild(div);
    });
  document.getElementById('fx-picker').classList.add('open');
}

function closeFxPicker() {
  document.getElementById('fx-picker').classList.remove('open');
}

document.getElementById('fx-picker').addEventListener('pointerdown', e => {
  if (e.target === document.getElementById('fx-picker')) closeFxPicker();
});

function fxInsert(slot, typeId) {
  if (fxLaneIdx < 0) return;
  // Optimistic: add placeholder locally (params zeroed, firmware will echo)
  if (!state.fx[fxLaneIdx]) state.fx[fxLaneIdx] = [];
  const chain = state.fx[fxLaneIdx];
  chain.splice(slot, 0, { slot, type: typeId, enabled: true, params: new Array(8).fill(0) });
  chain.forEach((f, i) => { f.slot = i; });
  renderFxChain();
  wsSend(new Uint8Array([CMD.SET_FX, fxLaneIdx, 1, slot, typeId]).buffer); // sub=insert
}

function fxReplace(slot, typeId) {
  if (fxLaneIdx < 0) return;
  const chain = state.fx[fxLaneIdx];
  if (!chain || slot < 0 || slot >= chain.length) return;
  // Optimistic: swap the node type in place, reset params
  chain[slot] = { slot, type: typeId, enabled: true, params: new Array(8).fill(0) };
  if (fxExpanded === slot) fxExpanded = -1;
  renderFxChain();
  // Remove old node then insert the new type at the same position
  wsSend(new Uint8Array([CMD.SET_FX, fxLaneIdx, 2, slot]).buffer);          // sub=remove
  wsSend(new Uint8Array([CMD.SET_FX, fxLaneIdx, 1, slot, typeId]).buffer);  // sub=insert
}

// ─────────────────────────────────────────────────────────────────────────────
// Synth params editor
// ─────────────────────────────────────────────────────────────────────────────
const SYNTH_TYPE_NAMES = [
  'Mono Wavetable', 'Poly Wavetable', 'Super Saw', 'FM 2-op', 'FM 4-op',
  'Subtractive', 'Plucked String', 'Bell', 'Pad Additive', 'Noise',
  'Bass', 'Lead', 'Chord', 'Drum BD', 'Drum SD', 'Drum HH',
  'Organ', 'Wavetable Morph', 'Vowel Formant', 'Bit-Crush',
];

let synthLaneIdx = -1;

function openSynthEditor(li) {
  synthLaneIdx = li;
  document.getElementById('synth-lane-name').textContent = laneName(li);
  // Navigate without highlighting a tab (sub-screen accessed from piano roll toolbar)
  document.querySelectorAll('section').forEach(s => s.classList.remove('active'));
  document.getElementById('s-synth').classList.add('active');
  document.querySelectorAll('.nav-tab').forEach(b => b.classList.remove('active'));
  wsSend(new Uint8Array([CMD.SET_SYNTH, li, (state.synthParams?.[li]?.typeId ?? 0)]).buffer);
  renderSynthParams();
}

function renderSynthParams() {
  const list  = document.getElementById('synth-param-list');
  const title = document.getElementById('synth-type-name');
  if (!list) return;
  const sp = state.synthParams?.[synthLaneIdx];
  const typeId = sp?.typeId ?? 0;
  if (title) title.textContent = SYNTH_TYPE_NAMES[typeId] || ('Type ' + typeId);

  list.innerHTML = '';
  // Type picker
  const row0 = document.createElement('div');
  row0.className = 'setting-row';
  row0.innerHTML = '<span class="setting-label">TYPE</span><div class="setting-ctrl">' +
    '<select onchange="sendSynthType(' + synthLaneIdx + ',+this.value)">' +
    SYNTH_TYPE_NAMES.map((n, i) => `<option value="${i}"${i===typeId?' selected':''}>${n}</option>`).join('') +
    '</select></div>';
  list.appendChild(row0);

  // FX link
  const rowFx = document.createElement('div');
  rowFx.className = 'setting-row';
  rowFx.innerHTML = `<span class="setting-label">FX</span><div class="setting-ctrl"><button class="btn" onclick="openFxEditor(${synthLaneIdx})">EDIT FX CHAIN</button></div>`;
  list.appendChild(rowFx);

  // Arpeggiator section
  const arpDiv = document.createElement('div');
  arpDiv.id = 'arp-section-' + synthLaneIdx;
  list.appendChild(arpDiv);
  renderArpSection(synthLaneIdx);
}

function sendSynthType(li, typeId) {
  wsSend(new Uint8Array([CMD.SET_SYNTH, li, typeId]).buffer);
  state.synthParams = state.synthParams || {};
  state.synthParams[li] = { typeId, params: [] };
  renderSynthParams();
}

const ARP_MODE_NAMES = ['Up','Down','Up-Down','Up-Down Incl','Down-Up','Order','Random','Chord','As Played'];
const ARP_VEL_NAMES  = ['Original','Accent','Fixed'];
const ARP_DIV_VALS   = [4, 8, 16, 32];

function getArp(li) {
  return state.arp[li] || {
    enabled: false, mode: 0, octave: 1, div: 16,
    gate: 80, velMode: 0, fixedVel: 100,
    latch: false, retrig: false, swing: 50
  };
}

function sendArp(li) {
  const a = getArp(li);
  const buf = new Uint8Array([
    CMD.SET_ARP, li,
    a.enabled ? 1 : 0,
    a.mode & 0xFF,
    a.octave & 0xFF,
    a.div & 0xFF,
    a.gate & 0xFF,
    a.velMode & 0xFF,
    a.fixedVel & 0xFF,
    a.latch ? 1 : 0,
    a.retrig ? 1 : 0,
    a.swing & 0xFF,
  ]);
  wsSend(buf.buffer);
}

function setArpField(li, field, val) {
  if (!state.arp[li]) state.arp[li] = getArp(li);
  state.arp[li][field] = val;
  sendArp(li);
  renderArpSection(li);
}

function renderArpSection(li) {
  const cont = document.getElementById('arp-section-' + li);
  if (!cont) return;
  const a = getArp(li);
  cont.innerHTML = `
    <div class="setting-row">
      <span class="setting-label">ARP</span>
      <div class="setting-ctrl">
        <button class="btn${a.enabled ? ' lime' : ''}" onclick="setArpField(${li},'enabled',${!a.enabled})">${a.enabled ? 'ON' : 'OFF'}</button>
      </div>
    </div>` + (a.enabled ? `
    <div class="setting-row">
      <span class="setting-label">MODE</span>
      <div class="setting-ctrl"><select onchange="setArpField(${li},'mode',+this.value)">
        ${ARP_MODE_NAMES.map((n,i) => `<option value="${i}"${i===a.mode?' selected':''}>${n}</option>`).join('')}
      </select></div>
    </div>
    <div class="setting-row">
      <span class="setting-label">OCTAVE</span>
      <div class="setting-ctrl">
        ${[1,2,3,4].map(v => `<button class="btn${v===a.octave?' active':''}" onclick="setArpField(${li},'octave',${v})">${v}</button>`).join('')}
      </div>
    </div>
    <div class="setting-row">
      <span class="setting-label">DIV</span>
      <div class="setting-ctrl">
        ${ARP_DIV_VALS.map(v => `<button class="btn${v===a.div?' active':''}" onclick="setArpField(${li},'div',${v})">1/${v}</button>`).join('')}
      </div>
    </div>
    <div class="setting-row">
      <span class="setting-label">GATE</span>
      <div class="setting-ctrl">
        <input type="range" min="10" max="100" value="${a.gate}" oninput="setArpField(${li},'gate',+this.value)" style="width:120px">
        <span style="font-family:var(--f-mono);font-size:12px;color:var(--t1)">${a.gate}%</span>
      </div>
    </div>
    <div class="setting-row">
      <span class="setting-label">SWING</span>
      <div class="setting-ctrl">
        <input type="range" min="50" max="75" value="${a.swing}" oninput="setArpField(${li},'swing',+this.value)" style="width:120px">
        <span style="font-family:var(--f-mono);font-size:12px;color:var(--t1)">${a.swing}</span>
      </div>
    </div>
    <div class="setting-row">
      <span class="setting-label">VEL</span>
      <div class="setting-ctrl"><select onchange="setArpField(${li},'velMode',+this.value)">
        ${ARP_VEL_NAMES.map((n,i) => `<option value="${i}"${i===a.velMode?' selected':''}>${n}</option>`).join('')}
      </select></div>
    </div>
    <div class="setting-row">
      <span class="setting-label">LATCH</span>
      <div class="setting-ctrl">
        <button class="btn${a.latch?' lime':''}" onclick="setArpField(${li},'latch',${!a.latch})">${a.latch?'ON':'OFF'}</button>
        <span class="setting-label" style="margin-left:12px">RETRIG</span>
        <button class="btn${a.retrig?' lime':''}" onclick="setArpField(${li},'retrig',${!a.retrig})">${a.retrig?'ON':'OFF'}</button>
      </div>
    </div>` : '');
}

// ─────────────────────────────────────────────────────────────────────────────
// Lane context menu (long-press / right-click)
// ─────────────────────────────────────────────────────────────────────────────
let ctxLaneIdx = -1;
const ctxMenu = document.getElementById('ctx-menu');

function openCtxMenu(e, li) {
  ctxLaneIdx = li;
  const x = Math.min(e.clientX, window.innerWidth  - 200);
  const y = Math.min(e.clientY, window.innerHeight - 140);
  ctxMenu.style.left = x + 'px';
  ctxMenu.style.top  = y + 'px';
  ctxMenu.classList.add('open');
  e.stopPropagation();
}

document.addEventListener('pointerdown', e => {
  if (!ctxMenu.contains(e.target)) ctxMenu.classList.remove('open');
});

function ctxRename() {
  ctxMenu.classList.remove('open');
  if (ctxLaneIdx < 0) return;
  const name = prompt('Lane name:', laneName(ctxLaneIdx));
  if (name === null) return;
  const enc = new TextEncoder().encode(name.slice(0, 30));
  const buf = new Uint8Array(2 + enc.length);
  buf[0] = CMD.RENAME_LANE;
  buf[1] = ctxLaneIdx;
  buf.set(enc, 2);
  wsSend(buf.buffer);
}

function ctxDuplicate() {
  ctxMenu.classList.remove('open');
  if (ctxLaneIdx < 0) return;
  wsSend(new Uint8Array([CMD.DUPLICATE_LANE, ctxLaneIdx]).buffer);
}

function ctxDelete() {
  ctxMenu.classList.remove('open');
  if (ctxLaneIdx < 0) return;
  if (!confirm(`Delete lane ${ctxLaneIdx + 1}?`)) return;
  wsSend(new Uint8Array([CMD.DEL_LANE, ctxLaneIdx]).buffer);
}

// ─────────────────────────────────────────────────────────────────────────────
// ADSR
// ─────────────────────────────────────────────────────────────────────────────
function renderAdsrCurve() {
  const path = document.getElementById('adsr-curve');
  if (!path) return;
  const a = Math.min(2000, Math.max(1, parseFloat(document.getElementById('adr-a').value) || 10));
  const d = Math.min(2000, Math.max(1, parseFloat(document.getElementById('adr-d').value) || 100));
  const s = Math.min(100, Math.max(0, parseFloat(document.getElementById('adr-s').value) || 70)) / 100;
  const r = Math.min(4000, Math.max(1, parseFloat(document.getElementById('adr-r').value) || 200));
  // Map ms → x-pixels across 200px SVG viewbox, 0=bottom, 60=top
  const total = a + d + 400 + r;  // sustain region fixed at 400ms equivalent
  const W = 200; const H = 60;
  const tx = t => t / total * W;
  const xa = tx(a);
  const xd = xa + tx(d);
  const xs = xd + tx(400);
  const xr = xs + tx(r);
  const sy = H * (1 - s);
  path.setAttribute('d',
    `M0,${H} L${xa.toFixed(1)},0 L${xd.toFixed(1)},${sy.toFixed(1)} L${xs.toFixed(1)},${sy.toFixed(1)} L${xr.toFixed(1)},${H}`
  );
}

function adsr_update(k, v) {
  const unit = k === 's' ? '%' : 'ms';
  document.getElementById('adv-' + k).textContent = v + unit;
  renderAdsrCurve();
  sendAdsr();
}

function sendAdsr() {
  if (selected_lane < 0) return;
  const a = parseFloat(document.getElementById('adr-a').value);
  const d = parseFloat(document.getElementById('adr-d').value);
  const s = parseFloat(document.getElementById('adr-s').value) / 100;
  const r = parseFloat(document.getElementById('adr-r').value);
  const buf = new ArrayBuffer(1 + 17);
  const dv  = new DataView(buf);
  dv.setUint8(0, CMD.SET_ADSR);
  dv.setUint8(1, selected_lane);
  dv.setFloat32(2,  a, true);
  dv.setFloat32(6,  d, true);
  dv.setFloat32(10, s, true);
  dv.setFloat32(14, r, true);
  wsSend(buf);
}

// ─────────────────────────────────────────────────────────────────────────────
// Audio stream
// ─────────────────────────────────────────────────────────────────────────────
let audioCtx   = null;
let audioOn    = false;
let nextTime   = 0;
const AUDIO_SR = 48000;
const BLOCK_FRAMES = 256;

function toggleAudio() {
  if (audioOn) {
    stopAudio();
  } else {
    startAudio();
  }
}

function startAudio() {
  if (!audioCtx) audioCtx = new (window.AudioContext || window.webkitAudioContext)({ sampleRate: AUDIO_SR });
  audioCtx.resume();
  audioOn = true;
  nextTime = audioCtx.currentTime + 0.1;
  document.getElementById('audio-btn').textContent = 'ON';
  document.getElementById('audio-btn').classList.add('lime');
  document.getElementById('audio-status').textContent = 'streaming';
  wsSend(new Uint8Array([CMD.AUDIO_STREAM, 1]).buffer);
}

function stopAudio() {
  audioOn = false;
  document.getElementById('audio-btn').textContent = 'OFF';
  document.getElementById('audio-btn').classList.remove('lime');
  document.getElementById('audio-status').textContent = 'off';
  wsSend(new Uint8Array([CMD.AUDIO_STREAM, 0]).buffer);
}

function handleAudioDropped() {
  audioOn = false;
  document.getElementById('audio-btn').textContent = 'OFF';
  document.getElementById('audio-btn').classList.remove('lime');
  document.getElementById('audio-status').textContent = 'dropped';
}

function handleAudioFrame(type, dv, len) {
  if (!audioOn || !audioCtx || type !== 0x10) return;
  // Frame: 1 byte type + 1024 bytes = 256 stereo int16 frames
  if (len < 1 + BLOCK_FRAMES * 4) return;
  const buf = audioCtx.createBuffer(2, BLOCK_FRAMES, AUDIO_SR);
  const L = buf.getChannelData(0);
  const R = buf.getChannelData(1);
  for (let f = 0; f < BLOCK_FRAMES; f++) {
    L[f] = dv.getInt16(1 + f * 4,     true) / 32768;
    R[f] = dv.getInt16(1 + f * 4 + 2, true) / 32768;
  }
  const src = audioCtx.createBufferSource();
  src.buffer = buf;
  src.connect(audioCtx.destination);
  const now = audioCtx.currentTime;
  if (nextTime < now + 0.01) nextTime = now + 0.05;
  src.start(nextTime);
  nextTime += BLOCK_FRAMES / AUDIO_SR;
}

// ─────────────────────────────────────────────────────────────────────────────
// Upload tab
// ─────────────────────────────────────────────────────────────────────────────
async function loadDirs() {
  try {
    const r = await fetch('/sd/ls?path=sounds');
    const j = await r.json();
    const sel = document.getElementById('dir-sel');
    sel.innerHTML = '<option value="sounds">sounds</option>';
    (j.dirs || []).forEach(d => {
      const o = document.createElement('option');
      o.value = 'sounds/' + d; o.textContent = d;
      sel.appendChild(o);
    });
  } catch(e) {}
}

function toggleNewFolder() {
  const ni = document.getElementById('dir-new');
  const sel = document.getElementById('dir-sel');
  const btn = document.getElementById('btn-new-folder');
  if (ni.style.display === 'none') {
    ni.style.display=''; sel.style.display='none'; btn.textContent='CANCEL';
  } else {
    ni.style.display='none'; sel.style.display=''; ni.value=''; btn.textContent='NEW';
  }
}

function getDir() {
  const ni = document.getElementById('dir-new');
  if (ni.style.display !== 'none' && ni.value.trim())
    return 'sounds/' + ni.value.trim().replace(/[^a-zA-Z0-9_\-. ]/g,'_');
  return document.getElementById('dir-sel').value;
}

let pickedFiles = [];
function onDrop(e) {
  e.preventDefault();
  document.getElementById('drop-zone').classList.remove('drag-over');
  onFilePick(e.dataTransfer.files);
}
function onFilePick(files) {
  pickedFiles = Array.from(files).filter(f => f.name.toLowerCase().endsWith('.wav'));
  renderFileList();
  document.getElementById('btn-upload').disabled = pickedFiles.length === 0;
}
function renderFileList() {
  const list = document.getElementById('file-list');
  list.innerHTML = '';
  pickedFiles.forEach((f, i) => {
    const row = document.createElement('div');
    row.className = 'file-row';
    row.innerHTML =
      `<span class="file-name">${esc(f.name)}</span>` +
      `<div class="prog-track"><div class="prog-fill" id="pf-${i}"></div></div>` +
      `<span class="file-status" id="ps-${i}"></span>`;
    list.appendChild(row);
  });
}
function esc(s) { return s.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;'); }

// GET /sd/stat?path=<sd-root-relative>. Returns {exists,size} or null on net error.
async function sdStat(relPath) {
  try {
    const r = await fetch('/sd/stat?path=' + encodeURIComponent(relPath));
    if (!r.ok) return null;
    return await r.json();
  } catch (e) { return null; }
}

async function startUpload() {
  if (!pickedFiles.length) return;
  document.getElementById('btn-upload').disabled = true;
  document.getElementById('upload-result').textContent = '';
  const dir = getDir();
  let saved = 0, skipped = 0, failed = 0;
  for (let i = 0; i < pickedFiles.length; i++) {
    const f = pickedFiles[i];
    const pf = document.getElementById('pf-'+i);
    const ps = document.getElementById('ps-'+i);
    ps.textContent = '…'; ps.className = 'file-status';

    // Skip if the SD already holds a file of the same name and size.
    // /upload writes under SDCARD_ROOT/<dir>, so the stat path is "<dir>/<name>".
    const relPath = (dir ? dir + '/' : '') + f.name;
    const stat = await sdStat(relPath);
    if (stat && stat.exists && stat.size === f.size) {
      pf.style.width = '100%';
      ps.textContent = 'skipped'; ps.className = 'file-status done';
      skipped++;
      continue;
    }

    const fd = new FormData(); fd.append('file', f, f.name);
    try {
      await new Promise((res, rej) => {
        const xhr = new XMLHttpRequest();
        xhr.open('POST', '/upload?path=' + encodeURIComponent(dir));
        xhr.upload.onprogress = e => { if (e.lengthComputable) pf.style.width = Math.round(e.loaded/e.total*100)+'%'; };
        xhr.onload = () => {
          if (xhr.status === 200) { pf.style.width='100%'; ps.textContent='done'; ps.className='file-status done'; saved++; res(); }
          else { ps.textContent='err '+xhr.status; ps.className='file-status err'; failed++; rej(); }
        };
        xhr.onerror = () => { ps.textContent='err'; ps.className='file-status err'; failed++; rej(); };
        xhr.send(fd);
      });
    } catch(e) {}
  }
  const parts = [saved+' saved'];
  if (skipped) parts.push(skipped+' skipped');
  if (failed)  parts.push(failed+' failed');
  document.getElementById('upload-result').textContent = parts.join(' | ');
  document.getElementById('btn-upload').disabled = false;
  loadDirs();
}

// ─────────────────────────────────────────────────────────────────────────────
// Sample cutter
// ─────────────────────────────────────────────────────────────────────────────
let cutAudioBuf = null;   // decoded AudioBuffer of loaded WAV
let cutPeaks    = null;   // Float32Array of peak per pixel column
let cutRegions  = [];     // [{id, startSec, endSec}]
let cutNextId   = 1;
let cutSelId    = -1;     // selected region id
let cutDrag     = null;   // {regionId, edge:'start'|'end'|'body', startX, origStart, origEnd}

// Populate source dir selector
async function loadCutDirs() {
  try {
    const r = await fetch('/sd/ls?path=sounds');
    const j = await r.json();
    const sel = document.getElementById('cut-dir-sel');
    sel.innerHTML = '<option value="sounds">sounds</option>';
    (j.dirs || []).forEach(d => {
      const o = document.createElement('option');
      o.value = 'sounds/' + d; o.textContent = d;
      sel.appendChild(o);
    });
  } catch(e) {}
  loadCutFiles();
}

// Populate source file selector for chosen dir
async function loadCutFiles() {
  const dir = document.getElementById('cut-dir-sel').value;
  const sel = document.getElementById('cut-file-sel');
  sel.innerHTML = '<option value="">— choose file —</option>';
  try {
    const r = await fetch('/sd/ls?path=' + encodeURIComponent(dir));
    const j = await r.json();
    (j.files || []).filter(f => /\.wav$/i.test(f)).forEach(f => {
      const o = document.createElement('option');
      o.value = dir + '/' + f; o.textContent = f;
      sel.appendChild(o);
    });
  } catch(e) {}
}

// Fetch WAV from /sd, decode, render waveform
async function loadWaveform() {
  const path = document.getElementById('cut-file-sel').value;
  if (!path) return;
  const msg = document.getElementById('wfm-msg');
  msg.textContent = 'Loading…'; msg.style.display = 'flex';
  try {
    const r = await fetch('/sd?path=' + encodeURIComponent(path));
    if (!r.ok) throw new Error(r.status);
    const ab = await r.arrayBuffer();
    if (!audioCtx) audioCtx = new (window.AudioContext || window.webkitAudioContext)({ sampleRate: AUDIO_SR });
    cutAudioBuf = await audioCtx.decodeAudioData(ab);
    cutRegions  = [];
    cutSelId    = -1;
    buildPeaks();
    renderWaveform();
    renderRegionList();
    msg.style.display = 'none';
    setCutStatus('Loaded: ' + path.split('/').pop() + '  ' + cutAudioBuf.duration.toFixed(2) + 's');
  } catch(e) {
    msg.textContent = 'Load failed: ' + e.message;
    setCutStatus('');
  }
}

// Build per-pixel peak array for waveform display
function buildPeaks() {
  const canvas = document.getElementById('wfm-canvas');
  const W = canvas.offsetWidth || window.innerWidth;
  const data  = cutAudioBuf.getChannelData(0);
  const total = data.length;
  cutPeaks = new Float32Array(W);
  for (let px = 0; px < W; px++) {
    const s0 = Math.floor(px / W * total);
    const s1 = Math.floor((px + 1) / W * total);
    let mx = 0;
    for (let s = s0; s < s1; s++) { const v = Math.abs(data[s]); if (v > mx) mx = v; }
    cutPeaks[px] = mx;
  }
}

// Render waveform + region overlays
function renderWaveform() {
  if (!cutPeaks) return;
  const canvas = document.getElementById('wfm-canvas');
  const W = canvas.width = canvas.offsetWidth || window.innerWidth;
  const H = canvas.height = 120;
  const ctx = canvas.getContext('2d');
  const dur = cutAudioBuf.duration;

  ctx.fillStyle = '#0E0E11';
  ctx.fillRect(0, 0, W, H);

  // Waveform peaks
  const cy = H / 2;
  ctx.fillStyle = '#3A3A45';
  for (let px = 0; px < W; px++) {
    const h = Math.max(1, cutPeaks[px] * cy * 0.95);
    ctx.fillRect(px, cy - h, 1, h * 2);
  }

  // Region overlays
  cutRegions.forEach(reg => {
    const x0 = Math.round(reg.startSec / dur * W);
    const x1 = Math.round(reg.endSec   / dur * W);
    const sel = reg.id === cutSelId;
    ctx.fillStyle = sel ? 'rgba(199,245,70,0.20)' : 'rgba(91,196,255,0.12)';
    ctx.fillRect(x0, 0, x1 - x0, H);
    // boundary lines
    ctx.fillStyle = sel ? '#C7F546' : '#5BC4FF';
    ctx.fillRect(x0, 0, 2, H);
    ctx.fillRect(x1 - 2, 0, 2, H);
    // label
    ctx.fillStyle = sel ? '#C7F546' : '#5BC4FF';
    ctx.font = '10px monospace';
    ctx.fillText('R' + reg.id, x0 + 4, 14);
  });
}

// Add region spanning 10–90% of file (or around selection)
function addRegion() {
  if (!cutAudioBuf) return;
  const dur = cutAudioBuf.duration;
  // Default: quarter-file window, stepping by 1/8 each time
  const step = dur / 8;
  const wid  = dur / 4;
  const off  = ((cutNextId - 1) % 6) * step;
  cutRegions.push({ id: cutNextId++, startSec: Math.min(off, dur - wid), endSec: Math.min(off + wid, dur) });
  cutSelId = cutRegions[cutRegions.length - 1].id;
  renderWaveform();
  renderRegionList();
}

function previewRegion() {
  const reg = cutRegions.find(r => r.id === cutSelId);
  if (!reg || !cutAudioBuf || !audioCtx) return;
  const sr     = cutAudioBuf.sampleRate;
  const startF = Math.round(reg.startSec * sr);
  const lenF   = Math.round((reg.endSec - reg.startSec) * sr);
  if (lenF <= 0) return;
  const slice  = audioCtx.createBuffer(cutAudioBuf.numberOfChannels, lenF, sr);
  for (let c = 0; c < cutAudioBuf.numberOfChannels; c++) {
    const src = cutAudioBuf.getChannelData(c).subarray(startF, startF + lenF);
    slice.getChannelData(c).set(src);
  }
  audioCtx.resume();
  const src = audioCtx.createBufferSource();
  src.buffer = slice;
  src.connect(audioCtx.destination);
  src.start();
}

function delRegion() {
  const idx = cutRegions.findIndex(r => r.id === cutSelId);
  if (idx < 0) return;
  cutRegions.splice(idx, 1);
  cutSelId = cutRegions.length ? cutRegions[Math.min(idx, cutRegions.length - 1)].id : -1;
  renderWaveform();
  renderRegionList();
}

function renderRegionList() {
  const list = document.getElementById('region-list');
  list.innerHTML = '';
  cutRegions.forEach(reg => {
    const row = document.createElement('div');
    row.className = 'region-row';
    row.style.padding = '0 8px 0 16px';
    const dur = reg.endSec - reg.startSec;
    row.innerHTML =
      `<span class="rname" onclick="cutSelId=${reg.id};renderWaveform();renderRegionList()">R${reg.id}</span>` +
      `<span class="rtime">${reg.startSec.toFixed(3)}s – ${reg.endSec.toFixed(3)}s (${dur.toFixed(3)}s)</span>` +
      `<span class="rdel" onclick="cutSelId=${reg.id};delRegion()">✕</span>`;
    if (reg.id === cutSelId) row.style.background = 'var(--bg-3)';
    list.appendChild(row);
  });
}

function setCutStatus(msg) {
  document.getElementById('cut-status').textContent = msg;
}

// Inline WAV encoder: Float32 channels → 16-bit stereo WAV ArrayBuffer
function encodeWav(audioBuf) {
  const numCh  = Math.min(audioBuf.numberOfChannels, 2);
  const numFr  = audioBuf.length;
  const sr     = audioBuf.sampleRate;
  const bps    = 16;
  const blockA = numCh * (bps >> 3);
  const byteR  = sr * blockA;
  const dataBy = numFr * blockA;
  const buf    = new ArrayBuffer(44 + dataBy);
  const dv     = new DataView(buf);
  const writeStr = (off, s) => { for (let i = 0; i < s.length; i++) dv.setUint8(off + i, s.charCodeAt(i)); };
  writeStr(0, 'RIFF');
  dv.setUint32(4,  36 + dataBy, true);
  writeStr(8, 'WAVE');
  writeStr(12, 'fmt ');
  dv.setUint32(16, 16, true);
  dv.setUint16(20, 1, true);   // PCM
  dv.setUint16(22, numCh, true);
  dv.setUint32(24, sr, true);
  dv.setUint32(28, byteR, true);
  dv.setUint16(32, blockA, true);
  dv.setUint16(34, bps, true);
  writeStr(36, 'data');
  dv.setUint32(40, dataBy, true);
  const chans = [];
  for (let c = 0; c < numCh; c++) chans.push(audioBuf.getChannelData(c));
  let off = 44;
  for (let f = 0; f < numFr; f++) {
    for (let c = 0; c < numCh; c++) {
      const s = Math.max(-1, Math.min(1, chans[c][f]));
      dv.setInt16(off, s < 0 ? s * 32768 : s * 32767, true);
      off += 2;
    }
  }
  return buf;
}

async function saveAllCuts() {
  if (!cutAudioBuf || !cutRegions.length) return;
  // outDir is relative to /sdcard/sounds/ (the upload endpoint's root)
  const outDir = document.getElementById('cut-out-dir').value.trim() || 'cuts';
  setCutStatus('Saving…');
  let saved = 0, skipped = 0, failed = 0;
  for (const reg of cutRegions) {
    const sr     = cutAudioBuf.sampleRate;
    const startF = Math.round(reg.startSec * sr);
    const lenF   = Math.round((reg.endSec - reg.startSec) * sr);
    if (lenF <= 0) continue;
    const numCh = Math.min(cutAudioBuf.numberOfChannels, 2);
    const slice  = audioCtx.createBuffer(numCh, lenF, sr);
    for (let c = 0; c < numCh; c++) {
      slice.getChannelData(c).set(cutAudioBuf.getChannelData(c).subarray(startF, startF + lenF));
    }
    const wavBuf  = encodeWav(slice);
    const srcName = (document.getElementById('cut-file-sel').value || 'cut').split('/').pop().replace(/\.wav$/i, '');
    const fname   = srcName + '_R' + reg.id + '.wav';
    const path    = outDir + '/' + fname;
    // /sounds/upload is rooted at /sdcard/sounds — prefix accordingly for the stat check.
    const stat = await sdStat('sounds/' + path);
    if (stat && stat.exists && stat.size === wavBuf.byteLength) {
      skipped++;
      continue;
    }
    try {
      const r = await fetch('/sounds/upload?path=' + encodeURIComponent(path), {
        method: 'PUT', body: wavBuf,
        headers: { 'Content-Type': 'audio/wav', 'Content-Length': wavBuf.byteLength },
      });
      if (r.ok) { saved++; } else { failed++; }
    } catch(e) { failed++; }
  }
  const parts = [saved + ' saved'];
  if (skipped) parts.push(skipped + ' skipped');
  if (failed)  parts.push(failed + ' failed');
  setCutStatus(parts.join(' | ') + ' → ' + outDir);
  loadDirs();  // refresh upload tab dirs too
}

// Waveform canvas interaction: click to select region, drag region edges/body
(function() {
  const canvas = document.getElementById('wfm-canvas');

  function secFromX(x) {
    if (!cutAudioBuf) return 0;
    return Math.max(0, Math.min(cutAudioBuf.duration, x / canvas.width * cutAudioBuf.duration));
  }

  function findHandle(x) {
    if (!cutAudioBuf) return null;
    const W = canvas.width;
    const dur = cutAudioBuf.duration;
    const EDGE = 8;
    for (let i = cutRegions.length - 1; i >= 0; i--) {
      const reg = cutRegions[i];
      const x0  = reg.startSec / dur * W;
      const x1  = reg.endSec   / dur * W;
      if (Math.abs(x - x0) <= EDGE) return { regionId: reg.id, edge: 'start' };
      if (Math.abs(x - x1) <= EDGE) return { regionId: reg.id, edge: 'end'   };
      if (x > x0 && x < x1)        return { regionId: reg.id, edge: 'body'  };
    }
    return null;
  }

  canvas.addEventListener('pointerdown', e => {
    const rect = canvas.getBoundingClientRect();
    const x    = (e.clientX - rect.left) * (canvas.width / rect.width);
    const h    = findHandle(x);
    if (h) {
      canvas.setPointerCapture(e.pointerId);
      const reg = cutRegions.find(r => r.id === h.regionId);
      cutDrag = { ...h, startX: x, origStart: reg.startSec, origEnd: reg.endSec };
      cutSelId = h.regionId;
    } else {
      cutDrag = null;
    }
    renderWaveform();
    renderRegionList();
  });

  canvas.addEventListener('pointermove', e => {
    if (!cutDrag || !cutAudioBuf) return;
    const rect = canvas.getBoundingClientRect();
    const x    = (e.clientX - rect.left) * (canvas.width / rect.width);
    const dx   = secFromX(x) - secFromX(cutDrag.startX);
    const reg  = cutRegions.find(r => r.id === cutDrag.regionId);
    if (!reg) return;
    const dur  = cutAudioBuf.duration;
    if (cutDrag.edge === 'start') {
      reg.startSec = Math.max(0, Math.min(reg.endSec - 0.01, cutDrag.origStart + dx));
    } else if (cutDrag.edge === 'end') {
      reg.endSec   = Math.max(reg.startSec + 0.01, Math.min(dur, cutDrag.origEnd + dx));
    } else {
      const w = cutDrag.origEnd - cutDrag.origStart;
      reg.startSec = Math.max(0, Math.min(dur - w, cutDrag.origStart + dx));
      reg.endSec   = reg.startSec + w;
    }
    renderWaveform();
    renderRegionList();
  });

  canvas.addEventListener('pointerup', () => { cutDrag = null; });
})();

// ─────────────────────────────────────────────────────────────────────────────
// LIVE play tab
// ─────────────────────────────────────────────────────────────────────────────
let liveOctave = 4;  // base octave for piano keyboard

function renderLive() {
  const container = document.getElementById('live-list');
  if (!container) return;
  container.innerHTML = '';
  const active = state.lanes.filter(l => l && l.active);
  if (!active.length) {
    container.innerHTML = '<div style="padding:32px 16px;color:var(--t3);font-family:var(--f-mono);font-size:12px;text-align:center">No active lanes</div>';
    return;
  }
  active.forEach(l => {
    const wrap = document.createElement('div');
    wrap.className = 'live-lane';
    const tk = TYPE_KEYS[l.type] || 'wav';
    const header = document.createElement('div');
    header.className = 'live-lane-header';
    header.innerHTML =
      `<div class="lane-bar ${tk}" style="height:100%;width:4px;flex-shrink:0;margin-right:8px"></div>` +
      `<span style="font-family:var(--f-mono);font-size:13px;color:var(--t0);flex:1">${String(l.idx+1).padStart(2,'0')} ${esc(l.name || TYPE_NAMES[l.type] || '?')}</span>` +
      `<button class="btn${l.mute?' muted':''}" onclick="event.stopPropagation();sendToggleMute(${l.idx})" style="${l.mute?'background:var(--red-dim);border-color:var(--red)':''}">MU</button>` +
      `<button class="btn" onclick="selectLane(${l.idx})">EDIT</button>`;
    wrap.appendChild(header);

    if (l.type === 0) {
      // WAV: filename + position bar + trigger/stop button
      const wavWrap = document.createElement('div');
      wavWrap.style.cssText = 'padding:8px 12px;display:flex;flex-direction:column;gap:6px';
      const fname = l.wav_path ? l.wav_path.split('/').pop() : '— no file —';
      const topRow = document.createElement('div');
      topRow.style.cssText = 'display:flex;align-items:center;gap:8px';
      const info = document.createElement('div');
      info.style.cssText = 'font-family:var(--f-mono);font-size:10px;color:var(--t2);flex:1;overflow:hidden;text-overflow:ellipsis;white-space:nowrap';
      info.textContent = fname;
      const trigBtn = document.createElement('button');
      trigBtn.className = 'btn';
      trigBtn.style.cssText = 'height:36px;padding:0 12px;font-size:16px';
      trigBtn.textContent = '▶';
      trigBtn.addEventListener('pointerdown', e => {
        e.stopPropagation();
        // Trigger: use DRUM_TRIGGER cmd with row=0xFF to mean "play WAV lane"
        // If firmware doesn't support it, send TOGGLE_MUTE as fallback
        wsSend(new Uint8Array([CMD.DRUM_TRIGGER, l.idx, 0, 100]).buffer);
        trigBtn.classList.add('active');
        setTimeout(() => trigBtn.classList.remove('active'), 200);
      });
      topRow.appendChild(info);
      topRow.appendChild(trigBtn);
      const posBar = document.createElement('div');
      posBar.style.cssText = 'height:8px;background:var(--bg-3);border-radius:4px;overflow:hidden';
      const posFill = document.createElement('div');
      posFill.style.cssText = 'height:100%;width:0%;background:var(--amber);border-radius:4px;transition:width .1s';
      posBar.appendChild(posFill);
      wavWrap.appendChild(topRow);
      wavWrap.appendChild(posBar);
      wrap.appendChild(wavWrap);
    } else if (l.type === 1) {
      // DRUM: pad buttons per row
      const padRow = document.createElement('div');
      padRow.className = 'live-pads';
      const seq = state.drum[l.idx];
      const rows = seq ? seq.rows : [];
      const nrows = Math.max(rows.length, 1);
      for (let ri = 0; ri < nrows; ri++) {
        const pad = document.createElement('div');
        pad.className = 'live-pad';
        pad.dataset.li = l.idx;
        pad.dataset.ri = ri;
        const rowName = rows[ri] && rows[ri].wav_path
          ? rows[ri].wav_path.split('/').pop().replace(/\.wav$/i,'').slice(0,6)
          : `R${ri+1}`;
        pad.textContent = rowName;
        pad.addEventListener('pointerdown', e => {
          e.preventDefault();
          pad.classList.add('active');
          wsSend(new Uint8Array([CMD.DRUM_TRIGGER, l.idx, ri, 100]).buffer);
        });
        pad.addEventListener('pointerup', () => pad.classList.remove('active'));
        pad.addEventListener('pointerleave', () => pad.classList.remove('active'));
        padRow.appendChild(pad);
      }
      wrap.appendChild(padRow);
    } else if (l.type === 3) {
      // 808 DRUM SYNTH: pad buttons per voice
      const padRow = document.createElement('div');
      padRow.className = 'live-pads';
      const seq = state.dsyn[l.idx];
      const rows = seq ? seq.rows : [];
      const nrows = Math.max(rows.length, 1);
      for (let ri = 0; ri < nrows; ri++) {
        const pad = document.createElement('div');
        pad.className = 'live-pad';
        pad.textContent = rows[ri] ? (VOICE_NAMES[rows[ri].voice] || `R${ri+1}`) : `R${ri+1}`;
        pad.addEventListener('pointerdown', e => {
          e.preventDefault();
          pad.classList.add('active');
          wsSend(new Uint8Array([CMD.SET_DSYN, l.idx, 6, ri, 110]).buffer);
        });
        pad.addEventListener('pointerup', () => pad.classList.remove('active'));
        pad.addEventListener('pointerleave', () => pad.classList.remove('active'));
        padRow.appendChild(pad);
      }
      wrap.appendChild(padRow);
    } else if (l.type === 2) {
      // SYNTH: info bar + EDIT button + 2-octave piano keyboard
      const synthBar = document.createElement('div');
      synthBar.style.cssText = 'display:flex;align-items:center;gap:8px;padding:4px 12px';
      const sp = state.synthParams?.[l.idx];
      const typeName = sp ? (SYNTH_TYPE_NAMES[sp.typeId] || 'Synth') : 'Synth';
      synthBar.innerHTML =
        `<span style="font-family:var(--f-mono);font-size:11px;color:var(--cyan)">${typeName}</span>` +
        `<span style="flex:1"></span>` +
        `<button class="btn" onclick="openSynthEditor(${l.idx})">EDIT</button>`;
      wrap.appendChild(synthBar);
      const kb = buildPianoKeyboard(l.idx);
      wrap.appendChild(kb);
    }
    container.appendChild(wrap);
  });
}

function buildPianoKeyboard(li) {
  const wrap = document.createElement('div');
  wrap.className = 'live-piano-wrap';

  // Octave controls
  const oct = document.createElement('div');
  oct.className = 'live-oct-row';
  oct.innerHTML =
    `<button class="btn" onclick="liveOctave=Math.max(0,liveOctave-1);renderLive()">OCT ▾</button>` +
    `<span style="font-family:var(--f-mono);font-size:12px;color:var(--t2);padding:0 8px" id="live-oct-label">OCT ${liveOctave}</span>` +
    `<button class="btn" onclick="liveOctave=Math.min(8,liveOctave+1);renderLive()">OCT ▴</button>`;
  wrap.appendChild(oct);

  const canvas = document.createElement('canvas');
  canvas.className = 'live-piano-canvas';
  const W = Math.max(window.innerWidth - 32, 300);
  const WK_W = Math.floor(W / 14);  // 14 white keys per 2 octaves
  const WK_H = 80;
  const BK_W = Math.round(WK_W * 0.6);
  const BK_H = Math.round(WK_H * 0.6);
  canvas.width = WK_W * 14;
  canvas.height = WK_H;
  canvas.style.width = canvas.width + 'px';
  canvas.style.height = WK_H + 'px';

  const ctx = canvas.getContext('2d');
  const heldNotes = new Set();

  // White key semitones within octave: C D E F G A B
  const WHITE_SEMI = [0, 2, 4, 5, 7, 9, 11];
  // Black keys: C# D# _ F# G# A# _ (per octave)
  const BLACK_SEMI = [1, 3, -1, 6, 8, 10, -1];

  function noteNum(octave, semi) { return octave * 12 + semi; }

  function drawKeyboard() {
    ctx.clearRect(0, 0, canvas.width, WK_H);
    for (let oct = 0; oct < 2; oct++) {
      const baseX = oct * WK_W * 7;
      WHITE_SEMI.forEach((semi, wi) => {
        const note = noteNum(liveOctave + oct, semi);
        ctx.fillStyle = heldNotes.has(note) ? '#C7F546' : '#E8E8EC';
        ctx.strokeStyle = '#333';
        ctx.fillRect(baseX + wi * WK_W, 0, WK_W - 1, WK_H);
        ctx.strokeRect(baseX + wi * WK_W, 0, WK_W - 1, WK_H);
      });
      BLACK_SEMI.forEach((semi, wi) => {
        if (semi < 0) return;
        const note = noteNum(liveOctave + oct, semi);
        ctx.fillStyle = heldNotes.has(note) ? '#C7F546' : '#0A0A0C';
        const bx = baseX + wi * WK_W + WK_W - Math.round(BK_W / 2);
        ctx.fillRect(bx, 0, BK_W, BK_H);
      });
    }
  }

  function hitNote(cx) {
    // Check black keys first (they're on top)
    for (let oct = 0; oct < 2; oct++) {
      const baseX = oct * WK_W * 7;
      const bSemis = BLACK_SEMI.map((s, wi) => ({s, wi})).filter(({s}) => s >= 0);
      for (const {s, wi} of bSemis) {
        const bx = baseX + wi * WK_W + WK_W - Math.round(BK_W / 2);
        if (cx >= bx && cx < bx + BK_W) return noteNum(liveOctave + oct, s);
      }
    }
    // White keys
    for (let oct = 0; oct < 2; oct++) {
      const baseX = oct * WK_W * 7;
      for (let wi = 0; wi < 7; wi++) {
        if (cx >= baseX + wi * WK_W && cx < baseX + (wi + 1) * WK_W)
          return noteNum(liveOctave + oct, WHITE_SEMI[wi]);
      }
    }
    return -1;
  }

  canvas.addEventListener('pointerdown', e => {
    e.preventDefault();
    canvas.setPointerCapture(e.pointerId);
    const rect = canvas.getBoundingClientRect();
    const note = hitNote(e.clientX - rect.left);
    if (note < 0) return;
    heldNotes.add(note);
    drawKeyboard();
    wsSend(new Uint8Array([CMD.NOTE_ON, li, note, 100]).buffer);
  });
  canvas.addEventListener('pointermove', e => {
    if (!heldNotes.size) return;
    const rect = canvas.getBoundingClientRect();
    const note = hitNote(e.clientX - rect.left);
    // Clear all held if moved to a different note
  });
  canvas.addEventListener('pointerup', e => {
    heldNotes.forEach(note => {
      wsSend(new Uint8Array([CMD.NOTE_OFF, li, note]).buffer);
    });
    heldNotes.clear();
    drawKeyboard();
  });
  canvas.addEventListener('pointerleave', e => {
    heldNotes.forEach(note => {
      wsSend(new Uint8Array([CMD.NOTE_OFF, li, note]).buffer);
    });
    heldNotes.clear();
    drawKeyboard();
  });

  drawKeyboard();
  wrap.appendChild(canvas);
  return wrap;
}

// ─────────────────────────────────────────────────────────────────────────────
// Song management
// ─────────────────────────────────────────────────────────────────────────────
function sendSaveSong() {
  const name = prompt('Save as:', state.songName || 'MySong');
  if (name === null) return;
  const trimmed = name.trim().slice(0, 60);
  const enc = new TextEncoder().encode(trimmed);
  const buf = new Uint8Array(1 + enc.length);
  buf[0] = CMD.SAVE_SONG;
  buf.set(enc, 1);
  wsSend(buf.buffer);
  state.songName = trimmed;
  state.songDirty = false;
  renderTopBar();
}

function sendLoadSong(name) {
  const enc = new TextEncoder().encode(name);
  const buf = new Uint8Array(1 + enc.length);
  buf[0] = CMD.LOAD_SONG;
  buf.set(enc, 1);
  wsSend(buf.buffer);
}

function sendNewSong() {
  if (!confirm('New song? Unsaved changes will be lost.')) return;
  wsSend(new Uint8Array([CMD.NEW_SONG]).buffer);
}

function requestSongList() {
  wsSend(new Uint8Array([CMD.LIST_SONGS]).buffer);
}

function renderSongList() {
  const listEl = document.getElementById('song-list');
  if (!listEl) return;
  listEl.innerHTML = '';
  if (!songList.length) {
    listEl.innerHTML = '<div style="padding:16px;font-family:var(--f-mono);font-size:12px;color:var(--t3)">No songs found</div>';
    return;
  }
  songList.forEach(name => {
    const row = document.createElement('div');
    row.className = 'region-row';
    row.style.cssText = 'display:flex;align-items:center;padding:0 16px;cursor:pointer';
    row.innerHTML =
      `<span style="flex:1;font-family:var(--f-mono);font-size:13px;color:var(--t0)">${esc(name)}</span>` +
      `<button class="btn" onclick="event.stopPropagation();sendLoadSong('${esc(name)}')">LOAD</button>` +
      `<button class="btn" style="margin-left:6px;color:var(--red);border-color:var(--red)" onclick="event.stopPropagation();deleteSong('${esc(name)}')">✕</button>`;
    listEl.appendChild(row);
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// Export mix
// ─────────────────────────────────────────────────────────────────────────────
let exportTimer = null;

function exportStart() {
  fetch('/export/start', { method: 'POST' })
    .then(r => r.json())
    .then(d => {
      if (!d.ok) { alert('Export start failed'); return; }
      document.getElementById('btn-export-start').style.display = 'none';
      document.getElementById('btn-export-stop').style.display  = '';
      document.getElementById('export-download').style.display  = 'none';
      document.getElementById('export-status').textContent = 'Recording… ' + (d.path || '');
      exportTimer = setInterval(exportPollStatus, 1000);
    })
    .catch(() => alert('Export start failed (network error)'));
}

function exportStop() {
  fetch('/export/stop', { method: 'POST' })
    .then(r => r.json())
    .then(d => {
      clearInterval(exportTimer); exportTimer = null;
      document.getElementById('btn-export-start').style.display = '';
      document.getElementById('btn-export-stop').style.display  = 'none';
      const secs = d.seconds || 0;
      const mm = String(Math.floor(secs / 60)).padStart(2, '0');
      const ss = String(Math.floor(secs % 60)).padStart(2, '0');
      document.getElementById('export-elapsed').textContent = '';
      document.getElementById('export-status').textContent  = `Saved — ${mm}:${ss}`;
      document.getElementById('export-download').style.display = 'block';
    })
    .catch(() => alert('Export stop failed (network error)'));
}

function exportPollStatus() {
  fetch('/export/status')
    .then(r => r.json())
    .then(d => {
      if (!d.active) { clearInterval(exportTimer); exportTimer = null; return; }
      const secs = d.seconds || 0;
      const mm = String(Math.floor(secs / 60)).padStart(2, '0');
      const ss = String(Math.floor(secs % 60)).padStart(2, '0');
      document.getElementById('export-elapsed').textContent = mm + ':' + ss;
    })
    .catch(() => {});
}

function deleteSong(name) {
  if (!confirm('Delete "' + name + '"?')) return;
  const enc = new TextEncoder().encode(name);
  const buf = new Uint8Array(2 + enc.length);
  buf[0] = CMD.SAVE_SONG;  // reuse save slot with 0xFF lane = delete signal
  // Actually use DEL_LANE byte: there is no DEL_SONG cmd in spec yet.
  // Fall back to HTTP DELETE if available, otherwise inform user.
  fetch('/songs/' + encodeURIComponent(name), { method: 'DELETE' })
    .then(r => {
      if (r.ok) {
        songList = songList.filter(s => s !== name);
        renderSongList();
      } else {
        alert('Delete failed: ' + r.status);
      }
    })
    .catch(() => alert('Delete failed (network error)'));
}

// ─────────────────────────────────────────────────────────────────────────────
// Settings helpers
// ─────────────────────────────────────────────────────────────────────────────
let uploadModeActive = false;

function setUploadMode(on) {
  uploadModeActive = on;
  document.getElementById('btn-upload-mode-start').style.display = on ? 'none' : '';
  document.getElementById('btn-upload-mode-stop').style.display  = on ? '' : 'none';
  document.getElementById('upload-mode-status').textContent = on
    ? 'Upload mode active — use the UPLOAD tab or PUT /sounds/upload'
    : 'Use the UPLOAD tab to send .wav files to SD card';
  // Notify firmware via SET_SETTINGS: [cmd, 0x01=upload_mode, 0/1]
  wsSend(new Uint8Array([CMD.SET_SETTINGS, 0x01, on ? 1 : 0]).buffer);
}

function sendDefaultBpm() {
  const bpm = parseFloat(document.getElementById('def-bpm').value) || 120;
  // SET_SETTINGS sub-key 0x02 = default BPM (f32)
  const buf = new ArrayBuffer(6);
  const dv  = new DataView(buf);
  dv.setUint8(0, CMD.SET_SETTINGS);
  dv.setUint8(1, 0x02);
  dv.setFloat32(2, bpm, true);
  wsSend(buf);
}

// ─────────────────────────────────────────────────────────────────────────────
// WiFi settings
// ─────────────────────────────────────────────────────────────────────────────
async function loadWifiConfig() {
  try {
    const r = await fetch('/settings/wifi');
    const j = await r.json();
    document.getElementById('cfg-ssid').value = j.ssid || '';
  } catch(e) {}
}

function togglePass() {
  const p = document.getElementById('cfg-pass');
  p.type = p.type === 'password' ? 'text' : 'password';
}

async function saveWifi() {
  const ssid  = document.getElementById('cfg-ssid').value.trim();
  const pass  = document.getElementById('cfg-pass').value;
  const pass2 = document.getElementById('cfg-pass2').value;
  const st    = document.getElementById('wifi-status');
  const setStatus = (m, c) => { st.style.color = c; st.textContent = m; };
  if (!ssid)           return setStatus('Network name required.',       'var(--red)');
  if (pass.length < 8) return setStatus('Password must be ≥ 8 chars.', 'var(--red)');
  if (pass !== pass2)  return setStatus('Passwords do not match.',      'var(--red)');
  setStatus('Saving…', 'var(--t2)');
  try {
    const body = 'ssid='+encodeURIComponent(ssid)+'&pass='+encodeURIComponent(pass);
    const r = await fetch('/settings/wifi', { method:'POST', headers:{'Content-Type':'application/x-www-form-urlencoded'}, body });
    if (r.ok) setStatus('Saved. Connect to "'+ssid+'" then reload.', 'var(--lime)');
    else setStatus('Error: '+await r.text(), 'var(--red)');
  } catch(e) {
    setStatus('Saved. Connect to "'+ssid+'" then reload.', 'var(--lime)');
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// Scenes A–H
// ─────────────────────────────────────────────────────────────────────────────
function sceneCapture(idx) {
  const name = String.fromCharCode(65 + idx);
  wsSend(new Uint8Array([CMD.SCENE_CAPTURE, idx]).buffer);
  document.querySelectorAll('.scene-btn').forEach((b, i) => b.classList.toggle('active', i === idx));
}

function sceneRecall(idx) {
  wsSend(new Uint8Array([CMD.SCENE_RECALL, idx]).buffer);
  document.querySelectorAll('.scene-btn').forEach((b, i) => b.classList.toggle('lime', i === idx));
}

// ─────────────────────────────────────────────────────────────────────────────
// Euclidean generator
// ─────────────────────────────────────────────────────────────────────────────
function openEuclideanPopup() {
  const p = document.getElementById('eucl-popup');
  if (p) p.style.display = p.style.display === 'none' ? 'flex' : 'none';
}

function openEuclidean() {
  if (selected_lane < 0) return;
  const hits  = parseInt(document.getElementById('euc-hits').value,  10) || 4;
  const steps = parseInt(document.getElementById('euc-steps').value, 10) || 16;
  const row   = parseInt(document.getElementById('euc-row').value,   10) || 0;
  // [cmd, lane, row, hits, steps, velocity=100]
  wsSend(new Uint8Array([CMD.DRUM_EUCLIDEAN, selected_lane, row, hits, steps, 100]).buffer);
}

// ─────────────────────────────────────────────────────────────────────────────
// Metronome
// ─────────────────────────────────────────────────────────────────────────────
let metronomeOn = false;
function toggleMetronome() {
  metronomeOn = !metronomeOn;
  wsSend(new Uint8Array([CMD.SET_METRONOME, metronomeOn ? 1 : 0]).buffer);
  const btn = document.getElementById('metro-btn');
  if (btn) { btn.classList.toggle('lime', metronomeOn); btn.textContent = metronomeOn ? 'ON' : 'OFF'; }
}

function onSendReturnRange(v) {
  document.getElementById('send-return-val').textContent = v + '%';
  const buf = new ArrayBuffer(6);
  const dv  = new DataView(buf);
  dv.setUint8(0, CMD.SET_MASTER);
  dv.setUint8(1, 0x04); // sub: send return level
  dv.setFloat32(2, v / 100.0, true);
  wsSend(buf);
}

function setMetronomeVol(v) {
  // sub-key 0x01 = volume
  const buf = new ArrayBuffer(3);
  const dv  = new DataView(buf);
  dv.setUint8(0, CMD.SET_METRONOME);
  dv.setUint8(1, 0x01);
  dv.setUint8(2, Math.round(v));
  wsSend(buf);
  document.getElementById('metro-vol-val').textContent = v + '%';
}

// ─────────────────────────────────────────────────────────────────────────────
// Stem export
// ─────────────────────────────────────────────────────────────────────────────
let stemTimer = null;
function stemStart() {
  fetch('/stem/start', { method: 'POST' })
    .then(r => r.ok ? r.json() : Promise.reject(r.status))
    .then(() => {
      document.getElementById('btn-stem-start').style.display = 'none';
      document.getElementById('btn-stem-stop').style.display  = '';
      document.getElementById('stem-status').textContent = 'Recording stems…';
      stemTimer = setInterval(stemPollStatus, 1000);
    })
    .catch(e => alert('Stem export start failed: ' + e));
}

function stemStop() {
  fetch('/stem/stop', { method: 'POST' })
    .then(r => r.ok ? r.json() : Promise.reject(r.status))
    .then(() => {
      clearInterval(stemTimer); stemTimer = null;
      document.getElementById('btn-stem-start').style.display = '';
      document.getElementById('btn-stem-stop').style.display  = 'none';
      document.getElementById('stem-status').textContent = 'Stems saved to /sdcard/stems/';
    })
    .catch(() => {});
}

function stemPollStatus() {
  fetch('/stem/status').then(r => r.json()).then(d => {
    if (!d.active) { clearInterval(stemTimer); stemTimer = null; }
    const secs = d.seconds || 0;
    const mm = String(Math.floor(secs / 60)).padStart(2,'0');
    const ss = String(Math.floor(secs % 60)).padStart(2,'0');
    const el = document.getElementById('stem-elapsed');
    if (el) el.textContent = mm + ':' + ss;
  }).catch(() => {});
}

// ─────────────────────────────────────────────────────────────────────────────
// Versioned save + templates
// ─────────────────────────────────────────────────────────────────────────────
function sendSaveVersioned() {
  wsSend(new Uint8Array([CMD.SAVE_VERSIONED]).buffer);
}

function sendSaveTemplate() {
  const name = prompt('Template name:', 'mytemplate');
  if (!name) return;
  const enc = new TextEncoder().encode(name);
  const buf = new Uint8Array(1 + enc.length);
  buf[0] = CMD.SAVE_TEMPLATE;
  buf.set(enc, 1);
  wsSend(buf.buffer);
}

function requestTemplateList() {
  fetch('/songs/templates')
    .then(r => r.json())
    .then(list => renderTemplateList(list))
    .catch(() => {});
}

function renderTemplateList(list) {
  const el = document.getElementById('template-list');
  if (!el) return;
  el.innerHTML = list.map(t =>
    `<div class="song-item"><span class="song-name">${t}</span>
     <button class="btn" onclick="sendLoadTemplate('${t}')">LOAD</button></div>`
  ).join('');
  if (!list.length) el.innerHTML = '<div style="padding:16px;color:var(--t3);font-family:var(--f-mono);font-size:12px">No templates saved</div>';
}

function sendLoadTemplate(name) {
  const enc = new TextEncoder().encode(name);
  const buf = new Uint8Array(1 + enc.length);
  buf[0] = CMD.LOAD_TEMPLATE;
  buf.set(enc, 1);
  wsSend(buf.buffer);
}

// ─────────────────────────────────────────────────────────────────────────────
// Send bus level
// ─────────────────────────────────────────────────────────────────────────────
function sendSendLevel(lane, v) {
  const buf = new ArrayBuffer(3);
  const dv  = new DataView(buf);
  dv.setUint8(0, CMD.SET_SEND_LEVEL);
  dv.setUint8(1, lane);
  dv.setUint8(2, Math.round(v * 255));
  wsSend(buf);
}

// ─────────────────────────────────────────────────────────────────────────────
// Per-lane groove
// ─────────────────────────────────────────────────────────────────────────────
function sendGroove(lane, swing, humanise, enabled) {
  const buf = new ArrayBuffer(5);
  const dv  = new DataView(buf);
  dv.setUint8(0, CMD.SET_GROOVE);
  dv.setUint8(1, lane);
  dv.setUint8(2, swing);       // 50–75
  dv.setInt8 (3, humanise);    // 0–20
  dv.setUint8(4, enabled ? 1 : 0);
  wsSend(buf);
}

// ─────────────────────────────────────────────────────────────────────────────
// Note repeat
// ─────────────────────────────────────────────────────────────────────────────
function sendNoteRepeat(lane, enabled, divTicks) {
  const buf = new ArrayBuffer(4);
  const dv  = new DataView(buf);
  dv.setUint8(0, CMD.SET_NOTE_REPEAT);
  dv.setUint8(1, lane);
  dv.setUint8(2, enabled ? 1 : 0);
  dv.setUint8(3, divTicks || 0);
  wsSend(buf);
}

// ─────────────────────────────────────────────────────────────────────────────
// Wavetable import
// ─────────────────────────────────────────────────────────────────────────────
async function wtImport() {
  const slot = parseInt(document.getElementById('wt-slot-sel').value, 10) || 1;
  const files = document.getElementById('wt-file-input').files;
  if (!files || !files.length) { alert('Choose a WAV file first.'); return; }
  const file = files[0];
  const status = document.getElementById('wt-import-status');
  status.textContent = 'Uploading…';
  try {
    const r = await fetch('/wt/upload?slot=' + slot, {
      method: 'POST',
      headers: { 'Content-Type': 'audio/wav' },
      body: file,
    });
    if (r.ok) {
      status.style.color = 'var(--lime)';
      status.textContent = 'Loaded into slot ' + slot;
    } else {
      status.style.color = 'var(--red)';
      status.textContent = 'Upload failed: ' + r.status;
    }
  } catch(e) {
    status.style.color = 'var(--red)';
    status.textContent = 'Upload error: ' + e;
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// Arrangement chain
// ─────────────────────────────────────────────────────────────────────────────
let arrangementSteps = []; // [{scene_idx, repeat}]

function renderArrangement() {
  const el = document.getElementById('arr-step-list');
  if (!el) return;
  el.innerHTML = arrangementSteps.map((s, i) =>
    `<div class="arr-step">
      <span class="arr-scene">SCN ${String.fromCharCode(65 + s.scene_idx)}</span>
      <button class="btn" onclick="arrChangeScene(${i}, -1)">◀</button>
      <button class="btn" onclick="arrChangeScene(${i}, 1)">▶</button>
      <span style="font-family:var(--f-mono);font-size:11px;color:var(--t2)">×</span>
      <input type="number" min="1" max="16" value="${s.repeat}" style="width:44px;background:var(--bg-3);color:var(--t0);border:1px solid var(--line-2);border-radius:6px;padding:0 6px;height:28px;font-family:var(--f-mono);font-size:13px" oninput="arrSetRepeat(${i},+this.value)">
      <button class="btn danger" onclick="arrRemoveStep(${i})">✕</button>
    </div>`
  ).join('') || '<div style="padding:12px;color:var(--t3);font-family:var(--f-mono);font-size:11px">No steps — add a scene</div>';
}

function arrAddStep() {
  arrangementSteps.push({ scene_idx: 0, repeat: 1 });
  renderArrangement();
  sendArrangement();
}

function arrRemoveStep(i) {
  arrangementSteps.splice(i, 1);
  renderArrangement();
  sendArrangement();
}

function arrChangeScene(i, delta) {
  arrangementSteps[i].scene_idx = (arrangementSteps[i].scene_idx + 8 + delta) % 8;
  renderArrangement();
  sendArrangement();
}

function arrSetRepeat(i, v) {
  arrangementSteps[i].repeat = Math.max(1, Math.min(16, v));
  sendArrangement();
}

function sendArrangement() {
  const enabled = document.getElementById('arr-enable')?.checked ? 1 : 0;
  const n = arrangementSteps.length;
  const buf = new Uint8Array(3 + n * 2);
  buf[0] = CMD.SET_ARRANGEMENT;
  buf[1] = enabled;
  buf[2] = n;
  for (let i = 0; i < n; i++) {
    buf[3 + i*2    ] = arrangementSteps[i].scene_idx;
    buf[3 + i*2 + 1] = arrangementSteps[i].repeat;
  }
  wsSend(buf.buffer);
}

// ─────────────────────────────────────────────────────────────────────────────
// Autosave interval
// ─────────────────────────────────────────────────────────────────────────────
function sendAutosaveInterval() {
  const bars = parseInt(document.getElementById('autosave-interval').value, 10) || 8;
  wsSend(new Uint8Array([CMD.SET_SETTINGS, 0x10, bars]).buffer);
}