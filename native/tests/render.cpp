// Render a parity scenario through the C++ core (see tests/parity/scenario.js).
//   badmood_render <scenario.txt> <out.f32>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>
#include "dsp/badmood.h"

struct Event { std::string kind, a, b; double value; };

static double input(long n, double rate) {   // same formula as scenario.js
    const double t = n / rate;
    const long k = static_cast<long>(std::floor(t / 0.75));
    const double fr[4] = {196, 247, 294, 330};
    const double tt = t - k * 0.75;
    return 0.35 * std::sin(2 * badmood::kPi * fr[k % 4] * t) * std::exp(-4 * tt);
}

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: badmood_render <scenario.txt> <out.f32>\n"); return 2; }
    std::ifstream in(argv[1]);
    if (!in) { std::fprintf(stderr, "cannot read %s\n", argv[1]); return 2; }
    double rate = 48000, seconds = 4;
    bool stereo = false;
    std::map<long, std::vector<Event>> events;
    std::string line;
    while (std::getline(in, line)) {
        const auto hash = line.find('#'); if (hash != std::string::npos) line.resize(hash);
        std::istringstream ss(line); std::string t0, t1, t2, t3;
        if (!(ss >> t0)) continue;
        if (t0 == "rate") { ss >> rate; continue; }
        if (t0 == "seconds") { ss >> seconds; continue; }
        if (t0 == "stereo") { int v = 0; ss >> v; stereo = v != 0; continue; }
        ss >> t1 >> t2 >> t3;
        events[std::stol(t0)].push_back(Event{t1, t2, t3, t1 == "fs" ? 0.0 : std::stod(t3)});
    }
    constexpr int B = 128;
    badmood::Core core; core.prepare(rate);
    const long nBlocks = static_cast<long>(std::ceil(seconds * rate / B));
    std::vector<float> out(static_cast<size_t>(nBlocks * B * 2));
    float x[B], xr[B], L[B], R[B];
    for (long b = 0; b < nBlocks; ++b) {
        auto it = events.find(b);
        if (it != events.end())
            for (const auto& e : it->second) {
                if (e.kind == "param") {
                    if (!badmood::setParam(core.p, e.a.c_str(), e.value)) { std::fprintf(stderr, "unknown param %s\n", e.a.c_str()); return 2; }
                } else if (e.kind == "cmd") core.command(e.a.c_str(), e.value != 0);
                else core.footswitch(e.a == "wet" ? 0 : 1, e.b == "down" ? badmood::FsDown : e.b == "shift" ? badmood::FsShift : badmood::FsUp);
            }
        for (int i = 0; i < B; ++i) { x[i] = static_cast<float>(input(b * B + i, rate)); xr[i] = static_cast<float>(0.8 * input(b * B + i + 7919, rate)); }
        core.process(x, stereo ? xr : nullptr, L, R, B);
        for (int i = 0; i < B; ++i) { out[static_cast<size_t>(2 * (b * B + i))] = L[i]; out[static_cast<size_t>(2 * (b * B + i) + 1)] = R[i]; }
    }
    std::ofstream o(argv[2], std::ios::binary);
    o.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size() * sizeof(float)));
    return o ? 0 : 1;
}
