'use strict';
const test = require('node:test');
const assert = require('node:assert');
const { MoodCore, speedMap, stretchMap, clockHz } = require('../src/mood-worklet.js');

const FS = 48000;
const BLOCK = 128;

// Run `seconds` of audio through the core; `src(t)` gives the input sample,
// `at` maps a block index to a callback (for footswitch presses).
function render(core, seconds, src, at = {}) {
  const n = Math.ceil((seconds * FS) / BLOCK);
  const L = new Float32Array(n * BLOCK), R = new Float32Array(n * BLOCK);
  const inp = new Float32Array(BLOCK), oL = new Float32Array(BLOCK), oR = new Float32Array(BLOCK);
  for (let b = 0; b < n; b++) {
    if (at[b]) at[b](core);
    for (let i = 0; i < BLOCK; i++) inp[i] = src((b * BLOCK + i) / FS);
    core.process(inp, oL, oR);
    L.set(oL, b * BLOCK); R.set(oR, b * BLOCK);
  }
  return { L, R };
}

const blk = (sec) => Math.round((sec * FS) / BLOCK);
const rms = (a, s, e) => { let acc = 0; const i0 = Math.floor(s * FS), i1 = Math.floor(e * FS); for (let i = i0; i < i1; i++) acc += a[i] * a[i]; return Math.sqrt(acc / (i1 - i0)); };
const finite = (a) => a.every(Number.isFinite);
const click = (t) => (t < 0.002 ? 0.8 : 0);
const tone = (f, dur) => (t) => (t < dur ? 0.4 * Math.sin(2 * Math.PI * f * t) : 0);

// Find the strongest output sample after `from` seconds.
function peakTime(a, from, to) {
  let best = 0, at = 0;
  for (let i = Math.floor(from * FS); i < Math.floor(to * FS); i++) if (Math.abs(a[i]) > best) { best = Math.abs(a[i]); at = i; }
  return at / FS;
}

test('bypass passes the dry signal untouched', () => {
  const core = new MoodCore(FS);
  const { L, R } = render(core, 0.5, tone(220, 1));
  const src = tone(220, 1);
  for (let i = 1000; i < 20000; i += 97) {
    assert.ok(Math.abs(L[i] - src(i / FS)) < 1e-6);
    assert.ok(Math.abs(R[i] - src(i / FS)) < 1e-6);
  }
});

test('delay repeats at the TIME setting, and half clock doubles it', () => {
  const measure = (clock) => {
    const core = new MoodCore(FS);
    core.setParam('wetMode', 1); core.setParam('mix', 1); core.setParam('dryKill', 1);
    core.setParam('modWet', 0.3); core.setParam('clock', clock);
    core.s.clock = clock; core.s.mix = 1; core.modeGain = [0, 1, 0];
    // time 0.5 → expMap(0.5, 0.02s, 2s) = 0.2 s at full clock
    core.footswitch('wet', 'down'); core.footswitch('wet', 'up');
    const { L } = render(core, 1.6, (t) => (t > 0.1 && t < 0.102 ? 0.8 : 0));
    return peakTime(L, 0.12, 1.5) - 0.1;
  };
  const full = measure(1);
  assert.ok(Math.abs(full - 0.2) < 0.01, `full clock echo at ${full}s`);
  // clock knob 0.5 → 11.3 kHz → ratio 0.236 → echo ≈ 0.85 s
  const ratio = clockHz(0.5) / FS;
  const slow = measure(0.5);
  assert.ok(Math.abs(slow - 0.2 / ratio) < 0.02, `low clock echo at ${slow}s, expected ${0.2 / ratio}`);
});

test('reverb tail decays and stays finite in every Modify position', () => {
  for (const m of [0, 0.5, 1]) {
    const core = new MoodCore(FS);
    core.setParam('wetMode', 0); core.setParam('modWet', m); core.setParam('time', 0.4);
    core.footswitch('wet', 'down'); core.footswitch('wet', 'up');
    const { L, R } = render(core, 4, click);
    assert.ok(finite(L) && finite(R));
    const early = rms(L, 0.1, 0.4), late = rms(L, 3.5, 4);
    assert.ok(early > 1e-3, `reverb produced output (m=${m})`);
    assert.ok(late < early * 0.2, `reverb decays (m=${m}): ${early} → ${late}`);
  }
});

test('reverb freeze holds the tail', () => {
  const core = new MoodCore(FS);
  core.setParam('time', 0.2);
  core.footswitch('wet', 'down'); core.footswitch('wet', 'up');
  const { L } = render(core, 3, tone(330, 0.5), {
    [blk(0.6)]: (c) => c.footswitch('wet', 'down'),
  });
  assert.ok(core.freeze);
  const a = rms(L, 1.2, 1.6), b = rms(L, 2.5, 2.9);
  assert.ok(a > 1e-3 && b > a * 0.7, `frozen tail sustains: ${a} → ${b}`);
});

test('delay self-oscillation stays bounded', () => {
  const core = new MoodCore(FS);
  core.setParam('wetMode', 1); core.setParam('modWet', 1); core.setParam('time', 0.2);
  core.footswitch('wet', 'down'); core.footswitch('wet', 'up');
  const { L } = render(core, 6, tone(440, 0.3));
  assert.ok(finite(L));
  assert.ok(L.every((v) => Math.abs(v) <= 1));
});

test('slip produces pitch-shifted slices in all directions', () => {
  for (const m of [0, 0.15, 0.4, 1]) {
    const core = new MoodCore(FS);
    core.setParam('wetMode', 2); core.setParam('modWet', m); core.setParam('dryKill', 1);
    core.footswitch('wet', 'down'); core.footswitch('wet', 'up');
    const { L } = render(core, 2, tone(220, 2));
    assert.ok(finite(L));
    assert.ok(rms(L, 1, 2) > 0.02, `slip output m=${m}`);
  }
});

test('tapping the loop switch captures what was just played', () => {
  const core = new MoodCore(FS);
  core.setParam('dryKill', 1); core.setParam('mix', 1);
  // play a tone for 1 s, then silence; tap at 1.0 s
  const { L } = render(core, 3, tone(330, 1), {
    [blk(1.0)]: (c) => { c.footswitch('loop', 'down'); c.footswitch('loop', 'up'); },
  });
  assert.strictEqual(core.state().loop, 'play');
  assert.ok(rms(L, 2, 3) > 0.1, 'loop keeps playing after input stops');
});

test('holding the loop switch records a fresh loop of that length', () => {
  const core = new MoodCore(FS);
  core.setParam('length', 1);
  render(core, 1.5, tone(330, 3), {
    [blk(0.5)]: (c) => c.footswitch('loop', 'down'),
    [blk(1.3)]: (c) => c.footswitch('loop', 'up'),
  });
  const secs = core.lsrc / FS;
  assert.ok(Math.abs(secs - 0.8) < 0.01, `loop length ${secs}s`);
});

test('tap while playing stops the loop; hold while playing overdubs', () => {
  const core = new MoodCore(FS);
  render(core, 2, tone(330, 1), {
    [blk(1.0)]: (c) => { c.footswitch('loop', 'down'); c.footswitch('loop', 'up'); },
    [blk(1.2)]: (c) => c.footswitch('loop', 'down'),
    [blk(1.4)]: (c) => assert.strictEqual(c.state().loop, 'dub'),
    [blk(1.8)]: (c) => c.footswitch('loop', 'up'),
  });
  assert.strictEqual(core.state().loop, 'play');
  render(core, 0.1, () => 0, { 0: (c) => { c.footswitch('loop', 'down'); c.footswitch('loop', 'up'); } });
  assert.strictEqual(core.state().loop, 'off');
});

test('every loop mode and routing stays finite', () => {
  for (const loopMode of [0, 1, 2]) for (const routing of [0, 1, 2]) for (const wetMode of [0, 1, 2]) {
    const core = new MoodCore(FS);
    Object.entries({ loopMode, routing, wetMode, modLoop: 0.1, clock: 0.3 }).forEach(([k, v]) => core.setParam(k, v));
    core.footswitch('wet', 'down'); core.footswitch('wet', 'up');
    const { L, R } = render(core, 2, (t) => 0.3 * Math.sin(2 * Math.PI * 196 * t) * Math.exp(-3 * (t % 0.5)), {
      [blk(1.0)]: (c) => { c.footswitch('loop', 'down'); c.footswitch('loop', 'up'); },
    });
    assert.ok(finite(L) && finite(R), `loop ${loopMode} routing ${routing} wet ${wetMode}`);
  }
});

test('loop buffer length is set by the clock (fixed sample count)', () => {
  for (const [knob, secs] of [[1, 64000 / FS], [0, 32]]) {
    const core = new MoodCore(FS);
    core.setParam('clock', knob); core.s.clock = knob;
    render(core, secs + 0.5, tone(330, 100), {
      [blk(secs + 0.4)]: (c) => { c.footswitch('loop', 'down'); c.footswitch('loop', 'up'); },
    });
    assert.strictEqual(core.lsrc, 63999, `clock ${knob}`);
  }
});

test('ENV mode: playing interrupts the loop with a repeating slice', () => {
  const core = new MoodCore(FS);
  core.setParam('loopMode', 0); core.setParam('modLoop', 0.8); core.setParam('length', 0.2);
  core.setParam('dryKill', 1); core.setParam('mix', 1);
  // record a 220 Hz loop, then play a loud 880 Hz note: the output should keep
  // repeating loop material (not 880 Hz, since dry is killed) and stay finite
  const src = (t) => (t < 1 ? 0.3 * Math.sin(2 * Math.PI * 220 * t) : t > 1.5 && t < 2.2 ? 0.5 * Math.sin(2 * Math.PI * 880 * t) : 0);
  const { L } = render(core, 3, src, {
    [blk(1.0)]: (c) => { c.footswitch('loop', 'down'); c.footswitch('loop', 'up'); },
    [blk(1.8)]: (c) => assert.ok(c.envGate, 'gate opens while playing'),
    [blk(2.8)]: (c) => assert.ok(!c.envGate, 'gate closes after you stop'),
  });
  assert.ok(finite(L) && rms(L, 1.6, 2.1) > 0.02);
});

test('speed maps hit musical anchors', () => {
  assert.strictEqual(speedMap(0.5, 0.04), 1);
  assert.strictEqual(speedMap(1, 0.04), 2);
  assert.strictEqual(speedMap(0, 0.04), -2);
  assert.strictEqual(speedMap(0.3, 0.04), 0.5);
  assert.strictEqual(stretchMap(0.5), 0);
  assert.strictEqual(stretchMap(1), 2);
  assert.strictEqual(stretchMap(0), -2);
});
