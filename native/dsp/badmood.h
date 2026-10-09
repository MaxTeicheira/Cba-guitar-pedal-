// BadMoodEmu DSP core, C++ port of src/badmood-worklet.js (the reference).
//
// Header-only, C++17, no exceptions/RTTI. All buffers are allocated in prepare();
// process() never allocates, locks, or throws, so it is safe on an audio thread
// (and on an embedded target: the largest buffers are sized from the sample rate).
// Scalars are double, audio buffers float and spectral state double, matching the JS
// (Number / Float32Array / Float64Array), and every expression keeps the JS evaluation
// order, so the two implementations agree to float precision (see tests/parity).
// When changing anything, change the JS first and mirror it here line for line.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace badmood {

constexpr double kPi = 3.14159265358979323846;
constexpr double kTau = 2.0 * kPi;
constexpr double kHoldSec = 0.35;
constexpr double kPresetHoldSec = 3.0;
constexpr double kComboTapWindowSec = 1.5;
constexpr int kLoopSamples = 128000;      // HALF = 64000, the MOOD MKII buffer (derived)
constexpr int kLoopSamplesHalf = 64000;
constexpr int kBlk = 256;
constexpr int kRelayTaps = 8;
constexpr double kRelayTmaxSec = 1.5;
constexpr double kFlipLagMaxSec = 0.8;
constexpr double kFlipBufSec = 4.5;
constexpr int kSoupN = 1024;
constexpr int kSoupHop = 256;
constexpr int kSoupBins = kSoupN / 2 + 1;
constexpr double kRadioClean = 0.035;

constexpr double kClockStepsHz[11] = {2000, 3000, 4000, 6000, 8000, 12000, 16000, 24000, 32000, 48000, 64000};
constexpr int kClockStepTop[11] = {4, 16, 28, 40, 53, 69, 83, 95, 109, 121, 127};
constexpr double kClockStepCenter[11] = {2, 10.5, 22.5, 34.5, 47, 61.5, 76.5, 89.5, 102.5, 115.5, 124.5};
constexpr int kBurstDivs[9] = {64, 48, 32, 24, 16, 12, 8, 6, 4};
constexpr double kSyncFracs[9] = {1.0 / 32, 1.0 / 16, 1.0 / 12, 1.0 / 8, 1.0 / 6, 1.0 / 4, 1.0 / 3, 1.0 / 2, 1};
constexpr double kRadioCenters[5] = {0, 32.0 / 127, 63.0 / 127, 97.0 / 127, 1};
constexpr const char* kRadioNames[5] = {"tape", "ambient", "orchestral", "shoegaze", "dance"};
constexpr double kOrchRatios[6] = {1, 0.5, 2, 1.5, 0.75, 4.0 / 3};
constexpr double kOrchOffs[6] = {0, 0.5, 0.25, 0.75, 0.125, 0.625};
constexpr double kOrchPans[6] = {0, -0.7, 0.7, -0.35, 0.35, 0};
constexpr double kDanceSpeeds[3] = {1, 0.5, 2};
constexpr const char* kMaskNames[6] = {"octave down", "ring", "crush", "filter", "reverse", "octave up"};

// Flip: the 48 chords of the CC17 table (MIDI manual p.7-8). Codes 0 -oct, 1 -5th,
// 2 -4th, 3 +4th, 4 +5th, 5 +oct; just intervals.
constexpr double kIntervalRatios[6] = {0.5, 2.0 / 3, 0.75, 4.0 / 3, 1.5, 2};
constexpr const char* kIntervalNames[6] = {"-oct", "-5th", "-4th", "+4th", "+5th", "+oct"};
struct Chord { int n; int note[4]; };
constexpr Chord kFlipChords[48] = {
    {1, {0}}, {1, {1}}, {1, {2}}, {1, {3}}, {1, {4}}, {1, {5}},
    {2, {0, 1}}, {2, {1, 5}}, {2, {0, 2}}, {2, {2, 5}}, {2, {0, 3}}, {2, {3, 5}}, {2, {0, 4}}, {2, {4, 5}}, {2, {0, 5}},
    {2, {1, 2}}, {2, {2, 3}}, {2, {1, 3}}, {2, {1, 4}}, {2, {2, 4}}, {2, {3, 4}},
    {3, {0, 1, 2}}, {3, {1, 2, 3}}, {3, {5, 4, 3}}, {3, {4, 3, 2}}, {3, {0, 1, 3}}, {3, {1, 3, 4}}, {3, {5, 4, 1}}, {3, {4, 1, 0}},
    {3, {0, 1, 5}}, {3, {0, 2, 3}}, {3, {2, 3, 5}}, {3, {5, 4, 2}}, {3, {4, 2, 0}}, {3, {0, 2, 5}}, {3, {1, 2, 4}}, {3, {0, 3, 4}},
    {3, {5, 2, 1}}, {3, {0, 3, 5}}, {3, {5, 4, 0}},
    {4, {0, 1, 2, 3}}, {4, {1, 2, 4, 0}}, {4, {2, 5, 0, 1}}, {4, {4, 0, 1, 3}}, {4, {1, 3, 5, 0}}, {4, {4, 5, 0, 1}},
    {4, {3, 4, 0, 2}}, {4, {3, 5, 0, 2}},
};
constexpr int kFlipChordStart[48] = {0, 3, 6, 8, 11, 14, 16, 19, 22, 24, 27, 30, 32, 35, 38, 40, 43, 45, 48, 51, 53, 56, 59, 61,
                                     64, 67, 69, 72, 75, 77, 80, 83, 85, 88, 90, 93, 96, 98, 101, 104, 106, 109, 112, 114, 117, 120, 122, 125};

inline size_t uz(int i) { return static_cast<size_t>(i); }
inline int ifloor(double x) { return static_cast<int>(std::floor(x)); }
// JS truthiness of a number (non-zero and not NaN) without -Wfloat-equal.
inline bool nz(double v) { return v < 0 || v > 0; }
inline double clampd(double x, double a, double b) { return x < a ? a : x > b ? b : x; }
inline int clampi(int x, int a, int b) { return x < a ? a : x > b ? b : x; }
inline double expMap(double t, double a, double b) { return a * std::pow(b / a, t); }

inline int flipChordIndex(double m) {
    const int cc = clampi(ifloor(m * 127 + 0.5), 0, 127);
    int idx = 0;
    for (int i = 0; i < 48; ++i) if (cc >= kFlipChordStart[i]) idx = i;
    return idx;
}

inline double clockHz(double knob, bool smooth) {
    if (!smooth) {
        const int cc = clampi(ifloor(knob * 127 + 0.5), 0, 127);
        for (int i = 0; i < 11; ++i) if (cc <= kClockStepTop[i]) return kClockStepsHz[i];
        return kClockStepsHz[10];
    }
    const double x = knob * 127;
    if (x <= kClockStepCenter[0]) return kClockStepsHz[0];
    if (x >= kClockStepCenter[10]) return kClockStepsHz[10];
    for (int i = 0; i < 10; ++i) {
        if (x < kClockStepCenter[i + 1]) {
            const double t = (x - kClockStepCenter[i]) / (kClockStepCenter[i + 1] - kClockStepCenter[i]);
            return kClockStepsHz[i] * std::pow(kClockStepsHz[i + 1] / kClockStepsHz[i], t);
        }
    }
    return kClockStepsHz[10];
}

inline double speedMap(double m, double snapOct) {
    double s;
    if (m < 0.28) s = -std::pow(2.0, 1 - (2 * m) / 0.28);
    else if (m < 0.3) s = 0.5;
    else if (m < 0.5) s = std::pow(2.0, -(0.5 - m) / 0.2);
    else s = std::pow(2.0, (m - 0.5) / 0.5);
    if (snapOct > 0) {
        const double oct = std::log2(std::fabs(s));
        const double r = std::floor(oct + 0.5);
        if (std::fabs(oct - r) < snapOct) s = (s < 0 ? -1.0 : 1.0) * std::pow(2.0, r);
    }
    return s;
}

inline int relayRepeats(double m) {
    if (m >= 0.94) return 9;
    return std::min(kRelayTaps, 1 + ifloor((m / 0.94) * kRelayTaps));
}

struct Tuning { int a, b; double u; };
inline Tuning radioTuning(double m) {
    if (m <= kRadioCenters[0] + kRadioClean) return {0, -1, 0};
    for (int i = 0; i < 4; ++i) {
        if (m < kRadioCenters[i + 1] || i == 3) {
            const double lo = kRadioCenters[i] + kRadioClean, hi = kRadioCenters[i + 1] - kRadioClean;
            if (m <= lo) return {i, -1, 0};
            if (m >= hi) return {i + 1, -1, 0};
            return {i, i + 1, (m - lo) / (hi - lo)};
        }
    }
    return {4, -1, 0};
}

inline double softClip(double x) {
    const double ax = std::fabs(x);
    if (ax < 0.9) return x;
    return (x < 0 ? -1.0 : 1.0) * (0.9 + 0.1 * std::tanh((ax - 0.9) / 0.1));
}

struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    void lowpass(double fc, double fs, double q) {
        const double w = (kTau * clampd(fc, 10, fs * 0.49)) / fs;
        const double cw = std::cos(w), alpha = std::sin(w) / (2 * q), a0 = 1 + alpha;
        b0 = (1 - cw) / 2 / a0; b1 = (1 - cw) / a0; b2 = b0;
        a1 = (-2 * cw) / a0; a2 = (1 - alpha) / a0;
    }
    void highpass(double fc, double fs, double q) {
        const double w = (kTau * clampd(fc, 10, fs * 0.49)) / fs;
        const double cw = std::cos(w), alpha = std::sin(w) / (2 * q), a0 = 1 + alpha;
        b0 = (1 + cw) / 2 / a0; b1 = -(1 + cw) / a0; b2 = b0;
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
    Biquad a, b;
    void set(double fc, double fs) { a.lowpass(fc, fs, 0.5412); b.lowpass(fc, fs, 1.3066); }
    double run(double x) { return b.run(a.run(x)); }
};

inline double readFrac(const std::vector<float>& buf, double pos) {
    const double n = static_cast<double>(buf.size());
    double p = std::fmod(pos, n); if (p < 0) p += n;
    if (p >= n) p = 0;
    const int i0 = ifloor(p); const double f = p - i0;
    const int i1 = i0 + 1 == static_cast<int>(buf.size()) ? 0 : i0 + 1;
    const double a = buf[uz(i0)], b = buf[uz(i1)];
    return a + (b - a) * f;
}

struct Rng {
    uint32_t s = 0;
    explicit Rng(uint32_t seed = 0) : s(seed) {}
    double next() { s = s * 1103515245u + 12345u; return s / 4294967296.0; }
};

// Hand-written iterative radix-2 FFT, identical to the JS FFT class.
struct FFT {
    int n = 0;
    std::vector<double> cosT, sinT;
    std::vector<int> rev;
    void init(int size) {
        n = size;
        cosT.assign(uz(n / 2), 0.0); sinT.assign(uz(n / 2), 0.0);
        for (int i = 0; i < n / 2; ++i) { cosT[uz(i)] = std::cos((kTau * i) / n); sinT[uz(i)] = std::sin((kTau * i) / n); }
        rev.assign(uz(n), 0);
        int bits = 0; while ((1 << bits) < n) ++bits;
        for (int i = 0; i < n; ++i) {
            int r = 0;
            for (int b = 0; b < bits; ++b) if (i & (1 << b)) r |= 1 << (bits - 1 - b);
            rev[uz(i)] = r;
        }
    }
    void run(double* re, double* im, bool inverse) const {
        for (int i = 0; i < n; ++i) {
            const int j = rev[uz(i)];
            if (j > i) { std::swap(re[i], re[j]); std::swap(im[i], im[j]); }
        }
        const double sgn = inverse ? 1.0 : -1.0;
        for (int size = 2; size <= n; size *= 2) {
            const int half = size / 2, step = n / size;
            for (int i = 0; i < n; i += size) {
                for (int j = 0; j < half; ++j) {
                    const double wr = cosT[uz(j * step)], wi = sgn * sinT[uz(j * step)];
                    const int k = i + j, l = k + half;
                    const double tr = wr * re[l] - wi * im[l];
                    const double ti = wr * im[l] + wi * re[l];
                    re[l] = re[k] - tr; im[l] = im[k] - ti;
                    re[k] += tr; im[k] += ti;
                }
            }
        }
    }
};

// SOLA-style delay-line pitch shifter (see the JS Shifter).
struct Shifter {
    double d = -1, dOld = 0, x = 1;
    void reset() { d = -1; dOld = 0; x = 1; }
    double run(const std::vector<float>& buf, int w, double base, double ratio, double W) {
        const int N = static_cast<int>(buf.size());
        const int X = ifloor(W * 0.25);
        const double step = 1 - ratio;
        if (d < 0) d = ratio > 1 ? W : 2;
        d += step;
        if (x < 1) {
            dOld += step;
            x += 1.0 / X;
            if (x >= 1) x = 1;
        } else if ((ratio > 1 && d - X * (ratio - 1) < 2) || (ratio < 1 && d + X * (1 - ratio) > W)) {
            const int J = ifloor(W * 0.5), S = ifloor(W * 0.3);
            const int M = X < 256 ? X : 256;
            const double pA = w - base - d;
            double best = -1, bestScore = -1e300;
            for (int k = 0; k <= S; k += 2) {
                const double cand = ratio > 1 ? d + J + k : d - J - k;
                if (cand < 2) break;
                const double pB = w - base - cand;
                double c = 0, e = 1e-9;
                for (int j = 0; j < M; j += 4) {
                    int qa = ifloor(pA + j * ratio) % N; if (qa < 0) qa += N;
                    int qb = ifloor(pB + j * ratio) % N; if (qb < 0) qb += N;
                    const double b = buf[uz(qb)];
                    c += buf[uz(qa)] * b; e += b * b;
                }
                const double score = c / std::sqrt(e);
                if (score > bestScore) { bestScore = score; best = cand; }
            }
            if (best < 0) best = ratio > 1 ? d + J : 2;
            dOld = d; d = best; x = 0;
        }
        const double lim = N - base - 2;
        if (d > lim) d = lim; else if (d < 1) d = 1;
        double y = readFrac(buf, w - base - d);
        if (x < 1) y = readFrac(buf, w - base - dOld) * (1 - x) + y * x;
        return y;
    }
};

// ───────────────────────── Wet channel: SOUP ─────────────────────────
struct Soup {
    double fs = 48000;
    FFT fft;
    std::vector<double> win, outL, outR, re, im, re2, im2;
    std::vector<double> mag, amp, pr, pi, rr, ri, xr, xi, gk, jr, ji;
    std::vector<float> inRing;
    int inW = 0, hop = 0, outPos = 0;
    Rng rng{0x50a9};
    double keyT = -1, keyM = -1; int keyInf = -1;
    double out[2] = {0, 0};

    void init(double sr) {
        fs = sr;
        fft.init(kSoupN);
        win.assign(uz(kSoupN), 0.0);
        for (int i = 0; i < kSoupN; ++i) win[uz(i)] = 0.5 - 0.5 * std::cos((kTau * i) / kSoupN);
        inRing.assign(uz(kSoupN), 0.f);
        inW = 0; hop = 0; outPos = 0;
        for (auto* v : {&outL, &outR, &re, &im, &re2, &im2}) v->assign(uz(kSoupN), 0.0);
        for (auto* v : {&mag, &amp, &pr, &pi, &rr, &ri, &xr, &xi, &gk}) v->assign(uz(kSoupBins), 0.0);
        const double qr[4] = {1, 0, -1, 0}, qi[4] = {0, 1, 0, -1};
        for (int k = 0; k < kSoupBins; ++k) { pr[uz(k)] = 1; pi[uz(k)] = 0; rr[uz(k)] = qr[k & 3]; ri[uz(k)] = qi[k & 3]; }
        jr.assign(256, 0.0); ji.assign(256, 0.0);
        for (int i = 0; i < 256; ++i) { jr[uz(i)] = std::cos((kTau * i) / 256); ji[uz(i)] = std::sin((kTau * i) / 256); }
        rng = Rng(0x50a9);
        keyT = -1; keyM = -1; keyInf = -1;
        out[0] = out[1] = 0;
    }
    void updateDecay(double time, double m, bool infinite) {
        if ((infinite ? 1 : 0) == keyInf && std::fabs(time - keyT) < 1e-3 && std::fabs(m - keyM) < 1e-3) return;
        keyT = time; keyM = m; keyInf = infinite ? 1 : 0;
        if (infinite) { for (int k = 0; k < kSoupBins; ++k) gk[uz(k)] = 1; return; }
        const double t60 = expMap(time, 0.4, 40);
        const double g = std::pow(10.0, (-3.0 * kSoupHop) / (t60 * fs));
        const double damp = 1.5 * (1 - m);
        for (int k = 0; k < kSoupBins; ++k) gk[uz(k)] = std::pow(g, 1 + (damp * k) / kSoupBins);
    }
    void frame(double inGain, double m, bool spread) {
        const int N = kSoupN;
        for (int i = 0; i < N; ++i) {
            int idx = inW + i; if (idx >= N) idx -= N;
            re[uz(i)] = inRing[uz(idx)] * win[uz(i)]; im[uz(i)] = 0;
        }
        fft.run(re.data(), im.data(), false);
        const double jit = (1 - m) * (1 - m);
        for (int k = 0; k < kSoupBins; ++k) {
            const size_t K = uz(k);
            const double xr0 = re[K] * inGain, xi0 = im[K] * inGain;
            const double mg = std::sqrt(xr0 * xr0 + xi0 * xi0);
            const double held = mag[K] * gk[K];
            if (mg > held) {
                mag[K] = mg;
                const double zr = xr0 * xr[K] + xi0 * xi[K];
                const double zi = xi0 * xr[K] - xr0 * xi[K];
                const double zm = std::sqrt(zr * zr + zi * zi);
                if (zm > 1e-12) { rr[K] = zr / zm; ri[K] = zi / zm; }
            } else mag[K] = held;
            xr[K] = xr0; xi[K] = xi0;
            double p0 = pr[K] * rr[K] - pi[K] * ri[K];
            double p1 = pr[K] * ri[K] + pi[K] * rr[K];
            const size_t j = uz(ifloor((rng.next() - 0.5) * jit * 256) & 255);
            const double qr = p0 * jr[j] - p1 * ji[j];
            const double qi = p0 * ji[j] + p1 * jr[j];
            const double pn = std::sqrt(qr * qr + qi * qi);
            if (pn > 1e-12) { p0 = qr / pn; p1 = qi / pn; } else { p0 = 1; p1 = 0; }
            pr[K] = p0; pi[K] = p1;
        }
        const double blurW = 0.7 * jit;
        for (int k = 0; k < kSoupBins; ++k) {
            const double a = mag[uz(k)];
            const double lo = k > 0 ? mag[uz(k - 1)] : a, hi = k < kSoupBins - 1 ? mag[uz(k + 1)] : a;
            const double b = (lo + 2 * a + hi) * 0.25;
            double mx = a; if (lo > mx) mx = lo; if (hi > mx) mx = hi;
            double pk = mx > 1e-20 ? a / mx : 0;
            pk = pk * pk; pk = pk * pk;
            amp[uz(k)] = (blurW * b + (1 - blurW) * a) * ((1 - m) + m * pk);
        }
        synth(outL, false);
        if (spread) synth(outR, true);
        else { const int o = outPos; for (int i = 0; i < N; ++i) { int q = o + i; if (q >= N) q -= N; outR[uz(q)] = outL[uz(q)]; } }
    }
    void synth(std::vector<double>& acc, bool rot) {
        const int N = kSoupN;
        for (int k = 0; k < kSoupBins; ++k) {
            double p0 = pr[uz(k)], p1 = pi[uz(k)];
            if (rot) {
                const size_t j = uz((k * 37) & 255);
                const double tr = p0 * jr[j] - p1 * ji[j];
                p1 = p0 * ji[j] + p1 * jr[j]; p0 = tr;
            }
            re2[uz(k)] = amp[uz(k)] * p0; im2[uz(k)] = amp[uz(k)] * p1;
        }
        im2[0] = 0; im2[uz(N / 2)] = 0;
        for (int k = 1; k < N / 2; ++k) { re2[uz(N - k)] = re2[uz(k)]; im2[uz(N - k)] = -im2[uz(k)]; }
        fft.run(re2.data(), im2.data(), true);
        const double scale = 1.1 / (N * 1.5);
        const int o = outPos;
        for (int i = 0; i < N; ++i) {
            int q = o + i; if (q >= N) q -= N;
            acc[uz(q)] += re2[uz(i)] * win[uz(i)] * scale;
        }
    }
    const double* tick(double input, double time, double m, bool freeze, bool infinite, bool spread) {
        inRing[uz(inW)] = static_cast<float>(input);
        if (++inW >= kSoupN) inW = 0;
        const size_t o = uz(outPos);
        out[0] = outL[o]; out[1] = outR[o];
        outL[o] = 0; outR[o] = 0;
        if (++outPos >= kSoupN) outPos = 0;
        if (++hop >= kSoupHop) {
            hop = 0;
            updateDecay(time, m, freeze || infinite);
            frame(freeze ? 0 : 1, m, spread);
        }
        return out;
    }
};

// ───────────────────────── Wet channel: RELAY ─────────────────────────
struct Relay {
    double fs = 48000;
    std::vector<float> buf;
    int w = 0;
    double T = -1, tapG[kRelayTaps] = {}, infG = 0, frzG = 0, soloG = 0, slew = 0;
    double out[2] = {0, 0};
    void init(double sr) {
        fs = sr;
        buf.assign(uz(static_cast<int>(std::ceil(kRelayTmaxSec * fs * kRelayTaps)) + 16), 0.f);
        w = 0; T = -1; infG = frzG = soloG = 0;
        for (auto& g : tapG) g = 0;
        slew = 1 - std::exp(-1 / (0.08 * fs));
        out[0] = out[1] = 0;
    }
    void snap(double Tt, int nRep) {
        T = Tt;
        for (int k = 0; k < kRelayTaps; ++k) tapG[k] = nRep > kRelayTaps ? (k == 0 ? 1 : 0) : (k < nRep ? 1 : 0);
        infG = nRep > kRelayTaps ? 1 : 0;
        soloG = infG;
    }
    const double* tick(double input, double Tt, int nRep, bool freeze, bool spread) {
        if (T < 0) snap(Tt, nRep);
        T += (Tt - T) * slew;
        const double Tc = T;
        const bool inf = nRep > kRelayTaps;
        infG += ((inf ? 1.0 : 0.0) - infG) * 0.002;
        frzG += ((freeze ? 1.0 : 0.0) - frzG) * 0.005;
        const bool solo = inf || freeze;
        soloG += ((solo ? 1.0 : 0.0) - soloG) * 0.002;
        const double fb = readFrac(buf, w - Tc);
        double v = input * (1 - frzG) + fb * (infG + (1 - infG) * frzG);
        if (v > 1 || v < -1) v = std::tanh(v);
        buf[uz(w)] = static_cast<float>(v);
        double l = 0, r = 0;
        for (int k = 0; k < kRelayTaps; ++k) {
            const double tgt = solo ? (k == 0 ? 1 : 0) : (k < nRep ? 1 : 0);
            tapG[k] += (tgt - tapG[k]) * 0.002;
            if (tapG[k] > 1e-5) {
                const double s = readFrac(buf, w - (k + 1) * Tc) * tapG[k];
                if (!spread) { l += s; r += s; }
                else if (k & 1) r += s;
                else l += s;
            }
        }
        if (spread && soloG > 1e-5) r += readFrac(buf, w - 1.5 * Tc) * soloG;
        if (++w >= static_cast<int>(buf.size())) w = 0;
        out[0] = l * 0.8; out[1] = r * 0.8;
        return out;
    }
};

// ───────────────────────── Wet channel: FLIP ──────────────────────────
struct Flip {
    double fs = 48000;
    std::vector<float> buf;
    int w = 0;
    Shifter sh[4];
    double ratio[4] = {1, 1, 1, 1}, gain[4] = {}, norm = 1, lag = -1, frzG = 0, W = 0, slew = 0;
    int frzP = 0; bool wasFrozen = false;
    double out[2] = {0, 0};
    void init(double sr) {
        fs = sr;
        buf.assign(uz(static_cast<int>(std::ceil(kFlipBufSec * fs))), 0.f);
        w = 0;
        for (auto& s : sh) s.reset();
        for (int k = 0; k < 4; ++k) { ratio[k] = 1; gain[k] = 0; }
        norm = 1; lag = -1; frzP = 0; frzG = 0; wasFrozen = false;
        W = 0.05 * fs;
        slew = 1 - std::exp(-1 / (0.05 * fs));
        out[0] = out[1] = 0;
    }
    const double* tick(double input, double lagT, const Chord& chord, bool freeze, bool spread) {
        const int n = chord.n;
        if (lag < 0) {
            lag = lagT;
            for (int k = 0; k < 4; ++k) gain[k] = k < n ? 1 : 0;
            norm = 0.9 / std::sqrt(static_cast<double>(n));
        }
        lag += (lagT - lag) * slew;
        if (freeze && !wasFrozen) {
            int P = ifloor(n * lag + 0.5);
            const int minP = ifloor(0.25 * fs);
            if (P < minP) P = minP;
            if (P > static_cast<int>(buf.size()) - 2) P = static_cast<int>(buf.size()) - 2;
            frzP = P;
        }
        wasFrozen = freeze;
        frzG += ((freeze ? 1.0 : 0.0) - frzG) * 0.005;
        const int N = static_cast<int>(buf.size());
        int rp = w - frzP; if (rp < 0) rp += N;
        const double v = frzP > 0 ? input * (1 - frzG) + buf[uz(rp)] * frzG : input;
        buf[uz(w)] = static_cast<float>(v);
        norm += (0.9 / std::sqrt(static_cast<double>(n)) - norm) * 0.002;
        double l = 0, r = 0;
        for (int k = 0; k < 4; ++k) {
            if (k < n) ratio[k] = kIntervalRatios[chord.note[k]];
            gain[k] += ((k < n ? 1.0 : 0.0) - gain[k]) * 0.002;
            if (gain[k] > 1e-5) {
                const double y = sh[k].run(buf, w, k * lag, ratio[k], W) * gain[k];
                if (!spread) { l += y; r += y; }
                else {
                    const double pan = k == 0 ? -0.8 : k == 1 ? 0.8 : k == 2 ? -0.35 : 0.35;
                    l += y * (pan > 0 ? 1 - pan : 1);
                    r += y * (pan < 0 ? 1 + pan : 1);
                }
            }
        }
        if (++w >= N) w = 0;
        out[0] = l * norm; out[1] = r * norm;
        return out;
    }
};

// ─────────────────────────────── Core ─────────────────────────────────
// Every control the pedal saves in a preset (p.18). Toggles are 0/1/2, dips 0/1.
struct Params {
    double time = 0.5, mix = 0.5, length = 0.5, modWet = 0.5, clock = 1, modLoop = 0.5;
    int wetMode = 0, routing = 0, loopMode = 1;
    double miso = 0, spread = 0, dryKill = 0, latch = 0, trails = 0, smooth = 0, dryGlue = 0, half = 0;
    double rampTime = 0, rampModWet = 0, rampClock = 0, rampModLoop = 0, rampLength = 0, bounce = 0, sweep = 0, polarity = 0;
    double rampSpeed = 0.5;
    double cross = 0, balance = 0.5, fade = 1, eq = 0.5, glue = 0.2, blend = 0;
    int sync = 1, spreadSel = 1, inputMod = 1;
    double volume = 0.5;
};

// Parameter names, in the same order as PARAM_DEFAULTS in the JS (used by the parity
// renderer, the plugin's parameter layout and presets).
constexpr const char* kParamNames[] = {
    "time", "mix", "length", "modWet", "clock", "modLoop", "wetMode", "routing", "loopMode",
    "miso", "spread", "dryKill", "latch", "trails", "smooth", "dryGlue", "half",
    "rampTime", "rampModWet", "rampClock", "rampModLoop", "rampLength", "bounce", "sweep", "polarity", "rampSpeed",
    "cross", "balance", "fade", "eq", "glue", "blend", "sync", "spreadSel", "inputMod", "volume"};
constexpr int kNumParams = static_cast<int>(sizeof(kParamNames) / sizeof(kParamNames[0]));

// Pointer to a parameter by name, so the renderer and the plugin share one mapping.
struct ParamRef { double* d; int* i; };
inline ParamRef paramRef(Params& p, const char* name) {
    struct E { const char* n; double Params::*d; int Params::*i; };
    static const E table[] = {
        {"time", &Params::time, nullptr}, {"mix", &Params::mix, nullptr}, {"length", &Params::length, nullptr},
        {"modWet", &Params::modWet, nullptr}, {"clock", &Params::clock, nullptr}, {"modLoop", &Params::modLoop, nullptr},
        {"wetMode", nullptr, &Params::wetMode}, {"routing", nullptr, &Params::routing}, {"loopMode", nullptr, &Params::loopMode},
        {"miso", &Params::miso, nullptr}, {"spread", &Params::spread, nullptr}, {"dryKill", &Params::dryKill, nullptr},
        {"latch", &Params::latch, nullptr}, {"trails", &Params::trails, nullptr}, {"smooth", &Params::smooth, nullptr},
        {"dryGlue", &Params::dryGlue, nullptr}, {"half", &Params::half, nullptr},
        {"rampTime", &Params::rampTime, nullptr}, {"rampModWet", &Params::rampModWet, nullptr}, {"rampClock", &Params::rampClock, nullptr},
        {"rampModLoop", &Params::rampModLoop, nullptr}, {"rampLength", &Params::rampLength, nullptr}, {"bounce", &Params::bounce, nullptr},
        {"sweep", &Params::sweep, nullptr}, {"polarity", &Params::polarity, nullptr}, {"rampSpeed", &Params::rampSpeed, nullptr},
        {"cross", &Params::cross, nullptr}, {"balance", &Params::balance, nullptr}, {"fade", &Params::fade, nullptr},
        {"eq", &Params::eq, nullptr}, {"glue", &Params::glue, nullptr}, {"blend", &Params::blend, nullptr},
        {"sync", nullptr, &Params::sync}, {"spreadSel", nullptr, &Params::spreadSel}, {"inputMod", nullptr, &Params::inputMod},
        {"volume", &Params::volume, nullptr},
    };
    for (const auto& e : table)
        if (std::strcmp(e.n, name) == 0) return {e.d ? &(p.*(e.d)) : nullptr, e.i ? &(p.*(e.i)) : nullptr};
    return {nullptr, nullptr};
}
// Mirrors BadMoodCore.setParam: toggles are rounded to the nearest position.
inline bool setParam(Params& p, const char* name, double v) {
    const ParamRef r = paramRef(p, name);
    if (r.d) { *r.d = v; return true; }
    if (r.i) { *r.i = ifloor(v + 0.5); return true; }
    return false;
}
inline double getParam(Params& p, const char* name) {
    const ParamRef r = paramRef(p, name);
    return r.d ? *r.d : r.i ? static_cast<double>(*r.i) : 0.0;
}

enum class Event { LoopPlay, LoopRec, HiddenOn, HiddenOff, TrueBypassOn, TrueBypassOff, PresetSave1, PresetSave2 };
enum class LoopLed { Rec, Play, Dub };
enum FsAction { FsUp = 0, FsDown = 1, FsShift = 2 };

struct State {
    bool wetOn, freeze, hidden, trueBypass, scramble;
    LoopLed loop;
    int pos, len, seam; uint32_t cycles;
    double clockHz;
    int nSlices, step, station, station2; double tune, mask;
    float peakIn, peakOut;
};

class Core {
public:
    Params p;

    void prepare(double sampleRate) {
        fs = sampleRate;
        started = false;
        hostCount = 0; phase = 0; clockRatio = 1; filterClock = -1;
        aa = reconL = reconR = LowPass4{};
        eqL = eqR = Biquad{}; eqKey = -1; glueKey = -1;
        gDrive = 1; gBias = 0; gTB = 0; gMake = 1; gDestroy = 0; gQ = 2048;
        prev[0] = prev[1] = cur[0] = cur[1] = 0;
        tbG = 0; vol = 1; rampShape = 1;
        soup.init(fs); relay.init(fs); flip.init(fs);
        wetOn = freeze = false; wetIn = wetOut = 0;
        for (auto& g : modeGain) g = 0;
        xW = 1; lW = 0; loopDirect = 1;
        buf.assign(uz(kLoopSamples), 0.f);
        L = kLoopSamples; pos = 0; seam = 0; cycles = 0;
        loopPlay = false; overdub = false; loopGain = 0;
        for (auto& g : lmGain) g = 0;
        lastLoopMode = -1;
        blockE.assign(uz((kLoopSamples + kBlk - 1) / kBlk), 0.0);
        eAcc = 0;
        rng = Rng(22222);
        for (int i = 0; i < 8; ++i) { slices[i] = 0; cand[i] = 0; }
        nSlices = 1; stepIdx = 0; stepPh = 0; stepLen = 0; curSlice = 0;
        inEnv = 0; scramble = false; scrHold = 0;
        for (auto& a : stActive) a = 0;
        tp = 0; tapeSpeed = 1;
        amPos = 0; for (int g = 0; g < 4; ++g) { amPh[g] = 0; amStart[g] = 0; sgPh[g] = 0; sgJit[g] = 0; }
        for (int j = 0; j < 6; ++j) { orPos[j] = 0; orEnv[j] = 0; orTgt[j] = 0; orCnt[j] = 0; }
        orNorm = 0.8; sgLp = 0; dnPos = 0; dnCnt = 0; dnIdx = 0;
        rStatic = 0; rWhPh = 0; rLp = 0; rHp = 0;
        so[0] = so[1] = 0;
        seamF = std::min(0.004 * fs, kLoopSamples / 4.0);
        maskRing.assign(uz(static_cast<int>(std::ceil(0.25 * fs))), 0.f);
        maskW = 0; maskEnv = 0; maskG = 0;
        maskSh[0].reset(); maskSh[1].reset();
        ringPh = 0; crushCnt = 0; crushV = 0; svfLow = 0; svfBand = 0; revPh = 0; revStart = 0;
        const int cl = static_cast<int>(std::ceil(0.03 * fs)) + 4;
        for (auto& b : crossBuf) b.assign(uz(cl), 0.f);
        crossW = 0; crossEnv = 0; crossPh = 0; crossD = 0;
        decorBuf.assign(uz(static_cast<int>(std::ceil(0.02 * fs)) + 4), 0.f);
        decorW = 0;
        rampPh = 1; rampTrig = false; lastEngaged = false;
        sw[0] = sw[1] = Switch{};
        comboT = 0; comboFirst = 0; comboFirstLong = false;
        hidden = false; presetDone = false;
        nComboTaps = 0; trueBypass = false;
        engaged = 0; nEvents = 0;
        peakIn = peakOut = 0;
        lastStations = {0, -1, 0};
    }

    // Mirrors BadMoodCore.command (MIDI CC 102/103/105/106/55).
    void command(const char* name, bool on) {
        if (std::strcmp(name, "wet") == 0) { wetOn = on; if (!on) freeze = false; }
        else if (std::strcmp(name, "loop") == 0) { if (on && !loopPlay) engageLoop(); else if (!on) { loopPlay = false; overdub = false; } }
        else if (std::strcmp(name, "freeze") == 0) { freeze = on; if (on) wetOn = true; }
        else if (std::strcmp(name, "overdub") == 0) { if (on && !loopPlay) engageLoop(); overdub = on; }
        else if (std::strcmp(name, "trueBypass") == 0) trueBypass = on;
    }

    // which: 0 = left (wet), 1 = right (loop). Mirrors BadMoodCore.footswitch.
    void footswitch(int which, FsAction action) {
        const int w = which == 0 ? 0 : 1;
        Switch& f = sw[w];
        Switch& o = sw[1 - w];
        const int64_t now = hostCount;
        if (action == FsDown) {
            if (f.down) return;
            f.down = true; f.t = now; f.held = false; f.combo = false; f.shift = false; f.momentary = false;
            if (o.down) {
                f.combo = true;
                if (!o.combo) { cancelHold(1 - w); o.combo = true; }
                comboT = now;
                comboFirst = 1 - w;
                comboFirstLong = static_cast<double>(now - o.t) >= kPresetHoldSec * fs;
                hidden = false; presetDone = false;
            }
            return;
        }
        if (action == FsShift) {
            if (f.down && !f.combo && !f.shift) { f.shift = true; cancelHold(w); }
            return;
        }
        if (!f.down) return;
        f.down = false;
        if (f.combo) {
            if (!o.down) {
                if (!hidden && !presetDone && static_cast<double>(now - comboT) < kHoldSec * fs) comboTap(now);
                if (hidden) pushEvent(Event::HiddenOff);
                hidden = false;
            }
            return;
        }
        if (f.shift) return;
        if (trueBypass) {
            if (!f.held) { trueBypass = false; pushEvent(Event::TrueBypassOff); }
            return;
        }
        if (w == 0) {
            if (f.held) {
                if (!nz(p.latch)) { freeze = false; if (f.momentary) wetOn = false; }
            } else {
                wetOn = !wetOn;
                if (!wetOn) freeze = false;
            }
        } else if (f.held) {
            if (!nz(p.latch)) overdub = false;
        } else if (!loopPlay) {
            engageLoop();
        } else {
            loopPlay = false; overdub = false;
            pushEvent(Event::LoopRec);
        }
    }

    // in may be null (silence); inR may be null (mono). Mirrors BadMoodCore.process.
    void process(const float* inL, const float* inR, float* outL, float* outR, int n) {
        if (!started) { started = true; snapSmoothers(); }
        const double a = 1 - std::exp(-1 / (0.03 * fs));
        const double aClock = 1 - std::exp(-1 / (0.06 * fs));
        updateSwitches();

        const bool rampOn = nz(p.rampTime) || nz(p.rampModWet) || nz(p.rampClock) || nz(p.rampModLoop) || nz(p.rampLength);
        const bool engagedNow = wetOn || loopPlay;
        if (engagedNow && !lastEngaged) rampTrig = true;
        lastEngaged = engagedNow;
        rampShape = 1;
        if (rampOn) {
            const double inc = n / (expMap(p.rampSpeed, 20, 0.25) * fs);
            if (nz(p.bounce)) {
                rampPh += inc; rampPh -= std::floor(rampPh);
                rampShape = rampPh < 0.5 ? 2 * rampPh : 2 - 2 * rampPh;
            } else {
                if (rampTrig) rampPh = 0;
                rampPh = std::min(1.0, rampPh + inc);
                rampShape = rampPh;
            }
        }
        rampTrig = false;
        const double tTime = nz(p.rampTime) ? rampValue(p.time) : p.time;
        const double tModWet = nz(p.rampModWet) ? rampValue(p.modWet) : p.modWet;
        const double tModLoop = nz(p.rampModLoop) ? rampValue(p.modLoop) : p.modLoop;
        const double tLength = nz(p.rampLength) ? rampValue(p.length) : p.length;
        const double tClockHz = clockHz(nz(p.rampClock) ? rampValue(p.clock) : p.clock, nz(p.smooth));

        double pkIn = 0, pkOut = 0;
        for (int i = 0; i < n; ++i) {
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

            if ((i & 31) == 0) {
                const double c = std::min(1.0, s.clockHz / fs);
                clockRatio = c;
                if (std::fabs(c - filterClock) > 0.002 * c) {
                    filterClock = c;
                    const double fc = std::min(0.45 * c * fs, 20000.0);
                    aa.set(fc, fs); reconL.set(fc, fs); reconR.set(fc, fs);
                }
                if (std::fabs(s.eq - eqKey) > 1e-4) {
                    eqKey = s.eq;
                    if (s.eq < 0.5) { const double f = expMap(s.eq / 0.5, 500, 20000); eqL.lowpass(f, fs, 0.707); eqR.lowpass(f, fs, 0.707); }
                    else { const double f = expMap((s.eq - 0.5) / 0.5, 20, 2500); eqL.highpass(f, fs, 0.707); eqR.highpass(f, fs, 0.707); }
                }
                vol = std::pow(10.0, ((s.volume - 0.5) * 24) / 20);
                if (std::fabs(s.glue - glueKey) > 1e-4) {
                    glueKey = s.glue;
                    const double g = s.glue;
                    gDrive = expMap(g, 1, 50);
                    gBias = 0.2 * g;
                    gTB = std::tanh(gBias);
                    gMake = 0.5 / (std::tanh(gDrive * 0.5 + gBias) - gTB);
                    gDestroy = clampd((g - 0.55) / 0.45, 0, 1);
                    gQ = expMap(gDestroy, 2048, 6);
                }
            }

            const double dL = inL ? static_cast<double>(inL[i]) : 0.0;
            const double dR = nz(p.miso) ? dL : inR ? static_cast<double>(inR[i]) : dL;
            const double mono = nz(p.miso) || !inR ? dL : 0.5 * (dL + dR);
            const double ad = std::max(std::fabs(dL), std::fabs(dR)); if (ad > pkIn) pkIn = ad;
            const double xa = aa.run(mono);
            phase += clockRatio;
            if (phase >= 1) { phase -= 1; coreTick(xa); }
            const double f = phase;
            double el = reconL.run(prev[0] + (cur[0] - prev[0]) * f);
            double er = reconR.run(prev[1] + (cur[1] - prev[1]) * f);
            if (std::fabs(s.eq - 0.5) > 0.01) { el = eqL.run(el); er = eqR.run(er); }
            el *= vol; er *= vol;

            const double engagedT = wetOn || loopPlay ? 1.0 : 0.0;
            engaged += (engagedT - engaged) * 0.001;
            const double dg = nz(p.dryKill) ? 0.0 : 1 - engaged * (1 - std::min(1.0, 2 * (1 - s.mix)));
            const double wg = std::min(1.0, 2 * s.mix);
            double l, r;
            if (nz(p.dryGlue) && engaged > 1e-4) {
                const double gl = glue(dL * dg + el * wg), gr = glue(dR * dg + er * wg);
                const double ul = dL * dg + glue(el) * wg, ur = dR * dg + glue(er) * wg;
                l = ul + (gl - ul) * engaged; r = ur + (gr - ur) * engaged;
            } else {
                l = dL * dg + glue(el) * wg;
                r = dR * dg + glue(er) * wg;
            }
            tbG += ((trueBypass ? 1.0 : 0.0) - tbG) * 0.002;
            if (tbG > 1e-5) { l += (dL - l) * tbG; r += (dR - r) * tbG; }
            l = softClip(l); r = softClip(r);
            outL[i] = static_cast<float>(l); outR[i] = static_cast<float>(r);
            const double ao = std::max(std::fabs(l), std::fabs(r)); if (ao > pkOut) pkOut = ao;
            ++hostCount;
        }
        peakIn = std::max(pkIn, peakIn * 0.9);
        peakOut = std::max(pkOut, peakOut * 0.9);
    }

    State state() const {
        return State{wetOn, freeze, hidden, trueBypass, scramble,
                     !loopPlay ? LoopLed::Rec : overdub ? LoopLed::Dub : LoopLed::Play,
                     pos, L, seam, cycles, clockRatio * fs, nSlices, curSlice,
                     lastStations.a, lastStations.b, lastStations.u, maskG,
                     static_cast<float>(peakIn), static_cast<float>(peakOut)};
    }

    // Peak envelope of the loop buffer, for drawing. Not real-time safe to call
    // concurrently with process(); the plugin copies under its own lock.
    void loopPeaks(float* out, int bins) const {
        const int n = L;
        for (int b = 0; b < bins; ++b) {
            const int a0 = static_cast<int>((static_cast<int64_t>(b) * n) / bins);
            const int z = std::max(a0 + 1, static_cast<int>((static_cast<int64_t>(b + 1) * n) / bins));
            float pk = 0;
            for (int i = a0; i < z; i += 4) pk = std::max(pk, std::fabs(buf[uz(i)]));
            out[b] = pk;
        }
    }
    int sliceStart(int i) const { return slices[i]; }

    // Events since the last call (loop engaged, hidden options, true bypass, preset save).
    int takeEvents(Event* out, int max) {
        const int n = std::min(nEvents, max);
        for (int i = 0; i < n; ++i) out[i] = events[i];
        nEvents = 0;
        return n;
    }

    // ---- read access for tests ----
    bool isLoopPlaying() const { return loopPlay; }
    bool isOverdubbing() const { return overdub; }
    bool isFrozen() const { return freeze; }
    bool isWetOn() const { return wetOn; }
    int loopLength() const { return L; }
    int sliceCount() const { return nSlices; }
    const std::vector<float>& loopBuffer() const { return buf; }
    int64_t samplesProcessed() const { return hostCount; }

private:
    struct Smoothed { double time = 0, mix = 0, length = 0, modWet = 0, modLoop = 0, balance = 0, cross = 0, blend = 0, glue = 0, volume = 0, eq = 0, clockHz = 0; };
    struct Switch { bool down = false; int64_t t = 0; bool held = false, combo = false, shift = false, momentary = false; };

    double fs = 48000;
    Smoothed s{};
    bool started = false;
    int64_t hostCount = 0;
    double phase = 0, clockRatio = 1, filterClock = -1;
    LowPass4 aa, reconL, reconR;
    Biquad eqL, eqR;
    double eqKey = -1, glueKey = -1;
    double gDrive = 1, gBias = 0, gTB = 0, gMake = 1, gDestroy = 0, gQ = 2048;
    double prev[2] = {0, 0}, cur[2] = {0, 0};
    double tbG = 0, vol = 1, rampShape = 1;

    Soup soup; Relay relay; Flip flip;
    bool wetOn = false, freeze = false;
    double wetIn = 0, wetOut = 0, modeGain[3] = {0, 0, 0};
    double xW = 1, lW = 0, loopDirect = 1;

    std::vector<float> buf;
    int L = kLoopSamples, pos = 0, seam = 0;
    uint32_t cycles = 0;
    bool loopPlay = false, overdub = false;
    double loopGain = 0, lmGain[3] = {0, 0, 0};
    int lastLoopMode = -1;
    std::vector<double> blockE;
    double eAcc = 0;
    Rng rng{22222};
    int slices[8] = {}, cand[8] = {};
    int nSlices = 1, stepIdx = 0, stepPh = 0, stepLen = 0, curSlice = 0;
    double inEnv = 0; bool scramble = false; int scrHold = 0;
    int stActive[5] = {};
    double tp = 0, tapeSpeed = 1;
    double amPos = 0, amPh[4] = {}, amStart[4] = {};
    double orPos[6] = {}, orEnv[6] = {}, orTgt[6] = {}; int orCnt[6] = {};
    double orNorm = 0.8;
    double sgPh[4] = {}, sgJit[4] = {}, sgLp = 0;
    double dnPos = 0; int dnCnt = 0, dnIdx = 0;
    double rStatic = 0, rWhPh = 0, rLp = 0, rHp = 0;
    double so[2] = {0, 0};
    double seamF = 192;
    std::vector<float> maskRing;
    int maskW = 0;
    double maskEnv = 0, maskG = 0;
    Shifter maskSh[2];
    double ringPh = 0; int crushCnt = 0; double crushV = 0, svfLow = 0, svfBand = 0, revPh = 0, revStart = 0;
    std::vector<float> crossBuf[4];
    int crossW = 0;
    double crossEnv = 0, crossPh = 0, crossD = 0;
    std::vector<float> decorBuf;
    int decorW = 0;
    double rampPh = 1; bool rampTrig = false, lastEngaged = false;
    Switch sw[2];
    int64_t comboT = 0; int comboFirst = 0; bool comboFirstLong = false;
    bool hidden = false, presetDone = false;
    int64_t comboTaps[3] = {}; int nComboTaps = 0;
    bool trueBypass = false;
    double engaged = 0;
    Event events[16] = {}; int nEvents = 0;
    double peakIn = 0, peakOut = 0;
    Tuning lastStations{0, -1, 0};

    void pushEvent(Event e) { if (nEvents < 16) events[nEvents++] = e; }

    void cancelHold(int w) {
        Switch& f = sw[w];
        if (!f.held) return;
        if (w == 0) {
            if (nz(p.latch)) freeze = !freeze; else freeze = false;
            if (f.momentary) wetOn = false;
        } else if (nz(p.latch)) overdub = !overdub;
        else overdub = false;
        f.held = false;
    }

    void comboTap(int64_t now) {
        const double win = kComboTapWindowSec * fs;
        if (nComboTaps == 3) { comboTaps[0] = comboTaps[1]; comboTaps[1] = comboTaps[2]; nComboTaps = 2; }
        comboTaps[nComboTaps++] = now;
        if (nComboTaps == 3 && static_cast<double>(now - comboTaps[0]) <= win) {
            trueBypass = !trueBypass;
            nComboTaps = 0;
            pushEvent(trueBypass ? Event::TrueBypassOn : Event::TrueBypassOff);
        }
    }

    void updateSwitches() {
        const int64_t now = hostCount;
        const double holdN = kHoldSec * fs;
        for (int w = 0; w < 2; ++w) {
            Switch& f = sw[w];
            if (!f.down || f.held || f.combo || f.shift || trueBypass || static_cast<double>(now - f.t) < holdN) continue;
            f.held = true;
            if (w == 0) {
                if (!wetOn) { wetOn = true; f.momentary = true; }
                freeze = nz(p.latch) ? !freeze : true;
            } else {
                if (!loopPlay) engageLoop();
                overdub = nz(p.latch) ? !overdub : true;
            }
        }
        const Switch& a = sw[0];
        const Switch& b = sw[1];
        if (a.down && b.down && a.combo && b.combo) {
            if (!hidden && !presetDone && static_cast<double>(now - comboT) >= holdN) { hidden = true; pushEvent(Event::HiddenOn); }
            if (comboFirstLong && !presetDone && static_cast<double>(now - comboT) >= kPresetHoldSec * fs) {
                presetDone = true;
                hidden = false;
                pushEvent(comboFirst == 1 ? Event::PresetSave2 : Event::PresetSave1);
            }
        }
    }

    void engageLoop() {
        loopPlay = true;
        seam = pos;
        resetLoopModes();
        pushEvent(Event::LoopPlay);
    }

    double loopRead(double q) const {
        const double Ld = L;
        double x = std::fmod(q, Ld); if (x < 0) x += Ld;
        if (x >= Ld) x = 0;
        const int i0 = ifloor(x); const double f = x - i0;
        const int i1 = i0 + 1 >= L ? 0 : i0 + 1;
        const double b0 = buf[uz(i0)], b1 = buf[uz(i1)];
        const double v = b0 + (b1 - b0) * f;
        double d = x - seam; if (d < 0) d += Ld;
        const double F = seamF;
        double g = d / F; const double g2 = (Ld - d) / F;
        if (g2 < g) g = g2;
        if (g > 1) g = 1;
        return v * g;
    }

    void resetLoopModes() {
        computeSlices();
        stepIdx = nSlices - 1; stepPh = 0; stepLen = 0; curSlice = 0;
        for (auto& st : stActive) st = 0;
        maskG = 0;
    }

    void computeSlices() {
        const int nb = (L + kBlk - 1) / kBlk;
        const double floorE = kBlk * 1e-6;
        const int minSep = std::max(2, nb / 16);
        int n = 0;
        for (int pass = 0; pass < 8; ++pass) {
            int best = -1; double bestS = 3;
            for (int b = 0; b < nb; ++b) {
                const double e = blockE[uz(b)];
                if (e <= floorE) continue;
                const double prv = blockE[uz(b == 0 ? nb - 1 : b - 1)];
                const double sc = e / (prv + floorE);
                if (sc <= bestS) continue;
                bool near = false;
                for (int c = 0; c < n; ++c) {
                    int d = b - cand[c]; if (d < 0) d = -d;
                    if (nb - d < d) d = nb - d;
                    if (d < minSep) { near = true; break; }
                }
                if (!near) { best = b; bestS = sc; }
            }
            if (best < 0) break;
            cand[n++] = best;
        }
        if (n == 0) {
            for (int i = 0; i < 4; ++i) slices[i] = static_cast<int>((static_cast<int64_t>(i) * L) / 4);
            nSlices = 4;
            return;
        }
        for (int i = 1; i < n; ++i) {
            const int v = cand[i]; int j = i - 1;
            while (j >= 0 && cand[j] > v) { cand[j + 1] = cand[j]; --j; }
            cand[j + 1] = v;
        }
        for (int i = 0; i < n; ++i) slices[i] = cand[i] * kBlk;
        nSlices = n;
    }

    double burstTick(double x) {
        const double ax = std::fabs(x);
        inEnv += (ax - inEnv) * (ax > inEnv ? 0.01 : 0.0005);
        const double thr = expMap(1 - s.modLoop, 0.01, 0.5);
        if (inEnv > thr) { scramble = true; scrHold = ifloor(0.25 * fs); }
        else if (scramble && --scrHold <= 0) scramble = false;
        const int di = clampi(ifloor(s.length * 8 + 0.5), 0, 8);
        int D = L / kBurstDivs[di]; if (D < 64) D = 64;
        if (stepPh >= stepLen) {
            stepPh = 0;
            if (scramble) {
                curSlice = ifloor(rng.next() * nSlices);
                stepLen = rng.next() < 0.35 ? D / 2 : D;
            } else {
                if (stepIdx + 1 >= nSlices) { computeSlices(); stepIdx = -1; }
                ++stepIdx;
                curSlice = stepIdx;
                stepLen = D;
            }
        }
        double F = 0.003 * fs; if (F > stepLen / 4.0) F = stepLen / 4.0;
        double e = stepPh / F; const double e2 = (stepLen - stepPh) / F;
        if (e2 < e) e = e2;
        if (e > 1) e = 1;
        const double y = loopRead(static_cast<double>(slices[curSlice] + stepPh)) * e;
        ++stepPh;
        const bool right = (curSlice & 1) == 1;
        so[0] = right ? y * 0.45 : y; so[1] = right ? y : y * 0.45;
        return y;
    }

    double stationTick(int st, bool spread) {
        const double len = s.length;
        const double Ld = L;
        double y = 0, l = 0, r = 0;
        if (!stActive[st]) {
            stActive[st] = 1;
            if (st == 0) { tp = pos; tapeSpeed = speedMap(len, 0.04); }
            else if (st == 1) { amPos = pos; for (int g = 0; g < 4; ++g) { amPh[g] = g * 0.25; amStart[g] = pos; } }
            else if (st == 2) {
                for (int j = 0; j < 6; ++j) { orPos[j] = pos + kOrchOffs[j] * Ld; orEnv[j] = j == 0 ? 1 : 0; orTgt[j] = j == 0 ? 1 : 0; orCnt[j] = 0; }
            } else if (st == 3) { for (int g = 0; g < 4; ++g) { sgPh[g] = g * 0.25; sgJit[g] = 0; } sgLp = 0; }
            else { dnPos = pos; dnCnt = 0; dnIdx = 0; }
        }
        if (st == 0) {
            y = loopRead(tp);
            tapeSpeed += (speedMap(len, 0.04) - tapeSpeed) * 0.0005;
            tp += tapeSpeed;
            if (tp >= Ld) tp -= Ld; else if (tp < 0) tp += Ld;
            l = y; r = y;
        } else if (st == 1) {
            const double speed = std::pow(2.0, -4 * (1 - len));
            double G = 0.2 * fs; if (G > Ld / 2) G = Ld / 2;
            const double J = ((1 - speed) * 0.5 + 0.02) * G;
            for (int g = 0; g < 4; ++g) {
                amPh[g] += 1 / G;
                if (amPh[g] >= 1) { amPh[g] -= 1; amStart[g] = amPos + (rng.next() - 0.5) * 2 * J; }
                const double sn = std::sin(kPi * amPh[g]);
                const double v = loopRead(amStart[g] + amPh[g] * G) * sn * sn * 0.5;
                y += v;
                if (g & 1) { l += v * 0.6; r += v; } else { l += v; r += v * 0.6; }
            }
            amPos += speed;
            if (amPos >= Ld) amPos -= Ld;
        } else if (st == 2) {
            const int V = 1 + ifloor(len * 5.999);
            for (int j = 0; j < 6; ++j) {
                const bool active = j < V;
                if (j == 0) orTgt[j] = 1;
                else if (active) {
                    if (--orCnt[j] <= 0) {
                        orCnt[j] = ifloor((1 + 3 * rng.next()) * fs);
                        orTgt[j] = rng.next() < 0.3 ? 0 : 0.4 + 0.6 * rng.next();
                    }
                } else orTgt[j] = 0;
                orEnv[j] += (orTgt[j] - orEnv[j]) * 0.00005;
                if (orEnv[j] > 1e-4) {
                    const double v = loopRead(orPos[j]) * orEnv[j];
                    y += v;
                    const double pan = spread ? kOrchPans[j] : 0;
                    l += v * (pan > 0 ? 1 - pan : 1);
                    r += v * (pan < 0 ? 1 + pan : 1);
                }
                orPos[j] += kOrchRatios[j];
                if (orPos[j] >= Ld) orPos[j] -= Ld;
            }
            orNorm += (0.8 / std::sqrt(static_cast<double>(V)) - orNorm) * 0.001;
            y *= orNorm; l *= orNorm; r *= orNorm;
        } else if (st == 3) {
            const double sel = len * 7;
            const int i0 = ifloor(sel); const double fr = sel - i0;
            const int i1 = i0 < 7 ? i0 + 1 : 7;
            const int i2 = (i0 + 4) & 7;
            double G = 0.25 * fs; if (G > Ld / 8) G = Ld / 8;
            const double c0 = seam + (i0 + 0.5) * Ld / 8, c1 = seam + (i1 + 0.5) * Ld / 8, c2 = seam + (i2 + 0.5) * Ld / 8;
            for (int g = 0; g < 4; ++g) {
                sgPh[g] += 1 / G;
                if (sgPh[g] >= 1) { sgPh[g] -= 1; sgJit[g] = (rng.next() - 0.5) * 0.4 * G; }
                const double ph = sgPh[g], jo = sgJit[g] - 0.5 * G;
                const double sn = std::sin(kPi * ph);
                double v = loopRead(c0 + jo + ph * G) * (1 - fr);
                if (fr > 0) v += loopRead(c1 + jo + ph * G) * fr;
                v += loopRead(c0 + jo + 2 * ph * G) * 0.35;
                v += loopRead(c2 + jo + ph * G) * 0.25;
                v *= sn * sn * 0.45;
                y += v;
                if (g & 1) { l += v * 0.5; r += v; } else { l += v; r += v * 0.5; }
            }
            sgLp += (y - sgLp) * 0.35;
            const double k = nz(y) ? sgLp / y : 1;
            y = sgLp; l *= k; r *= k;
        } else {
            const int k = clampi(ifloor(len * 4 + 0.5), 0, 4);
            int seg = L / (1 << k); if (seg < 64) seg = 64;
            if (dnCnt >= seg) { dnCnt = 0; dnIdx = (dnIdx + 1) % 3; dnPos = pos; }
            double F = 0.004 * fs; if (F > seg / 4.0) F = seg / 4.0;
            double e = dnCnt / F; const double e2 = (seg - dnCnt) / F;
            if (e2 < e) e = e2;
            if (e > 1) e = 1;
            y = loopRead(dnPos) * e;
            dnPos += kDanceSpeeds[dnIdx];
            if (dnPos >= Ld) dnPos -= Ld;
            ++dnCnt;
            l = y; r = y;
        }
        so[0] = l; so[1] = r;
        return y;
    }

    double radioTick(bool spread) {
        const Tuning t = radioTuning(s.modLoop);
        lastStations = t;
        const int a = t.a, b = t.b;
        for (int i = 0; i < 5; ++i) if (i != a && i != b) stActive[i] = 0;
        double y = stationTick(a, spread);
        double l = so[0], r = so[1];
        if (b < 0) { so[0] = l; so[1] = r; return y; }
        const double u = t.u;
        const double gA = std::cos(u * kPi * 0.5), gB = std::sin(u * kPi * 0.5);
        const double yb = stationTick(b, spread);
        const double mix = y * gA + yb * gB;
        l = l * gA + so[0] * gB; r = r * gA + so[1] * gB;
        const double sn = std::sin(kPi * u);
        const double n1 = rng.next() * 2 - 1, n2 = rng.next() * 2 - 1, n3 = rng.next();
        rStatic += (n1 - rStatic) * 0.3;
        const double crack = n3 < 0.003 * sn ? n2 : 0;
        rWhPh += kTau * (0.004 + 0.03 * u);
        if (rWhPh > kTau) rWhPh -= kTau;
        rLp += (mix - rLp) * 0.25;
        rHp += (rLp - rHp) * 0.02;
        const double band = rLp - rHp;
        const double noise = rStatic * 0.25 * sn + crack * 0.3 * sn + std::sin(rWhPh) * 0.04 * sn;
        const double dry = 1 - 0.6 * sn;
        y = mix * dry + band * 0.6 * sn + noise;
        so[0] = l * dry + band * 0.6 * sn + noise;
        so[1] = r * dry + band * 0.6 * sn + noise;
        return y;
    }

    double maskTick() {
        const double y0 = loopRead(pos);
        maskRing[uz(maskW)] = static_cast<float>(y0);
        const int mw = maskW;
        if (++maskW >= static_cast<int>(maskRing.size())) maskW = 0;
        const double ay = std::fabs(y0);
        maskEnv += (ay - maskEnv) * (ay > maskEnv ? 0.05 : 0.0008);
        const double m = s.modLoop;
        const bool off = m <= 0.005;
        const double one = 1 - m;
        const double thr = 0.25 * one * one;
        const double tgt = !off && maskEnv > thr ? 1 : 0;
        maskG += (tgt - maskG) * (tgt > maskG ? 0.02 : 0.002);
        if (!nz(tgt) && maskG < 1e-6) maskG = 0;
        ringPh += kTau * 0.0075;
        if (ringPh > kTau) ringPh -= kTau;
        if (++crushCnt >= 6) { crushCnt = 0; crushV = std::floor(y0 * 12 + 0.5) / 12; }
        if (!nz(maskG)) { so[0] = y0; so[1] = y0; return y0; }
        const double sel = s.length * 5;
        const int t0 = ifloor(sel); const double fr = sel - t0;
        double tr = maskTreat(t0, y0, mw) * (1 - fr);
        if (fr > 0) tr += maskTreat(t0 + 1, y0, mw) * fr;
        const double y = y0 + (tr - y0) * maskG;
        so[0] = y; so[1] = y;
        return y;
    }

    double maskTreat(int t, double y0, int mw) {
        if (t == 0) return maskSh[0].run(maskRing, mw, 0, 0.5, 0.04 * fs);
        if (t == 1) return y0 * std::sin(ringPh);
        if (t == 2) return crushV;
        if (t == 3) {
            double e = maskEnv * 4; if (e > 1) e = 1;
            const double F = 2 * std::sin(kPi * (0.01 + 0.15 * e));
            svfLow += F * svfBand;
            const double high = y0 - svfLow - 0.3 * svfBand;
            svfBand += F * high;
            return svfBand * 1.5;
        }
        if (t == 4) {
            const double G = 0.1 * fs;
            revPh += 1;
            if (revPh >= G) { revPh -= G; revStart = pos; }
            double v = 0;
            for (int h = 0; h < 2; ++h) {
                double ph = revPh / G + h * 0.5; if (ph >= 1) ph -= 1;
                const double sn = std::sin(kPi * ph);
                v += loopRead(revStart - h * 0.5 * G - ph * G) * sn * sn;
            }
            return v;
        }
        return maskSh[1].run(maskRing, mw, 0, 2, 0.04 * fs);
    }

    int loopLengthFor() const {
        int n = nz(p.half) ? kLoopSamplesHalf : kLoopSamples;
        if (p.sync == 0) {
            const int P = ifloor(4 * relayPeriod() + 0.5);
            if (P < n) n = P;
            if (n < 1024) n = 1024;
        }
        return n;
    }
    double relayPeriod() const { return expMap(s.time, 0.03, kRelayTmaxSec) * fs; }
    double syncedTime(double t) const {
        if (p.sync != 2) return t;
        const int idx = clampi(ifloor(s.time * 8.999), 0, 8);
        double T = L * kSyncFracs[idx];
        const double mx = kRelayTmaxSec * fs;
        while (T > mx) T *= 0.5;
        return T;
    }

    void coreTick(double x) {
        const bool spreadWet = nz(p.spread) && p.spreadSel != 2;
        const bool spreadLoop = nz(p.spread) && p.spreadSel != 0;

        const int Lnew = loopLengthFor();
        if (Lnew != L) {
            L = Lnew;
            if (pos >= L) pos = pos % L;
            if (seam >= L) seam = seam % L;
            if (tp >= L) tp = std::fmod(tp, static_cast<double>(L));
        }
        seamF = std::min(0.004 * fs, L / 4.0);

        // 1. micro-looper read
        if (p.loopMode != lastLoopMode) { lastLoopMode = p.loopMode; resetLoopModes(); }
        loopGain += ((loopPlay ? 1.0 : 0.0) - loopGain) * 0.003;
        double loopL = 0, loopR = 0, loopMono = 0;
        for (int k = 0; k < 3; ++k) lmGain[k] += ((p.loopMode == k ? 1.0 : 0.0) - lmGain[k]) * 0.003;
        if (loopGain > 1e-4) {
            if (lmGain[0] > 1e-4) { const double y = burstTick(x); loopMono += y * lmGain[0]; loopL += so[0] * lmGain[0]; loopR += so[1] * lmGain[0]; }
            if (lmGain[1] > 1e-4) { const double y = radioTick(spreadLoop); loopMono += y * lmGain[1]; loopL += so[0] * lmGain[1]; loopR += so[1] * lmGain[1]; }
            if (lmGain[2] > 1e-4) { const double y = maskTick(); loopMono += y * lmGain[2]; loopL += so[0] * lmGain[2]; loopR += so[1] * lmGain[2]; }
            if (!spreadLoop) { loopL = loopMono; loopR = loopMono; }
            else if (p.loopMode != 0) {
                decorBuf[uz(decorW)] = static_cast<float>(loopR);
                loopR = readFrac(decorBuf, decorW - 0.011 * fs) * 0.5 + loopR * 0.5;
                if (++decorW >= static_cast<int>(decorBuf.size())) decorW = 0;
            }
            loopMono *= loopGain; loopL *= loopGain; loopR *= loopGain;
        }

        // 2. wet channel
        const bool both = wetOn && loopPlay;
        const int rt = both ? p.routing : 0;
        xW += ((rt == 2 ? 0.0 : 1.0) - xW) * 0.003;
        lW += ((rt >= 1 ? 1.0 : 0.0) - lW) * 0.003;
        const double wetSrc = x * xW + loopMono * lW;
        const bool tb = trueBypass;
        wetIn += ((wetOn && !tb ? 1.0 : 0.0) - wetIn) * 0.003;
        wetOut += ((wetOn || nz(p.trails) ? 1.0 : 0.0) - wetOut) * 0.003;
        for (int k = 0; k < 3; ++k) modeGain[k] += ((p.wetMode == k ? 1.0 : 0.0) - modeGain[k]) * 0.002;
        const double wIn = wetSrc * wetIn;
        double wl = 0, wr = 0;
        if (modeGain[0] > 1e-4) {
            const double* o = soup.tick(wIn, s.time, s.modWet, freeze, s.time > 0.985, spreadWet);
            wl += o[0] * modeGain[0]; wr += o[1] * modeGain[0];
        }
        if (modeGain[1] > 1e-4) {
            const double T = syncedTime(relayPeriod());
            const double* o = relay.tick(wIn, T, relayRepeats(s.modWet), freeze, spreadWet);
            wl += o[0] * modeGain[1]; wr += o[1] * modeGain[1];
        }
        if (modeGain[2] > 1e-4) {
            double lag = syncedTime(kFlipLagMaxSec * fs * s.time * s.time);
            if (lag > kFlipLagMaxSec * fs) lag = kFlipLagMaxSec * fs;
            const double* o = flip.tick(wIn, lag, kFlipChords[flipChordIndex(s.modWet)], freeze, spreadWet);
            wl += o[0] * modeGain[2]; wr += o[1] * modeGain[2];
        }
        wl *= wetOut; wr *= wetOut;

        // 3. CROSS
        const size_t cw = uz(crossW);
        crossBuf[0][cw] = static_cast<float>(wl); crossBuf[1][cw] = static_cast<float>(wr);
        crossBuf[2][cw] = static_cast<float>(loopL); crossBuf[3][cw] = static_cast<float>(loopR);
        const int im = p.inputMod;
        const double src = im == 1 ? std::fabs(x) : im == 0 ? std::fabs(wl + wr) * 0.5 : std::fabs(loopMono);
        crossEnv += (src - crossEnv) * (src > crossEnv ? 0.01 : 0.0015);
        if (s.cross > 1e-4) {
            double duck = crossEnv * 5; if (duck > 1) duck = 1;
            crossPh += kTau * (0.0002 + 0.0015 * duck);
            if (crossPh > kTau) crossPh -= kTau;
            const double amp = 1 - s.cross * duck * (0.55 + 0.45 * std::sin(crossPh));
            crossD += (s.cross * duck * 0.012 * fs - crossD) * 0.0015;
            if (im != 0) { wl = readFrac(crossBuf[0], crossW - crossD) * amp; wr = readFrac(crossBuf[1], crossW - crossD) * amp; }
            if (im != 2) {
                loopL = readFrac(crossBuf[2], crossW - crossD) * amp; loopR = readFrac(crossBuf[3], crossW - crossD) * amp;
                loopMono = (loopL + loopR) * 0.5;
            }
        } else crossD = 0;
        if (++crossW >= static_cast<int>(crossBuf[0].size())) crossW = 0;

        // 4. micro-looper write
        {
            const int i = pos;
            double v;
            bool write = true;
            if (tb) { v = 0; write = false; }
            else if (!loopPlay) v = x + (wl + wr) * 0.5;
            else if (overdub) {
                const double fb = p.fade > 0.97 ? 1 : 0.25 + 0.75 * p.fade;
                v = buf[uz(i)] * fb + x;
            } else { v = 0; write = false; }
            if (write) {
                if (v > 1 || v < -1) v = std::tanh(v);
                buf[uz(i)] = static_cast<float>(v);
                eAcc += v * v;
            }
            if ((i + 1) % kBlk == 0 || i + 1 == L) {
                if (write) blockE[uz(i / kBlk)] = eAcc;
                eAcc = 0;
            }
            if (++pos >= L) { pos = 0; ++cycles; }
        }

        // 5. channel balance
        const double direct = both && p.routing >= 1 ? s.blend : 1;
        loopDirect += (direct - loopDirect) * 0.003;
        const double bal = s.balance;
        const double wetLvl = std::min(1.0, 2 * (1 - bal)), loopLvl = std::min(1.0, 2 * bal);
        prev[0] = cur[0]; prev[1] = cur[1];
        cur[0] = wl * wetLvl + loopL * loopDirect * loopLvl;
        cur[1] = wr * wetLvl + loopR * loopDirect * loopLvl;
    }

    double glue(double x) const {
        if (s.glue < 0.001) return x;
        double y = std::tanh(gDrive * x + gBias) - gTB;
        if (gDestroy > 0) {
            const double f = std::sin(gDrive * x * 1.7);
            y += (f - y) * gDestroy;
            y = std::floor(y * gQ + 0.5) / gQ;
        }
        return y * gMake;
    }

    double rampValue(double v) const { return nz(p.sweep) ? 1 - (1 - v) * rampShape : v * rampShape; }

    void snapSmoothers() {
        s.time = p.time; s.mix = p.mix; s.length = p.length; s.modWet = p.modWet; s.modLoop = p.modLoop;
        s.balance = p.balance; s.cross = p.cross; s.blend = p.blend; s.glue = p.glue; s.volume = p.volume; s.eq = p.eq;
        s.clockHz = clockHz(p.clock, nz(p.smooth));
        for (int k = 0; k < 3; ++k) { modeGain[k] = p.wetMode == k ? 1 : 0; lmGain[k] = p.loopMode == k ? 1 : 0; }
        lastLoopMode = p.loopMode;
    }
};

}  // namespace badmood
