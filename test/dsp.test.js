'use strict';
const test = require('node:test');
const assert = require('node:assert');
const {
  BadMoodCore, clockHz, relayRepeats, radioTuning, flipChordIndex, expMap, FLIP_CHORDS,
  RADIO_CENTERS, RELAY_TMAX_SEC, LOOP_SAMPLES, LOOP_SAMPLES_HALF, PRESET_KEYS, snapshotParams, applySnapshot, FFT,
} = require('../src/badmood-worklet.js');

const BLOCK = 128;

// Run `seconds` of audio through the core; `src(t)` gives the input sample,
// `at` maps a block index to a callback (footswitch presses, param changes).
function render(core, seconds, src, at = {}) {
  const FS = core.fs;
  const n = Math.ceil((seconds * FS) / BLOCK);
  const L = new Float32Array(n * BLOCK), R = new Float32Array(n * BLOCK);
  const inp = new Float32Array(BLOCK), oL = new Float32Array(BLOCK), oR = new Float32Array(BLOCK);
  for (let b = 0; b < n; b++) {
    if (at[b]) at[b](core);
    for (let i = 0; i < BLOCK; i++) inp[i] = src((b * BLOCK + i) / FS);
    core.process(inp, null, oL, oR);
    L.set(oL, b * BLOCK); R.set(oR, b * BLOCK);
  }
  return { L, R };
}
function make(params, fs = 48000) {
  const c = new BadMoodCore(fs);
  for (const k in params) c.setParam(k, params[k]);
  return c;
}
const blk = (sec, fs = 48000) => Math.round((sec * fs) / BLOCK);
const rms = (a, s, e, fs = 48000) => { let acc = 0; const i0 = Math.floor(s * fs), i1 = Math.floor(e * fs); for (let i = i0; i < i1; i++) acc += a[i] * a[i]; return Math.sqrt(acc / (i1 - i0)); };
const finite = (a) => a.every(Number.isFinite);
const tone = (f, dur, amp = 0.4) => (t) => (t < dur ? amp * Math.sin(2 * Math.PI * f * t) : 0);
const tap = (which) => (c) => { c.footswitch(which, 'down'); c.footswitch(which, 'up'); };
const FX = { mix: 1, dryKill: 1, glue: 0 };   // effect only, no saturation
// plucked phrase: a different note every 0.2 s
const phrase = (t) => {
  const k = Math.floor(t / 0.2), tt = t - k * 0.2;
  const f = [196, 330, 247, 440, 294, 523, 370, 220, 392, 262, 494, 349][k % 12];
  return 0.4 * Math.sin(2 * Math.PI * f * t) * Math.exp(-12 * tt);
};
// Goertzel power of frequency f in a[i0..i1)
function power(a, f, s, e, fs = 48000) {
  const i0 = Math.floor(s * fs), i1 = Math.floor(e * fs);
  const w = (2 * Math.PI * f) / fs, c = 2 * Math.cos(w);
  let s1 = 0, s2 = 0;
  for (let i = i0; i < i1; i++) { const s0 = a[i] + c * s1 - s2; s2 = s1; s1 = s0; }
  return (s1 * s1 + s2 * s2 - c * s1 * s2) / ((i1 - i0) * (i1 - i0));
}
function corr(a, b, s, e, fs = 48000) {
  let ab = 0, aa = 0, bb = 0;
  for (let i = Math.floor(s * fs); i < Math.floor(e * fs); i++) { ab += a[i] * b[i]; aa += a[i] * a[i]; bb += b[i] * b[i]; }
  return ab / Math.sqrt(aa * bb + 1e-30);
}

test('bypass passes the dry signal untouched (stereo)', () => {
  const core = new BadMoodCore(48000);
  const src = tone(220, 1);
  const { L, R } = render(core, 0.5, src);
  for (let i = 1000; i < 20000; i += 97) {
    assert.ok(Math.abs(L[i] - src(i / 48000)) < 1e-6);
    assert.ok(Math.abs(R[i] - src(i / 48000)) < 1e-6);
  }
});

test('FFT round-trips and matches a direct DFT bin', () => {
  const n = 64, f = new FFT(n);
  const re = new Float64Array(n), im = new Float64Array(n), x = new Float64Array(n);
  for (let i = 0; i < n; i++) { x[i] = Math.sin(i * 0.7) + 0.3 * Math.cos(i * 2.1); re[i] = x[i]; }
  f.run(re, im, false);
  let dr = 0, di = 0;
  for (let i = 0; i < n; i++) { dr += x[i] * Math.cos((2 * Math.PI * 5 * i) / n); di -= x[i] * Math.sin((2 * Math.PI * 5 * i) / n); }
  assert.ok(Math.abs(re[5] - dr) < 1e-9 && Math.abs(im[5] - di) < 1e-9);
  f.run(re, im, true);
  for (let i = 0; i < n; i++) assert.ok(Math.abs(re[i] / n - x[i]) < 1e-12);
});

test('CLOCK moves in the documented harmonized steps; SMOOTH removes the stepping', () => {
  assert.strictEqual(clockHz(0, false), 2000);
  assert.strictEqual(clockHz(1, false), 64000);
  assert.strictEqual(clockHz(64 / 127, false), 12000);   // CC 54-69
  assert.strictEqual(clockHz(100 / 127, false), 32000);  // CC 96-109
  const seen = new Set();
  for (let v = 0; v <= 127; v++) seen.add(clockHz(v / 127, false));
  assert.deepStrictEqual([...seen], [2000, 3000, 4000, 6000, 8000, 12000, 16000, 24000, 32000, 48000, 64000]);
  let prev = 0, distinct = 0;
  for (let v = 0; v <= 1000; v++) { const h = clockHz(v / 1000, true); assert.ok(h >= prev); if (h !== prev) distinct++; prev = h; }
  assert.ok(distinct > 500, 'smooth clock is continuous');
});

test('loop length follows CLOCK (64k → 32k doubles it) and HALF halves it', () => {
  // 64 kHz host so the full clock is reachable
  const fs = 64000;
  const period = (params) => {
    const core = make(params, fs);
    const times = [];
    let last = 0;
    const n = Math.ceil((14 * fs) / BLOCK);
    const z = new Float32Array(BLOCK), a = new Float32Array(BLOCK), b = new Float32Array(BLOCK);
    for (let i = 0; i < n; i++) { core.process(z, null, a, b); if (core.cycles !== last) { last = core.cycles; times.push(core.hostCount); } }
    return [(times[2] - times[1]) / fs, core.state().len];
  };
  const [full, len] = period({ clock: 1 });
  assert.strictEqual(len, LOOP_SAMPLES);
  assert.ok(Math.abs(full - LOOP_SAMPLES / 64000) < 0.01, `64k loop ${full}s`);
  const [half32] = period({ clock: 100 / 127 });
  assert.ok(Math.abs(half32 - 2 * full) < 0.02, `32k loop ${half32}s`);
  const [halfDip, len2] = period({ clock: 1, half: 1 });
  assert.strictEqual(len2, LOOP_SAMPLES_HALF);
  assert.ok(Math.abs(halfDip - full / 2) < 0.01, `HALF loop ${halfDip}s`);
});

test('Relay plays exactly N equal-volume repeats; at max they never fade', () => {
  const time = 0.5;
  const T = expMap(time, 0.03, RELAY_TMAX_SEC);
  for (const [m, N] of [[0.05, 1], [0.3, 3], [0.8, 7]]) {
    assert.strictEqual(relayRepeats(m), N);
    const core = make({ ...FX, wetMode: 1, time, modWet: m });
    const t0 = 0.05;
    const { L } = render(core, t0 + (N + 2) * T + 0.1, (t) => (t > t0 && t < t0 + 0.03 ? 0.5 * Math.sin(2 * Math.PI * 1000 * t) : 0), { 0: tap('wet') });
    const lv = [];
    for (let k = 1; k <= N + 1; k++) lv.push(rms(L, t0 + k * T - 0.005, t0 + k * T + 0.035));
    for (let k = 0; k < N; k++) assert.ok(Math.abs(lv[k] / lv[0] - 1) < 0.05, `N=${N} repeat ${k + 1} level ${lv[k]} vs ${lv[0]}`);
    assert.ok(lv[N] < lv[0] * 0.01, `N=${N}: no repeat ${N + 1} (${lv[N]})`);
  }
  // infinite: the 10th repeat is as loud as the first
  const core = make({ ...FX, wetMode: 1, time: 0.2, modWet: 1 });
  const T2 = expMap(0.2, 0.03, RELAY_TMAX_SEC);
  const { L } = render(core, 0.05 + 11 * T2, (t) => (t > 0.05 && t < 0.07 ? 0.5 * Math.sin(2 * Math.PI * 1000 * t) : 0), { 0: tap('wet') });
  const r1 = rms(L, 0.05 + T2 - 0.003, 0.05 + T2 + 0.025), r10 = rms(L, 0.05 + 10 * T2 - 0.003, 0.05 + 10 * T2 + 0.025);
  assert.ok(Math.abs(r10 / r1 - 1) < 0.1, `infinite repeats hold: ${r1} → ${r10}`);
});

test('Flip produces the selected intervals', () => {
  const f0 = 220;
  const cases = [[5, [2]], [1, [2 / 3]], [3, [4 / 3]], [6, [0.5, 2 / 3]]];   // +oct, -5th, +4th, (-oct,-5th)
  for (const [idx, ratios] of cases) {
    const start = [0, 3, 6, 8, 11, 14, 16][idx];
    const m = (start + 0.4) / 127;
    assert.strictEqual(flipChordIndex(m), idx);
    const core = make({ ...FX, wetMode: 2, time: 0, modWet: m });
    const { L } = render(core, 1.2, tone(f0, 2, 0.3), { 0: tap('wet') });
    const pIn = power(L, f0, 0.5, 1.2);
    for (const r of ratios) {
      const pOut = power(L, f0 * r, 0.5, 1.2);
      assert.ok(pOut > 20 * pIn, `chord ${idx}: ${f0 * r} Hz ${pOut} vs ${f0} Hz ${pIn}`);
    }
  }
  assert.strictEqual(FLIP_CHORDS.length, 48);
  assert.deepStrictEqual(FLIP_CHORDS[flipChordIndex(1)], [3, 5, 0, 2]);   // chord 48: +4th +oct -oct -4th
});

test('Flip spreads the notes in time by TIME (lag)', () => {
  // chord 7 (-oct, -5th): the second note arrives one lag after the first
  const m = 16.4 / 127, time = 0.5, lag = 0.8 * time * time;
  const core = make({ ...FX, wetMode: 2, time, modWet: m });
  const { L } = render(core, 1.2, (t) => (t > 0.1 && t < 0.5 ? 0.3 * Math.sin(2 * Math.PI * 330 * t) : 0), { 0: tap('wet') });
  const early = power(L, 330 * 2 / 3, 0.15, 0.1 + lag - 0.02), late = power(L, 330 * 2 / 3, 0.12 + lag, 0.45 + lag);
  assert.ok(late > 20 * early, `-5th waits for the lag: ${early} → ${late}`);
  assert.ok(power(L, 165, 0.15, 0.45) > 20 * early, '-oct is immediate');
});

test('freeze holds the sound in every wet mode', () => {
  for (const wetMode of [0, 1, 2]) {
    const core = make({ ...FX, wetMode, time: 0.3, modWet: wetMode === 1 ? 0.1 : 0.5 });
    const { L } = render(core, 3.5, tone(330, 1.3), {
      0: tap('wet'),
      [blk(0.7)]: (c) => c.footswitch('wet', 'down'),
    });
    assert.ok(core.freeze);
    const a = rms(L, 1.5, 2.0), b = rms(L, 3.0, 3.5);
    assert.ok(a > 5e-3 && b > a * 0.6 && b < a * 1.6, `mode ${wetMode} frozen: ${a} → ${b}`);
  }
  // releasing the hold (LATCH off) unfreezes
  const core = make({ ...FX, wetMode: 0 });
  render(core, 1.5, tone(330, 0.5), { 0: tap('wet'), [blk(0.6)]: (c) => c.footswitch('wet', 'down'), [blk(1.2)]: (c) => c.footswitch('wet', 'up') });
  assert.ok(!core.freeze && core.wetOn);
});

test('Soup decays with TIME and is more tonal clockwise', () => {
  for (const time of [0.2, 0.6]) {
    const core = make({ ...FX, wetMode: 0, time });
    const { L, R } = render(core, 5, tone(440, 0.4), { 0: tap('wet') });
    assert.ok(finite(L) && finite(R));
    const early = rms(L, 0.5, 0.8), late = rms(L, 4.5, 5);
    assert.ok(early > 1e-2 && late < early * 0.5, `time ${time}: ${early} → ${late}`);
  }
  const purity = (m) => {
    const core = make({ ...FX, wetMode: 0, time: 0.9, modWet: m });
    const { L } = render(core, 2, tone(440, 0.6), { 0: tap('wet') });
    return power(L, 440, 1, 2) / (rms(L, 1, 2) ** 2);
  };
  assert.ok(purity(1) > 1.5 * purity(0), `synthetic is purer: ${purity(1)} vs ${purity(0)}`);
});

test('the looper is always listening: tap plays back what you just played', () => {
  const core = make({ ...FX, loopMode: 2, modLoop: 0 });
  const { L } = render(core, 4, tone(330, 2.7), { [blk(2.7)]: tap('loop') });
  assert.strictEqual(core.state().loop, 'play');
  assert.ok(rms(L, 3, 4) > 0.1, 'loop keeps playing after input stops');
  // tap again: back to recording (REPLACE), output stops
  render(core, 0.5, () => 0, { 0: tap('loop') });
  assert.strictEqual(core.state().loop, 'rec');
});

test('hold overdubs; overdubs record only the clean input, recording captures the wet channel', () => {
  // wet Relay on while recording: its repeats land in the loop after the input stops
  const rec = make({ ...FX, wetMode: 1, time: 0.3, modWet: 1, loopMode: 2, modLoop: 0 });
  render(rec, 1.5, tone(330, 0.3), { 0: tap('wet') });
  const tail = rec.buf.slice(Math.floor(0.6 * 48000), Math.floor(1.4 * 48000));
  assert.ok(Math.sqrt(tail.reduce((a, v) => a + v * v, 0) / tail.length) > 0.05, 'wet repeats recorded while bypassed');

  // overdub: input silent except a burst; Relay repeats of that burst are NOT written
  const core = make({ ...FX, wetMode: 1, time: 0.3, modWet: 1, loopMode: 2, modLoop: 0 });
  const t1 = 0.2;
  render(core, 1.0, () => 0, { 0: tap('loop'), [blk(0.05)]: tap('wet') });
  core.footswitch('loop', 'down');
  const startPos = core.pos;
  render(core, 2.0, (t) => (t > t1 && t < t1 + 0.1 ? 0.4 * Math.sin(2 * Math.PI * 330 * t) : 0));
  assert.strictEqual(core.state().loop, 'dub');
  core.footswitch('loop', 'up');
  assert.strictEqual(core.state().loop, 'play');
  let e = 0, cnt = 0;
  for (let i = 0; i < core.L; i++) { const d = (i - startPos + core.L) % core.L; if (d > (t1 + 0.15) * 48000 && d < 1.9 * 48000) { e += core.buf[i] ** 2; cnt++; } }
  assert.ok(Math.sqrt(e / cnt) < 1e-3, `no wet repeats in the overdub (${Math.sqrt(e / cnt)})`);
});

test('switching loop modes never erases the loop', () => {
  const core = make({ ...FX, loopMode: 0 });
  render(core, 3, phrase, { [blk(2.8)]: tap('loop') });
  const before = core.buf.slice();
  render(core, 2, () => 0, { [blk(0.5)]: (c) => c.setParam('loopMode', 1), [blk(1)]: (c) => c.setParam('loopMode', 2), [blk(1.5)]: (c) => c.setParam('loopMode', 0) });
  assert.deepStrictEqual(core.buf, before);
});

test('Burst makes up to 8 slices from the unique sounds in the loop', () => {
  const core = make({ ...FX, loopMode: 0, modLoop: 0 });
  render(core, 3, phrase, { [blk(2.9)]: tap('loop') });
  assert.ok(core.nSlices >= 6 && core.nSlices <= 8, `${core.nSlices} slices from 13 notes`);
  for (let i = 1; i < core.nSlices; i++) assert.ok(core.slices[i] > core.slices[i - 1]);
  const { L } = render(core, 2, () => 0);
  assert.ok(rms(L, 0.2, 2) > 0.02);
  // three notes → three steps
  const c3 = make({ ...FX, loopMode: 0, modLoop: 0 });
  render(c3, 3, (t) => [0.3, 1.1, 1.9].reduce((a, t0, k) => a + (t > t0 ? 0.4 * Math.sin(2 * Math.PI * (200 + 100 * k) * t) * Math.exp(-20 * (t - t0)) : 0), 0), { [blk(2.9)]: tap('loop') });
  assert.strictEqual(c3.nSlices, 3);
});

test('Burst scrambles the pattern when you play louder than the threshold', () => {
  const run = (playAlong) => {
    const core = make({ ...FX, loopMode: 0, modLoop: 1 });
    render(core, 6, (t) => (t < 2.9 ? phrase(t) : playAlong && t > 3.5 ? 0.3 * Math.sin(2 * Math.PI * 600 * t) : 0), { [blk(2.9)]: tap('loop') });
    return core.scramble;
  };
  assert.strictEqual(run(false), false);
  assert.strictEqual(run(true), true);
});

test('each Radio station sounds different, and there is a clean spot at each', () => {
  for (let i = 0; i < 5; i++) assert.deepStrictEqual(radioTuning(RADIO_CENTERS[i]), [i, -1, 0]);
  const t = radioTuning((RADIO_CENTERS[1] + RADIO_CENTERS[2]) / 2);
  assert.strictEqual(t[0], 1); assert.strictEqual(t[1], 2);
  const outs = [];
  for (let st = 0; st < 5; st++) {
    const core = make({ ...FX, loopMode: 1, modLoop: RADIO_CENTERS[st], length: st === 0 ? 0.9 : 0.3 });
    const { L } = render(core, 6, phrase, { [blk(2.9)]: tap('loop') });
    assert.ok(finite(L) && rms(L, 3.5, 6) > 0.01, `station ${st} sounds`);
    outs.push(L);
  }
  for (let a = 0; a < 5; a++) for (let b = a + 1; b < 5; b++) {
    const c = corr(outs[a], outs[b], 3.5, 6);
    assert.ok(Math.abs(c) < 0.9, `stations ${a} and ${b} differ (corr ${c.toFixed(3)})`);
  }
});

test('Radio interference between stations adds static', () => {
  const quiet = (m) => {
    const core = make({ ...FX, loopMode: 1, modLoop: m });
    const { L } = render(core, 4, (t) => (t < 2.7 ? phrase(t) : 0), { [blk(2.7)]: tap('loop') });
    return power(L, 9000, 3, 4) + power(L, 11000, 3, 4);
  };
  assert.ok(quiet((RADIO_CENTERS[0] + RADIO_CENTERS[1]) / 2) > 5 * quiet(0));
});

test('Mask with MODIFY fully down is the pure loop (identical to Radio Tape at 1x)', () => {
  const ev = { [blk(2.9)]: tap('loop') };
  const a = render(make({ ...FX, loopMode: 2, modLoop: 0, length: 0.5 }), 5, phrase, ev).L;
  const b = render(make({ ...FX, loopMode: 1, modLoop: 0, length: 0.5 }), 5, phrase, ev).L;
  for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) assert.fail(`sample ${i}: ${a[i]} vs ${b[i]}`);
  // and the pure loop is the recording: playback = the input one loop length earlier
  const LS = LOOP_SAMPLES / 48000;
  const ref = new Float32Array(a.length);
  for (let i = 0; i < a.length; i++) ref[i] = phrase(i / 48000 - LS);
  assert.ok(corr(a, ref, 3.2, 4.5) > 0.99, `pure loop = recording (corr ${corr(a, ref, 3.2, 4.5)})`);
  const c = make({ ...FX, loopMode: 2, modLoop: 1, length: 0.1 });
  const masked = render(c, 5, phrase, ev).L;
  assert.ok(Math.abs(corr(masked, a, 3.5, 5)) < 0.9, 'MODIFY up disguises the loop');
});

test('Glue adds harmonics and stays transparent at zero', () => {
  const thd = (g) => {
    const core = make({ glue: g, dryGlue: 1, mix: 0, wetMode: 1, modWet: 0 });
    const { L } = render(core, 1, tone(440, 2, 0.4), { 0: tap('wet') });
    return (power(L, 880, 0.5, 1) + power(L, 1320, 0.5, 1)) / power(L, 440, 0.5, 1);
  };
  assert.ok(thd(0) < 1e-6, `glue 0 clean: ${thd(0)}`);
  const low = thd(0.2), high = thd(0.9);
  assert.ok(low > 1e-4, `glue low warms: ${low}`);
  assert.ok(high > 5 * low, `glue high thrashes: ${high} vs ${low}`);
});

test('Cross modulates pitch and level from the input', () => {
  const run = (cross) => {
    const core = make({ ...FX, wetMode: 1, modWet: 1, time: 0.3, cross, inputMod: 1 });
    const { L } = render(core, 3, (t) => 0.3 * Math.sin(2 * Math.PI * 300 * t) * (t % 0.5 < 0.1 ? 1 : 0.1), { 0: tap('wet') });
    return L;
  };
  const a = run(0), b = run(1);
  assert.ok(finite(b));
  assert.ok(Math.abs(corr(a, b, 1, 3)) < 0.95, 'cross changes the wet channel');
});

test('routing a loop through the wet channel makes it 100% wet unless BLEND', () => {
  const run = (blend) => {
    const core = make({ ...FX, wetMode: 1, modWet: 0, time: 0.5, routing: 2, loopMode: 2, modLoop: 0, blend });
    const { L } = render(core, 4, (t) => (t < 2.6 ? phrase(t) : 0), { [blk(2.7)]: tap('loop'), [blk(2.8)]: tap('wet') });
    return L;
  };
  const wet = run(0), blended = run(1);
  assert.ok(rms(blended, 3.2, 4) > rms(wet, 3.2, 4) * 1.1, 'BLEND adds the clean loop');
});

test('footswitch gestures: hidden options, true bypass, preset save', () => {
  const fs = 48000;
  const core = new BadMoodCore(fs);
  const evs = [];
  const run = (sec, at) => { render(core, sec, phrase, at); while (core.events.length) evs.push(core.events.shift()); };
  // hold both → hidden options, nothing else happens
  run(1, { 10: (c) => c.footswitch('wet', 'down'), 12: (c) => c.footswitch('loop', 'down') });
  assert.ok(core.hidden && !core.wetOn && !core.freeze && !core.loopPlay);
  run(0.1, { 0: (c) => { c.footswitch('wet', 'up'); c.footswitch('loop', 'up'); } });
  assert.ok(!core.hidden && !core.wetOn && !core.loopPlay);
  // triple-tap both → true bypass; the looper stops listening
  const both = (c) => { c.footswitch('wet', 'down'); c.footswitch('loop', 'down'); c.footswitch('wet', 'up'); c.footswitch('loop', 'up'); };
  run(1, { 5: both, 30: both, 60: both });
  assert.ok(core.trueBypass && evs.includes('true-bypass-on'));
  const b0 = core.buf.slice();
  run(0.5, {});
  assert.deepStrictEqual(core.buf, b0, 'nothing recorded in true bypass');
  run(0.1, { 0: tap('wet') });
  assert.ok(!core.trueBypass && !core.wetOn, 'a tap exits true bypass and does nothing else');
  // right switch 3 s, then left 3 s → save to the right slot (Preset 2)
  run(7, { 0: (c) => c.footswitch('loop', 'down'), [blk(3.2)]: (c) => c.footswitch('wet', 'down') });
  assert.ok(evs.includes('preset-save-2'));
  assert.ok(!core.overdub, 'the first switch\'s overdub is cancelled');
});

test('knob moved while a switch is held cancels the switch (EFFECT VOLUME / ramp MIX)', () => {
  const core = new BadMoodCore(48000);
  render(core, 0.8, phrase, { 0: (c) => c.footswitch('loop', 'down'), 10: (c) => c.footswitch('loop', 'shift'), 100: (c) => c.footswitch('loop', 'up') });
  assert.ok(!core.loopPlay && !core.overdub);
});

test('LATCH makes holds toggle', () => {
  const core = make({ latch: 1 });
  render(core, 1.5, phrase, { 0: tap('wet'), 5: (c) => c.footswitch('wet', 'down'), [blk(0.6)]: (c) => c.footswitch('wet', 'up') });
  assert.ok(core.freeze, 'freeze stays after release');
  render(core, 1, phrase, { 0: (c) => c.footswitch('wet', 'down'), [blk(0.6)]: (c) => c.footswitch('wet', 'up') });
  assert.ok(!core.freeze, 'held again: unfreezes');
});

test('SYNC: looper synced to wet takes its length from TIME; wet synced to looper steps TIME', () => {
  const c = make({ sync: 0, time: 0.3 });
  render(c, 0.1, () => 0);
  const P = expMap(0.3, 0.03, RELAY_TMAX_SEC) * 48000;
  assert.strictEqual(c.L, Math.floor(4 * P + 0.5));
  const d = make({ sync: 2, time: 1 });
  render(d, 0.1, () => 0);
  let T = d.L; while (T > RELAY_TMAX_SEC * 48000) T *= 0.5;
  assert.strictEqual(d.syncedTime(1234), T);
});

test('bounce sweeps the selected knob; ramp moves once when the pedal turns on', () => {
  const core = make({ rampClock: 1, bounce: 1, rampSpeed: 1, clock: 1 });
  const seen = new Set();
  for (let k = 0; k < 20; k++) { render(core, 0.05, () => 0); seen.add(Math.round(core.state().clockHz)); }
  assert.ok(seen.size > 4, `clock moves: ${[...seen]}`);
  const r = make({ rampTime: 1, rampSpeed: 0.8, time: 0.9 });
  render(r, 0.2, () => 0);
  assert.ok(Math.abs(r.s.time - 0.9) < 1e-3, 'at rest before the pedal is turned on');
  render(r, 0.05, () => 0, { 0: tap('wet') });
  assert.ok(r.s.time < 0.5, `ramp restarts from the bottom (${r.s.time})`);
  render(r, 3, () => 0);
  assert.ok(Math.abs(r.s.time - 0.9) < 1e-3, 'and settles at the knob');
});

test('presets round-trip every control, dip and hidden option', () => {
  const a = new BadMoodCore(48000);
  const rnd = (() => { let s = 7; return () => ((s = (s * 16807) % 2147483647) / 2147483647); })();
  for (const k of PRESET_KEYS) a.setParam(k, ['wetMode', 'routing', 'loopMode', 'sync', 'spreadSel', 'inputMod'].includes(k) ? Math.floor(rnd() * 3) : rnd() < 0.5 && k.length > 3 && !/time|mix|length|mod|clock|cross|balance|fade|eq|glue|blend|volume|Speed/.test(k) ? 1 : rnd());
  const snap = JSON.parse(JSON.stringify(snapshotParams(a.p)));
  const b = new BadMoodCore(48000);
  applySnapshot(b, snap);
  assert.deepStrictEqual(snapshotParams(b.p), snapshotParams(a.p));
  const oa = render(a, 1, phrase, { 0: tap('wet') }).L, ob = render(b, 1, phrase, { 0: tap('wet') }).L;
  assert.deepStrictEqual(oa, ob);
});

test('MISO takes the left input to both sides; stereo dry passes through', () => {
  const core = make({});
  const n = 128, L = new Float32Array(n), R = new Float32Array(n), oL = new Float32Array(n), oR = new Float32Array(n);
  for (let i = 0; i < n; i++) { L[i] = 0.3; R[i] = -0.2; }
  core.process(L, R, oL, oR);
  assert.ok(Math.abs(oL[5] - 0.3) < 1e-6 && Math.abs(oR[5] + 0.2) < 1e-6);
  core.setParam('miso', 1);
  core.process(L, R, oL, oR);
  assert.ok(Math.abs(oR[5] - 0.3) < 1e-6);
});

test('every mode, routing and option combination stays finite and bounded', () => {
  for (const loopMode of [0, 1, 2]) for (const routing of [0, 1, 2]) for (const wetMode of [0, 1, 2]) {
    const core = make({ loopMode, routing, wetMode, modLoop: 0.4, modWet: 0.97, clock: 0.3, spread: 1, cross: 0.8, glue: 0.9, eq: 0.2, volume: 1 });
    const { L, R } = render(core, 2.5, phrase, { 0: tap('wet'), [blk(1.5)]: tap('loop') });
    assert.ok(finite(L) && finite(R), `loop ${loopMode} routing ${routing} wet ${wetMode}`);
    assert.ok(L.every((v) => Math.abs(v) <= 1) && R.every((v) => Math.abs(v) <= 1));
  }
});
