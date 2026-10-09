// BadMoodEmu DSP core: the reference implementation.
//
// Loads in an AudioWorkletGlobalScope (registers 'badmood-processor') and in Node
// (exports BadMoodCore for offline tests and rendering). native/dsp/badmood.h is a
// line-for-line C++ port; tests/parity renders both and compares them, so any change
// here must be mirrored there with the same arithmetic order.
//
// Signal flow (host rate outside the brackets, CLOCK rate inside):
//
//   in L/R ─┬─────────────────────────────────────────── dry L/R ───────────────┐
//           └─ mono ─ anti-alias LPF ─► [ CORE ] ─► reconstruction LPF ─► EQ ─► GLUE ─► volume ─► MIX ─► out
//
//   CORE tick order (keep it; the C++ port and the RNG draws depend on it):
//     1. micro-looper read (Burst / Radio / Mask) from the loop buffer
//     2. wet channel input from ROUTING, then Soup / Relay / Flip
//     3. CROSS modulation of the wet and loop pairs
//     4. micro-looper write (always-listening record, or overdub), position advance
//     5. channel balance into the core output pair
//
// Conventions: scalars are doubles, audio buffers Float32Array, spectral state
// Float64Array. Times are in core samples at full clock (seconds * host fs), so a
// lower CLOCK stretches every time and loop, as on the pedal.
'use strict';

const TAU = Math.PI * 2;
const clamp = (x, a, b) => (x < a ? a : x > b ? b : x);
const expMap = (t, a, b) => a * Math.pow(b / a, t);

const HOLD_SEC = 0.35;           // footswitch hold threshold
const PRESET_HOLD_SEC = 3;       // preset save: hold one switch 3 s, then the other 3 s (manual p.18)
const COMBO_TAP_WINDOW_SEC = 1.5; // true bypass: three taps of both switches within this window
// Micro-looper buffer: a fixed sample count, so CLOCK sets the loop length. HALF "matches
// the original MOOD" (manual p.45), whose MKII buffer is 64 000 samples, so BAD MOOD's is 2x.
const LOOP_SAMPLES = 128000;
const LOOP_SAMPLES_HALF = 64000;
const BLK = 256;                 // Burst onset analysis block (core samples)
// CLOCK steps and their CC18 ranges (MIDI manual p.5). Value 16 is listed for both 3k and
// 4k; we give it to 3k.
const CLOCK_STEPS_HZ = [2000, 3000, 4000, 6000, 8000, 12000, 16000, 24000, 32000, 48000, 64000];
const CLOCK_STEP_TOP = [4, 16, 28, 40, 53, 69, 83, 95, 109, 121, 127];
const CLOCK_STEP_CENTER = [2, 10.5, 22.5, 34.5, 47, 61.5, 76.5, 89.5, 102.5, 115.5, 124.5];
const CLOCK_MIN_HZ = 2000;
const CLOCK_MAX_HZ = 64000;      // capped at the host rate
const RELAY_TAPS = 8;            // finite repeats 1..8, then infinite
const RELAY_TMAX_SEC = 1.5;
const FLIP_LAG_MAX_SEC = 0.8;
const FLIP_BUF_SEC = 4.5;
const SOUP_N = 1024;
const SOUP_HOP = 256;
const SOUP_BINS = SOUP_N / 2 + 1;
const BURST_DIVS = [64, 48, 32, 24, 16, 12, 8, 6, 4];
const SYNC_FRACS = [1 / 32, 1 / 16, 1 / 12, 1 / 8, 1 / 6, 1 / 4, 1 / 3, 1 / 2, 1];
// Radio stations on MODIFY: CC19 values 0 / 32 / 63 / 97 / 127 (MIDI manual p.5).
const RADIO_CENTERS = [0, 32 / 127, 63 / 127, 97 / 127, 1];
const RADIO_NAMES = ['tape', 'ambient', 'orchestral', 'shoegaze', 'dance'];
const RADIO_CLEAN = 0.035;       // half-width of the clean spot at each station
const ORCH_RATIOS = [1, 0.5, 2, 1.5, 0.75, 4 / 3];
const ORCH_OFFS = [0, 0.5, 0.25, 0.75, 0.125, 0.625];
const ORCH_PANS = [0, -0.7, 0.7, -0.35, 0.35, 0];
const DANCE_SPEEDS = [1, 0.5, 2];
const MASK_NAMES = ['octave down', 'ring', 'crush', 'filter', 'reverse', 'octave up'];

// Flip harmonies: the 48 chords of the CC17 table (MIDI manual p.7-8), in note order.
// Codes: 0 -oct, 1 -5th, 2 -4th, 3 +4th, 4 +5th, 5 +oct. Just intervals (assumed).
const INTERVAL_RATIOS = [0.5, 2 / 3, 0.75, 4 / 3, 1.5, 2];
const INTERVAL_NAMES = ['-oct', '-5th', '-4th', '+4th', '+5th', '+oct'];
const FLIP_CHORDS = [
  [0], [1], [2], [3], [4], [5],
  [0, 1], [1, 5], [0, 2], [2, 5], [0, 3], [3, 5], [0, 4], [4, 5], [0, 5],
  [1, 2], [2, 3], [1, 3], [1, 4], [2, 4], [3, 4],
  [0, 1, 2], [1, 2, 3], [5, 4, 3], [4, 3, 2], [0, 1, 3], [1, 3, 4], [5, 4, 1], [4, 1, 0],
  [0, 1, 5], [0, 2, 3], [2, 3, 5], [5, 4, 2], [4, 2, 0], [0, 2, 5], [1, 2, 4], [0, 3, 4],
  [5, 2, 1], [0, 3, 5], [5, 4, 0],
  [0, 1, 2, 3], [1, 2, 4, 0], [2, 5, 0, 1], [4, 0, 1, 3], [1, 3, 5, 0], [4, 5, 0, 1],
  [3, 4, 0, 2], [3, 5, 0, 2],
];
// First CC17 value of each chord.
const FLIP_CHORD_START = [0, 3, 6, 8, 11, 14, 16, 19, 22, 24, 27, 30, 32, 35, 38, 40, 43, 45, 48, 51, 53, 56, 59, 61,
  64, 67, 69, 72, 75, 77, 80, 83, 85, 88, 90, 93, 96, 98, 101, 104, 106, 109, 112, 114, 117, 120, 122, 125];

function flipChordIndex(m) {
  const cc = clamp(Math.floor(m * 127 + 0.5), 0, 127);
  let idx = 0;
  for (let i = 0; i < 48; i++) if (cc >= FLIP_CHORD_START[i]) idx = i;
  return idx;
}

// CLOCK knob → core sample rate. Stepped (default) or continuous with the SMOOTH dip.
function clockHz(knob, smooth) {
  if (!smooth) {
    const cc = clamp(Math.floor(knob * 127 + 0.5), 0, 127);
    for (let i = 0; i < 11; i++) if (cc <= CLOCK_STEP_TOP[i]) return CLOCK_STEPS_HZ[i];
    return CLOCK_STEPS_HZ[10];
  }
  const x = knob * 127;
  if (x <= CLOCK_STEP_CENTER[0]) return CLOCK_STEPS_HZ[0];
  if (x >= CLOCK_STEP_CENTER[10]) return CLOCK_STEPS_HZ[10];
  for (let i = 0; i < 10; i++) {
    if (x < CLOCK_STEP_CENTER[i + 1]) {
      const t = (x - CLOCK_STEP_CENTER[i]) / (CLOCK_STEP_CENTER[i + 1] - CLOCK_STEP_CENTER[i]);
      return CLOCK_STEPS_HZ[i] * Math.pow(CLOCK_STEPS_HZ[i + 1] / CLOCK_STEPS_HZ[i], t);
    }
  }
  return CLOCK_STEPS_HZ[10];
}

// Radio Tape speed (from MoodEmu's TAPE map): CCW reverse 2x → 0.5x, then forward
// 0.5x → 1x at noon → 2x at full CW, snapping to octaves.
function speedMap(m, snapOct) {
  let s;
  if (m < 0.28) s = -Math.pow(2, 1 - (2 * m) / 0.28);
  else if (m < 0.3) s = 0.5;
  else if (m < 0.5) s = Math.pow(2, -(0.5 - m) / 0.2);
  else s = Math.pow(2, (m - 0.5) / 0.5);
  if (snapOct > 0) {
    const oct = Math.log2(Math.abs(s));
    const r = Math.floor(oct + 0.5);
    if (Math.abs(oct - r) < snapOct) s = (s < 0 ? -1 : 1) * Math.pow(2, r);
  }
  return s;
}

// Relay MODIFY → number of repeats; 9 means infinite (stable, piles up like a looper).
function relayRepeats(m) {
  if (m >= 0.94) return 9;
  return Math.min(RELAY_TAPS, 1 + Math.floor((m / 0.94) * RELAY_TAPS));
}

// Radio MODIFY → up to two stations and the interference position between them.
// Returns [stationA, stationB (-1 if clean), u (0..1 across the gap)].
function radioTuning(m) {
  if (m <= RADIO_CENTERS[0] + RADIO_CLEAN) return [0, -1, 0];
  for (let i = 0; i < 4; i++) {
    if (m < RADIO_CENTERS[i + 1] || i === 3) {
      const lo = RADIO_CENTERS[i] + RADIO_CLEAN, hi = RADIO_CENTERS[i + 1] - RADIO_CLEAN;
      if (m <= lo) return [i, -1, 0];
      if (m >= hi) return [i + 1, -1, 0];
      return [i, i + 1, (m - lo) / (hi - lo)];
    }
  }
  return [4, -1, 0];
}

function softClip(x) {
  const ax = Math.abs(x);
  if (ax < 0.9) return x;
  return (x < 0 ? -1 : 1) * (0.9 + 0.1 * Math.tanh((ax - 0.9) / 0.1));
}

// Transposed direct form II biquad.
class Biquad {
  constructor() { this.b0 = 1; this.b1 = 0; this.b2 = 0; this.a1 = 0; this.a2 = 0; this.z1 = 0; this.z2 = 0; }
  lowpass(fc, fs, q) {
    const w = (TAU * clamp(fc, 10, fs * 0.49)) / fs;
    const cw = Math.cos(w), alpha = Math.sin(w) / (2 * q), a0 = 1 + alpha;
    this.b0 = (1 - cw) / 2 / a0; this.b1 = (1 - cw) / a0; this.b2 = this.b0;
    this.a1 = (-2 * cw) / a0; this.a2 = (1 - alpha) / a0;
  }
  highpass(fc, fs, q) {
    const w = (TAU * clamp(fc, 10, fs * 0.49)) / fs;
    const cw = Math.cos(w), alpha = Math.sin(w) / (2 * q), a0 = 1 + alpha;
    this.b0 = (1 + cw) / 2 / a0; this.b1 = -(1 + cw) / a0; this.b2 = this.b0;
    this.a1 = (-2 * cw) / a0; this.a2 = (1 - alpha) / a0;
  }
  run(x) {
    const y = this.b0 * x + this.z1;
    this.z1 = this.b1 * x - this.a1 * y + this.z2;
    this.z2 = this.b2 * x - this.a2 * y;
    return y;
  }
}

// 4th-order Butterworth low-pass (two cascaded biquads).
class LowPass4 {
  constructor() { this.a = new Biquad(); this.b = new Biquad(); }
  set(fc, fs) { this.a.lowpass(fc, fs, 0.5412); this.b.lowpass(fc, fs, 1.3066); }
  run(x) { return this.b.run(this.a.run(x)); }
}

// Linear-interpolated read from a circular buffer.
function readFrac(buf, pos) {
  const n = buf.length;
  let p = pos % n; if (p < 0) p += n;
  if (p >= n) p = 0;
  const i0 = Math.floor(p), f = p - i0;
  const i1 = i0 + 1 === n ? 0 : i0 + 1;
  return buf[i0] + (buf[i1] - buf[i0]) * f;
}

// 32-bit LCG shared by every random decision (same sequence in the C++ port).
class Rng {
  constructor(seed) { this.s = seed >>> 0; }
  next() { this.s = (Math.imul(this.s, 1103515245) + 12345) >>> 0; return this.s / 4294967296; }
}

// Hand-written iterative radix-2 FFT (in place, decimation in time). The C++ port
// uses the same code; no library FFT, so both produce the same numbers.
class FFT {
  constructor(n) {
    this.n = n;
    this.cos = new Float64Array(n / 2);
    this.sin = new Float64Array(n / 2);
    for (let i = 0; i < n / 2; i++) { this.cos[i] = Math.cos((TAU * i) / n); this.sin[i] = Math.sin((TAU * i) / n); }
    this.rev = new Int32Array(n);
    let bits = 0; while ((1 << bits) < n) bits++;
    for (let i = 0; i < n; i++) {
      let r = 0;
      for (let b = 0; b < bits; b++) if (i & (1 << b)) r |= 1 << (bits - 1 - b);
      this.rev[i] = r;
    }
  }
  // inverse = true: e^{+j}, no 1/n scaling (the caller scales)
  run(re, im, inverse) {
    const n = this.n;
    for (let i = 0; i < n; i++) {
      const j = this.rev[i];
      if (j > i) {
        let t = re[i]; re[i] = re[j]; re[j] = t;
        t = im[i]; im[i] = im[j]; im[j] = t;
      }
    }
    const sgn = inverse ? 1 : -1;
    for (let size = 2; size <= n; size *= 2) {
      const half = size / 2, step = n / size;
      for (let i = 0; i < n; i += size) {
        for (let j = 0; j < half; j++) {
          const wr = this.cos[j * step], wi = sgn * this.sin[j * step];
          const k = i + j, l = k + half;
          const tr = wr * re[l] - wi * im[l];
          const ti = wr * im[l] + wi * re[l];
          re[l] = re[k] - tr; im[l] = im[k] - ti;
          re[k] += tr; im[k] += ti;
        }
      }
    }
  }
}

// Delay-line pitch shifter, SOLA style: one read head moves through the buffer at
// `ratio`; when it runs out of room a second head takes over through a short linear
// crossfade, at a jump (about W/2 plus a search range) chosen by normalised
// correlation so that periodic sounds keep their phase and pitch. Delays are measured
// back from w (the newest sample) beyond `base`.
class Shifter {
  constructor() { this.d = -1; this.dOld = 0; this.x = 1; }
  run(buf, w, base, ratio, W) {
    const N = buf.length;
    const X = Math.floor(W * 0.25);
    const step = 1 - ratio;
    if (this.d < 0) this.d = ratio > 1 ? W : 2;
    this.d += step;
    if (this.x < 1) {
      this.dOld += step;
      this.x += 1 / X;
      if (this.x >= 1) this.x = 1;
    } else if ((ratio > 1 && this.d - X * (ratio - 1) < 2) || (ratio < 1 && this.d + X * (1 - ratio) > W)) {
      const J = Math.floor(W * 0.5), S = Math.floor(W * 0.3);
      const M = X < 256 ? X : 256;
      const pA = w - base - this.d;
      let best = -1, bestScore = -1e300;
      for (let k = 0; k <= S; k += 2) {
        const cand = ratio > 1 ? this.d + J + k : this.d - J - k;
        if (cand < 2) break;
        const pB = w - base - cand;
        let c = 0, e = 1e-9;
        for (let j = 0; j < M; j += 4) {
          let qa = Math.floor(pA + j * ratio) % N; if (qa < 0) qa += N;
          let qb = Math.floor(pB + j * ratio) % N; if (qb < 0) qb += N;
          const b = buf[qb];
          c += buf[qa] * b; e += b * b;
        }
        const score = c / Math.sqrt(e);
        if (score > bestScore) { bestScore = score; best = cand; }
      }
      if (best < 0) best = ratio > 1 ? this.d + J : 2;
      this.dOld = this.d; this.d = best; this.x = 0;
    }
    const lim = N - base - 2;
    if (this.d > lim) this.d = lim; else if (this.d < 1) this.d = 1;
    let y = readFrac(buf, w - base - this.d);
    if (this.x < 1) y = readFrac(buf, w - base - this.dOld) * (1 - this.x) + y * this.x;
    return y;
  }
}

// ───────────────────────── Wet channel: SOUP ─────────────────────────
// Spectral reverb: STFT analysis, per-bin magnitude memory that decays, resynthesis
// with phase-vocoder rotations (no atan2) plus random phase jitter.
// MODIFY CCW: diffuse (random phases, smeared spectrum, highs decay first).
// MODIFY CW: synthetic (coherent sustained partials, peaks emphasised, flat decay).
class Soup {
  constructor(fs) {
    this.fs = fs;
    this.fft = new FFT(SOUP_N);
    this.win = new Float64Array(SOUP_N);
    for (let i = 0; i < SOUP_N; i++) this.win[i] = 0.5 - 0.5 * Math.cos((TAU * i) / SOUP_N);
    this.inRing = new Float32Array(SOUP_N);
    this.inW = 0;
    this.hop = 0;
    this.outL = new Float64Array(SOUP_N);
    this.outR = new Float64Array(SOUP_N);
    this.outPos = 0;
    this.re = new Float64Array(SOUP_N); this.im = new Float64Array(SOUP_N);
    this.re2 = new Float64Array(SOUP_N); this.im2 = new Float64Array(SOUP_N);
    this.mag = new Float64Array(SOUP_BINS);
    this.amp = new Float64Array(SOUP_BINS);
    this.pr = new Float64Array(SOUP_BINS); this.pi = new Float64Array(SOUP_BINS);
    this.rr = new Float64Array(SOUP_BINS); this.ri = new Float64Array(SOUP_BINS);
    this.xr = new Float64Array(SOUP_BINS); this.xi = new Float64Array(SOUP_BINS);
    this.gk = new Float64Array(SOUP_BINS);
    // expected rotation of bin k per hop (hop = N/4): e^{j·πk/2}, exact
    const qr = [1, 0, -1, 0], qi = [0, 1, 0, -1];
    for (let k = 0; k < SOUP_BINS; k++) { this.pr[k] = 1; this.pi[k] = 0; this.rr[k] = qr[k & 3]; this.ri[k] = qi[k & 3]; }
    this.jr = new Float64Array(256); this.ji = new Float64Array(256);
    for (let i = 0; i < 256; i++) { this.jr[i] = Math.cos((TAU * i) / 256); this.ji[i] = Math.sin((TAU * i) / 256); }
    this.rng = new Rng(0x50a9);
    this.keyT = -1; this.keyM = -1; this.keyInf = -1;
    this.out = [0, 0];
  }
  updateDecay(time, m, infinite) {
    if (infinite === this.keyInf && Math.abs(time - this.keyT) < 1e-3 && Math.abs(m - this.keyM) < 1e-3) return;
    this.keyT = time; this.keyM = m; this.keyInf = infinite;
    if (infinite) { for (let k = 0; k < SOUP_BINS; k++) this.gk[k] = 1; return; }
    const t60 = expMap(time, 0.4, 40);
    const g = Math.pow(10, (-3 * SOUP_HOP) / (t60 * this.fs));
    const damp = 1.5 * (1 - m);
    for (let k = 0; k < SOUP_BINS; k++) this.gk[k] = Math.pow(g, 1 + (damp * k) / SOUP_BINS);
  }
  frame(inGain, m, spread) {
    const N = SOUP_N, re = this.re, im = this.im;
    for (let i = 0; i < N; i++) {
      let idx = this.inW + i; if (idx >= N) idx -= N;
      re[i] = this.inRing[idx] * this.win[i]; im[i] = 0;
    }
    this.fft.run(re, im, false);
    const jit = (1 - m) * (1 - m);
    for (let k = 0; k < SOUP_BINS; k++) {
      const xr = re[k] * inGain, xi = im[k] * inGain;
      const mag = Math.sqrt(xr * xr + xi * xi);
      const held = this.mag[k] * this.gk[k];
      if (mag > held) {
        this.mag[k] = mag;
        const zr = xr * this.xr[k] + xi * this.xi[k];
        const zi = xi * this.xr[k] - xr * this.xi[k];
        const zm = Math.sqrt(zr * zr + zi * zi);
        if (zm > 1e-12) { this.rr[k] = zr / zm; this.ri[k] = zi / zm; }
      } else this.mag[k] = held;
      this.xr[k] = xr; this.xi[k] = xi;
      let pr = this.pr[k] * this.rr[k] - this.pi[k] * this.ri[k];
      let pi = this.pr[k] * this.ri[k] + this.pi[k] * this.rr[k];
      const j = Math.floor((this.rng.next() - 0.5) * jit * 256) & 255;
      const qr = pr * this.jr[j] - pi * this.ji[j];
      const qi = pr * this.ji[j] + pi * this.jr[j];
      const pn = Math.sqrt(qr * qr + qi * qi);
      if (pn > 1e-12) { pr = qr / pn; pi = qi / pn; } else { pr = 1; pi = 0; }
      this.pr[k] = pr; this.pi[k] = pi;
    }
    // magnitude shaping: blur (diffuse) ↔ peak emphasis (synthetic)
    const blurW = 0.7 * jit;
    for (let k = 0; k < SOUP_BINS; k++) {
      const a = this.mag[k];
      const lo = k > 0 ? this.mag[k - 1] : a, hi = k < SOUP_BINS - 1 ? this.mag[k + 1] : a;
      const b = (lo + 2 * a + hi) * 0.25;
      let mx = a; if (lo > mx) mx = lo; if (hi > mx) mx = hi;
      let pk = mx > 1e-20 ? a / mx : 0;
      pk = pk * pk; pk = pk * pk;
      this.amp[k] = (blurW * b + (1 - blurW) * a) * ((1 - m) + m * pk);
    }
    this.synth(this.outL, 0);
    if (spread) this.synth(this.outR, 1);
    else { const o = this.outPos; for (let i = 0; i < N; i++) { let q = o + i; if (q >= N) q -= N; this.outR[q] = this.outL[q]; } }
  }
  synth(acc, rot) {
    const N = SOUP_N, re = this.re2, im = this.im2;
    for (let k = 0; k < SOUP_BINS; k++) {
      let pr = this.pr[k], pi = this.pi[k];
      if (rot) {
        const j = (k * 37) & 255;
        const tr = pr * this.jr[j] - pi * this.ji[j];
        pi = pr * this.ji[j] + pi * this.jr[j]; pr = tr;
      }
      re[k] = this.amp[k] * pr; im[k] = this.amp[k] * pi;
    }
    im[0] = 0; im[N / 2] = 0;
    for (let k = 1; k < N / 2; k++) { re[N - k] = re[k]; im[N - k] = -im[k]; }
    this.fft.run(re, im, true);
    const scale = 1.1 / (N * 1.5);
    const o = this.outPos;
    for (let i = 0; i < N; i++) {
      let q = o + i; if (q >= N) q -= N;
      acc[q] += re[i] * this.win[i] * scale;
    }
  }
  tick(input, time, m, freeze, infinite, spread) {
    this.inRing[this.inW] = input;
    if (++this.inW >= SOUP_N) this.inW = 0;
    const o = this.outPos;
    this.out[0] = this.outL[o]; this.out[1] = this.outR[o];
    this.outL[o] = 0; this.outR[o] = 0;
    if (++this.outPos >= SOUP_N) this.outPos = 0;
    if (++this.hop >= SOUP_HOP) {
      this.hop = 0;
      this.updateDecay(time, m, freeze || infinite);
      this.frame(freeze ? 0 : 1, m, spread);
    }
    return this.out;
  }
}

// ───────────────────────── Wet channel: RELAY ─────────────────────────
// A delay whose repeats do not fade: D[n] = x[n] + g·D[n−T], output = Σ taps at k·T.
// Finite: g = 0 and taps 1..N at unity. Infinite: g = 1, tap 1 only.
class Relay {
  constructor(fs) {
    this.fs = fs;
    this.buf = new Float32Array(Math.ceil(RELAY_TMAX_SEC * fs * RELAY_TAPS) + 16);
    this.w = 0;
    this.T = -1;
    this.tapG = new Float64Array(RELAY_TAPS);
    this.infG = 0; this.frzG = 0; this.soloG = 0;
    this.slew = 1 - Math.exp(-1 / (0.08 * fs));
    this.out = [0, 0];
  }
  snap(Tt, nRep) {
    this.T = Tt;
    for (let k = 0; k < RELAY_TAPS; k++) this.tapG[k] = nRep > RELAY_TAPS ? (k === 0 ? 1 : 0) : (k < nRep ? 1 : 0);
    this.infG = nRep > RELAY_TAPS ? 1 : 0;
    this.soloG = this.infG;
  }
  tick(input, Tt, nRep, freeze, spread) {
    if (this.T < 0) this.snap(Tt, nRep);
    this.T += (Tt - this.T) * this.slew;
    const T = this.T;
    const inf = nRep > RELAY_TAPS;
    this.infG += ((inf ? 1 : 0) - this.infG) * 0.002;
    this.frzG += ((freeze ? 1 : 0) - this.frzG) * 0.005;
    const solo = inf || freeze;
    this.soloG += ((solo ? 1 : 0) - this.soloG) * 0.002;
    const fb = readFrac(this.buf, this.w - T);
    let v = input * (1 - this.frzG) + fb * (this.infG + (1 - this.infG) * this.frzG);
    if (v > 1 || v < -1) v = Math.tanh(v);
    this.buf[this.w] = v;
    let l = 0, r = 0;
    for (let k = 0; k < RELAY_TAPS; k++) {
      const tgt = solo ? (k === 0 ? 1 : 0) : (k < nRep ? 1 : 0);
      this.tapG[k] += (tgt - this.tapG[k]) * 0.002;
      if (this.tapG[k] > 1e-5) {
        const s = readFrac(this.buf, this.w - (k + 1) * T) * this.tapG[k];
        if (!spread) { l += s; r += s; }
        else if (k & 1) r += s;
        else l += s;
      }
    }
    if (spread && this.soloG > 1e-5) r += readFrac(this.buf, this.w - 1.5 * T) * this.soloG;
    if (++this.w >= this.buf.length) this.w = 0;
    this.out[0] = l * 0.8; this.out[1] = r * 0.8;
    return this.out;
  }
}

// ───────────────────────── Wet channel: FLIP ──────────────────────────
// Chordcaster: up to four pitch-shifted voices, voice k delayed by k·lag.
// Freeze recirculates the input buffer so the chord pattern repeats.
class Flip {
  constructor(fs) {
    this.fs = fs;
    this.buf = new Float32Array(Math.ceil(FLIP_BUF_SEC * fs));
    this.w = 0;
    this.sh = [new Shifter(), new Shifter(), new Shifter(), new Shifter()];
    this.ratio = new Float64Array(4).fill(1);
    this.gain = new Float64Array(4);
    this.norm = 1;
    this.lag = -1;
    this.frzP = 0; this.frzG = 0; this.wasFrozen = false;
    this.W = 0.05 * fs;
    this.slew = 1 - Math.exp(-1 / (0.05 * fs));
    this.out = [0, 0];
  }
  tick(input, lagT, chord, freeze, spread) {
    const n = chord.length;
    if (this.lag < 0) {
      this.lag = lagT;
      for (let k = 0; k < 4; k++) this.gain[k] = k < n ? 1 : 0;
      this.norm = 0.9 / Math.sqrt(n);
    }
    this.lag += (lagT - this.lag) * this.slew;
    if (freeze && !this.wasFrozen) {
      let P = Math.floor(n * this.lag + 0.5);
      const minP = Math.floor(0.25 * this.fs);
      if (P < minP) P = minP;
      if (P > this.buf.length - 2) P = this.buf.length - 2;
      this.frzP = P;
    }
    this.wasFrozen = freeze;
    this.frzG += ((freeze ? 1 : 0) - this.frzG) * 0.005;
    const N = this.buf.length;
    let rp = this.w - this.frzP; if (rp < 0) rp += N;
    const v = this.frzP > 0 ? input * (1 - this.frzG) + this.buf[rp] * this.frzG : input;
    this.buf[this.w] = v;
    this.norm += (0.9 / Math.sqrt(n) - this.norm) * 0.002;
    let l = 0, r = 0;
    for (let k = 0; k < 4; k++) {
      if (k < n) this.ratio[k] = INTERVAL_RATIOS[chord[k]];
      this.gain[k] += ((k < n ? 1 : 0) - this.gain[k]) * 0.002;
      if (this.gain[k] > 1e-5) {
        const y = this.sh[k].run(this.buf, this.w, k * this.lag, this.ratio[k], this.W) * this.gain[k];
        if (!spread) { l += y; r += y; }
        else {
          const pan = k === 0 ? -0.8 : k === 1 ? 0.8 : k === 2 ? -0.35 : 0.35;
          l += y * (pan > 0 ? 1 - pan : 1);
          r += y * (pan < 0 ? 1 + pan : 1);
        }
      }
    }
    if (++this.w >= N) this.w = 0;
    this.out[0] = l * this.norm; this.out[1] = r * this.norm;
    return this.out;
  }
}

// ─────────────────────────────── Core ─────────────────────────────────
const PARAM_DEFAULTS = {
  // knobs
  time: 0.5, mix: 0.5, length: 0.5, modWet: 0.5, clock: 1, modLoop: 0.5,
  // toggles: wet Soup/Relay/Flip, routing In/In+Loop/Loop, loop Burst/Radio/Mask
  wetMode: 0, routing: 0, loopMode: 1,
  // customize dips (manual p.44-45), all off by default (p.5)
  miso: 0, spread: 0, dryKill: 0, latch: 0, trails: 0, smooth: 0, dryGlue: 0, half: 0,
  // ramping dips (p.46-47) and ramp speed (MIX while ramping, CC20)
  rampTime: 0, rampModWet: 0, rampClock: 0, rampModLoop: 0, rampLength: 0, bounce: 0, sweep: 0, polarity: 0,
  rampSpeed: 0.5,
  // hidden options (p.14-17), at their documented defaults
  cross: 0, balance: 0.5, fade: 1, eq: 0.5, glue: 0.2, blend: 0,
  sync: 1, spreadSel: 1, inputMod: 1, volume: 0.5,
};
const PRESET_KEYS = Object.keys(PARAM_DEFAULTS);
const INT_PARAMS = ['wetMode', 'routing', 'loopMode', 'sync', 'spreadSel', 'inputMod'];

function snapshotParams(p) {
  const o = {};
  for (const k of PRESET_KEYS) o[k] = p[k];
  return o;
}
function applySnapshot(core, snap) {
  for (const k of PRESET_KEYS) if (typeof snap[k] === 'number') core.setParam(k, snap[k]);
}

class BadMoodCore {
  constructor(fs) {
    this.fs = fs;
    this.p = Object.assign({}, PARAM_DEFAULTS);
    this.s = { time: 0, mix: 0, length: 0, modWet: 0, modLoop: 0, balance: 0, cross: 0, blend: 0, glue: 0, volume: 0, eq: 0, clockHz: 0 };
    this.started = false;
    this.hostCount = 0;
    this.phase = 0;
    this.clockRatio = 1;
    this.aa = new LowPass4();
    this.reconL = new LowPass4();
    this.reconR = new LowPass4();
    this.filterClock = -1;
    this.eqL = new Biquad(); this.eqR = new Biquad();
    this.eqKey = -1;
    this.glueKey = -1;
    this.gDrive = 1; this.gBias = 0; this.gTB = 0; this.gMake = 1; this.gDestroy = 0; this.gQ = 2048;
    this.prev = [0, 0];
    this.cur = [0, 0];
    this.tbG = 0;
    this.vol = 1;
    this.rampShape = 1;

    // wet channel
    this.soup = new Soup(fs);
    this.relay = new Relay(fs);
    this.flip = new Flip(fs);
    this.wetOn = false;
    this.freeze = false;
    this.wetIn = 0; this.wetOut = 0;
    this.modeGain = new Float64Array(3);
    this.xW = 1; this.lW = 0; this.loopDirect = 1;

    // micro-looper
    this.buf = new Float32Array(LOOP_SAMPLES);
    this.L = LOOP_SAMPLES;
    this.pos = 0;            // master position: always-listening write head and pure-loop read head
    this.seam = 0;           // where recording stopped (fades hide the jump)
    this.cycles = 0;
    this.loopPlay = false;   // false: recording (bypassed), true: playback
    this.overdub = false;
    this.loopGain = 0;
    this.lmGain = new Float64Array(3);
    this.lastLoopMode = -1;
    this.blockE = new Float64Array(Math.ceil(LOOP_SAMPLES / BLK));
    this.eAcc = 0;
    this.rng = new Rng(22222);
    // Burst
    this.slices = new Int32Array(8);
    this.cand = new Int32Array(8);
    this.nSlices = 1;
    this.stepIdx = 0; this.stepPh = 0; this.stepLen = 0; this.curSlice = 0;
    this.inEnv = 0; this.scramble = false; this.scrHold = 0;
    // Radio
    this.stActive = new Int32Array(5);
    this.tp = 0; this.tapeSpeed = 1;
    this.amPos = 0; this.amPh = new Float64Array(4); this.amStart = new Float64Array(4);
    this.orPos = new Float64Array(6); this.orEnv = new Float64Array(6); this.orTgt = new Float64Array(6); this.orCnt = new Int32Array(6);
    this.orNorm = 0.8;
    this.sgPh = new Float64Array(4); this.sgJit = new Float64Array(4); this.sgLp = 0;
    this.dnPos = 0; this.dnCnt = 0; this.dnIdx = 0;
    this.rStatic = 0; this.rWhPh = 0; this.rLp = 0; this.rHp = 0;
    this.so = [0, 0];
    this.seamF = Math.min(0.004 * fs, LOOP_SAMPLES / 4);
    // Mask
    this.maskRing = new Float32Array(Math.ceil(0.25 * fs));
    this.maskW = 0;
    this.maskEnv = 0; this.maskG = 0;
    this.maskSh = [new Shifter(), new Shifter()];
    this.ringPh = 0; this.crushCnt = 0; this.crushV = 0;
    this.svfLow = 0; this.svfBand = 0;
    this.revPh = 0; this.revStart = 0;
    // Cross
    const cl = Math.ceil(0.03 * fs) + 4;
    this.crossBuf = [new Float32Array(cl), new Float32Array(cl), new Float32Array(cl), new Float32Array(cl)];
    this.crossW = 0;
    this.crossEnv = 0; this.crossPh = 0; this.crossD = 0;
    // decorrelation for spread loop modes
    this.decorBuf = new Float32Array(Math.ceil(0.02 * fs) + 4);
    this.decorW = 0;

    // ramping
    this.rampPh = 1; this.rampTrig = false; this.lastEngaged = false;

    // footswitches: 0 = left (wet), 1 = right (loop)
    this.sw = [this.newSwitch(), this.newSwitch()];
    this.comboT = 0; this.comboFirst = 0; this.comboFirstLong = false;
    this.hidden = false; this.presetDone = false;
    this.comboTaps = [];
    this.trueBypass = false;
    this.engaged = 0;
    this.events = [];
    this.peakIn = 0; this.peakOut = 0;
    this.lastStations = [0, -1, 0];
  }

  newSwitch() { return { down: false, t: 0, held: false, combo: false, shift: false, momentary: false }; }

  setParam(name, value) {
    if (!(name in this.p)) return;
    this.p[name] = INT_PARAMS.indexOf(name) >= 0 ? Math.floor(value + 0.5) : value;
  }

  // Direct state commands (MIDI CC 102/103/105/106/55 in the plugin).
  command(name, on) {
    if (name === 'wet') { this.wetOn = on; if (!on) this.freeze = false; }
    else if (name === 'loop') { if (on && !this.loopPlay) this.engageLoop(); else if (!on) { this.loopPlay = false; this.overdub = false; } }
    else if (name === 'freeze') { this.freeze = on; if (on) this.wetOn = true; }
    else if (name === 'overdub') { if (on && !this.loopPlay) this.engageLoop(); this.overdub = on; }
    else if (name === 'trueBypass') this.trueBypass = on;
  }

  // ── footswitches ──
  // which: 'wet' (left) | 'loop' (right); action: 'down' | 'up' | 'shift'
  // 'shift' = a knob moved while this switch is held (EFFECT VOLUME, ramp MIX):
  // the switch then does nothing on release.
  footswitch(which, action) {
    const w = which === 'wet' ? 0 : 1;
    const f = this.sw[w], o = this.sw[1 - w];
    const now = this.hostCount;
    if (action === 'down') {
      if (f.down) return;
      f.down = true; f.t = now; f.held = false; f.combo = false; f.shift = false; f.momentary = false;
      if (o.down) {
        f.combo = true;
        if (!o.combo) { this.cancelHold(1 - w); o.combo = true; }
        this.comboT = now;
        this.comboFirst = 1 - w;
        this.comboFirstLong = now - o.t >= PRESET_HOLD_SEC * this.fs;
        this.hidden = false; this.presetDone = false;
      }
      return;
    }
    if (action === 'shift') {
      if (f.down && !f.combo && !f.shift) { f.shift = true; this.cancelHold(w); }
      return;
    }
    if (!f.down) return;
    f.down = false;
    if (f.combo) {
      if (!o.down) {
        if (!this.hidden && !this.presetDone && now - this.comboT < HOLD_SEC * this.fs) this.comboTap(now);
        if (this.hidden) this.events.push('hidden-off');
        this.hidden = false;
      }
      return;
    }
    if (f.shift) return;
    if (this.trueBypass) {
      if (!f.held) { this.trueBypass = false; this.events.push('true-bypass-off'); }
      return;
    }
    if (w === 0) {
      if (f.held) {
        if (!this.p.latch) { this.freeze = false; if (f.momentary) this.wetOn = false; }
      } else {
        this.wetOn = !this.wetOn;
        if (!this.wetOn) this.freeze = false;
      }
    } else if (f.held) {
      if (!this.p.latch) this.overdub = false;
    } else if (!this.loopPlay) {
      this.engageLoop();
    } else {
      // tap while playing: back to the always-listening state, which REPLACEs the loop
      this.loopPlay = false; this.overdub = false;
      this.events.push('loop-rec');
    }
  }

  cancelHold(w) {
    const f = this.sw[w];
    if (!f.held) return;
    if (w === 0) {
      if (this.p.latch) this.freeze = !this.freeze; else this.freeze = false;
      if (f.momentary) this.wetOn = false;
    } else if (this.p.latch) this.overdub = !this.overdub;
    else this.overdub = false;
    f.held = false;
  }

  comboTap(now) {
    const win = COMBO_TAP_WINDOW_SEC * this.fs;
    this.comboTaps.push(now);
    while (this.comboTaps.length > 3) this.comboTaps.shift();
    if (this.comboTaps.length === 3 && now - this.comboTaps[0] <= win) {
      this.trueBypass = !this.trueBypass;
      this.comboTaps.length = 0;
      this.events.push(this.trueBypass ? 'true-bypass-on' : 'true-bypass-off');
    }
  }

  updateSwitches() {
    const now = this.hostCount;
    const holdN = HOLD_SEC * this.fs;
    for (let w = 0; w < 2; w++) {
      const f = this.sw[w];
      if (!f.down || f.held || f.combo || f.shift || this.trueBypass || now - f.t < holdN) continue;
      f.held = true;
      if (w === 0) {
        if (!this.wetOn) { this.wetOn = true; f.momentary = true; }
        this.freeze = this.p.latch ? !this.freeze : true;
      } else {
        if (!this.loopPlay) this.engageLoop();
        this.overdub = this.p.latch ? !this.overdub : true;
      }
    }
    const a = this.sw[0], b = this.sw[1];
    if (a.down && b.down && a.combo && b.combo) {
      if (!this.hidden && !this.presetDone && now - this.comboT >= holdN) { this.hidden = true; this.events.push('hidden-on'); }
      if (this.comboFirstLong && !this.presetDone && now - this.comboT >= PRESET_HOLD_SEC * this.fs) {
        this.presetDone = true;
        this.hidden = false;
        // right switch first saves the right slot (Preset 2), left first the left slot (Preset 1)
        this.events.push(this.comboFirst === 1 ? 'preset-save-2' : 'preset-save-1');
      }
    }
  }

  engageLoop() {
    this.loopPlay = true;
    this.seam = this.pos;
    this.resetLoopModes();
    this.events.push('loop-play');
  }

  // ── micro-looper ──
  loopRead(q) {
    const L = this.L;
    let x = q % L; if (x < 0) x += L;
    if (x >= L) x = 0;
    const i0 = Math.floor(x), f = x - i0;
    const i1 = i0 + 1 >= L ? 0 : i0 + 1;
    const v = this.buf[i0] + (this.buf[i1] - this.buf[i0]) * f;
    let d = x - this.seam; if (d < 0) d += L;
    const F = this.seamF;
    let g = d / F; const g2 = (L - d) / F;
    if (g2 < g) g = g2;
    if (g > 1) g = 1;
    return v * g;
  }

  resetLoopModes() {
    // Burst
    this.computeSlices();
    this.stepIdx = this.nSlices - 1; this.stepPh = 0; this.stepLen = 0; this.curSlice = 0;
    // Radio
    for (let i = 0; i < 5; i++) this.stActive[i] = 0;
    // Mask
    this.maskG = 0;
  }

  // Burst: pick up to 8 "unique sounds" (energy jumps) in the loop, in loop order.
  computeSlices() {
    const L = this.L;
    const nb = Math.ceil(L / BLK);
    const floorE = BLK * 1e-6;
    const minSep = Math.max(2, Math.floor(nb / 16));
    let n = 0;
    for (let pass = 0; pass < 8; pass++) {
      let best = -1, bestS = 3;
      for (let b = 0; b < nb; b++) {
        const e = this.blockE[b];
        if (e <= floorE) continue;
        const prev = this.blockE[b === 0 ? nb - 1 : b - 1];
        const s = e / (prev + floorE);
        if (s <= bestS) continue;
        let near = false;
        for (let c = 0; c < n; c++) {
          let d = b - this.cand[c]; if (d < 0) d = -d;
          if (nb - d < d) d = nb - d;
          if (d < minSep) { near = true; break; }
        }
        if (!near) { best = b; bestS = s; }
      }
      if (best < 0) break;
      this.cand[n++] = best;
    }
    if (n === 0) {
      // nothing stands out: four equal steps
      for (let i = 0; i < 4; i++) this.slices[i] = Math.floor((i * L) / 4);
      this.nSlices = 4;
      return;
    }
    for (let i = 1; i < n; i++) { // insertion sort, ascending position
      const v = this.cand[i]; let j = i - 1;
      while (j >= 0 && this.cand[j] > v) { this.cand[j + 1] = this.cand[j]; j--; }
      this.cand[j + 1] = v;
    }
    for (let i = 0; i < n; i++) this.slices[i] = this.cand[i] * BLK;
    this.nSlices = n;
  }

  burstTick(x) {
    const fs = this.fs;
    const L = this.L;
    const a = Math.abs(x);
    this.inEnv += (a - this.inEnv) * (a > this.inEnv ? 0.01 : 0.0005);
    const thr = expMap(1 - this.s.modLoop, 0.01, 0.5);
    if (this.inEnv > thr) { this.scramble = true; this.scrHold = Math.floor(0.25 * fs); }
    else if (this.scramble && --this.scrHold <= 0) this.scramble = false;
    const di = clamp(Math.floor(this.s.length * 8 + 0.5), 0, 8);
    let D = Math.floor(L / BURST_DIVS[di]); if (D < 64) D = 64;
    if (this.stepPh >= this.stepLen) {
      this.stepPh = 0;
      if (this.scramble) {
        this.curSlice = Math.floor(this.rng.next() * this.nSlices);
        this.stepLen = this.rng.next() < 0.35 ? Math.floor(D / 2) : D;
      } else {
        if (this.stepIdx + 1 >= this.nSlices) { this.computeSlices(); this.stepIdx = -1; }
        this.stepIdx++;
        this.curSlice = this.stepIdx;
        this.stepLen = D;
      }
    }
    let F = 0.003 * fs; if (F > this.stepLen / 4) F = this.stepLen / 4;
    let e = this.stepPh / F; const e2 = (this.stepLen - this.stepPh) / F;
    if (e2 < e) e = e2;
    if (e > 1) e = 1;
    const y = this.loopRead(this.slices[this.curSlice] + this.stepPh) * e;
    this.stepPh++;
    const right = (this.curSlice & 1) === 1;
    this.so[0] = right ? y * 0.45 : y; this.so[1] = right ? y : y * 0.45;
    return y;
  }

  // Radio station tick; writes stereo into this.so and returns mono.
  stationTick(st, spread) {
    const fs = this.fs, L = this.L, len = this.s.length;
    let y = 0, l = 0, r = 0;
    if (!this.stActive[st]) {
      this.stActive[st] = 1;
      if (st === 0) { this.tp = this.pos; this.tapeSpeed = speedMap(len, 0.04); }
      else if (st === 1) { this.amPos = this.pos; for (let g = 0; g < 4; g++) { this.amPh[g] = g * 0.25; this.amStart[g] = this.pos; } }
      else if (st === 2) {
        for (let j = 0; j < 6; j++) { this.orPos[j] = this.pos + ORCH_OFFS[j] * L; this.orEnv[j] = j === 0 ? 1 : 0; this.orTgt[j] = j === 0 ? 1 : 0; this.orCnt[j] = 0; }
      } else if (st === 3) { for (let g = 0; g < 4; g++) { this.sgPh[g] = g * 0.25; this.sgJit[g] = 0; } this.sgLp = 0; }
      else { this.dnPos = this.pos; this.dnCnt = 0; this.dnIdx = 0; }
    }
    if (st === 0) {
      // TAPE: the whole loop, LENGTH = speed and direction
      y = this.loopRead(this.tp);
      this.tapeSpeed += (speedMap(len, 0.04) - this.tapeSpeed) * 0.0005;
      this.tp += this.tapeSpeed;
      if (this.tp >= L) this.tp -= L; else if (this.tp < 0) this.tp += L;
      l = y; r = y;
    } else if (st === 1) {
      // AMBIENT: granular slow-down at the original pitch, LENGTH = speed 1/16x → 1x
      const speed = Math.pow(2, -4 * (1 - len));
      let G = 0.2 * fs; if (G > L / 2) G = L / 2;
      const J = ((1 - speed) * 0.5 + 0.02) * G;
      for (let g = 0; g < 4; g++) {
        this.amPh[g] += 1 / G;
        if (this.amPh[g] >= 1) { this.amPh[g] -= 1; this.amStart[g] = this.amPos + (this.rng.next() - 0.5) * 2 * J; }
        const s = Math.sin(Math.PI * this.amPh[g]);
        const v = this.loopRead(this.amStart[g] + this.amPh[g] * G) * s * s * 0.5;
        y += v;
        if (g & 1) { l += v * 0.6; r += v; } else { l += v; r += v * 0.6; }
      }
      this.amPos += speed;
      if (this.amPos >= L) this.amPos -= L;
    } else if (st === 2) {
      // ORCHESTRAL: LENGTH = number of voices (1..6), each a playhead at its own pitch,
      // offset and pan, fading in and out at random
      const V = 1 + Math.floor(len * 5.999);
      for (let j = 0; j < 6; j++) {
        const active = j < V;
        if (j === 0) this.orTgt[j] = 1;
        else if (active) {
          if (--this.orCnt[j] <= 0) {
            this.orCnt[j] = Math.floor((1 + 3 * this.rng.next()) * fs);
            this.orTgt[j] = this.rng.next() < 0.3 ? 0 : 0.4 + 0.6 * this.rng.next();
          }
        } else this.orTgt[j] = 0;
        this.orEnv[j] += (this.orTgt[j] - this.orEnv[j]) * 0.00005;
        if (this.orEnv[j] > 1e-4) {
          const v = this.loopRead(this.orPos[j]) * this.orEnv[j];
          y += v;
          const pan = spread ? ORCH_PANS[j] : 0;
          l += v * (pan > 0 ? 1 - pan : 1);
          r += v * (pan < 0 ? 1 + pan : 1);
        }
        this.orPos[j] += ORCH_RATIOS[j];
        if (this.orPos[j] >= L) this.orPos[j] -= L;
      }
      this.orNorm += (0.8 / Math.sqrt(V) - this.orNorm) * 0.001;
      y *= this.orNorm; l *= this.orNorm; r *= this.orNorm;
    } else if (st === 3) {
      // SHOEGAZE: eight frozen moments; LENGTH selects (and blends) between them, with
      // an octave-up layer and a layer from the opposite moment stacked on top
      const sel = len * 7;
      const i0 = Math.floor(sel), fr = sel - i0;
      const i1 = i0 < 7 ? i0 + 1 : 7;
      const i2 = (i0 + 4) & 7;
      let G = 0.25 * fs; if (G > L / 8) G = L / 8;
      const c0 = this.seam + (i0 + 0.5) * L / 8, c1 = this.seam + (i1 + 0.5) * L / 8, c2 = this.seam + (i2 + 0.5) * L / 8;
      for (let g = 0; g < 4; g++) {
        this.sgPh[g] += 1 / G;
        if (this.sgPh[g] >= 1) { this.sgPh[g] -= 1; this.sgJit[g] = (this.rng.next() - 0.5) * 0.4 * G; }
        const ph = this.sgPh[g], jo = this.sgJit[g] - 0.5 * G;
        const s = Math.sin(Math.PI * ph);
        let v = this.loopRead(c0 + jo + ph * G) * (1 - fr);
        if (fr > 0) v += this.loopRead(c1 + jo + ph * G) * fr;
        v += this.loopRead(c0 + jo + 2 * ph * G) * 0.35;
        v += this.loopRead(c2 + jo + ph * G) * 0.25;
        v *= s * s * 0.45;
        y += v;
        if (g & 1) { l += v * 0.5; r += v; } else { l += v; r += v * 0.5; }
      }
      this.sgLp += (y - this.sgLp) * 0.35;
      const k = y !== 0 ? this.sgLp / y : 1;
      y = this.sgLp; l *= k; r *= k;
    } else {
      // DANCE: rotates normal → half → double speed; LENGTH = rotation speed
      const k = clamp(Math.floor(len * 4 + 0.5), 0, 4);
      let seg = Math.floor(L / (1 << k)); if (seg < 64) seg = 64;
      if (this.dnCnt >= seg) { this.dnCnt = 0; this.dnIdx = (this.dnIdx + 1) % 3; this.dnPos = this.pos; }
      let F = 0.004 * fs; if (F > seg / 4) F = seg / 4;
      let e = this.dnCnt / F; const e2 = (seg - this.dnCnt) / F;
      if (e2 < e) e = e2;
      if (e > 1) e = 1;
      y = this.loopRead(this.dnPos) * e;
      this.dnPos += DANCE_SPEEDS[this.dnIdx];
      if (this.dnPos >= L) this.dnPos -= L;
      this.dnCnt++;
      l = y; r = y;
    }
    this.so[0] = l; this.so[1] = r;
    return y;
  }

  radioTick(spread) {
    const t = radioTuning(this.s.modLoop);
    this.lastStations = t;
    const a = t[0], b = t[1];
    for (let i = 0; i < 5; i++) if (i !== a && i !== b) this.stActive[i] = 0;
    let y = this.stationTick(a, spread);
    let l = this.so[0], r = this.so[1];
    if (b < 0) { this.so[0] = l; this.so[1] = r; return y; }
    // between stations: cross-talk, static, crackle and a heterodyne whistle
    const u = t[2];
    const gA = Math.cos(u * Math.PI * 0.5), gB = Math.sin(u * Math.PI * 0.5);
    const yb = this.stationTick(b, spread);
    const mix = y * gA + yb * gB;
    l = l * gA + this.so[0] * gB; r = r * gA + this.so[1] * gB;
    const s = Math.sin(Math.PI * u);
    const n1 = this.rng.next() * 2 - 1, n2 = this.rng.next() * 2 - 1, n3 = this.rng.next();
    this.rStatic += (n1 - this.rStatic) * 0.3;
    const crack = n3 < 0.003 * s ? n2 : 0;
    this.rWhPh += TAU * (0.004 + 0.03 * u);
    if (this.rWhPh > TAU) this.rWhPh -= TAU;
    this.rLp += (mix - this.rLp) * 0.25;
    this.rHp += (this.rLp - this.rHp) * 0.02;
    const band = this.rLp - this.rHp;
    const noise = this.rStatic * 0.25 * s + crack * 0.3 * s + Math.sin(this.rWhPh) * 0.04 * s;
    const dry = 1 - 0.6 * s;
    y = mix * dry + band * 0.6 * s + noise;
    this.so[0] = l * dry + band * 0.6 * s + noise;
    this.so[1] = r * dry + band * 0.6 * s + noise;
    return y;
  }

  // Mask: loud parts of the loop (envelope over the MODIFY threshold) are replaced by a
  // treatment chosen by LENGTH. MODIFY fully down = the pure loop.
  maskTick() {
    const y0 = this.loopRead(this.pos);
    this.maskRing[this.maskW] = y0;
    const mw = this.maskW;
    if (++this.maskW >= this.maskRing.length) this.maskW = 0;
    const a = Math.abs(y0);
    this.maskEnv += (a - this.maskEnv) * (a > this.maskEnv ? 0.05 : 0.0008);
    const m = this.s.modLoop;
    const off = m <= 0.005;
    const one = 1 - m;
    const thr = 0.25 * one * one;
    const tgt = !off && this.maskEnv > thr ? 1 : 0;
    this.maskG += (tgt - this.maskG) * (tgt > this.maskG ? 0.02 : 0.002);
    if (tgt === 0 && this.maskG < 1e-6) this.maskG = 0;
    this.ringPh += TAU * 0.0075;
    if (this.ringPh > TAU) this.ringPh -= TAU;
    if (++this.crushCnt >= 6) { this.crushCnt = 0; this.crushV = Math.floor(y0 * 12 + 0.5) / 12; }
    if (this.maskG === 0) { this.so[0] = y0; this.so[1] = y0; return y0; }
    const sel = this.s.length * 5;
    const t0 = Math.floor(sel), fr = sel - t0;
    let tr = this.maskTreat(t0, y0, mw) * (1 - fr);
    if (fr > 0) tr += this.maskTreat(t0 + 1, y0, mw) * fr;
    const y = y0 + (tr - y0) * this.maskG;
    this.so[0] = y; this.so[1] = y;
    return y;
  }

  maskTreat(t, y0, mw) {
    const fs = this.fs;
    if (t === 0) return this.maskSh[0].run(this.maskRing, mw, 0, 0.5, 0.04 * fs);
    if (t === 1) return y0 * Math.sin(this.ringPh);
    if (t === 2) return this.crushV;
    if (t === 3) {
      let e = this.maskEnv * 4; if (e > 1) e = 1;
      const F = 2 * Math.sin(Math.PI * (0.01 + 0.15 * e));
      this.svfLow += F * this.svfBand;
      const high = y0 - this.svfLow - 0.3 * this.svfBand;
      this.svfBand += F * high;
      return this.svfBand * 1.5;
    }
    if (t === 4) {
      const G = 0.1 * fs;
      this.revPh += 1;
      if (this.revPh >= G) { this.revPh -= G; this.revStart = this.pos; }
      let v = 0;
      for (let h = 0; h < 2; h++) {
        let ph = this.revPh / G + h * 0.5; if (ph >= 1) ph -= 1;
        const s = Math.sin(Math.PI * ph);
        v += this.loopRead(this.revStart - h * 0.5 * G - ph * G) * s * s;
      }
      return v;
    }
    return this.maskSh[1].run(this.maskRing, mw, 0, 2, 0.04 * fs);
  }

  // Loop length in samples: fixed buffer (CLOCK sets its duration), HALF, or synced to the
  // wet channel (SYNC left: four Relay periods).
  loopLength() {
    let n = this.p.half ? LOOP_SAMPLES_HALF : LOOP_SAMPLES;
    if (this.p.sync === 0) {
      const P = Math.floor(4 * this.relayPeriod() + 0.5);
      if (P < n) n = P;
      if (n < 1024) n = 1024;
    }
    return n;
  }
  relayPeriod() { return expMap(this.s.time, 0.03, RELAY_TMAX_SEC) * this.fs; }
  // TIME in core samples for Relay delay / Flip lag; SYNC right steps it to loop fractions.
  syncedTime(t) {
    if (this.p.sync !== 2) return t;
    const idx = clamp(Math.floor(this.s.time * 8.999), 0, 8);
    let T = this.L * SYNC_FRACS[idx];
    const max = RELAY_TMAX_SEC * this.fs;
    while (T > max) T *= 0.5;
    return T;
  }

  coreTick(x) {
    const p = this.p, s = this.s, fs = this.fs;
    const spreadWet = p.spread && p.spreadSel !== 2;
    const spreadLoop = p.spread && p.spreadSel !== 0;

    // loop length can follow HALF / SYNC
    const L = this.loopLength();
    if (L !== this.L) {
      this.L = L;
      if (this.pos >= L) this.pos = this.pos % L;
      if (this.seam >= L) this.seam = this.seam % L;
      if (this.tp >= L) this.tp = this.tp % L;
    }
    this.seamF = Math.min(0.004 * fs, L / 4);

    // 1. micro-looper read
    if (p.loopMode !== this.lastLoopMode) { this.lastLoopMode = p.loopMode; this.resetLoopModes(); }
    this.loopGain += ((this.loopPlay ? 1 : 0) - this.loopGain) * 0.003;
    let loopL = 0, loopR = 0, loopMono = 0;
    for (let k = 0; k < 3; k++) this.lmGain[k] += ((p.loopMode === k ? 1 : 0) - this.lmGain[k]) * 0.003;
    if (this.loopGain > 1e-4) {
      if (this.lmGain[0] > 1e-4) { const y = this.burstTick(x); loopMono += y * this.lmGain[0]; loopL += this.so[0] * this.lmGain[0]; loopR += this.so[1] * this.lmGain[0]; }
      if (this.lmGain[1] > 1e-4) { const y = this.radioTick(spreadLoop); loopMono += y * this.lmGain[1]; loopL += this.so[0] * this.lmGain[1]; loopR += this.so[1] * this.lmGain[1]; }
      if (this.lmGain[2] > 1e-4) { const y = this.maskTick(); loopMono += y * this.lmGain[2]; loopL += this.so[0] * this.lmGain[2]; loopR += this.so[1] * this.lmGain[2]; }
      if (!spreadLoop) { loopL = loopMono; loopR = loopMono; }
      else if (p.loopMode !== 0) {
        // modes without their own stereo idea get a short decorrelated right side
        this.decorBuf[this.decorW] = loopR;
        loopR = readFrac(this.decorBuf, this.decorW - 0.011 * fs) * 0.5 + loopR * 0.5;
        if (++this.decorW >= this.decorBuf.length) this.decorW = 0;
      }
      loopMono *= this.loopGain; loopL *= this.loopGain; loopR *= this.loopGain;
    }

    // 2. wet channel: routing only matters when both channels are on
    const both = this.wetOn && this.loopPlay;
    const rt = both ? p.routing : 0;
    this.xW += ((rt === 2 ? 0 : 1) - this.xW) * 0.003;
    this.lW += ((rt >= 1 ? 1 : 0) - this.lW) * 0.003;
    const wetSrc = x * this.xW + loopMono * this.lW;
    const tb = this.trueBypass;
    this.wetIn += ((this.wetOn && !tb ? 1 : 0) - this.wetIn) * 0.003;
    this.wetOut += ((this.wetOn || p.trails ? 1 : 0) - this.wetOut) * 0.003;
    for (let k = 0; k < 3; k++) this.modeGain[k] += ((p.wetMode === k ? 1 : 0) - this.modeGain[k]) * 0.002;
    const wIn = wetSrc * this.wetIn;
    const freeze = this.freeze;
    let wl = 0, wr = 0;
    if (this.modeGain[0] > 1e-4) {
      const o = this.soup.tick(wIn, s.time, s.modWet, freeze, s.time > 0.985, spreadWet);
      wl += o[0] * this.modeGain[0]; wr += o[1] * this.modeGain[0];
    }
    if (this.modeGain[1] > 1e-4) {
      const T = this.syncedTime(this.relayPeriod());
      const o = this.relay.tick(wIn, T, relayRepeats(s.modWet), freeze, spreadWet);
      wl += o[0] * this.modeGain[1]; wr += o[1] * this.modeGain[1];
    }
    if (this.modeGain[2] > 1e-4) {
      let lag = this.syncedTime(FLIP_LAG_MAX_SEC * fs * s.time * s.time);
      if (lag > FLIP_LAG_MAX_SEC * fs) lag = FLIP_LAG_MAX_SEC * fs;
      const o = this.flip.tick(wIn, lag, FLIP_CHORDS[flipChordIndex(s.modWet)], freeze, spreadWet);
      wl += o[0] * this.modeGain[2]; wr += o[1] * this.modeGain[2];
    }
    wl *= this.wetOut; wr *= this.wetOut;

    // 3. CROSS: envelope of the source bends pitch (short modulated delay) and sputters
    // the level of the target pair(s). INPUT MOD: 0 wet→loop, 1 input→both, 2 loop→wet.
    const cb = this.crossBuf;
    const cw = this.crossW;
    cb[0][cw] = wl; cb[1][cw] = wr; cb[2][cw] = loopL; cb[3][cw] = loopR;
    const im = p.inputMod;
    const src = im === 1 ? Math.abs(x) : im === 0 ? Math.abs(wl + wr) * 0.5 : Math.abs(loopMono);
    this.crossEnv += (src - this.crossEnv) * (src > this.crossEnv ? 0.01 : 0.0015);
    if (s.cross > 1e-4) {
      let duck = this.crossEnv * 5; if (duck > 1) duck = 1;
      this.crossPh += TAU * (0.0002 + 0.0015 * duck);
      if (this.crossPh > TAU) this.crossPh -= TAU;
      const amp = 1 - s.cross * duck * (0.55 + 0.45 * Math.sin(this.crossPh));
      this.crossD += (s.cross * duck * 0.012 * fs - this.crossD) * 0.0015;
      if (im !== 0) { wl = readFrac(cb[0], cw - this.crossD) * amp; wr = readFrac(cb[1], cw - this.crossD) * amp; }
      if (im !== 2) {
        loopL = readFrac(cb[2], cw - this.crossD) * amp; loopR = readFrac(cb[3], cw - this.crossD) * amp;
        loopMono = (loopL + loopR) * 0.5;
      }
    } else this.crossD = 0;
    if (++this.crossW >= cb[0].length) this.crossW = 0;

    // 4. micro-looper write. Always listening while bypassed: records the input plus the
    // wet channel, regardless of routing (p.41). Overdubs record only the clean input (p.32).
    {
      const i = this.pos;
      let v;
      let write = true;
      if (tb) { v = 0; write = false; }
      else if (!this.loopPlay) v = x + (wl + wr) * 0.5;
      else if (this.overdub) {
        const fb = p.fade > 0.97 ? 1 : 0.25 + 0.75 * p.fade;
        v = this.buf[i] * fb + x;
      } else { v = 0; write = false; }
      if (write) {
        if (v > 1 || v < -1) v = Math.tanh(v);
        this.buf[i] = v;
        this.eAcc += v * v;
      }
      if ((i + 1) % BLK === 0 || i + 1 === this.L) {
        if (write) this.blockE[Math.floor(i / BLK)] = this.eAcc;
        this.eAcc = 0;
      }
      if (++this.pos >= this.L) { this.pos = 0; this.cycles++; }
    }

    // 5. channel balance; a loop routed through the wet channel is 100 % wet unless BLEND
    const direct = both && p.routing >= 1 ? s.blend : 1;
    this.loopDirect += (direct - this.loopDirect) * 0.003;
    const bal = s.balance;
    const wetLvl = Math.min(1, 2 * (1 - bal)), loopLvl = Math.min(1, 2 * bal);
    this.prev[0] = this.cur[0]; this.prev[1] = this.cur[1];
    this.cur[0] = wl * wetLvl + loopL * this.loopDirect * loopLvl;
    this.cur[1] = wr * wetLvl + loopR * this.loopDirect * loopLvl;
  }

  // Glue: end-of-chain saturator. Warm (biased tanh) low, folded and crushed high.
  glue(x) {
    if (this.s.glue < 0.001) return x;
    let y = Math.tanh(this.gDrive * x + this.gBias) - this.gTB;
    if (this.gDestroy > 0) {
      const f = Math.sin(this.gDrive * x * 1.7);
      y += (f - y) * this.gDestroy;
      y = Math.floor(y * this.gQ + 0.5) / this.gQ;
    }
    return y * this.gMake;
  }

  rampValue(v) {
    const sh = this.rampShape;
    return this.p.sweep ? 1 - (1 - v) * sh : v * sh;
  }

  snapSmoothers() {
    const p = this.p, s = this.s;
    s.time = p.time; s.mix = p.mix; s.length = p.length; s.modWet = p.modWet; s.modLoop = p.modLoop;
    s.balance = p.balance; s.cross = p.cross; s.blend = p.blend; s.glue = p.glue; s.volume = p.volume; s.eq = p.eq;
    s.clockHz = clockHz(p.clock, p.smooth);
    for (let k = 0; k < 3; k++) { this.modeGain[k] = p.wetMode === k ? 1 : 0; this.lmGain[k] = p.loopMode === k ? 1 : 0; }
    this.lastLoopMode = p.loopMode;
  }

  // inL/inR: Float32Array (inR may be null = mono), outL/outR: Float32Array
  process(inL, inR, outL, outR) {
    const n = outL.length;
    const p = this.p, s = this.s, fs = this.fs;
    if (!this.started) { this.started = true; this.snapSmoothers(); }
    const a = 1 - Math.exp(-1 / (0.03 * fs));
    const aClock = 1 - Math.exp(-1 / (0.06 * fs));
    this.updateSwitches();

    // ramping / bounce (p.46-47): modulates the selected knobs between the knob position and
    // one end (SWEEP), at the ramp speed
    const rampOn = p.rampTime || p.rampModWet || p.rampClock || p.rampModLoop || p.rampLength;
    const engagedNow = this.wetOn || this.loopPlay;
    if (engagedNow && !this.lastEngaged) this.rampTrig = true;
    this.lastEngaged = engagedNow;
    this.rampShape = 1;
    if (rampOn) {
      const inc = n / (expMap(p.rampSpeed, 20, 0.25) * fs);
      if (p.bounce) {
        this.rampPh += inc; this.rampPh -= Math.floor(this.rampPh);
        this.rampShape = this.rampPh < 0.5 ? 2 * this.rampPh : 2 - 2 * this.rampPh;
      } else {
        if (this.rampTrig) this.rampPh = 0;
        this.rampPh = Math.min(1, this.rampPh + inc);
        this.rampShape = this.rampPh;
      }
    }
    this.rampTrig = false;
    const tTime = p.rampTime ? this.rampValue(p.time) : p.time;
    const tModWet = p.rampModWet ? this.rampValue(p.modWet) : p.modWet;
    const tModLoop = p.rampModLoop ? this.rampValue(p.modLoop) : p.modLoop;
    const tLength = p.rampLength ? this.rampValue(p.length) : p.length;
    const tClockHz = clockHz(p.rampClock ? this.rampValue(p.clock) : p.clock, p.smooth);

    let pkIn = 0, pkOut = 0;
    for (let i = 0; i < n; i++) {
      s.time += (tTime - s.time) * a;
      s.mix += (p.mix - s.mix) * a;
      s.length += (tLength - s.length) * a;
      s.modWet += (tModWet - s.modWet) * a;
      s.modLoop += (tModLoop - s.modLoop) * a;
      s.balance += (p.balance - s.balance) * a;
      s.cross += (p.cross - s.cross) * a;
      s.blend += (p.blend - s.blend) * a;
      s.glue += (p.glue - s.glue) * a;
      s.volume += (p.volume - s.volume) * a;
      s.eq += (p.eq - s.eq) * a;
      s.clockHz += (tClockHz - s.clockHz) * aClock;

      if ((i & 31) === 0) {
        const c = Math.min(1, s.clockHz / fs);
        this.clockRatio = c;
        if (Math.abs(c - this.filterClock) > 0.002 * c) {
          this.filterClock = c;
          const fc = Math.min(0.45 * c * fs, 20000);
          this.aa.set(fc, fs); this.reconL.set(fc, fs); this.reconR.set(fc, fs);
        }
        if (Math.abs(s.eq - this.eqKey) > 1e-4) {
          this.eqKey = s.eq;
          if (s.eq < 0.5) { const f = expMap(s.eq / 0.5, 500, 20000); this.eqL.lowpass(f, fs, 0.707); this.eqR.lowpass(f, fs, 0.707); }
          else { const f = expMap((s.eq - 0.5) / 0.5, 20, 2500); this.eqL.highpass(f, fs, 0.707); this.eqR.highpass(f, fs, 0.707); }
        }
        this.vol = Math.pow(10, ((s.volume - 0.5) * 24) / 20);
        if (Math.abs(s.glue - this.glueKey) > 1e-4) {
          this.glueKey = s.glue;
          const g = s.glue;
          this.gDrive = expMap(g, 1, 50);
          this.gBias = 0.2 * g;
          this.gTB = Math.tanh(this.gBias);
          this.gMake = 0.5 / (Math.tanh(this.gDrive * 0.5 + this.gBias) - this.gTB);
          this.gDestroy = clamp((g - 0.55) / 0.45, 0, 1);
          this.gQ = expMap(this.gDestroy, 2048, 6);
        }
      }

      const dL = inL ? inL[i] : 0;
      const dR = p.miso ? dL : inR ? inR[i] : dL;
      const mono = p.miso || !inR ? dL : 0.5 * (dL + dR);
      const ad = Math.max(Math.abs(dL), Math.abs(dR)); if (ad > pkIn) pkIn = ad;
      const xa = this.aa.run(mono);
      this.phase += this.clockRatio;
      if (this.phase >= 1) { this.phase -= 1; this.coreTick(xa); }
      const f = this.phase;
      let el = this.reconL.run(this.prev[0] + (this.cur[0] - this.prev[0]) * f);
      let er = this.reconR.run(this.prev[1] + (this.cur[1] - this.prev[1]) * f);
      if (Math.abs(s.eq - 0.5) > 0.01) { el = this.eqL.run(el); er = this.eqR.run(er); }
      el *= this.vol; er *= this.vol;

      // dry stays at unity up to noon, then fades; true unity when nothing is engaged
      const engagedT = this.wetOn || this.loopPlay ? 1 : 0;
      this.engaged += (engagedT - this.engaged) * 0.001;
      const dg = p.dryKill ? 0 : 1 - this.engaged * (1 - Math.min(1, 2 * (1 - s.mix)));
      const wg = Math.min(1, 2 * s.mix);
      let l, r;
      if (p.dryGlue && this.engaged > 1e-4) {
        // DRY GLUE: the dry signal goes through GLUE with the effects (when engaged)
        const gl = this.glue(dL * dg + el * wg), gr = this.glue(dR * dg + er * wg);
        const ul = dL * dg + this.glue(el) * wg, ur = dR * dg + this.glue(er) * wg;
        l = ul + (gl - ul) * this.engaged; r = ur + (gr - ur) * this.engaged;
      } else {
        l = dL * dg + this.glue(el) * wg;
        r = dR * dg + this.glue(er) * wg;
      }
      // true bypass: the untouched input (relay bypass ignores DRY KILL)
      this.tbG += ((this.trueBypass ? 1 : 0) - this.tbG) * 0.002;
      if (this.tbG > 1e-5) { l += (dL - l) * this.tbG; r += (dR - r) * this.tbG; }
      l = softClip(l); r = softClip(r);
      outL[i] = l; outR[i] = r;
      const ao = Math.max(Math.abs(l), Math.abs(r)); if (ao > pkOut) pkOut = ao;
      this.hostCount++;
    }
    this.peakIn = Math.max(pkIn, this.peakIn * 0.9);
    this.peakOut = Math.max(pkOut, this.peakOut * 0.9);
  }

  // Snapshot for the UI.
  state() {
    const t = this.lastStations;
    return {
      wetOn: this.wetOn, freeze: this.freeze,
      loop: !this.loopPlay ? 'rec' : this.overdub ? 'dub' : 'play',
      hidden: this.hidden, trueBypass: this.trueBypass,
      pos: this.pos, len: this.L, seam: this.seam, cycles: this.cycles,
      clockHz: this.clockRatio * this.fs,
      slices: Array.from(this.slices.subarray(0, this.nSlices)), step: this.curSlice, scramble: this.scramble,
      station: t[0], station2: t[1], tune: t[2], mask: this.maskG,
      peakIn: this.peakIn, peakOut: this.peakOut,
    };
  }

  loopPeaks(nBins) {
    const out = new Float32Array(nBins);
    const n = this.L;
    for (let b = 0; b < nBins; b++) {
      const a = Math.floor((b * n) / nBins), z = Math.max(a + 1, Math.floor(((b + 1) * n) / nBins));
      let pk = 0;
      for (let i = a; i < z; i += 4) { const v = Math.abs(this.buf[i]); if (v > pk) pk = v; }
      out[b] = pk;
    }
    return out;
  }
}

if (typeof AudioWorkletProcessor !== 'undefined') {
  class BadMoodProcessor extends AudioWorkletProcessor {
    constructor() {
      super();
      this.core = new BadMoodCore(sampleRate);
      this.sinceState = 0;
      this.waveTimer = 0;
      this.port.onmessage = (e) => {
        const m = e.data;
        if (m.type === 'param') this.core.setParam(m.name, m.value);
        else if (m.type === 'fs') this.core.footswitch(m.which, m.action);
        else if (m.type === 'cmd') this.core.command(m.name, m.value);
      };
    }
    process(inputs, outputs) {
      const out = outputs[0];
      const outL = out[0], outR = out[1] || out[0];
      const inp = inputs[0];
      const inL = inp && inp.length ? inp[0] : null;
      const inR = inp && inp.length > 1 ? inp[1] : null;
      const core = this.core;
      core.process(inL, inR, outL, outR);
      this.sinceState += outL.length;
      if (this.sinceState >= sampleRate / 30) {
        this.sinceState = 0;
        while (core.events.length) this.port.postMessage({ type: 'event', ev: core.events.shift() });
        this.port.postMessage({ type: 'state', st: core.state() });
        if (++this.waveTimer % 4 === 0) this.port.postMessage({ type: 'wave', peaks: core.loopPeaks(400) });
      }
      return true;
    }
  }
  registerProcessor('badmood-processor', BadMoodProcessor);
}

if (typeof module !== 'undefined' && module.exports) {
  module.exports = {
    BadMoodCore, FFT, clockHz, speedMap, expMap, relayRepeats, radioTuning, flipChordIndex,
    FLIP_CHORDS, INTERVAL_RATIOS, INTERVAL_NAMES, RADIO_NAMES, RADIO_CENTERS, MASK_NAMES, BURST_DIVS,
    CLOCK_STEPS_HZ, LOOP_SAMPLES, LOOP_SAMPLES_HALF, HOLD_SEC, PRESET_HOLD_SEC, RELAY_TMAX_SEC, FLIP_LAG_MAX_SEC,
    PARAM_DEFAULTS, PRESET_KEYS, snapshotParams, applySnapshot,
  };
}
