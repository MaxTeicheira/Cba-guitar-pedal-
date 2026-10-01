// Render a parity scenario through the C++ core (see tests/parity/scenario.js).
//   mood_render <scenario.txt> <out.f32>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>
#include "dsp/mood.h"

struct Event { bool isParam; std::string a, b; double value; };

static void setParam(mood::Params& p, const std::string& n, double v) {
    if (n == "time") p.time = v; else if (n == "mix") p.mix = v; else if (n == "length") p.length = v;
    else if (n == "modWet") p.modWet = v; else if (n == "clock") p.clock = v; else if (n == "modLoop") p.modLoop = v;
    else if (n == "wetMode") p.wetMode = static_cast<int>(v); else if (n == "routing") p.routing = static_cast<int>(v);
    else if (n == "loopMode") p.loopMode = static_cast<int>(v);
    else if (n == "trails") p.trails = v != 0; else if (n == "dryKill") p.dryKill = v != 0; else if (n == "latch") p.latch = v != 0;
    else if (n == "spread") p.spread = v != 0; else if (n == "smooth") p.smooth = v != 0; else if (n == "balance") p.balance = v;
    else { std::fprintf(stderr, "unknown param %s\n", n.c_str()); std::exit(2); }
}

static float input(long n, double rate) {   // same formula as scenario.js
    const double t = n / rate;
    const long k = static_cast<long>(std::floor(t / 0.75));
    const double fr[4] = {196, 247, 294, 330};
    const double tt = t - k * 0.75;
    return static_cast<float>(0.35 * std::sin(2 * mood::kPi * fr[k % 4] * t) * std::exp(-4 * tt));
}

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: mood_render <scenario.txt> <out.f32>\n"); return 2; }
    std::ifstream in(argv[1]);
    if (!in) { std::fprintf(stderr, "cannot read %s\n", argv[1]); return 2; }
    double rate = 48000, seconds = 4;
    std::map<long, std::vector<Event>> events;
    std::string line;
    while (std::getline(in, line)) {
        const auto hash = line.find('#'); if (hash != std::string::npos) line.resize(hash);
        std::istringstream ss(line); std::string t0, t1, t2, t3;
        if (!(ss >> t0)) continue;
        if (t0 == "rate") { ss >> rate; continue; }
        if (t0 == "seconds") { ss >> seconds; continue; }
        ss >> t1 >> t2 >> t3;
        events[std::stol(t0)].push_back(Event{t1 == "param", t2, t3, t1 == "param" ? std::stod(t3) : 0.0});
    }
    constexpr int B = 128;
    mood::Core core; core.prepare(rate);
    const long nBlocks = static_cast<long>(std::ceil(seconds * rate / B));
    std::vector<float> out(static_cast<size_t>(nBlocks * B * 2));
    float x[B], L[B], R[B];
    for (long b = 0; b < nBlocks; ++b) {
        auto it = events.find(b);
        if (it != events.end())
            for (const auto& e : it->second) {
                if (e.isParam) setParam(core.p, e.a, e.value);
                else core.footswitch(e.a == "wet" ? 0 : 1, e.b == "down");
            }
        for (int i = 0; i < B; ++i) x[i] = input(b * B + i, rate);
        core.process(x, L, R, B);
        for (int i = 0; i < B; ++i) { out[static_cast<size_t>(2 * (b * B + i))] = L[i]; out[static_cast<size_t>(2 * (b * B + i) + 1)] = R[i]; }
    }
    std::ofstream o(argv[2], std::ios::binary);
    o.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size() * sizeof(float)));
    return o ? 0 : 1;
}
