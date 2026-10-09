// UI and audio graph for BadMoodEmu.
// badmood-worklet.js is also loaded as a classic script so the readouts and presets can
// reuse its tables and mappings (PARAM_DEFAULTS, clockHz, relayRepeats, FLIP_CHORDS, ...).
'use strict';

(() => {
  const MAIN = [
    { id: 'time', row: 'top' }, { id: 'mix', row: 'top' }, { id: 'length', row: 'top' },
    { id: 'modWet', row: 'bottom' }, { id: 'clock', row: 'bottom' }, { id: 'modLoop', row: 'bottom' },
  ];
  // what the knobs and toggles set while both footswitches are held (manual p.14)
  const HIDDEN_MAIN = ['cross', 'balance', 'fade', 'eq', 'glue', 'blend'];
  const TOGGLES = ['wetMode', 'routing', 'loopMode'];
  const HIDDEN_TOGGLES = ['sync', 'spreadSel', 'inputMod'];
  const TOGGLE_LABELS = {
    wetMode: ['Soup', 'Relay', 'Flip'], routing: ['In', 'In + Loop', 'Loop'], loopMode: ['Burst', 'Radio', 'Mask'],
    sync: ['Loop>Wet', 'Unsynced', 'Wet>Loop'], spreadSel: ['Wet', 'Both', 'Loop'], inputMod: ['Wet>Loop', 'Input', 'Loop>Wet'],
    preset: ['Preset 1', 'Live', 'Preset 2'],
  };
  const DIPS_CUSTOM = [['miso', 'MISO'], ['spread', 'Spread'], ['dryKill', 'Dry kill'], ['latch', 'Latch'], ['trails', 'Trails'], ['smooth', 'Smooth'], ['dryGlue', 'Dry glue'], ['half', 'Half']];
  const DIPS_RAMP = [['rampTime', 'Time'], ['rampModWet', 'Modify wet'], ['rampClock', 'Clock'], ['rampModLoop', 'Modify loop'], ['rampLength', 'Length'], ['bounce', 'Bounce'], ['sweep', 'Sweep top'], ['polarity', 'Polarity']];
  const HIDDEN_PANEL = [['cross', 'Cross'], ['balance', 'Level balance'], ['fade', 'Fade'], ['eq', 'EQ'], ['glue', 'Glue'], ['blend', 'Blend'], ['volume', 'Effect volume'], ['rampSpeed', 'Ramp speed']];
  const HIDDEN_IDS = ['cross', 'balance', 'fade', 'eq', 'glue', 'blend', 'sync', 'spreadSel', 'inputMod', 'volume'];
  const LABEL = {
    time: 'TIME', mix: 'MIX', length: 'LENGTH', modWet: 'MODIFY (WET)', clock: 'CLOCK', modLoop: 'MODIFY (LOOP)',
    cross: 'CROSS', balance: 'LEVEL BALANCE', fade: 'FADE', eq: 'EQ', glue: 'GLUE', blend: 'BLEND', volume: 'EFFECT VOLUME',
    rampSpeed: 'RAMP SPEED', wetMode: 'WET MODE', routing: 'ROUTING', loopMode: 'LOOP MODE', sync: 'SYNC', spreadSel: 'SPREAD',
    inputMod: 'INPUT MOD', preset: 'PRESET',
  };

  const params = Object.assign({}, PARAM_DEFAULTS, { preset: 1 });
  const presets = { slots: [null, null], live: null };
  try {
    const saved = JSON.parse(localStorage.getItem('badmood-emu-params') || 'null');
    if (saved) for (const k in saved) if (k in params && typeof saved[k] === 'number') params[k] = saved[k];
    const ps = JSON.parse(localStorage.getItem('badmood-emu-presets') || 'null');
    if (ps && Array.isArray(ps.slots)) { presets.slots = ps.slots.slice(0, 2); presets.live = ps.live || null; }
  } catch (_) { /* storage unavailable */ }

  let ctx = null, node = null, inGain = null, source = null, liveStream = null;
  let srcKind = 'demo', fileBuffer = null, demoBuffer = null;
  let sampleRate = 48000;
  let lastWave = null, lastState = null;
  const held = { wet: false, loop: false };
  const ui = {};

  const $ = (id) => document.getElementById(id);
  const fmtTime = (s) => (s < 1 ? `${Math.round(s * 1000)} ms` : `${s.toFixed(2)} s`);
  const fmtSpeed = (s) => `${s < 0 ? 'reverse ' : ''}${Math.abs(s).toFixed(2)}x`;
  const pct = (x) => `${Math.round(x * 100)}%`;
  const clockRatio = () => Math.min(1, clockHz(params.clock, !!params.smooth) / sampleRate);
  const rampOn = () => DIPS_RAMP.slice(0, 5).some(([id]) => params[id]);

  // ── Readouts: what each control is doing right now, in real-world units ──
  function describe(id) {
    const v = params[id], c = clockRatio();
    const L = (params.half ? LOOP_SAMPLES_HALF : LOOP_SAMPLES) / (c * sampleRate);
    switch (id) {
      case 'time':
        if (params.wetMode === 0) return v > 0.985 ? 'decay ∞' : `decay ${fmtTime(expMap(v, 0.4, 40) / c)}`;
        if (params.wetMode === 1) return `delay ${fmtTime(expMap(v, 0.03, RELAY_TMAX_SEC) / c)}`;
        return `lag ${fmtTime((FLIP_LAG_MAX_SEC * v * v) / c)}`;
      case 'mix': return `dry ${pct(params.dryKill ? 0 : Math.min(1, 2 * (1 - v)))} · fx ${pct(Math.min(1, 2 * v))}`;
      case 'clock': {
        const hz = clockHz(v, !!params.smooth);
        return hz > sampleRate ? `${(hz / 1000).toFixed(1)} kHz (capped at ${(sampleRate / 1000).toFixed(1)})` : `${(hz / 1000).toFixed(1)} kHz`;
      }
      case 'modWet': {
        if (params.wetMode === 0) return `character ${pct(v)}${v > 0.6 ? ' (synthetic)' : v < 0.3 ? ' (diffuse)' : ''}`;
        if (params.wetMode === 1) { const n = relayRepeats(v); return n > 8 ? 'infinite repeats' : `${n} repeat${n > 1 ? 's' : ''}`; }
        const i = flipChordIndex(v);
        return `chord ${i + 1}: ${FLIP_CHORDS[i].map((k) => INTERVAL_NAMES[k]).join(' ')}`;
      }
      case 'length': {
        if (params.loopMode === 0) return `step ${fmtTime(L / BURST_DIVS[Math.min(8, Math.round(v * 8))])}`;
        if (params.loopMode === 2) { const t = Math.min(5, Math.floor(v * 5)), fr = v * 5 - t; return `mask: ${MASK_NAMES[t]}${fr > 0.05 && t < 5 ? ` › ${MASK_NAMES[t + 1]}` : ''}`; }
        switch (radioTuning(params.modLoop)[0]) {
          case 0: return `tape ${fmtSpeed(speedMap(v, 0.04))}`;
          case 1: return `ambient ${Math.pow(2, -4 * (1 - v)).toFixed(2)}x, same pitch`;
          case 2: return `orchestral: ${1 + Math.floor(v * 5.999)} voices`;
          case 3: return `shoegaze: moment ${(v * 7 + 1).toFixed(1)} of 8`;
          default: return `dance: rotate every ${fmtTime(L / (1 << Math.min(4, Math.round(v * 4))))}`;
        }
      }
      case 'modLoop': {
        if (params.loopMode === 0) return `sensitivity ${pct(v)}`;
        if (params.loopMode === 2) return v <= 0.005 ? 'off: pure loop' : `threshold ${pct(v)}`;
        const t = radioTuning(v);
        return t[1] < 0 ? `station: ${RADIO_NAMES[t[0]]}` : `static: ${RADIO_NAMES[t[0]]} / ${RADIO_NAMES[t[1]]}`;
      }
      case 'rampSpeed': return `ramp period ${fmtTime(expMap(v, 20, 0.25))}`;
      case 'cross': return v < 1e-3 ? 'off' : pct(v);
      case 'balance': return `wet ${pct(Math.min(1, 2 * (1 - v)))} · loop ${pct(Math.min(1, 2 * v))}`;
      case 'fade': return v > 0.97 ? 'no fade' : `keeps ${pct(0.25 + 0.75 * v)} per pass`;
      case 'eq': return Math.abs(v - 0.5) <= 0.01 ? 'flat' : v < 0.5 ? `darker ${pct((0.5 - v) * 2)}` : `brighter ${pct((v - 0.5) * 2)}`;
      case 'glue': return v < 1e-3 ? 'off' : `${pct(v)}${v > 0.55 ? ' (destroy)' : ''}`;
      case 'blend': return `clean loop ${pct(v)}`;
      case 'volume': return `${((v - 0.5) * 24).toFixed(1)} dB`;
      default: return TOGGLE_LABELS[id] ? TOGGLE_LABELS[id][v].toLowerCase() : String(v);
    }
  }
  const SUB = {
    time: () => ['decay', 'delay time', 'lag'][params.wetMode],
    modWet: () => ['character', 'repeats', 'harmony'][params.wetMode] + ' · wet',
    length: () => ['step size', 'station dependent', 'character'][params.loopMode],
    modLoop: () => ['sensitivity', 'scan', 'threshold'][params.loopMode] + ' · loop',
    clock: () => (params.smooth ? 'sample rate (smooth)' : 'sample rate (stepped)'),
    mix: () => 'dry / fx',
  };

  function lcd(label, value) { ui.lcdLabel.textContent = label; ui.lcdValue.textContent = value; }
  function showLcd(id) { lcd(LABEL[id] || id.toUpperCase(), describe(id)); }

  function persist() {
    try {
      localStorage.setItem('badmood-emu-params', JSON.stringify(params));
      localStorage.setItem('badmood-emu-presets', JSON.stringify(presets));
    } catch (_) { /* ignore */ }
  }
  function send(name, value) {
    params[name] = value;
    if (node && name !== 'preset') node.port.postMessage({ type: 'param', name, value });
    persist();
  }

  // ── Knobs (the six on the pedal are rebindable) ──
  const knobs = [];
  function boundId(slot) {
    if (lastState && lastState.hidden) return HIDDEN_MAIN[slot];
    if (slot === 1) {
      if (held.loop && !held.wet) return 'volume';
      if (rampOn() && !held.wet) return 'rampSpeed';
    }
    return MAIN[slot].id;
  }
  function makeKnob(slot) {
    const wrap = document.createElement('div');
    wrap.className = 'knob-wrap';
    const label = document.createElement('div');
    label.className = 'knob-label';
    const el = document.createElement('div');
    el.className = 'knob';
    el.tabIndex = 0;
    el.setAttribute('role', 'slider');
    el.setAttribute('aria-valuemin', '0');
    el.setAttribute('aria-valuemax', '100');
    let ticks = '';
    for (let i = 0; i <= 10; i++) {
      const a = ((-135 + i * 27) * Math.PI) / 180;
      const r1 = 46, r2 = i % 5 === 0 ? 50 : 48.5;
      ticks += `<line x1="${50 + r1 * Math.sin(a)}" y1="${50 - r1 * Math.cos(a)}" x2="${50 + r2 * Math.sin(a)}" y2="${50 - r2 * Math.cos(a)}"/>`;
    }
    el.innerHTML = `<svg class="ticks" viewBox="0 0 100 100" aria-hidden="true">${ticks}</svg><div class="cap"></div>`;
    const sub = document.createElement('div');
    sub.className = 'knob-sub';
    wrap.append(label, el, sub);
    const k = { slot, wrap, el, label, sub, id: null };
    const set = (v) => {
      v = Math.min(1, Math.max(0, v));
      send(k.id, v);
      draw();
      if (k.id === 'clock' || k.id === 'smooth') refreshSubs();
      showLcd(k.id);
    };
    const draw = () => {
      const v = params[k.id];
      el.style.setProperty('--rot', `${-135 + 270 * v}deg`);
      el.setAttribute('aria-valuenow', String(Math.round(v * 100)));
      el.setAttribute('aria-valuetext', describe(k.id));
    };
    k.draw = draw;
    let drag = null;
    el.addEventListener('pointerdown', (e) => {
      el.setPointerCapture(e.pointerId);
      // a knob moved while one switch is held: that switch does nothing (EFFECT VOLUME, ramp MIX)
      if (!(lastState && lastState.hidden)) for (const w of ['wet', 'loop']) if (held[w] && node) node.port.postMessage({ type: 'fs', which: w, action: 'shift' });
      drag = { y: e.clientY, v: params[k.id] };
      el.focus();
      showLcd(k.id);
    });
    el.addEventListener('pointermove', (e) => { if (drag) set(drag.v + (drag.y - e.clientY) / (e.shiftKey ? 800 : 200)); });
    const end = () => (drag = null);
    el.addEventListener('pointerup', end);
    el.addEventListener('pointercancel', end);
    el.addEventListener('wheel', (e) => { e.preventDefault(); set(params[k.id] - e.deltaY * (e.shiftKey ? 0.0002 : 0.001)); }, { passive: false });
    el.addEventListener('dblclick', () => set(PARAM_DEFAULTS[k.id]));
    el.addEventListener('keydown', (e) => {
      const step = e.shiftKey ? 0.05 : 0.01;
      const key = e.key;
      if (key === 'ArrowUp' || key === 'ArrowRight') set(params[k.id] + step);
      else if (key === 'ArrowDown' || key === 'ArrowLeft') set(params[k.id] - step);
      else if (key === 'Home') set(0);
      else if (key === 'End') set(1);
      else return;
      e.preventDefault();
      e.stopPropagation();
    });
    el.addEventListener('focus', () => showLcd(k.id));
    return k;
  }

  // ── 3-way toggles (the three on the pedal are rebindable; PRESET is fixed) ──
  function makeToggle(host, onSet) {
    const lever = document.createElement('button');
    lever.className = 'lever';
    const opts = document.createElement('div');
    opts.className = 'opts';
    opts.setAttribute('role', 'radiogroup');
    const btns = [0, 1, 2].map((i) => {
      const b = document.createElement('button');
      b.setAttribute('role', 'radio');
      b.addEventListener('click', () => t.set(i));
      opts.append(b);
      return b;
    });
    host.append(lever, opts);
    const t = {
      id: null,
      bind(id) {
        t.id = id;
        TOGGLE_LABELS[id].forEach((s, i) => (btns[i].textContent = s.toUpperCase()));
        lever.setAttribute('aria-label', `${LABEL[id]} toggle: click to cycle`);
        opts.setAttribute('aria-label', LABEL[id]);
        t.draw();
      },
      draw() {
        const i = params[t.id];
        lever.style.setProperty('--pos', String(i));
        btns.forEach((b, j) => b.setAttribute('aria-checked', String(i === j)));
      },
      set(i) {
        if (onSet) onSet(t.id, i);
        else { send(t.id, i); refreshSubs(); }
        t.draw();
        lcd(LABEL[t.id], t.id === 'routing' ? `wet hears: ${TOGGLE_LABELS.routing[i].toLowerCase()}` : TOGGLE_LABELS[t.id][i].toLowerCase());
      },
    };
    lever.addEventListener('click', () => t.set((params[t.id] + 1) % 3));
    return t;
  }

  function rebind() {
    for (const k of knobs) {
      const id = boundId(k.slot);
      if (id === k.id) continue;
      k.id = id;
      const alt = id !== MAIN[k.slot].id;
      k.wrap.classList.toggle('alt', alt);
      k.el.classList.toggle('alt', alt);
      k.label.textContent = alt ? LABEL[id].replace('LEVEL ', '').replace('EFFECT ', 'FX ') : MAIN[k.slot].id.startsWith('mod') ? 'MODIFY' : LABEL[id];
      k.el.setAttribute('aria-label', LABEL[id]);
      k.draw();
    }
    const hidden = !!(lastState && lastState.hidden);
    ui.toggles.forEach((t, i) => { const id = hidden ? HIDDEN_TOGGLES[i] : TOGGLES[i]; if (t.id !== id) t.bind(id); });
    ui.pedal.classList.toggle('hidden-layer', hidden);
    ui.tagline.textContent = hidden ? 'hidden options · release both switches to return' : 'wet channel · micro-looper · emulator';
    refreshSubs();
  }

  function refreshSubs() {
    for (const k of knobs) {
      const alt = k.id !== MAIN[k.slot].id;
      k.sub.textContent = alt ? (k.id === 'volume' ? 'effect volume' : k.id === 'rampSpeed' ? 'ramp speed' : 'hidden option') : SUB[k.id]();
      k.el.setAttribute('aria-valuetext', describe(k.id));
    }
    ui.lcdClock.textContent = `${((clockRatio() * sampleRate) / 1000).toFixed(1)} kHz`;
  }

  // Redraw every control from params (after a preset recall or a reset).
  function refreshAll() {
    knobs.forEach((k) => k.draw());
    ui.toggles.forEach((t) => t.draw());
    ui.presetToggle.draw();
    ui.dips.forEach((d) => d.draw());
    ui.hiddenCtl.forEach((h) => h.draw());
    rebind();
  }

  // ── Presets: Preset 1 / Live / Preset 2 (manual p.18) ──
  function applySnap(snap) {
    if (!snap) return;
    for (const k of PRESET_KEYS) if (typeof snap[k] === 'number') send(k, snap[k]);
    refreshAll();
  }
  function setPreset(pos) {
    if (params.preset === 1 && pos !== 1) presets.live = snapshotParams(params);
    params.preset = pos;
    if (pos === 1) applySnap(presets.live);
    else applySnap(presets.slots[pos / 2]);
    persist();
    ui.presetToggle.draw();
    if (pos !== 1 && !presets.slots[pos / 2]) lcd('PRESET', `preset ${pos / 2 + 1} is empty: live settings kept`);
  }
  let savedUntil = 0;
  function savePreset(slot) {
    presets.slots[slot] = snapshotParams(params);
    persist();
    savedUntil = performance.now() + 900;
    lcd('PRESET', `saved to preset ${slot + 1}`);
  }

  // ── Footswitches ──
  function footswitch(which, action) {
    const btn = which === 'wet' ? ui.fsWet : ui.fsLoop;
    btn.classList.toggle('down', action === 'down');
    held[which] = action === 'down';
    rebind();
    if (!node) {
      if (action === 'down') lcd('NO AUDIO', 'Press Start audio first');
      return;
    }
    node.port.postMessage({ type: 'fs', which, action });
  }
  function bindFootswitch(btn, which) {
    let down = false;
    btn.addEventListener('pointerdown', (e) => { btn.setPointerCapture(e.pointerId); down = true; footswitch(which, 'down'); });
    const up = () => { if (down) { down = false; footswitch(which, 'up'); } };
    btn.addEventListener('pointerup', up);
    btn.addEventListener('pointercancel', up);
    btn.addEventListener('keydown', (e) => { if ((e.key === ' ' || e.key === 'Enter') && !e.repeat && !down) { e.preventDefault(); down = true; footswitch(which, 'down'); } });
    btn.addEventListener('keyup', (e) => { if (e.key === ' ' || e.key === 'Enter') { e.preventDefault(); up(); } });
  }

  // ── Demo phrase: Karplus-Strong fingerpicking, 8 s seamless loop ──
  function makeDemo(fs) {
    const len = 8 * fs;
    const out = new Float32Array(len);
    const midi = (n) => 440 * Math.pow(2, (n - 69) / 12);
    const pluck = (t0, note, amp, decay) => {
      const f = midi(note);
      const N = Math.max(2, Math.round(fs / f));
      const buf = new Float32Array(N);
      let lp = 0;
      for (let i = 0; i < N; i++) { lp += 0.55 * (Math.random() * 2 - 1 - lp); buf[i] = lp; }
      const rho = Math.pow(0.001, 1 / (f * decay));
      const start = Math.floor(t0 * fs);
      const n = Math.floor(decay * fs);
      let idx = 0;
      for (let k = 0; k < n; k++) {
        const a = buf[idx], b = buf[idx + 1 === N ? 0 : idx + 1];
        out[(start + k) % len] += a * amp;
        buf[idx] = rho * 0.5 * (a + b);
        idx = idx + 1 === N ? 0 : idx + 1;
      }
    };
    // Em9, Cmaj7, G6, D/F#: eighth-note arpeggios at 90 bpm; last bar leaves space
    const chords = [[40, 47, 55, 62, 66, 59], [48, 55, 59, 64, 67, 64], [43, 50, 59, 64, 67, 62], [42, 50, 57, 66]];
    chords.forEach((notes, c) => notes.forEach((n, i) => pluck(c * 2 + i / 3, n, i === 0 ? 0.32 : 0.22 + 0.04 * Math.random(), i === 0 ? 3.5 : 2.2)));
    let pk = 0;
    for (let i = 0; i < len; i++) pk = Math.max(pk, Math.abs(out[i]));
    for (let i = 0; i < len; i++) out[i] *= 0.5 / pk;
    const ab = new AudioBuffer({ length: len, numberOfChannels: 1, sampleRate: fs });
    ab.copyToChannel(out, 0);
    return ab;
  }

  // ── Audio graph ──
  async function loadWorklet() {
    try {
      await ctx.audioWorklet.addModule('badmood-worklet.js');
    } catch (_) {
      const code = await (await fetch('badmood-worklet.js')).text();
      const url = URL.createObjectURL(new Blob([code], { type: 'text/javascript' }));
      await ctx.audioWorklet.addModule(url);
    }
  }

  async function startAudio() {
    if (ctx) {
      if (ctx.state === 'running') { await ctx.suspend(); setPower(false); }
      else { await ctx.resume(); setPower(true); }
      return;
    }
    ui.power.disabled = true;
    try {
      ctx = new AudioContext({ latencyHint: 'interactive' });
      sampleRate = ctx.sampleRate;
      await loadWorklet();
      node = new AudioWorkletNode(ctx, 'badmood-processor', {
        numberOfInputs: 1, numberOfOutputs: 1, outputChannelCount: [2],
        channelCount: 2, channelCountMode: 'explicit', channelInterpretation: 'speakers',
      });
      for (const k of PRESET_KEYS) node.port.postMessage({ type: 'param', name: k, value: params[k] });
      node.port.onmessage = onWorkletMessage;
      inGain = new GainNode(ctx, { gain: Number(ui.inGain.value) });
      inGain.connect(node);
      node.connect(ctx.destination);
      await ctx.resume();
      setPower(true);
      refreshSubs();
      await selectSource(srcKind);
      lcd('READY', 'Tap L to loop what you just played, A for wet');
    } catch (err) {
      lcd('AUDIO ERROR', 'This browser could not start the audio engine. Try a current Chrome, Edge, Firefox or Safari.');
      console.error(err);
      ctx = null; node = null;
    } finally {
      ui.power.disabled = false;
    }
  }

  function setPower(on) {
    ui.power.textContent = on ? 'Pause audio' : 'Resume audio';
    ui.power.classList.toggle('on', on);
  }

  function stopSource() {
    if (source) { try { source.stop(); } catch (_) { /* not a buffer source */ } source.disconnect(); source = null; }
    if (liveStream) { liveStream.getTracks().forEach((t) => t.stop()); liveStream = null; }
  }

  function playBuffer(buf) {
    source = new AudioBufferSourceNode(ctx, { buffer: buf, loop: true });
    source.connect(inGain);
    source.start();
  }

  async function selectSource(kind) {
    srcKind = kind;
    document.querySelectorAll('.seg button').forEach((b) => b.setAttribute('aria-checked', String(b.dataset.src === kind)));
    if (kind === 'file' && !fileBuffer) ui.fileInput.click();
    const hints = {
      demo: 'A looping fingerpicked phrase, synthesized in your browser.',
      file: fileBuffer ? 'Your file, looping. Choose Audio file again to pick another.' : 'Pick a guitar recording (WAV, MP3, …). It loops.',
      live: 'Guitar via your audio interface. Use headphones to avoid feedback.',
    };
    ui.hint.textContent = hints[kind];
    if (!ctx) return;
    stopSource();
    if (kind === 'demo') {
      if (!demoBuffer || demoBuffer.sampleRate !== ctx.sampleRate) demoBuffer = makeDemo(ctx.sampleRate);
      playBuffer(demoBuffer);
    } else if (kind === 'file') {
      if (fileBuffer) playBuffer(fileBuffer);
    } else {
      try {
        liveStream = await navigator.mediaDevices.getUserMedia({ audio: { echoCancellation: false, noiseSuppression: false, autoGainControl: false } });
        source = new MediaStreamAudioSourceNode(ctx, { mediaStream: liveStream });
        source.connect(inGain);
      } catch (_) {
        ui.hint.textContent = 'Live input is blocked here. Allow microphone access, or run the emulator locally (see README). Audio file works everywhere.';
      }
    }
  }

  // ── Worklet → UI ──
  function onWorkletMessage(e) {
    const m = e.data;
    if (m.type === 'state') renderState(m.st);
    else if (m.type === 'wave') { lastWave = m.peaks; drawLoop(); }
    else if (m.type === 'event') {
      const ev = m.ev;
      if (ev === 'preset-save-1') savePreset(0);
      else if (ev === 'preset-save-2') savePreset(1);
      else if (ev === 'hidden-on') lcd('HIDDEN OPTIONS', 'knobs and toggles now set hidden options');
      else if (ev === 'hidden-off') lcd('HIDDEN OPTIONS', 'back to the main controls');
      else if (ev === 'true-bypass-on') lcd('TRUE BYPASS', 'on: tap either switch to leave');
      else if (ev === 'true-bypass-off') lcd('TRUE BYPASS', 'off');
      else if (ev === 'loop-play') lcd('MICRO-LOOPER', 'playing');
      else if (ev === 'loop-rec') lcd('MICRO-LOOPER', 'listening (replacing the loop)');
    }
  }

  let prevCycles = 0, flashUntil = 0, prevHidden = false, prevLoop = '';
  function renderState(st) {
    lastState = st;
    if (st.hidden !== prevHidden) { prevHidden = st.hidden; rebind(); }
    const now = performance.now();
    // recording: the LED blinks once per loop length
    if (st.cycles !== prevCycles) { prevCycles = st.cycles; flashUntil = now + 120; }
    const tb = st.trueBypass;
    ui.ledWet.classList.toggle('on', tb || st.wetOn);
    ui.ledWet.classList.toggle('red', tb);
    ui.ledWet.classList.toggle('blink', tb || st.freeze);
    ui.ledLoop.classList.toggle('on', tb || st.loop !== 'rec' || now < flashUntil);
    ui.ledLoop.classList.toggle('blink', tb || st.loop === 'dub');
    ui.ledMid.classList.toggle('on', tb || st.hidden || now < savedUntil);
    ui.ledMid.classList.toggle('red', tb);
    ui.ledMid.classList.toggle('blink', tb || (!st.hidden && now < savedUntil));
    if (st.loop !== prevLoop) { if (st.loop === 'dub') lcd('MICRO-LOOPER', 'overdubbing…'); prevLoop = st.loop; }
    const meter = (el, pk) => (el.style.width = `${Math.min(1, Math.max(0, (20 * Math.log10(pk + 1e-9) + 60) / 60)) * 100}%`);
    meter(ui.mIn, st.peakIn);
    meter(ui.mOut, st.peakOut);
    ui.lcdClock.textContent = `${(st.clockHz / 1000).toFixed(1)} kHz`;
    ui.lcdLoop.textContent = `loop ${fmtTime(st.len / st.clockHz)}`;
    drawLoop();
  }

  function drawLoop() {
    const cv = ui.canvas, g = cv.getContext('2d');
    const w = cv.width, h = cv.height;
    g.clearRect(0, 0, w, h);
    const st = lastState;
    if (!lastWave || !st) {
      g.fillStyle = '#5d5660';
      g.font = '24px "IBM Plex Mono", monospace';
      g.textAlign = 'center';
      g.fillText('start audio to see the loop', w / 2, h / 2 + 8);
      return;
    }
    const n = lastWave.length, playing = st.loop !== 'rec';
    for (let i = 0; i < n; i++) {
      const x = (i / n) * w, a = Math.min(1, lastWave[i] * 1.6) * (h / 2 - 6);
      g.fillStyle = playing ? '#e8a94c' : '#58505c';
      g.fillRect(x, h / 2 - a, Math.max(1, w / n - 0.5), 2 * a + 1);
    }
    const xAt = (s) => (s / st.len) * w;
    if (playing && params.loopMode === 0) {
      st.slices.forEach((s, i) => { g.fillStyle = i === st.step ? '#5ee08a' : 'rgba(243,231,214,.35)'; g.fillRect(xAt(s) - 1, 0, i === st.step ? 3 : 1.5, h); });
    }
    if (playing) { g.fillStyle = 'rgba(243,231,214,.25)'; g.fillRect(xAt(st.seam), 4, 1.5, h - 8); }
    g.fillStyle = playing ? '#ff5a4a' : 'rgba(255,90,74,.6)';
    g.fillRect(xAt(st.pos) - 1.5, 0, 3, h);
    let tag = playing ? (st.loop === 'dub' ? 'overdubbing' : 'playing') : 'listening (recording)';
    if (playing && params.loopMode === 1) tag += ` · ${st.station2 < 0 ? RADIO_NAMES[st.station] : `${RADIO_NAMES[st.station]} / ${RADIO_NAMES[st.station2]} (static)`}`;
    else if (playing && params.loopMode === 0) tag += ` · ${st.slices.length} steps${st.scramble ? ', scrambled' : ''}`;
    else if (playing && params.loopMode === 2) tag += ` · mask ${Math.round(st.mask * 100)}%`;
    g.fillStyle = '#958d97';
    g.font = '20px "IBM Plex Mono", monospace';
    g.textAlign = 'left';
    g.fillText(tag, 10, 24);
  }

  // ── Build the UI ──
  function makeDip(host, id, label) {
    const box = document.createElement('div');
    box.className = 'dip';
    const b = document.createElement('button');
    b.id = `dip-${id}`;
    b.setAttribute('role', 'switch');
    b.setAttribute('aria-label', label);
    const d = { draw: () => b.setAttribute('aria-checked', String(!!params[id])) };
    b.addEventListener('click', () => {
      send(id, params[id] ? 0 : 1);
      d.draw();
      lcd('DIP', `${label.toLowerCase()} ${params[id] ? 'on' : 'off'}`);
      rebind();
    });
    d.draw();
    const lab = document.createElement('span');
    lab.textContent = label;
    box.append(b, lab);
    host.append(box);
    return d;
  }

  function makeHiddenControls() {
    const grid = $('hidden-grid');
    const out = [];
    for (const [id, label] of HIDDEN_PANEL) {
      const l = document.createElement('label');
      const o = document.createElement('output');
      const r = document.createElement('input');
      r.type = 'range'; r.min = '0'; r.max = '1'; r.step = '0.005';
      r.id = `h-${id}`;
      l.append(`${label} `, o, r);
      grid.append(l);
      const h = { draw: () => { r.value = String(params[id]); o.textContent = describe(id); } };
      r.addEventListener('input', () => { send(id, Number(r.value)); h.draw(); showLcd(id); knobs.forEach((k) => k.id === id && k.draw()); });
      r.addEventListener('dblclick', () => { send(id, PARAM_DEFAULTS[id]); h.draw(); });
      h.draw();
      out.push(h);
    }
    for (const id of HIDDEN_TOGGLES) {
      const l = document.createElement('label');
      const s = document.createElement('select');
      s.id = `h-${id}`;
      TOGGLE_LABELS[id].forEach((t, i) => { const op = document.createElement('option'); op.value = String(i); op.textContent = t; s.append(op); });
      l.append(LABEL[id], s);
      grid.append(l);
      const h = { draw: () => { s.value = String(params[id]); } };
      s.addEventListener('change', () => { send(id, Number(s.value)); lcd(LABEL[id], TOGGLE_LABELS[id][params[id]].toLowerCase()); ui.toggles.forEach((t) => t.id === id && t.draw()); });
      h.draw();
      out.push(h);
    }
    return out;
  }

  function init() {
    ui.lcdLabel = $('lcd-label'); ui.lcdValue = $('lcd-value'); ui.lcdClock = $('lcd-clock'); ui.lcdLoop = $('lcd-loop');
    ui.fsWet = $('fs-wet'); ui.fsLoop = $('fs-loop'); ui.ledWet = $('led-wet'); ui.ledLoop = $('led-loop'); ui.ledMid = $('led-mid');
    ui.power = $('btn-power'); ui.inGain = $('in-gain'); ui.mIn = $('m-in'); ui.mOut = $('m-out');
    ui.hint = $('src-hint'); ui.fileInput = $('file-input'); ui.canvas = $('loop-canvas');
    ui.pedal = $('pedal'); ui.tagline = $('tagline');

    for (let i = 0; i < 6; i++) {
      const k = makeKnob(i);
      knobs.push(k);
      $(MAIN[i].row === 'top' ? 'row-top' : 'row-bottom').append(k.wrap);
    }
    ui.toggles = [0, 1, 2].map((i) => makeToggle($(`tg-${i}`)));
    ui.presetToggle = makeToggle($('tg-preset'), (id, i) => setPreset(i));
    ui.presetToggle.bind('preset');
    ui.dips = [...DIPS_CUSTOM.map(([id, l]) => makeDip($('dips-custom'), id, l)), ...DIPS_RAMP.map(([id, l]) => makeDip($('dips-ramp'), id, l))];
    ui.hiddenCtl = makeHiddenControls();
    rebind();

    $('save-1').addEventListener('click', () => savePreset(0));
    $('save-2').addEventListener('click', () => savePreset(1));
    $('reset-hidden').addEventListener('click', () => {
      for (const id of HIDDEN_IDS) send(id, PARAM_DEFAULTS[id]);
      refreshAll();
      lcd('HIDDEN OPTIONS', 'reset to defaults');
    });

    ui.inGain.addEventListener('input', () => { if (inGain) inGain.gain.value = Number(ui.inGain.value); });
    ui.power.addEventListener('click', startAudio);
    document.querySelectorAll('.seg button').forEach((b) => b.addEventListener('click', () => {
      if (b.dataset.src === 'file' && srcKind === 'file') { ui.fileInput.click(); return; }
      selectSource(b.dataset.src);
    }));
    ui.fileInput.addEventListener('change', async () => {
      const f = ui.fileInput.files[0];
      if (!f) return;
      try {
        const data = await f.arrayBuffer();
        const tmp = ctx || new OfflineAudioContext(1, 1, 48000);
        fileBuffer = await tmp.decodeAudioData(data);
        ui.hint.textContent = `${f.name}, looping.`;
        if (ctx) selectSource('file');
      } catch (_) {
        ui.hint.textContent = 'That file could not be decoded. Try WAV or MP3.';
      }
      ui.fileInput.value = '';
    });

    bindFootswitch(ui.fsWet, 'wet');
    bindFootswitch(ui.fsLoop, 'loop');
    const keyHeld = {};
    const keyMap = { a: 'wet', l: 'loop' };
    window.addEventListener('keydown', (e) => {
      const which = keyMap[e.key.toLowerCase()];
      if (!which || e.repeat || keyHeld[which] || (e.target instanceof Element && e.target.matches('input, textarea, select'))) return;
      keyHeld[which] = true;
      footswitch(which, 'down');
    });
    window.addEventListener('keyup', (e) => {
      const which = keyMap[e.key.toLowerCase()];
      if (which && keyHeld[which]) { keyHeld[which] = false; footswitch(which, 'up'); }
    });
    window.addEventListener('blur', () => { for (const w in keyHeld) if (keyHeld[w]) { keyHeld[w] = false; footswitch(w, 'up'); } });

    refreshSubs();
    drawLoop();
  }

  init();
})();
