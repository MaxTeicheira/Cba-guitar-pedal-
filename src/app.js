// UI and audio graph for the Mood emulator.
// mood-worklet.js is also loaded as a classic script so the knob readouts can
// reuse its mapping functions (expMap, speedMap, stretchMap, clockHz, LOOP_SAMPLES, ...).
'use strict';

(() => {
  const KNOBS = [
    { id: 'time', label: 'Time', row: 'top', def: 0.5 },
    { id: 'mix', label: 'Mix', row: 'top', def: 0.5 },
    { id: 'length', label: 'Length', row: 'top', def: 0.75 },
    { id: 'modWet', label: 'Modify', row: 'bottom', def: 0.5 },
    { id: 'clock', label: 'Clock', row: 'bottom', def: 1 },
    { id: 'modLoop', label: 'Modify', row: 'bottom', def: 0.5 },
  ];
  const DIPS = [
    { id: 'trails', label: 'Trails', def: 1 },
    { id: 'dryKill', label: 'Dry kill', def: 0 },
    { id: 'latch', label: 'Latch', def: 0 },
    { id: 'spread', label: 'Spread', def: 0 },
    { id: 'smooth', label: 'Smooth', def: 0 },
  ];
  const WET_MODES = ['Reverb', 'Delay', 'Slip'];
  const LOOP_MODES = ['Env', 'Tape', 'Stretch'];
  const ROUTES = ['In', 'In + Loop', 'Loop'];

  const params = { wetMode: 0, routing: 0, loopMode: 0, balance: 0.5 };
  KNOBS.forEach((k) => (params[k.id] = k.def));
  DIPS.forEach((d) => (params[d.id] = d.def));
  try {
    const saved = JSON.parse(localStorage.getItem('mood-emu-params') || 'null');
    if (saved) for (const k in saved) if (k in params && typeof saved[k] === 'number') params[k] = saved[k];
  } catch (_) { /* storage unavailable */ }

  let ctx = null, node = null, inGain = null, source = null, liveStream = null;
  let srcKind = 'demo', fileBuffer = null, demoBuffer = null;
  let sampleRate = 48000;
  let lastWave = null, lastState = null;
  const ui = {};

  const $ = (id) => document.getElementById(id);
  const clamp = (x, a, b) => Math.min(b, Math.max(a, x));
  const clockRatio = () => Math.min(1, clockHz(params.clock) / sampleRate);
  const fmtTime = (s) => (s < 1 ? `${Math.round(s * 1000)} ms` : `${s.toFixed(2)} s`);
  const fmtSpeed = (s) => (s === 0 ? 'frozen' : `${s < 0 ? 'reverse ' : ''}${Math.abs(s).toFixed(2)}x`);
  const pct = (x) => `${Math.round(x * 100)}%`;

  // ── Readouts: what each control is doing right now, in real-world units ──
  function describe(id) {
    const v = params[id], c = clockRatio();
    switch (id) {
      case 'time':
        if (params.wetMode === 0) return v > 0.985 ? 'decay ∞' : `decay ${fmtTime(expMap(v, 0.4, 25) / c)}`;
        if (params.wetMode === 1) return `delay ${fmtTime(expMap(v, 0.02, 2) / c)}`;
        return `refresh ${fmtTime(expMap(v, 0.05, 1.5) / c)}`;
      case 'mix': return `dry ${pct(params.dryKill ? 0 : Math.min(1, 2 * (1 - v)))} · fx ${pct(Math.min(1, 2 * v))}`;
      case 'length': {
        const full = LOOP_SAMPLES / (c * sampleRate);
        if (params.loopMode === 1) return `loop ${fmtTime(full * expMap(v, 0.02, 1))} of ${fmtTime(full)}`;
        return `slice ${fmtTime((params.loopMode === 0 ? expMap(v, 0.01, 1) : expMap(v, 0.01, 0.5)) / c)}`;
      }
      case 'modWet':
        if (params.wetMode === 0) return `smear ${pct(v)}`;
        if (params.wetMode === 1) return `feedback ${pct(v * 1.1)}${v > 0.91 ? ' (runaway)' : ''}`;
        return `speed ${fmtSpeed(speedMap(v, 0.15))}`;
      case 'clock': {
        const hz = clockHz(v);
        return hz > sampleRate ? `${(hz / 1000).toFixed(1)} kHz (capped at ${(sampleRate / 1000).toFixed(1)})` : `sample rate ${(hz / 1000).toFixed(1)} kHz`;
      }
      case 'modLoop':
        if (params.loopMode === 0) return `sensitivity ${pct(v)}`;
        if (params.loopMode === 1) return `tape ${fmtSpeed(speedMap(v, 0.04))}`;
        return `stretch ${fmtSpeed(stretchMap(v))}, same pitch`;
      case 'balance': return `wet ${pct(Math.min(1, 2 * (1 - v)))} · loop ${pct(Math.min(1, 2 * v))}`;
      default: return '';
    }
  }
  const SUB = {
    time: () => ['decay', 'delay', 'refresh'][params.wetMode],
    modWet: () => ['smear', 'feedback', 'speed'][params.wetMode] + ' · wet',
    modLoop: () => ['sensitivity', 'speed', 'stretch'][params.loopMode] + ' · loop',
    clock: () => 'sample rate',
    mix: () => 'dry / fx',
    length: () => ['slice', 'loop length', 'slice'][params.loopMode],
  };
  const LABEL = { time: 'TIME', mix: 'MIX', length: 'LENGTH', modWet: 'MODIFY (WET)', clock: 'CLOCK', modLoop: 'MODIFY (LOOP)', balance: 'BALANCE' };

  function showLcd(id) {
    ui.lcdLabel.textContent = LABEL[id] || id.toUpperCase();
    ui.lcdValue.textContent = describe(id);
  }

  function send(name, value) {
    params[name] = value;
    if (node) node.port.postMessage({ type: 'param', name, value });
    try { localStorage.setItem('mood-emu-params', JSON.stringify(params)); } catch (_) { /* ignore */ }
  }

  function refreshSubs() {
    for (const k of KNOBS) {
      ui.knobs[k.id].sub.textContent = SUB[k.id]();
      ui.knobs[k.id].el.setAttribute('aria-valuetext', describe(k.id));
    }
    ui.lcdClock.textContent = `${((clockRatio() * sampleRate) / 1000).toFixed(1)} kHz`;
  }

  // ── Knob ──
  function makeKnob(def) {
    const wrap = document.createElement('div');
    wrap.className = 'knob-wrap';
    const label = document.createElement('div');
    label.className = 'knob-label';
    label.textContent = def.label;
    const el = document.createElement('div');
    el.className = 'knob';
    el.tabIndex = 0;
    el.id = `knob-${def.id}`;
    el.setAttribute('role', 'slider');
    el.setAttribute('aria-label', LABEL[def.id]);
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

    const set = (v, quiet) => {
      v = clamp(v, 0, 1);
      el.style.setProperty('--rot', `${-135 + 270 * v}deg`);
      el.setAttribute('aria-valuenow', String(Math.round(v * 100)));
      send(def.id, v);
      el.setAttribute('aria-valuetext', describe(def.id));
      if (def.id === 'clock' && !quiet) refreshSubs();
      if (!quiet) showLcd(def.id);
    };
    let drag = null;
    el.addEventListener('pointerdown', (e) => {
      el.setPointerCapture(e.pointerId);
      drag = { y: e.clientY, v: params[def.id] };
      el.focus();
      showLcd(def.id);
    });
    el.addEventListener('pointermove', (e) => {
      if (!drag) return;
      set(drag.v + (drag.y - e.clientY) / (e.shiftKey ? 800 : 200));
    });
    const end = () => (drag = null);
    el.addEventListener('pointerup', end);
    el.addEventListener('pointercancel', end);
    el.addEventListener('wheel', (e) => { e.preventDefault(); set(params[def.id] - e.deltaY * (e.shiftKey ? 0.0002 : 0.001)); }, { passive: false });
    el.addEventListener('dblclick', () => set(def.def));
    el.addEventListener('keydown', (e) => {
      const step = e.shiftKey ? 0.05 : 0.01;
      const k = e.key;
      if (k === 'ArrowUp' || k === 'ArrowRight') set(params[def.id] + step);
      else if (k === 'ArrowDown' || k === 'ArrowLeft') set(params[def.id] - step);
      else if (k === 'Home') set(0);
      else if (k === 'End') set(1);
      else return;
      e.preventDefault();
      e.stopPropagation();
    });
    el.addEventListener('focus', () => showLcd(def.id));
    set(params[def.id], true);
    return { wrap, el, sub, set };
  }

  // ── 3-way toggle ──
  function makeToggle(host) {
    const param = host.dataset.param;
    const labels = host.dataset.labels.split('|');
    const lever = document.createElement('button');
    lever.className = 'lever';
    lever.setAttribute('aria-label', `${param} toggle: click to cycle`);
    const opts = document.createElement('div');
    opts.className = 'opts';
    opts.setAttribute('role', 'radiogroup');
    const btns = labels.map((t, i) => {
      const b = document.createElement('button');
      b.textContent = t;
      b.setAttribute('role', 'radio');
      b.addEventListener('click', () => set(i));
      opts.append(b);
      return b;
    });
    host.append(lever, opts);
    const set = (i, quiet) => {
      lever.style.setProperty('--pos', String(i));
      btns.forEach((b, j) => b.setAttribute('aria-checked', String(i === j)));
      send(param, i);
      refreshSubs();
      if (!quiet) {
        ui.lcdLabel.textContent = { wetMode: 'WET MODE', routing: 'ROUTING', loopMode: 'LOOP MODE' }[param];
        ui.lcdValue.textContent = param === 'routing' ? `wet hears: ${ROUTES[i].toLowerCase()}` : (param === 'wetMode' ? WET_MODES : LOOP_MODES)[i].toLowerCase();
      }
    };
    lever.addEventListener('click', () => set((params[param] + 1) % 3));
    return set;
  }

  // ── Footswitches ──
  function footswitch(which, action) {
    const btn = which === 'wet' ? ui.fsWet : ui.fsLoop;
    btn.classList.toggle('down', action === 'down');
    if (!node) {
      if (action === 'down') { ui.lcdLabel.textContent = 'NO AUDIO'; ui.lcdValue.textContent = 'Press Start audio first'; }
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
    // Em9, Cmaj7, G6, D/F# — eighth-note arpeggios at 90 bpm; last bar leaves space
    const chords = [
      [40, 47, 55, 62, 66, 59],
      [48, 55, 59, 64, 67, 64],
      [43, 50, 59, 64, 67, 62],
      [42, 50, 57, 66],
    ];
    const eighth = 1 / 3;
    chords.forEach((notes, c) => notes.forEach((n, i) => {
      const t = c * 2 + i * eighth;
      pluck(t, n, i === 0 ? 0.32 : 0.22 + 0.04 * Math.random(), i === 0 ? 3.5 : 2.2);
    }));
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
      await ctx.audioWorklet.addModule('mood-worklet.js');
    } catch (_) {
      const code = await (await fetch('mood-worklet.js')).text();
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
      node = new AudioWorkletNode(ctx, 'mood-processor', {
        numberOfInputs: 1, numberOfOutputs: 1, outputChannelCount: [2],
        channelCount: 1, channelCountMode: 'explicit', channelInterpretation: 'speakers',
      });
      for (const k in params) node.port.postMessage({ type: 'param', name: k, value: params[k] });
      node.port.onmessage = onWorkletMessage;
      inGain = new GainNode(ctx, { gain: Number(ui.inGain.value) });
      inGain.connect(node);
      node.connect(ctx.destination);
      await ctx.resume();
      setPower(true);
      refreshSubs();
      await selectSource(srcKind);
      ui.lcdLabel.textContent = 'READY';
      ui.lcdValue.textContent = 'Tap L to loop, A for wet';
    } catch (err) {
      ui.lcdLabel.textContent = 'AUDIO ERROR';
      ui.lcdValue.textContent = 'This browser could not start the audio engine. Try a current Chrome, Edge, Firefox or Safari.';
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
    if (kind === 'file' && !fileBuffer) { ui.fileInput.click(); }
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
    else if (m.type === 'wave') { lastWave = m; drawLoop(); }
    else if (m.type === 'event' && m.ev === 'loop-stop') { ui.lcdLabel.textContent = 'LOOP'; ui.lcdValue.textContent = 'stopped'; }
  }

  let prevLoop = 'off', prevCycles = 0, flashUntil = 0;
  function renderState(st) {
    lastState = st;
    // while the looper is off, its LED blinks each time the buffer completes a new loop
    if (st.histCycles !== prevCycles) { prevCycles = st.histCycles; flashUntil = performance.now() + 120; }
    const flash = st.loop === 'off' && performance.now() < flashUntil;
    ui.ledWet.classList.toggle('on', st.wetOn);
    ui.ledWet.classList.toggle('blink', st.freeze);
    ui.ledLoop.classList.toggle('on', st.loop !== 'off' || flash);
    ui.ledLoop.classList.toggle('blink', st.loop === 'rec' || st.loop === 'dub');
    if (st.loop !== prevLoop) {
      const msg = { rec: 'recording…', dub: 'overdubbing…', play: 'playing', off: null }[st.loop];
      if (msg) { ui.lcdLabel.textContent = 'LOOP'; ui.lcdValue.textContent = msg; }
      prevLoop = st.loop;
    }
    const meter = (el, pk) => (el.style.width = `${clamp((20 * Math.log10(pk + 1e-9) + 60) / 60, 0, 1) * 100}%`);
    meter(ui.mIn, st.peakIn);
    meter(ui.mOut, st.peakOut);
    ui.lcdClock.textContent = `${(st.clockHz / 1000).toFixed(1)} kHz`;
    ui.lcdLoop.textContent = st.lsrc ? `loop ${fmtTime(st.winLen / st.clockHz)}` : 'no loop';
    drawLoop();
  }

  function drawLoop() {
    const cv = ui.canvas, g = cv.getContext('2d');
    const w = cv.width, h = cv.height;
    g.clearRect(0, 0, w, h);
    const st = lastState;
    if (!lastWave || !st || !st.lsrc) {
      g.fillStyle = '#5d6562';
      g.font = '24px "IBM Plex Mono", monospace';
      g.textAlign = 'center';
      g.fillText('no loop captured yet', w / 2, h / 2 + 8);
      return;
    }
    const { peaks } = lastWave;
    const n = peaks.length;
    const ws = st.winStart / st.lsrc;
    g.fillStyle = 'rgba(232,169,76,0.14)';
    g.fillRect(ws * w, 0, w - ws * w, h);
    for (let i = 0; i < n; i++) {
      const x = (i / n) * w, a = Math.min(1, peaks[i] * 1.6) * (h / 2 - 4);
      g.fillStyle = i / n >= ws ? '#e8a94c' : '#4c5552';
      g.fillRect(x, h / 2 - a, Math.max(1, w / n - 0.5), 2 * a + 1);
    }
    const px = ((st.winStart + st.pos) / st.lsrc) * w;
    g.fillStyle = st.loop === 'off' ? '#5d6562' : '#ff5a4a';
    g.fillRect(px - 1.5, 0, 3, h);
  }

  // ── Build the UI ──
  function init() {
    ui.lcdLabel = $('lcd-label'); ui.lcdValue = $('lcd-value'); ui.lcdClock = $('lcd-clock'); ui.lcdLoop = $('lcd-loop');
    ui.fsWet = $('fs-wet'); ui.fsLoop = $('fs-loop'); ui.ledWet = $('led-wet'); ui.ledLoop = $('led-loop');
    ui.power = $('btn-power'); ui.inGain = $('in-gain'); ui.mIn = $('m-in'); ui.mOut = $('m-out');
    ui.hint = $('src-hint'); ui.fileInput = $('file-input'); ui.canvas = $('loop-canvas');

    ui.knobs = {};
    for (const k of KNOBS) {
      const kn = makeKnob(k);
      ui.knobs[k.id] = kn;
      $(k.row === 'top' ? 'row-top' : 'row-bottom').append(kn.wrap);
    }
    document.querySelectorAll('.toggle').forEach((t) => makeToggle(t)(params[t.dataset.param], true));

    const dips = $('dips');
    for (const d of DIPS) {
      const box = document.createElement('div');
      box.className = 'dip';
      const b = document.createElement('button');
      b.id = `dip-${d.id}`;
      b.setAttribute('role', 'switch');
      b.setAttribute('aria-label', d.label);
      const set = (v) => { b.setAttribute('aria-checked', String(!!v)); send(d.id, v ? 1 : 0); };
      b.addEventListener('click', () => {
        set(!params[d.id]);
        ui.lcdLabel.textContent = 'DIP';
        ui.lcdValue.textContent = `${d.label.toLowerCase()} ${params[d.id] ? 'on' : 'off'}`;
        if (d.id === 'dryKill') refreshSubs();
      });
      set(params[d.id]);
      const lab = document.createElement('span');
      lab.textContent = d.label;
      box.append(b, lab);
      dips.append(box);
    }

    const bal = $('balance');
    bal.value = String(params.balance);
    bal.addEventListener('input', () => { send('balance', Number(bal.value)); showLcd('balance'); });

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
    const held = {};
    const keyMap = { a: 'wet', l: 'loop' };
    window.addEventListener('keydown', (e) => {
      const which = keyMap[e.key.toLowerCase()];
      if (!which || e.repeat || held[which] || e.target.matches('input, textarea')) return;
      held[which] = true;
      footswitch(which, 'down');
    });
    window.addEventListener('keyup', (e) => {
      const which = keyMap[e.key.toLowerCase()];
      if (which && held[which]) { held[which] = false; footswitch(which, 'up'); }
    });
    window.addEventListener('blur', () => { for (const w in held) if (held[w]) { held[w] = false; footswitch(w, 'up'); } });

    refreshSubs();
    drawLoop();
  }

  init();
})();
