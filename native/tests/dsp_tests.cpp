// Property tests for the C++ core. Behavioural equivalence with the JS reference
// is covered separately by tests/parity (bit-for-bit within float rounding).
#include <cmath>
#include <cstdio>
#include <functional>
#include <vector>
#include "dsp/badmood.h"

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); ++failures; } } while (0)

constexpr int B = 128;
struct Out { std::vector<float> L, R; };
using Src = std::function<double(double)>;
using At = std::function<void(badmood::Core&, long)>;

static Out render(badmood::Core& c, double fs, double secs, const Src& src, const At& at = nullptr) {
    const long n = static_cast<long>(std::ceil(secs * fs / B));
    Out o; o.L.resize(static_cast<size_t>(n * B)); o.R.resize(static_cast<size_t>(n * B));
    float x[B];
    for (long b = 0; b < n; ++b) {
        if (at) at(c, b);
        for (int i = 0; i < B; ++i) x[i] = static_cast<float>(src((b * B + i) / fs));
        c.process(x, nullptr, o.L.data() + b * B, o.R.data() + b * B, B);
    }
    return o;
}
static long blk(double s, double fs = 48000) { return std::lround(s * fs / B); }
static double rms(const std::vector<float>& a, double s, double e, double fs = 48000) {
    double acc = 0; const size_t i0 = static_cast<size_t>(s * fs), i1 = static_cast<size_t>(e * fs);
    for (size_t i = i0; i < i1; ++i) acc += static_cast<double>(a[i]) * a[i];
    return std::sqrt(acc / static_cast<double>(i1 - i0));
}
static double power(const std::vector<float>& a, double f, double s, double e, double fs = 48000) {
    const size_t i0 = static_cast<size_t>(s * fs), i1 = static_cast<size_t>(e * fs);
    const double c = 2 * std::cos(2 * badmood::kPi * f / fs);
    double s1 = 0, s2 = 0;
    for (size_t i = i0; i < i1; ++i) { const double s0 = a[i] + c * s1 - s2; s2 = s1; s1 = s0; }
    const double n = static_cast<double>(i1 - i0);
    return (s1 * s1 + s2 * s2 - c * s1 * s2) / (n * n);
}
static bool finiteBounded(const std::vector<float>& a) { for (float v : a) if (!std::isfinite(v) || std::fabs(v) > 1.f) return false; return true; }
static Src tone(double f, double dur, double amp = 0.4) { return [=](double t) { return t < dur ? amp * std::sin(2 * badmood::kPi * f * t) : 0.0; }; }
static double phrase(double t) {
    const double fr[12] = {196, 330, 247, 440, 294, 523, 370, 220, 392, 262, 494, 349};
    const long k = static_cast<long>(std::floor(t / 0.2));
    return 0.4 * std::sin(2 * badmood::kPi * fr[k % 12] * t) * std::exp(-12 * (t - k * 0.2));
}
static void tap(badmood::Core& c, int which) { c.footswitch(which, badmood::FsDown); c.footswitch(which, badmood::FsUp); }
static void fx(badmood::Core& c) { c.p.mix = 1; c.p.dryKill = 1; c.p.glue = 0; }

int main() {
    const double FS = 48000;
    {   // bypass is transparent
        badmood::Core c; c.prepare(FS);
        auto src = tone(220, 1);
        auto o = render(c, FS, 0.5, src);
        double worst = 0;
        for (size_t i = 1000; i < 20000; ++i) worst = std::max(worst, std::fabs(o.L[i] - src(static_cast<double>(i) / FS)));
        CHECK(worst < 1e-6, "bypass error %g", worst);
    }
    {   // CLOCK steps from the CC18 table
        CHECK(badmood::clockHz(0, false) == 2000 && badmood::clockHz(1, false) == 64000 && badmood::clockHz(64.0 / 127, false) == 12000, "clock steps");
    }
    {   // loop length vs CLOCK (64k vs 32k at a 64 kHz host) and HALF
        auto period = [](double clock, bool half, int& len) {
            const double fs = 64000;
            badmood::Core c; c.p.clock = clock; c.p.half = half ? 1 : 0; c.prepare(fs);
            std::vector<int64_t> wraps; uint32_t last = 0;
            float z[B] = {}, a[B], b[B];
            for (long i = 0; i < static_cast<long>(14 * fs / B); ++i) {
                c.process(z, nullptr, a, b, B);
                if (c.state().cycles != last) { last = c.state().cycles; wraps.push_back(c.samplesProcessed()); }
            }
            len = c.loopLength();
            return wraps.size() > 2 ? static_cast<double>(wraps[2] - wraps[1]) / fs : 0.0;
        };
        int len = 0;
        const double full = period(1, false, len);
        CHECK(len == badmood::kLoopSamples && std::fabs(full - 2.0) < 0.01, "64k loop %g s (len %d)", full, len);
        const double slow = period(100.0 / 127, false, len);
        CHECK(std::fabs(slow - 4.0) < 0.02, "32k loop %g s", slow);
        const double half = period(1, true, len);
        CHECK(len == badmood::kLoopSamplesHalf && std::fabs(half - 1.0) < 0.01, "HALF loop %g s", half);
    }
    {   // Relay: exactly N equal repeats
        const double time = 0.5, T = badmood::expMap(time, 0.03, badmood::kRelayTmaxSec);
        for (int N : {1, 3, 7}) {
            const double m = N == 1 ? 0.05 : N == 3 ? 0.3 : 0.8;
            CHECK(badmood::relayRepeats(m) == N, "relayRepeats(%g)", m);
            badmood::Core c; fx(c); c.p.wetMode = 1; c.p.time = time; c.p.modWet = m; c.prepare(FS);
            tap(c, 0);
            auto o = render(c, FS, 0.05 + (N + 2) * T + 0.1, [](double t) { return t > 0.05 && t < 0.08 ? 0.5 * std::sin(2 * badmood::kPi * 1000 * t) : 0.0; });
            const double r1 = rms(o.L, 0.05 + T - 0.005, 0.05 + T + 0.035);
            for (int k = 1; k <= N; ++k) {
                const double rk = rms(o.L, 0.05 + k * T - 0.005, 0.05 + k * T + 0.035);
                CHECK(std::fabs(rk / r1 - 1) < 0.05, "N=%d repeat %d: %g vs %g", N, k, rk, r1);
            }
            const double after = rms(o.L, 0.05 + (N + 1) * T - 0.005, 0.05 + (N + 1) * T + 0.035);
            CHECK(after < r1 * 0.01, "N=%d: extra repeat %g", N, after);
        }
    }
    {   // Flip: +oct and -5th
        for (int idx : {5, 1}) {
            const double ratio = idx == 5 ? 2.0 : 2.0 / 3;
            badmood::Core c; fx(c); c.p.wetMode = 2; c.p.time = 0; c.p.modWet = (badmood::kFlipChordStart[idx] + 0.4) / 127; c.prepare(FS);
            CHECK(badmood::flipChordIndex(c.p.modWet) == idx, "chord index");
            tap(c, 0);
            auto o = render(c, FS, 1.2, tone(220, 2, 0.3));
            CHECK(power(o.L, 220 * ratio, 0.5, 1.2) > 20 * power(o.L, 220, 0.5, 1.2), "Flip chord %d interval", idx);
        }
    }
    {   // freeze holds in every wet mode
        for (int wm = 0; wm < 3; ++wm) {
            badmood::Core c; fx(c); c.p.wetMode = wm; c.p.time = 0.3; c.p.modWet = wm == 1 ? 0.1 : 0.5; c.prepare(FS);
            tap(c, 0);
            auto o = render(c, FS, 3.5, tone(330, 1.3), [](badmood::Core& cc, long b) { if (b == blk(0.7)) cc.footswitch(0, badmood::FsDown); });
            const double a = rms(o.L, 1.5, 2.0), b = rms(o.L, 3.0, 3.5);
            CHECK(c.isFrozen() && a > 5e-3 && b > a * 0.6 && b < a * 1.6, "wet mode %d freeze %g -> %g", wm, a, b);
        }
    }
    {   // Burst: up to 8 slices
        badmood::Core c; fx(c); c.p.loopMode = 0; c.p.modLoop = 0; c.prepare(FS);
        render(c, FS, 3, phrase, [](badmood::Core& cc, long b) { if (b == blk(2.9)) tap(cc, 1); });
        CHECK(c.sliceCount() >= 2 && c.sliceCount() <= 8, "burst slices %d", c.sliceCount());
    }
    {   // Mask at MODIFY 0 is the pure loop: identical to Radio Tape at 1x
        auto run = [&](int mode) {
            badmood::Core c; fx(c); c.p.loopMode = mode; c.p.modLoop = 0; c.p.length = 0.5; c.prepare(FS);
            return render(c, FS, 5, phrase, [](badmood::Core& cc, long b) { if (b == blk(2.9)) tap(cc, 1); }).L;
        };
        const auto a = run(2), b = run(1);
        size_t diff = 0; for (size_t i = 0; i < a.size(); ++i) if (a[i] != b[i]) ++diff;
        CHECK(diff == 0, "mask vs tape: %zu samples differ", diff);
    }
    {   // every Radio station sounds
        for (int st = 0; st < 5; ++st) {
            badmood::Core c; fx(c); c.p.loopMode = 1; c.p.modLoop = badmood::kRadioCenters[st]; c.p.length = 0.3; c.prepare(FS);
            auto o = render(c, FS, 5, phrase, [](badmood::Core& cc, long b) { if (b == blk(2.9)) tap(cc, 1); });
            CHECK(finiteBounded(o.L) && rms(o.L, 3.5, 5) > 0.01, "station %d silent", st);
        }
    }
    {   // Glue adds harmonics; zero is clean
        auto thd = [&](double g) {
            badmood::Core c; c.p.glue = g; c.p.dryGlue = 1; c.p.mix = 0; c.p.wetMode = 1; c.p.modWet = 0; c.prepare(FS);
            tap(c, 0);
            auto o = render(c, FS, 1, tone(440, 2, 0.4));
            return (power(o.L, 880, 0.5, 1) + power(o.L, 1320, 0.5, 1)) / power(o.L, 440, 0.5, 1);
        };
        const double z = thd(0), lo = thd(0.2), hi = thd(0.9);
        CHECK(z < 1e-6 && lo > 1e-4 && hi > 5 * lo, "glue thd %g %g %g", z, lo, hi);
    }
    {   // presets: every named parameter round-trips through a Params copy
        badmood::Core a, b;
        int k = 0;
        for (const char* n : badmood::kParamNames) badmood::setParam(a.p, n, (k++ % 3) == 0 ? 1.0 : (k % 7) / 7.0);
        for (const char* n : badmood::kParamNames) badmood::setParam(b.p, n, badmood::getParam(a.p, n));
        for (const char* n : badmood::kParamNames) CHECK(badmood::getParam(a.p, n) == badmood::getParam(b.p, n), "param %s", n);
        a.prepare(FS); b.prepare(FS); tap(a, 0); tap(b, 0);
        auto oa = render(a, FS, 1, phrase), ob = render(b, FS, 1, phrase);
        CHECK(oa.L == ob.L && oa.R == ob.R, "preset copy renders identically");
        CHECK(badmood::kNumParams == 36, "param count %d", badmood::kNumParams);
    }
    {   // gestures: both held = hidden, triple tap both = true bypass (no recording)
        badmood::Core c; c.prepare(FS);
        render(c, FS, 1, phrase, [](badmood::Core& cc, long b) {
            if (b == 10) cc.footswitch(0, badmood::FsDown);
            if (b == 12) cc.footswitch(1, badmood::FsDown);
        });
        CHECK(c.state().hidden && !c.isWetOn() && !c.isLoopPlaying(), "hidden options hold");
        c.footswitch(0, badmood::FsUp); c.footswitch(1, badmood::FsUp);
        auto both = [](badmood::Core& cc) { cc.footswitch(0, badmood::FsDown); cc.footswitch(1, badmood::FsDown); cc.footswitch(0, badmood::FsUp); cc.footswitch(1, badmood::FsUp); };
        render(c, FS, 1, phrase, [&](badmood::Core& cc, long b) { if (b == 5 || b == 30 || b == 60) both(cc); });
        CHECK(c.state().trueBypass, "true bypass");
        const auto before = c.loopBuffer();
        render(c, FS, 0.5, phrase);
        CHECK(before == c.loopBuffer(), "looper does not listen in true bypass");
    }
    {   // every combination stays finite and bounded
        for (int lm = 0; lm < 3; ++lm) for (int r = 0; r < 3; ++r) for (int wm = 0; wm < 3; ++wm) {
            badmood::Core c; c.p.loopMode = lm; c.p.routing = r; c.p.wetMode = wm; c.p.modLoop = 0.4; c.p.modWet = 0.97;
            c.p.clock = 0.3; c.p.spread = 1; c.p.cross = 0.8; c.p.glue = 0.9; c.p.eq = 0.2; c.p.volume = 1;
            c.prepare(FS); tap(c, 0);
            auto o = render(c, FS, 2.5, phrase, [](badmood::Core& cc, long b) { if (b == blk(1.5)) tap(cc, 1); });
            CHECK(finiteBounded(o.L) && finiteBounded(o.R), "loop %d routing %d wet %d", lm, r, wm);
        }
    }
    std::printf(failures ? "%d failure(s)\n" : "all DSP tests passed\n", failures);
    return failures ? 1 : 0;
}
