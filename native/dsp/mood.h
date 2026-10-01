// Mood emulator DSP core, C++ port of src/mood-worklet.js (the reference).
//
// Header-only, C++17, no exceptions/RTTI. All buffers are allocated in prepare();
// process() never allocates, locks, or throws, so it is safe on an audio thread.
// Scalars are double and buffers float, matching the JS (Number / Float32Array),
// so the two implementations agree to float precision (see tests/parity).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace mood {

constexpr double kPi = 3.14159265358979323846;
constexpr double kTau = 2.0 * kPi;
constexpr double kHoldSec = 0.35;          // footswitch hold threshold
constexpr int kLoopSamples = 64000;        // fixed: CLOCK sets loop length (1 s @ 64 kHz … 32 s @ 2 kHz)
constexpr double kClockMinHz = 2000.0;
constexpr double kClockMaxHz = 64000.0;    // capped at the host rate
constexpr double kDelayMaxSec = 2.1;
constexpr double kSlipBufSec = 5.5;

inline double clampd(double x, double a, double b) { return x < a ? a : x > b ? b : x; }
inline double expMap(double t, double a, double b) { return a * std::pow(b / a, t); }
inline double clockHz(double knob) { return expMap(knob, kClockMinHz, kClockMaxHz); }
inline double sgn(double x) { return x > 0 ? 1.0 : x < 0 ? -1.0 : 0.0; }
inline double jsRound(double x) { return std::floor(x + 0.5); }
inline double jsMod(double a, double n) { return std::fmod(a, n); }   // JS % on doubles

// TAPE / SLIP speed: CCW reverse 2x → 0.5x, then forward 0.5x → 1x at noon → 2x CW.
inline double speedMap(double m, double snapOct) {
    double s;
    if (m < 0.28) s = -std::pow(2.0, 1.0 - (2.0 * m) / 0.28);
    else if (m < 0.3) s = 0.5;
    else if (m < 0.5) s = std::pow(2.0, -(0.5 - m) / 0.2);
    else s = std::pow(2.0, (m - 0.5) / 0.5);
    if (snapOct > 0) {
        const double oct = std::log2(std::fabs(s));
        const double r = jsRound(oct);
        if (std::fabs(oct - r) < snapOct) s = sgn(s) * std::pow(2.0, r);
    }
    return s;
}

// STRETCH: bipolar. Noon freezes; CW forward 1/8x → 2x, CCW the same in reverse.
inline double stretchMap(double m) {
    const double d = m - 0.5;
    if (std::fabs(d) < 0.03) return 0.0;
    const double t = (std::fabs(d) - 0.03) / 0.47;
    return sgn(d) * std::pow(2.0, -3.0 + 4.0 * t);
}

inline double softClip(double x) {
    const double ax = std::fabs(x);
    if (ax < 0.9) return x;
    return sgn(x) * (0.9 + 0.1 * std::tanh((ax - 0.9) / 0.1));
}

struct LowPass {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    void set(double fc, double fs, double q) {
        const double w = (kTau * clampd(fc, 10, fs * 0.49)) / fs;
        const double cw = std::cos(w), alpha = std::sin(w) / (2 * q), a0 = 1 + alpha;
        b0 = (1 - cw) / 2 / a0; b1 = (1 - cw) / a0; b2 = b0;
        a1 = (-2 * cw) / a0; a2 = (1 - alpha) / a0;
    }
    double run(double x) {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

struct LowPass4 {
    LowPass a, b;
    void set(double fc, double fs) { a.set(fc, fs, 0.5412); b.set(fc, fs, 1.3066); }
    double run(double x) { return b.run(a.run(x)); }
};

struct Allpass {
    std::vector<float> buf; int i = 0; double g = 0;
    void init(int len, double gain) { buf.assign(static_cast<size_t>(std::max(1, len)), 0.f); i = 0; g = gain; }
    double run(double x) {
        const double d = buf[static_cast<size_t>(i)];
        const double v = x + g * d;
        buf[static_cast<size_t>(i)] = static_cast<float>(v);
        if (++i >= static_cast<int>(buf.size())) i = 0;
        return d - g * v;
    }
};

inline double readFrac(const std::vector<float>& buf, double pos) {
    const double n = static_cast<double>(buf.size());
    double p = jsMod(pos, n); if (p < 0) p += n;
    const int i0 = static_cast<int>(std::floor(p)); const double f = p - i0;
    const int i1 = i0 + 1 == static_cast<int>(buf.size()) ? 0 : i0 + 1;
    const double a = buf[static_cast<size_t>(i0)], b = buf[static_cast<size_t>(i1)];
    return a + (b - a) * f;
}

// ───────────────────────── Wet channel: REVERB ─────────────────────────
struct Reverb {
    std::vector<float> lines[8];
    int len[8] = {}, w[8] = {};
    float lp[8] = {}, x[8] = {}, g[8] = {};
    double modPh[8] = {}, modRate[8] = {};
    Allpass diff[4];
    double fs = 48000, t60 = -1, out[2] = {0, 0};

    void init(double sr) {
        fs = sr; t60 = -1;
        const double k = fs / 48000.0;
        const int base[8] = {1031, 1327, 1523, 1789, 2053, 2311, 2617, 2963};
        const double rates[8] = {0.31, 0.43, 0.53, 0.61, 0.37, 0.47, 0.57, 0.67};
        for (int i = 0; i < 8; ++i) {
            len[i] = static_cast<int>(jsRound(base[i] * k));
            lines[i].assign(static_cast<size_t>(len[i] + 64), 0.f);
            w[i] = 0; lp[i] = 0; x[i] = 0; g[i] = 0;
            modPh[i] = i * 0.77; modRate[i] = (kTau * rates[i]) / fs;
        }
        const int dl[4] = {142, 107, 379, 277};
        for (int i = 0; i < 4; ++i) diff[i].init(static_cast<int>(jsRound(dl[i] * k)), 0.7);
    }
    void setDecay(double t) {
        if (std::fabs(t - t60) < 1e-4) return;
        t60 = t;
        for (int i = 0; i < 8; ++i) g[i] = static_cast<float>(std::pow(10.0, (-3.0 * len[i]) / (t * fs)));
    }
    // modify = smear: CCW clear and still, CW washed out
    const double* tick(double input, double modify, bool freeze, bool infinite) {
        double d = input * 0.35;
        const double dg = 0.45 + 0.3 * modify;
        for (auto& a : diff) { a.g = dg; d = a.run(d); }
        const double damp = freeze ? 1.0 : 1.0 - std::exp((-kTau * expMap(modify, 9000, 3500)) / fs);
        const double depth = 1 + modify * modify * 18 * (fs / 48000.0);
        double sum = 0;
        for (int i = 0; i < 8; ++i) {
            modPh[i] += modRate[i];
            if (modPh[i] > kTau) modPh[i] -= kTau;
            const double delay = len[i] - depth * (1 + std::sin(modPh[i]));
            const double v = readFrac(lines[i], w[i] - delay);
            lp[i] = static_cast<float>(lp[i] + damp * (v - lp[i]));
            const double gg = freeze || infinite ? 1.0 : g[i];
            x[i] = static_cast<float>(lp[i] * gg);
            sum += x[i];
        }
        const double h = sum * 0.25;   // Householder reflection
        double l = 0, r = 0;
        for (int i = 0; i < 8; ++i) {
            const double y = x[i] - h;
            lines[i][static_cast<size_t>(w[i])] = static_cast<float>(y + (freeze ? 0.0 : d));
            if (++w[i] >= static_cast<int>(lines[i].size())) w[i] = 0;
            if (i & 1) r += (i & 2) ? -x[i] : x[i];
            else l += (i & 2) ? -x[i] : x[i];
        }
        out[0] = l * 0.6; out[1] = r * 0.6;
        return out;
    }
};

// ───────────────────────── Wet channel: DELAY ─────────────────────────
struct Delay {
    std::vector<float> buf; int w = 0;
    double fs = 48000, d = -1, lp = 0, hp = 0, lpA = 0, hpA = 0, slew = 0;
    void init(double sr) {
        fs = sr;
        buf.assign(static_cast<size_t>(std::ceil(kDelayMaxSec * fs) + 8), 0.f);
        w = 0; d = -1; lp = hp = 0;
        lpA = 1 - std::exp((-kTau * 6500) / fs);
        hpA = 1 - std::exp((-kTau * 70) / fs);
        slew = 1 - std::exp(-1 / (0.12 * fs));
    }
    double tick(double input, double delaySamples, double modify, bool freeze) {
        if (d < 0) d = delaySamples;
        d += (delaySamples - d) * slew;
        const double y = readFrac(buf, w - d);
        lp += lpA * (y - lp);
        hp += hpA * (lp - hp);
        const double fbSig = freeze ? y : lp - hp;
        const double fb = freeze ? 1.0 : modify * 1.1;
        double v = (freeze ? 0.0 : input) + fbSig * fb;
        if (v > 1 || v < -1) v = std::tanh(v);
        buf[static_cast<size_t>(w)] = static_cast<float>(v);
        if (++w >= static_cast<int>(buf.size())) w = 0;
        return y;
    }
};

// ───────────────────────── Wet channel: SLIP ──────────────────────────
struct Slip {
    struct Head { double pos = 0, g = 0; int dir = 0; };
    std::vector<float> buf; int w = 0;
    Head heads[2]; int cur = 0;
    double fs = 48000, count = 1e9, lastStart = 0, speed = 1;
    void init(double sr) {
        fs = sr;
        buf.assign(static_cast<size_t>(std::ceil(kSlipBufSec * fs)), 0.f);
        w = 0; heads[0] = heads[1] = Head{}; cur = 0; count = 1e9; lastStart = 0; speed = 1;
    }
    double tick(double input, double sliceLen, double speedTarget, bool freeze) {
        const int n = static_cast<int>(buf.size());
        if (!freeze) { buf[static_cast<size_t>(w)] = static_cast<float>(input); if (++w >= n) w = 0; }
        speed += (speedTarget - speed) * 0.002;
        const double s = speed;
        const double xf = std::max(32.0, std::min(0.25 * sliceLen, 0.03 * fs));
        if (++count >= sliceLen) {
            count = 0;
            const double margin = xf + 64;
            double start;
            if (freeze) start = lastStart;
            else { start = w - (margin + std::max(0.0, s - 1) * (sliceLen + xf)); lastStart = start; }
            cur ^= 1;
            heads[cur].pos = start; heads[cur].g = 0; heads[cur].dir = 1;
            heads[cur ^ 1].dir = -1;
        }
        const double step = 1 / xf;
        double out = 0;
        for (auto& h : heads) {
            if (h.dir > 0) h.g = std::min(1.0, h.g + step);
            else if (h.dir < 0) h.g = std::max(0.0, h.g - step);
            if (h.g > 0) { out += readFrac(buf, h.pos) * std::sin(h.g * kPi * 0.5); h.pos += s; }
        }
        return out;
    }
};

// ─────────────────────────────── Core ─────────────────────────────────
enum class LoopState { Off, Play };
enum class LoopLed { Off, Play, Rec, Dub };

struct Params {
    double time = 0.5, mix = 0.5, length = 0.75, modWet = 0.5, clock = 1, modLoop = 0.5;
    int wetMode = 0, routing = 0, loopMode = 0;   // reverb/delay/slip, in/in+loop/loop, env/tape/stretch
    bool trails = true, dryKill = false, latch = false, spread = false, smooth = false;
    double balance = 0.5;
};

struct State {
    bool wetOn, freeze; LoopLed loop;
    int lsrc, winStart, winLen; double pos, clockHz; uint32_t histCycles;
    float peakIn, peakOut;
};

class Core {
public:
    Params p;

    void prepare(double sampleRate) {
        fs = sampleRate;
        s = Smoothed{p.time, p.mix, p.length, p.modWet, p.clock, p.modLoop, p.balance};
        hostCount = 0; phase = 0; clockRatio = 1; filterClock = -1;
        aa = reconL = reconR = LowPass4{};
        prev[0] = prev[1] = cur[0] = cur[1] = 0;
        reverb.init(fs); delay.init(fs); slip.init(fs);
        wetOn = freeze = wetMomentary = false;
        wetIn = wetOut = 0; modeGain[0] = 1; modeGain[1] = modeGain[2] = 0;
        decor[0].init(static_cast<int>(jsRound(113 * fs / 48000)), 0.6);
        decor[1].init(static_cast<int>(jsRound(337 * fs / 48000)), 0.6);
        loopDirect = 1;
        hist.assign(kLoopSamples, 0.f); loopBuf.assign(kLoopSamples, 0.f);
        histW = histCount = 0; histCycles = 0; lsrc = 0;
        loopState = LoopState::Off; overdub = false;
        loopGain = 0; loopPos = 0; tapeSpeed = 1; stretchSpeed = 0;
        env = 0; envGate = false; envHold = 0; envMix = 0; sliceStart = slicePh = 0;
        grain[0] = {0, 0}; grain[1] = {0.5, 0};
        rng = 22222;
        fsWet = FsWet{}; fsLoop = FsLoop{};
        engaged = 0; peakIn = peakOut = 0;
        loopWaveDirty = false;
    }

    // which: 0 = wet, 1 = loop. Mirrors MoodCore.footswitch.
    void footswitch(int which, bool down) {
        const int64_t now = hostCount;
        if (which == 0) {
            if (down) {
                fsWet = FsWet{true, now, false};
                if (!wetOn) { wetOn = true; wetMomentary = true; } else wetMomentary = false;
            } else if (fsWet.active) {
                const bool held = fsWet.held;
                fsWet.active = false;
                if (held) {
                    if (!p.latch) { freeze = false; if (wetMomentary) wetOn = false; }
                } else if (!wetMomentary) { wetOn = false; freeze = false; }
                wetMomentary = false;
            }
            return;
        }
        if (down) {
            const bool playing = loopState == LoopState::Play;
            fsLoop = FsLoop{true, now, false, histW, playing};
            if (playing) overdub = true;
        } else if (fsLoop.active) {
            const FsLoop d = fsLoop;
            fsLoop.active = false;
            if (d.playing) {
                overdub = false;
                if (!d.held) loopState = LoopState::Off;
                else loopWaveDirty = true;
            } else if (d.held) {
                int n = histW - d.histIdx;
                if (n < 0) n += static_cast<int>(hist.size());
                capture(n);
            } else capture(histCount);
        }
    }

    // in may be null (silence). Mirrors MoodCore.process.
    void process(const float* in, float* outL, float* outR, int n) {
        const double a = 1 - std::exp(-1 / (0.03 * fs));
        const double aClock = 1 - std::exp(-1 / ((p.smooth ? 1.5 : 0.08) * fs));
        const double holdN = kHoldSec * fs;
        if (fsWet.active && !fsWet.held && hostCount - fsWet.t >= holdN) {
            fsWet.held = true;
            freeze = p.latch ? !freeze : true;
        }
        if (fsLoop.active && !fsLoop.held && hostCount - fsLoop.t >= holdN) fsLoop.held = true;

        double pkIn = 0, pkOut = 0;
        for (int i = 0; i < n; ++i) {
            s.time += (p.time - s.time) * a;
            s.mix += (p.mix - s.mix) * a;
            s.length += (p.length - s.length) * a;
            s.modWet += (p.modWet - s.modWet) * a;
            s.modLoop += (p.modLoop - s.modLoop) * a;
            s.balance += (p.balance - s.balance) * a;
            s.clock += (p.clock - s.clock) * aClock;
            if ((i & 31) == 0) {
                const double c = std::min(1.0, clockHz(s.clock) / fs);
                clockRatio = c;
                if (std::fabs(c - filterClock) > 0.002 * c) {
                    filterClock = c;
                    const double fc = std::min(0.45 * c * fs, 20000.0);
                    aa.set(fc, fs); reconL.set(fc, fs); reconR.set(fc, fs);
                }
            }
            const double dry = in ? in[i] : 0.0;
            pkIn = std::max(pkIn, std::fabs(dry));
            const double xa = aa.run(dry);
            phase += clockRatio;
            if (phase >= 1) { phase -= 1; coreTick(xa); }
            const double f = phase;
            const double el = reconL.run(prev[0] + (cur[0] - prev[0]) * f);
            const double er = reconR.run(prev[1] + (cur[1] - prev[1]) * f);
            const double engagedT = wetOn || loopState == LoopState::Play ? 1.0 : 0.0;
            engaged += (engagedT - engaged) * 0.001;
            const double dg = p.dryKill ? 0.0 : 1 - engaged * (1 - std::min(1.0, 2 * (1 - s.mix)));
            const double wg = std::min(1.0, 2 * s.mix);
            const double l = softClip(dry * dg + el * wg);
            const double r = softClip(dry * dg + er * wg);
            outL[i] = static_cast<float>(l); outR[i] = static_cast<float>(r);
            pkOut = std::max(pkOut, std::max(std::fabs(l), std::fabs(r)));
            ++hostCount;
        }
        peakIn = std::max(pkIn, peakIn * 0.9);
        peakOut = std::max(pkOut, peakOut * 0.9);
    }

    State state() const {
        LoopLed led = loopState == LoopState::Play ? LoopLed::Play : LoopLed::Off;
        if (fsLoop.active && fsLoop.held && !fsLoop.playing) led = LoopLed::Rec;
        else if (overdub && loopState == LoopState::Play) led = LoopLed::Dub;
        const Window win = lsrc > 0 ? loopWindow() : Window{0, 0};
        return State{wetOn, freeze, led, lsrc, win.start, win.W, loopPos, clockRatio * fs, histCycles,
                     static_cast<float>(peakIn), static_cast<float>(peakOut)};
    }

    // Peak envelope of the captured loop, for drawing. Not real-time safe to call
    // concurrently with process(); the plugin copies under its own lock.
    void loopPeaks(float* out, int bins) const {
        for (int b = 0; b < bins; ++b) out[b] = 0;
        if (!lsrc) return;
        for (int b = 0; b < bins; ++b) {
            const int a0 = static_cast<int>((static_cast<int64_t>(b) * lsrc) / bins);
            const int z = std::max(a0 + 1, static_cast<int>((static_cast<int64_t>(b + 1) * lsrc) / bins));
            float pk = 0;
            for (int i = a0; i < z; i += 4) pk = std::max(pk, std::fabs(loopBuf[static_cast<size_t>(i)]));
            out[b] = pk;
        }
    }

    bool takeLoopWaveDirty() { const bool d = loopWaveDirty; loopWaveDirty = false; return d; }
    bool envGateOpen() const { return envGate; }
    int capturedLength() const { return lsrc; }

private:
    struct Smoothed { double time, mix, length, modWet, clock, modLoop, balance; };
    struct FsWet { bool active = false; int64_t t = 0; bool held = false; };
    struct FsLoop { bool active = false; int64_t t = 0; bool held = false; int histIdx = 0; bool playing = false; };
    struct Grain { double ph, start; };
    struct Window { int W, start; };

    double fs = 48000;
    Smoothed s{};
    int64_t hostCount = 0;
    double phase = 0, clockRatio = 1, filterClock = -1;
    LowPass4 aa, reconL, reconR;
    double prev[2] = {0, 0}, cur[2] = {0, 0};

    Reverb reverb; Delay delay; Slip slip;
    bool wetOn = false, freeze = false, wetMomentary = false;
    double wetIn = 0, wetOut = 0, modeGain[3] = {1, 0, 0};
    Allpass decor[2];
    double loopDirect = 1;

    std::vector<float> hist, loopBuf;
    int histW = 0, histCount = 0, lsrc = 0;
    uint32_t histCycles = 0;
    LoopState loopState = LoopState::Off;
    bool overdub = false;
    double loopGain = 0, loopPos = 0, tapeSpeed = 1, stretchSpeed = 0;
    double env = 0, envHold = 0, envMix = 0, sliceStart = 0, slicePh = 0;
    bool envGate = false;
    Grain grain[2] = {{0, 0}, {0.5, 0}};
    uint32_t rng = 22222;
    FsWet fsWet; FsLoop fsLoop;
    double engaged = 0, peakIn = 0, peakOut = 0;
    bool loopWaveDirty = false;

    double rand01() { rng = rng * 1103515245u + 12345u; return rng / 4294967296.0; }

    void capture(int n) {
        n = std::min({n, static_cast<int>(hist.size()) - 1, histCount});
        if (n < 64) return;
        const int N = static_cast<int>(hist.size());
        int r = histW - n; if (r < 0) r += N;
        for (int i = 0; i < n; ++i) { loopBuf[static_cast<size_t>(i)] = hist[static_cast<size_t>(r)]; if (++r >= N) r = 0; }
        lsrc = n; loopPos = 0;
        tapeSpeed = speedMap(s.modLoop, 0.04);
        stretchSpeed = stretchMap(s.modLoop);
        grain[0] = {0, 0}; grain[1] = {0.5, 0};
        loopState = LoopState::Play;
        loopWaveDirty = true;
    }

    Window loopWindow() const {
        if (p.loopMode != 1) return {std::max(64, lsrc), 0};
        const int W = std::max(64, static_cast<int>(std::floor(lsrc * expMap(s.length, 0.02, 1))));
        return {W, lsrc - W};
    }

    double loopTick(double x) {
        hist[static_cast<size_t>(histW)] = static_cast<float>(x);
        if (++histW >= static_cast<int>(hist.size())) { histW = 0; ++histCycles; }
        if (histCount < static_cast<int>(hist.size())) ++histCount;

        const double ax = std::fabs(x);
        env += (ax - env) * (ax > env ? 0.02 : 0.0005);

        const double target = loopState == LoopState::Play ? 1.0 : 0.0;
        loopGain += (target - loopGain) * 0.003;
        if (loopState == LoopState::Off && loopGain < 1e-4) { loopGain = 0; return 0; }
        if (lsrc < 64) return 0;

        const Window win = loopWindow();
        const int W = win.W, start = win.start;
        const double Wd = W;
        const int mode = p.loopMode;
        const double m = s.modLoop;
        double pos = jsMod(loopPos, Wd); if (pos < 0) pos += Wd;
        auto rd = [&](double q0) {
            double q = jsMod(q0, Wd); if (q < 0) q += Wd;
            const int i0 = static_cast<int>(std::floor(q)); const double f = q - i0;
            const int i1 = i0 + 1 >= W ? 0 : i0 + 1;
            const double a = loopBuf[static_cast<size_t>(start + i0)], b = loopBuf[static_cast<size_t>(start + i1)];
            return a + (b - a) * f;
        };
        const double F = std::min(0.004 * fs, Wd / 4);
        auto edgeAt = [&](double q0) { double q = jsMod(q0, Wd); if (q < 0) q += Wd; return std::min({1.0, q / F, (Wd - q) / F}); };
        double y;

        if (mode == 2) {
            const double G = std::min(expMap(s.length, 0.01 * fs, 0.5 * fs), Wd);
            const double dir = stretchSpeed < 0 ? -1.0 : 1.0;
            y = 0;
            for (auto& g : grain) {
                g.ph += 1 / G;
                if (g.ph >= 1) { g.ph -= 1; g.start = pos + (rand01() - 0.5) * 0.2 * G; }
                const double q = g.start + dir * g.ph * G;
                const double sw = std::sin(kPi * g.ph);
                y += rd(q) * sw * sw * edgeAt(q);
            }
            stretchSpeed += (stretchMap(m) - stretchSpeed) * 0.001;
            pos += stretchSpeed;
        } else if (mode == 1) {
            y = rd(pos) * edgeAt(pos);
            tapeSpeed += (speedMap(m, 0.04) - tapeSpeed) * 0.00025;
            pos += tapeSpeed;
        } else {
            const double thr = expMap(1 - m, 0.008, 0.35);
            if (env > thr) {
                if (!envGate) { envGate = true; sliceStart = pos; slicePh = 0; }
                envHold = 0.08 * fs;
            } else if (envGate && --envHold <= 0) envGate = false;
            envMix += ((envGate ? 1.0 : 0.0) - envMix) * 0.005;
            const double mainV = rd(pos) * edgeAt(pos);
            double sl = 0;
            if (envMix > 1e-4) {
                const double S = std::min(expMap(s.length, 0.01 * fs, 1.0 * fs), Wd);
                if (slicePh >= S) slicePh -= S;
                const double F2 = std::min(0.004 * fs, S / 4);
                sl = rd(sliceStart + slicePh) * std::min({1.0, slicePh / F2, (S - slicePh) / F2});
                slicePh += 1;
            }
            y = mainV * (1 - envMix) + sl * envMix;
            pos += 1;
        }

        if (overdub) {
            int q = static_cast<int>(jsMod(std::floor(mode == 1 ? loopPos : pos), Wd)); if (q < 0) q += W;
            float& cell = loopBuf[static_cast<size_t>(start + q)];
            cell = static_cast<float>(cell * 0.97 + x);
        }
        loopPos = jsMod(jsMod(pos, Wd) + Wd, Wd);
        return y * loopGain;
    }

    void coreTick(double x) {
        const double loopOut = loopTick(x);
        const double wetSrc = p.routing == 0 ? x : p.routing == 1 ? x + loopOut : loopOut;
        const double inT = wetOn ? 1.0 : 0.0;
        const double outT = wetOn || p.trails ? 1.0 : 0.0;
        wetIn += (inT - wetIn) * 0.003;
        wetOut += (outT - wetOut) * 0.003;
        for (int k = 0; k < 3; ++k) modeGain[k] += ((p.wetMode == k ? 1.0 : 0.0) - modeGain[k]) * 0.002;

        const double wIn = wetSrc * wetIn;
        double wl = 0, wr = 0;
        if (modeGain[0] > 1e-4) {
            reverb.setDecay(expMap(s.time, 0.4, 25));
            const double* o = reverb.tick(wIn, s.modWet, freeze, s.time > 0.985);
            wl += o[0] * modeGain[0]; wr += o[1] * modeGain[0];
        }
        double mono = 0;
        if (modeGain[1] > 1e-4) mono += delay.tick(wIn, expMap(s.time, 0.02 * fs, 2 * fs), s.modWet, freeze) * modeGain[1];
        if (modeGain[2] > 1e-4) mono += slip.tick(wIn, expMap(s.time, 0.05 * fs, 1.5 * fs), speedMap(s.modWet, 0.15), freeze) * modeGain[2];
        wl += mono;
        wr += p.spread ? decor[1].run(decor[0].run(mono)) : mono;
        if (p.spread) {
            const double mid = (wl + wr) * 0.5, side = (wl - wr) * 0.5 * 1.6;
            wl = mid + side; wr = mid - side;
        }
        wl *= wetOut; wr *= wetOut;

        const double directT = p.routing == 0 || !wetOn ? 1.0 : 0.0;
        loopDirect += (directT - loopDirect) * 0.003;
        const double bal = s.balance;
        const double wetLvl = std::min(1.0, 2 * (1 - bal)), loopLvl = std::min(1.0, 2 * bal);
        const double ld = loopOut * loopDirect * loopLvl;
        prev[0] = cur[0]; prev[1] = cur[1];
        cur[0] = wl * wetLvl + ld;
        cur[1] = wr * wetLvl + ld;
    }
};

}  // namespace mood
