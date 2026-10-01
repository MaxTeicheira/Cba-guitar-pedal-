// Mood emulator DSP core.
//
// Loads in an AudioWorkletGlobalScope (registers 'mood-processor') and in Node
// (exports MoodCore for offline tests and rendering).
//
// Signal flow
//   input ─┬───────────────────────────────────────────── dry ──┐
//          └─ anti-alias LPF ─► [ CORE @ clock rate ] ─► reconstruction LPF ─► mix ─► out L/R
//
// Everything inside the core runs at a variable "clock" rate (fs * c, c ≤ 1),
// like the pedal's CLOCK knob: lower clock = longer delays/loops, lower
// bandwidth, and anything already in a buffer shifts down in pitch. All times
// below are expressed in core samples at full clock (seconds * fs).
'use strict';

const TAU = Math.PI * 2;
const clamp = (x, a, b) => (x < a ? a : x > b ? b : x);
const expMap = (t, a, b) => a * Math.pow(b / a, t);

const HOLD_SEC = 0.35;        // footswitch hold threshold
// Micro-looper buffer is a fixed number of samples, so CLOCK sets loop length:
// 64 000 samples = 1 s at 64 kHz … 32 s at 2 kHz (MKII doubled the MKI's 0.5–16 s).
const LOOP_SAMPLES = 64000;
const CLOCK_MIN_HZ = 2000;
const CLOCK_MAX_HZ = 64000;   // capped at the host sample rate (usually 44.1/48 kHz)
const DELAY_MAX_SEC = 2.1;
const SLIP_BUF_SEC = 5.5;
const clockHz = (knob) => expMap(knob, CLOCK_MIN_HZ, CLOCK_MAX_HZ);

// Shared speed map for TAPE (loop) and SLIP (wet).
// CCW: reverse 2x → 0.5x, then forward 0.5x → 1x at noon → 2x at full CW.
function speedMap(m, snapOct) {
  let s;
  if (m < 0.28) s = -Math.pow(2, 1 - (2 * m) / 0.28);
  else if (m < 0.3) s = 0.5;
  else if (m < 0.5) s = Math.pow(2, -(0.5 - m) / 0.2);
  else s = Math.pow(2, (m - 0.5) / 0.5);
  if (snapOct > 0) {
    const oct = Math.log2(Math.abs(s));
    const r = Math.round(oct);
    if (Math.abs(oct - r) < snapOct) s = Math.sign(s) * Math.pow(2, r);
  }
  return s;
}

// STRETCH: bipolar. Noon freezes; CW plays forward 1/8x → 2x, CCW the same in reverse.
// Pitch is unaffected (granular), only the speed through the loop changes.
function stretchMap(m) {
  const d = m - 0.5;
  if (Math.abs(d) < 0.03) return 0;
  const t = (Math.abs(d) - 0.03) / 0.47;
  return Math.sign(d) * Math.pow(2, -3 + 4 * t);
}

// Transposed direct form II biquad, low-pass only.
class LowPass {
  constructor() { this.b0 = 1; this.b1 = 0; this.b2 = 0; this.a1 = 0; this.a2 = 0; this.z1 = 0; this.z2 = 0; }
  set(fc, fs, q) {
    const w = (TAU * clamp(fc, 10, fs * 0.49)) / fs;
    const cw = Math.cos(w), alpha = Math.sin(w) / (2 * q), a0 = 1 + alpha;
    this.b0 = (1 - cw) / 2 / a0; this.b1 = (1 - cw) / a0; this.b2 = this.b0;
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
  constructor() { this.a = new LowPass(); this.b = new LowPass(); }
  set(fc, fs) { this.a.set(fc, fs, 0.5412); this.b.set(fc, fs, 1.3066); }
  run(x) { return this.b.run(this.a.run(x)); }
}

class Allpass {
  constructor(len, g) { this.buf = new Float32Array(len); this.i = 0; this.g = g; }
  run(x) {
    const d = this.buf[this.i];
    const v = x + this.g * d;
    this.buf[this.i] = v;
    if (++this.i >= this.buf.length) this.i = 0;
    return d - this.g * v;
  }
}

// Linear-interpolated read from a circular buffer.
function readFrac(buf, pos) {
  const n = buf.length;
  let p = pos % n; if (p < 0) p += n;
  const i0 = Math.floor(p), f = p - i0;
  const i1 = i0 + 1 === n ? 0 : i0 + 1;
  return buf[i0] + (buf[i1] - buf[i0]) * f;
}

// ───────────────────────── Wet channel: REVERB ─────────────────────────
class Reverb {
  constructor(fs) {
    const k = fs / 48000;
    const lens = [1031, 1327, 1523, 1789, 2053, 2311, 2617, 2963].map((l) => Math.round(l * k));
    this.lines = lens.map((l) => new Float32Array(l + 64));
    this.len = lens;
    this.w = new Int32Array(8);
    this.lp = new Float32Array(8);
    this.modPh = new Float64Array(8).map((_, i) => i * 0.77);
    this.modRate = [0.31, 0.43, 0.53, 0.61, 0.37, 0.47, 0.57, 0.67].map((r) => (TAU * r) / fs);
    this.diff = [142, 107, 379, 277].map((l) => new Allpass(Math.round(l * k), 0.7));
    this.fs = fs;
    this.g = new Float32Array(8);
    this.t60 = -1;
    this.out = [0, 0];
    this.x = new Float32Array(8);
  }
  setDecay(t60) {
    if (Math.abs(t60 - this.t60) < 1e-4) return;
    this.t60 = t60;
    for (let i = 0; i < 8; i++) this.g[i] = Math.pow(10, (-3 * this.len[i]) / (t60 * this.fs));
  }
  // modify = smear: CCW clear and still, CW washed out (more input diffusion,
  // deeper delay modulation, softer highs)
  tick(input, modify, freeze, infinite) {
    let d = input * 0.35;
    const dg = 0.45 + 0.3 * modify;
    for (let i = 0; i < 4; i++) { this.diff[i].g = dg; d = this.diff[i].run(d); }
    const damp = freeze ? 1 : 1 - Math.exp((-TAU * expMap(modify, 9000, 3500)) / this.fs);
    const depth = 1 + modify * modify * 18 * (this.fs / 48000);
    const x = this.x;
    let sum = 0;
    for (let i = 0; i < 8; i++) {
      const line = this.lines[i];
      this.modPh[i] += this.modRate[i];
      if (this.modPh[i] > TAU) this.modPh[i] -= TAU;
      const delay = this.len[i] - depth * (1 + Math.sin(this.modPh[i]));
      const v = readFrac(line, this.w[i] - delay);
      this.lp[i] += damp * (v - this.lp[i]);
      const g = freeze || infinite ? 1 : this.g[i];
      x[i] = this.lp[i] * g;
      sum += x[i];
    }
    const h = sum * 0.25; // Householder reflection: x - (2/N)·Σx
    let l = 0, r = 0;
    for (let i = 0; i < 8; i++) {
      const y = x[i] - h;
      const line = this.lines[i];
      line[this.w[i]] = y + (freeze ? 0 : d);
      if (++this.w[i] >= line.length) this.w[i] = 0;
      if (i & 1) r += i & 2 ? -x[i] : x[i];
      else l += i & 2 ? -x[i] : x[i];
    }
    this.out[0] = l * 0.6;
    this.out[1] = r * 0.6;
    return this.out;
  }
}

// ───────────────────────── Wet channel: DELAY ─────────────────────────
class Delay {
  constructor(fs) {
    this.fs = fs;
    this.buf = new Float32Array(Math.ceil(DELAY_MAX_SEC * fs) + 8);
    this.w = 0;
    this.d = -1; // snapped to the knob on the first tick
    this.lp = 0; this.hp = 0;
    this.lpA = 1 - Math.exp((-TAU * 6500) / fs);
    this.hpA = 1 - Math.exp((-TAU * 70) / fs);
    this.slew = 1 - Math.exp(-1 / (0.12 * fs)); // tape-like time glide
  }
  // modify → feedback, past 3 o'clock it self-oscillates (soft-limited)
  tick(input, delaySamples, modify, freeze) {
    if (this.d < 0) this.d = delaySamples;
    this.d += (delaySamples - this.d) * this.slew;
    const y = readFrac(this.buf, this.w - this.d);
    this.lp += this.lpA * (y - this.lp);
    this.hp += this.hpA * (this.lp - this.hp);
    const fbSig = freeze ? y : this.lp - this.hp;
    const fb = freeze ? 1 : modify * 1.1;
    let v = (freeze ? 0 : input) + fbSig * fb;
    v = v > 1 || v < -1 ? Math.tanh(v) : v;
    this.buf[this.w] = v;
    if (++this.w >= this.buf.length) this.w = 0;
    return y;
  }
}

// ───────────────────────── Wet channel: SLIP ──────────────────────────
// Repeating slices of recent audio replayed at another speed/direction.
class Slip {
  constructor(fs) {
    this.fs = fs;
    this.buf = new Float32Array(Math.ceil(SLIP_BUF_SEC * fs));
    this.w = 0;
    this.heads = [{ pos: 0, g: 0, dir: 0 }, { pos: 0, g: 0, dir: 0 }];
    this.cur = 0;
    this.count = 1e9;
    this.lastStart = 0;
    this.speed = 1;
  }
  tick(input, sliceLen, speedTarget, freeze) {
    const n = this.buf.length;
    if (!freeze) {
      this.buf[this.w] = input;
      if (++this.w >= n) this.w = 0;
    }
    this.speed += (speedTarget - this.speed) * 0.002;
    const s = this.speed;
    const xf = Math.max(32, Math.min(0.25 * sliceLen, 0.03 * this.fs));
    if (++this.count >= sliceLen) {
      this.count = 0;
      const margin = xf + 64;
      let start;
      if (freeze) start = this.lastStart;
      else {
        const behind = margin + Math.max(0, s - 1) * (sliceLen + xf);
        start = this.w - behind;
        this.lastStart = start;
      }
      this.cur ^= 1;
      const h = this.heads[this.cur];
      h.pos = start; h.g = 0; h.dir = 1;
      this.heads[this.cur ^ 1].dir = -1;
    }
    const step = 1 / xf;
    let out = 0;
    for (let k = 0; k < 2; k++) {
      const h = this.heads[k];
      if (h.dir > 0) h.g = Math.min(1, h.g + step);
      else if (h.dir < 0) h.g = Math.max(0, h.g - step);
      if (h.g > 0) {
        out += readFrac(this.buf, h.pos) * Math.sin(h.g * Math.PI * 0.5);
        h.pos += s;
      }
    }
    return out;
  }
}

// ─────────────────────────────── Core ─────────────────────────────────
class MoodCore {
  constructor(fs) {
    this.fs = fs;
    this.p = {
      time: 0.5, mix: 0.5, length: 0.75, modWet: 0.5, clock: 1, modLoop: 0.5,
      wetMode: 0, routing: 0, loopMode: 0,
      trails: 1, dryKill: 0, latch: 0, spread: 0, smooth: 0, balance: 0.5,
    };
    // host-rate smoothed knobs
    this.s = { time: 0.5, mix: 0.5, length: 0.75, modWet: 0.5, clock: 1, modLoop: 0.5, balance: 0.5 };
    this.hostCount = 0;
    this.phase = 0;
    this.clockRatio = 1;
    this.aa = new LowPass4();
    this.reconL = new LowPass4();
    this.reconR = new LowPass4();
    this.filterClock = -1;
    this.prev = [0, 0];
    this.cur = [0, 0];

    // wet channel
    this.reverb = new Reverb(fs);
    this.delay = new Delay(fs);
    this.slip = new Slip(fs);
    this.wetOn = false;
    this.freeze = false;
    this.wetMomentary = false;
    this.wetIn = 0; this.wetOut = 0;           // core-rate gains
    this.modeGain = [1, 0, 0];                  // crossfade between wet modes
    this.decor = [new Allpass(Math.round(113 * fs / 48000), 0.6), new Allpass(Math.round(337 * fs / 48000), 0.6)];
    this.loopDirect = 1;

    // loop channel
    const nLoop = LOOP_SAMPLES;
    this.histCycles = 0;
    this.hist = new Float32Array(nLoop);
    this.histW = 0;
    this.histCount = 0;
    this.loopBuf = new Float32Array(nLoop);
    this.lsrc = 0;
    this.loopState = 'off';                     // off | play
    this.overdub = false;
    this.loopGain = 0;
    this.loopPos = 0;
    this.tapeSpeed = 1;
    this.stretchSpeed = 0;
    this.env = 0;
    this.envGate = false;
    this.envHold = 0;
    this.envMix = 0;
    this.slice = { start: 0, ph: 0 };
    this.grain = [{ ph: 0, start: 0 }, { ph: 0.5, start: 0 }];
    this.rng = 22222;

    this.fsDown = { wet: null, loop: null };   // press state for footswitches
    this.engaged = 0;
    this.events = [];                           // UI notifications (loop captured, ...)
    this.peakIn = 0; this.peakOut = 0;
  }

  setParam(name, value) {
    if (name in this.p) this.p[name] = value;
  }

  // which: 'wet' | 'loop', action: 'down' | 'up'
  footswitch(which, action) {
    const now = this.hostCount;
    if (which === 'wet') {
      if (action === 'down') {
        this.fsDown.wet = { t: now, held: false };
        if (!this.wetOn) { this.wetOn = true; this.wetMomentary = true; }
        else this.wetMomentary = false;
      } else if (this.fsDown.wet) {
        const held = this.fsDown.wet.held;
        this.fsDown.wet = null;
        if (held) {
          if (!this.p.latch) {
            this.freeze = false;
            if (this.wetMomentary) this.wetOn = false;
          }
        } else if (!this.wetMomentary) {
          this.wetOn = false;
          this.freeze = false;
        }
        this.wetMomentary = false;
      }
      return;
    }
    if (action === 'down') {
      const playing = this.loopState === 'play';
      this.fsDown.loop = { t: now, held: false, histIdx: this.histW, histCount: this.histCount, playing };
      if (playing) this.overdub = true;
    } else if (this.fsDown.loop) {
      const d = this.fsDown.loop;
      this.fsDown.loop = null;
      if (d.playing) {
        this.overdub = false;
        if (!d.held) { this.loopState = 'off'; this.events.push('loop-stop'); }
        else this.events.push('loop-wave');
      } else if (d.held) {
        let n = this.histW - d.histIdx;
        if (n < 0) n += this.hist.length;
        this.capture(n);
      } else {
        this.capture(this.histCount); // retroactive: grab everything heard so far
      }
    }
  }

  // Copy the most recent n samples of history into the loop buffer and play.
  capture(n) {
    n = Math.min(n, this.hist.length - 1, this.histCount);
    if (n < 64) return;
    const N = this.hist.length;
    let r = this.histW - n; if (r < 0) r += N;
    for (let i = 0; i < n; i++) {
      this.loopBuf[i] = this.hist[r];
      if (++r >= N) r = 0;
    }
    this.lsrc = n;
    this.loopPos = 0;
    this.tapeSpeed = speedMap(this.s.modLoop, 0.04);
    this.stretchSpeed = stretchMap(this.s.modLoop);
    this.grain[0].ph = 0; this.grain[1].ph = 0.5;
    this.grain[0].start = 0; this.grain[1].start = 0;
    this.loopState = 'play';
    this.events.push('loop-wave');
  }

  // Loop region being played. In TAPE, LENGTH trims the loop to its most recent
  // part; in ENV and STRETCH LENGTH sets slice size, so the whole capture plays.
  loopWindow() {
    if (this.p.loopMode !== 1) return { W: Math.max(64, this.lsrc), start: 0 };
    const W = Math.max(64, Math.floor(this.lsrc * expMap(this.s.length, 0.02, 1)));
    return { W, start: this.lsrc - W };
  }

  rand() { this.rng = (Math.imul(this.rng, 1103515245) + 12345) >>> 0; return this.rng / 4294967296; }

  loopTick(x) {
    // always listening
    this.hist[this.histW] = x;
    if (++this.histW >= this.hist.length) { this.histW = 0; this.histCycles++; }
    if (this.histCount < this.hist.length) this.histCount++;

    const fs = this.fs;
    const a = Math.abs(x);
    this.env += (a - this.env) * (a > this.env ? 0.02 : 0.0005);

    const target = this.loopState === 'play' ? 1 : 0;
    this.loopGain += (target - this.loopGain) * 0.003;
    if (this.loopState === 'off' && this.loopGain < 1e-4) { this.loopGain = 0; return 0; }
    if (this.lsrc < 64) return 0;

    const { W, start } = this.loopWindow();
    const buf = this.loopBuf;
    const mode = this.p.loopMode;
    const m = this.s.modLoop;
    let pos = this.loopPos % W; if (pos < 0) pos += W;
    const rd = (p) => {
      let q = p % W; if (q < 0) q += W;
      const i0 = Math.floor(q), f = q - i0;
      const i1 = i0 + 1 >= W ? 0 : i0 + 1;
      return buf[start + i0] + (buf[start + i1] - buf[start + i0]) * f;
    };
    const F = Math.min(0.004 * fs, W / 4);
    const edgeAt = (p) => { let q = p % W; if (q < 0) q += W; return Math.min(1, q / F, (W - q) / F); };
    let y;

    if (mode === 2) {
      // STRETCH: two Hann grains at 1x pitch; LENGTH = grain (slice) size,
      // MODIFY = speed and direction of the position through the loop.
      const G = Math.min(expMap(this.s.length, 0.01 * fs, 0.5 * fs), W);
      const dir = this.stretchSpeed < 0 ? -1 : 1;
      y = 0;
      for (let k = 0; k < 2; k++) {
        const g = this.grain[k];
        g.ph += 1 / G;
        if (g.ph >= 1) { g.ph -= 1; g.start = pos + (this.rand() - 0.5) * 0.2 * G; }
        const p = g.start + dir * g.ph * G;
        const sw = Math.sin(Math.PI * g.ph);
        y += rd(p) * sw * sw * edgeAt(p);
      }
      this.stretchSpeed += (stretchMap(m) - this.stretchSpeed) * 0.001;
      pos += this.stretchSpeed;
    } else if (mode === 1) {
      // TAPE: LENGTH = loop length, MODIFY = speed and direction (pitch follows)
      y = rd(pos) * edgeAt(pos);
      this.tapeSpeed += (speedMap(m, 0.04) - this.tapeSpeed) * 0.00025;
      pos += this.tapeSpeed;
    } else {
      // ENV: playing interrupts the loop. The slice under the playhead when you
      // hit a note repeats until you stop; then the loop carries on in time.
      // LENGTH = slice size (grains → short phrases), MODIFY = sensitivity.
      const thr = expMap(1 - m, 0.008, 0.35);
      if (this.env > thr) {
        if (!this.envGate) {
          this.envGate = true;
          this.slice.start = pos;
          this.slice.ph = 0;
        }
        this.envHold = 0.08 * fs;
      } else if (this.envGate && --this.envHold <= 0) this.envGate = false;
      this.envMix += ((this.envGate ? 1 : 0) - this.envMix) * 0.005;
      const main = rd(pos) * edgeAt(pos);
      let sl = 0;
      if (this.envMix > 1e-4) {
        const S = Math.min(expMap(this.s.length, 0.01 * fs, 1.0 * fs), W);
        const sl0 = this.slice;
        if (sl0.ph >= S) sl0.ph -= S;
        const F2 = Math.min(0.004 * fs, S / 4);
        sl = rd(sl0.start + sl0.ph) * Math.min(1, sl0.ph / F2, (S - sl0.ph) / F2);
        sl0.ph += 1;
      }
      y = main * (1 - this.envMix) + sl * this.envMix;
      pos += 1;
    }

    if (this.overdub) {
      let q = Math.floor(mode === 1 ? this.loopPos : pos) % W; if (q < 0) q += W;
      const i = start + q;
      buf[i] = buf[i] * 0.97 + x;
    }
    this.loopPos = ((pos % W) + W) % W;
    return y * this.loopGain;
  }

  coreTick(x) {
    const p = this.p, s = this.s;
    const loopOut = this.loopTick(x);

    // wet channel input routing: IN / IN+LOOP / LOOP
    const wetSrc = p.routing === 0 ? x : p.routing === 1 ? x + loopOut : loopOut;
    const wetOn = this.wetOn;
    const inT = wetOn ? 1 : 0;
    const outT = wetOn || p.trails ? 1 : 0;
    this.wetIn += (inT - this.wetIn) * 0.003;
    this.wetOut += (outT - this.wetOut) * 0.003;
    for (let k = 0; k < 3; k++) this.modeGain[k] += ((p.wetMode === k ? 1 : 0) - this.modeGain[k]) * 0.002;

    const freeze = this.freeze;
    const wIn = wetSrc * this.wetIn;
    let wl = 0, wr = 0;
    const fs = this.fs;
    if (this.modeGain[0] > 1e-4) {
      this.reverb.setDecay(expMap(s.time, 0.4, 25));
      const o = this.reverb.tick(wIn, s.modWet, freeze, s.time > 0.985);
      wl += o[0] * this.modeGain[0]; wr += o[1] * this.modeGain[0];
    }
    let mono = 0;
    if (this.modeGain[1] > 1e-4) {
      mono += this.delay.tick(wIn, expMap(s.time, 0.02 * fs, 2 * fs), s.modWet, freeze) * this.modeGain[1];
    }
    if (this.modeGain[2] > 1e-4) {
      mono += this.slip.tick(wIn, expMap(s.time, 0.05 * fs, 1.5 * fs), speedMap(s.modWet, 0.15), freeze) * this.modeGain[2];
    }
    wl += mono;
    wr += p.spread ? this.decor[1].run(this.decor[0].run(mono)) : mono;
    if (p.spread) { // widen reverb too
      const mid = (wl + wr) * 0.5, side = (wl - wr) * 0.5 * 1.6;
      wl = mid + side; wr = mid - side;
    }
    wl *= this.wetOut; wr *= this.wetOut;

    // loop heard directly unless it is routed through an active wet channel
    const directT = p.routing === 0 || !wetOn ? 1 : 0;
    this.loopDirect += (directT - this.loopDirect) * 0.003;
    const bal = s.balance;
    const wetLvl = Math.min(1, 2 * (1 - bal)), loopLvl = Math.min(1, 2 * bal);
    const ld = loopOut * this.loopDirect * loopLvl;
    this.prev[0] = this.cur[0]; this.prev[1] = this.cur[1];
    this.cur[0] = wl * wetLvl + ld;
    this.cur[1] = wr * wetLvl + ld;
  }

  // in: Float32Array (mono), outL/outR: Float32Array
  process(input, outL, outR) {
    const n = outL.length;
    const p = this.p, s = this.s, fs = this.fs;
    const a = 1 - Math.exp(-1 / (0.03 * fs));
    const aClock = 1 - Math.exp(-1 / ((p.smooth ? 1.5 : 0.08) * fs)); // Smooth dip: glide the clock
    const holdN = HOLD_SEC * fs;
    // footswitch hold detection
    const fw = this.fsDown.wet;
    if (fw && !fw.held && this.hostCount - fw.t >= holdN) {
      fw.held = true;
      this.freeze = p.latch ? !this.freeze : true;
    }
    const fl = this.fsDown.loop;
    if (fl && !fl.held && this.hostCount - fl.t >= holdN) fl.held = true;

    let pkIn = 0, pkOut = 0;
    for (let i = 0; i < n; i++) {
      s.time += (p.time - s.time) * a;
      s.mix += (p.mix - s.mix) * a;
      s.length += (p.length - s.length) * a;
      s.modWet += (p.modWet - s.modWet) * a;
      s.modLoop += (p.modLoop - s.modLoop) * a;
      s.balance += (p.balance - s.balance) * a;
      s.clock += (p.clock - s.clock) * aClock;

      if ((i & 31) === 0) {
        const c = Math.min(1, clockHz(s.clock) / fs);
        this.clockRatio = c;
        if (Math.abs(c - this.filterClock) > 0.002 * c) {
          this.filterClock = c;
          const fc = Math.min(0.45 * c * fs, 20000);
          this.aa.set(fc, fs); this.reconL.set(fc, fs); this.reconR.set(fc, fs);
        }
      }

      const dry = input ? input[i] : 0;
      const ad = Math.abs(dry); if (ad > pkIn) pkIn = ad;
      const xa = this.aa.run(dry);
      this.phase += this.clockRatio;
      if (this.phase >= 1) { this.phase -= 1; this.coreTick(xa); }
      const f = this.phase;
      const el = this.reconL.run(this.prev[0] + (this.cur[0] - this.prev[0]) * f);
      const er = this.reconR.run(this.prev[1] + (this.cur[1] - this.prev[1]) * f);

      // dry stays at unity up to noon, then fades; true unity when nothing is engaged
      const engagedT = this.wetOn || this.loopState === 'play' ? 1 : 0;
      this.engaged += (engagedT - this.engaged) * 0.001;
      const dg = p.dryKill ? 0 : 1 - this.engaged * (1 - Math.min(1, 2 * (1 - s.mix)));
      const wg = Math.min(1, 2 * s.mix);
      let l = dry * dg + el * wg;
      let r = dry * dg + er * wg;
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
    const fl = this.fsDown.loop;
    let loop = this.loopState;
    if (fl && fl.held && !fl.playing) loop = 'rec';
    else if (this.overdub && this.loopState === 'play') loop = 'dub';
    const win = this.lsrc > 0 ? this.loopWindow() : { W: 0, start: 0 };
    return {
      wetOn: this.wetOn, freeze: this.freeze, loop,
      lsrc: this.lsrc, winStart: win.start, winLen: win.W, pos: this.loopPos,
      clockHz: this.clockRatio * this.fs, histCycles: this.histCycles, peakIn: this.peakIn, peakOut: this.peakOut,
    };
  }

  loopPeaks(nBins) {
    const out = new Float32Array(nBins);
    const n = this.lsrc;
    if (!n) return out;
    for (let b = 0; b < nBins; b++) {
      const a = Math.floor((b * n) / nBins), z = Math.max(a + 1, Math.floor(((b + 1) * n) / nBins));
      let pk = 0;
      for (let i = a; i < z; i += 4) { const v = Math.abs(this.loopBuf[i]); if (v > pk) pk = v; }
      out[b] = pk;
    }
    return out;
  }
}

function softClip(x) {
  const ax = Math.abs(x);
  if (ax < 0.9) return x;
  return Math.sign(x) * (0.9 + 0.1 * Math.tanh((ax - 0.9) / 0.1));
}

if (typeof AudioWorkletProcessor !== 'undefined') {
  class MoodProcessor extends AudioWorkletProcessor {
    constructor() {
      super();
      this.core = new MoodCore(sampleRate);
      this.mono = new Float32Array(128);
      this.sinceState = 0;
      this.dubTimer = 0;
      this.port.onmessage = (e) => {
        const m = e.data;
        if (m.type === 'param') this.core.setParam(m.name, m.value);
        else if (m.type === 'fs') this.core.footswitch(m.which, m.action);
      };
    }
    process(inputs, outputs) {
      const out = outputs[0];
      const outL = out[0], outR = out[1] || out[0];
      const inp = inputs[0];
      let mono = null;
      if (inp && inp.length) {
        if (this.mono.length !== outL.length) this.mono = new Float32Array(outL.length);
        mono = this.mono;
        if (inp.length === 1) mono.set(inp[0]);
        else for (let i = 0; i < mono.length; i++) mono[i] = 0.5 * (inp[0][i] + inp[1][i]);
      }
      const core = this.core;
      core.process(mono, outL, outR);
      this.sinceState += outL.length;
      if (this.sinceState >= sampleRate / 30) {
        this.sinceState = 0;
        const st = core.state();
        let wave = false;
        while (core.events.length) {
          const ev = core.events.shift();
          if (ev === 'loop-wave') wave = true;
          else this.port.postMessage({ type: 'event', ev });
        }
        if (st.loop === 'dub' && ++this.dubTimer % 10 === 0) wave = true;
        this.port.postMessage({ type: 'state', st });
        if (wave) this.port.postMessage({ type: 'wave', peaks: core.loopPeaks(400), lsrc: core.lsrc });
      }
      return true;
    }
  }
  registerProcessor('mood-processor', MoodProcessor);
}

if (typeof module !== 'undefined' && module.exports) {
  module.exports = { MoodCore, speedMap, stretchMap, expMap, clockHz, LOOP_SAMPLES, CLOCK_MIN_HZ, CLOCK_MAX_HZ, HOLD_SEC };
}
