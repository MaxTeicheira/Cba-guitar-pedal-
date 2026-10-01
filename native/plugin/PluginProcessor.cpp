#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace {
using APF = juce::AudioParameterFloat;
using APC = juce::AudioParameterChoice;
using APB = juce::AudioParameterBool;

juce::String fmtTime(double s) { return s < 1 ? juce::String(juce::roundToInt(s * 1000)) + " ms" : juce::String(s, 2) + " s"; }
juce::String fmtSpeed(double s) {
    if (std::fabs(s) < 1e-9) return "frozen";
    return (s < 0 ? "reverse " : "") + juce::String(std::fabs(s), 2) + "x";
}
juce::String pct(double x) { return juce::String(juce::roundToInt(x * 100)) + "%"; }

struct FloatDef { const char* id; const char* name; float def; };
const FloatDef kKnobs[] = {
    {"time", "Time", 0.5f}, {"mix", "Mix", 0.5f}, {"length", "Length", 0.75f},
    {"modWet", "Modify (wet)", 0.5f}, {"clock", "Clock", 1.0f}, {"modLoop", "Modify (loop)", 0.5f},
    {"balance", "Balance (wet/loop)", 0.5f},
};
}  // namespace

MoodProcessor::MoodProcessor()
    : AudioProcessor(BusesProperties().withInput("Input", juce::AudioChannelSet::stereo(), true)
                                      .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      apvts(*this, nullptr, "MOOD", [this] {
          juce::AudioProcessorValueTreeState::ParameterLayout layout;
          for (const auto& k : kKnobs) {
              const juce::String id = k.id;
              layout.add(std::make_unique<APF>(juce::ParameterID(id, 1), k.name, juce::NormalisableRange<float>(0.f, 1.f), k.def,
                  juce::AudioParameterFloatAttributes().withStringFromValueFunction(
                      [this, id](float v, int) { return describe(id, v); })));
          }
          layout.add(std::make_unique<APC>(juce::ParameterID("wetMode", 1), "Wet mode", juce::StringArray{"Reverb", "Delay", "Slip"}, 0));
          layout.add(std::make_unique<APC>(juce::ParameterID("routing", 1), "Routing", juce::StringArray{"In", "In + Loop", "Loop"}, 0));
          layout.add(std::make_unique<APC>(juce::ParameterID("loopMode", 1), "Loop mode", juce::StringArray{"Env", "Tape", "Stretch"}, 0));
          layout.add(std::make_unique<APB>(juce::ParameterID("trails", 1), "Dip: Trails", true));
          layout.add(std::make_unique<APB>(juce::ParameterID("dryKill", 1), "Dip: Dry kill", false));
          layout.add(std::make_unique<APB>(juce::ParameterID("latch", 1), "Dip: Latch", false));
          layout.add(std::make_unique<APB>(juce::ParameterID("spread", 1), "Dip: Spread", false));
          layout.add(std::make_unique<APB>(juce::ParameterID("smooth", 1), "Dip: Smooth clock", false));
          layout.add(std::make_unique<APB>(juce::ParameterID("wetSwitch", 1), "Wet footswitch (hold = freeze)", false));
          layout.add(std::make_unique<APB>(juce::ParameterID("loopSwitch", 1), "Loop footswitch (tap/hold)", false));
          return layout;
      }()) {
    formats_.registerBasicFormats();
    // The standalone starts on the built-in phrase so it makes sound with nothing plugged in;
    // in a DAW the plugin processes the track.
    if (wrapperType == wrapperType_Standalone) source = Demo;
    core_.prepare(48000.0);
}

juce::String MoodProcessor::describe(const juce::String& id, float v) const {
    const auto* wm = apvts.getRawParameterValue("wetMode");
    const auto* lm = apvts.getRawParameterValue("loopMode");
    const auto* ck = apvts.getRawParameterValue("clock");
    if (wm == nullptr || lm == nullptr || ck == nullptr) return juce::String(v, 3);   // still constructing
    const int wetMode = juce::roundToInt(wm->load()), loopMode = juce::roundToInt(lm->load());
    const double sr = getSampleRate() > 0 ? getSampleRate() : 48000.0;
    const double c = std::min(1.0, mood::clockHz(ck->load()) / sr);
    if (id == "time") {
        if (wetMode == 0) return v > 0.985f ? juce::String("decay infinite") : "decay " + fmtTime(mood::expMap(v, 0.4, 25) / c);
        if (wetMode == 1) return "delay " + fmtTime(mood::expMap(v, 0.02, 2) / c);
        return "refresh " + fmtTime(mood::expMap(v, 0.05, 1.5) / c);
    }
    if (id == "mix") {
        const auto* dkp = apvts.getRawParameterValue("dryKill");
        const bool dk = dkp != nullptr && dkp->load() > 0.5f;
        return "dry " + pct(dk ? 0 : std::min(1.0, 2.0 * (1 - v))) + " / fx " + pct(std::min(1.0, 2.0 * v));
    }
    if (id == "length") {
        const double full = mood::kLoopSamples / (c * sr);
        if (loopMode == 1) return "loop " + fmtTime(full * mood::expMap(v, 0.02, 1)) + " of " + fmtTime(full);
        return "slice " + fmtTime((loopMode == 0 ? mood::expMap(v, 0.01, 1) : mood::expMap(v, 0.01, 0.5)) / c);
    }
    if (id == "modWet") {
        if (wetMode == 0) return "smear " + pct(v);
        if (wetMode == 1) return "feedback " + pct(v * 1.1) + (v > 0.91f ? " (runaway)" : "");
        return "speed " + fmtSpeed(mood::speedMap(v, 0.15));
    }
    if (id == "clock") {
        const double hz = mood::clockHz(v);
        return hz > sr ? juce::String(hz / 1000, 1) + " kHz (capped at " + juce::String(sr / 1000, 1) + ")"
                       : "sample rate " + juce::String(hz / 1000, 1) + " kHz";
    }
    if (id == "modLoop") {
        if (loopMode == 0) return "sensitivity " + pct(v);
        if (loopMode == 1) return "tape " + fmtSpeed(mood::speedMap(v, 0.04));
        return "stretch " + fmtSpeed(mood::stretchMap(v)) + ", same pitch";
    }
    if (id == "balance") return "wet " + pct(std::min(1.0, 2.0 * (1 - v))) + " / loop " + pct(std::min(1.0, 2.0 * v));
    return juce::String(v, 3);
}

juce::String MoodProcessor::describe(const juce::String& id) const {
    const auto* p = apvts.getRawParameterValue(id);
    return p ? describe(id, p->load()) : juce::String();
}

void MoodProcessor::makeDemo(double sr) {
    // Karplus-Strong fingerpicking, 8 s seamless loop (same phrase as the web version).
    const int len = static_cast<int>(8 * sr);
    demo_.setSize(1, len);
    demo_.clear();
    float* out = demo_.getWritePointer(0);
    juce::Random rnd(1234);
    auto pluck = [&](double t0, int note, float amp, double decay) {
        const double f = 440.0 * std::pow(2.0, (note - 69) / 12.0);
        const int N = std::max(2, juce::roundToInt(sr / f));
        std::vector<float> buf(static_cast<size_t>(N));
        float lp = 0;
        for (auto& b : buf) { lp += 0.55f * (rnd.nextFloat() * 2 - 1 - lp); b = lp; }
        const float rho = static_cast<float>(std::pow(0.001, 1.0 / (f * decay)));
        const int start = static_cast<int>(t0 * sr), n = static_cast<int>(decay * sr);
        int idx = 0;
        for (int k = 0; k < n; ++k) {
            const int nx = idx + 1 == N ? 0 : idx + 1;
            const float a = buf[static_cast<size_t>(idx)], b = buf[static_cast<size_t>(nx)];
            out[(start + k) % len] += a * amp;
            buf[static_cast<size_t>(idx)] = rho * 0.5f * (a + b);
            idx = nx;
        }
    };
    const std::vector<std::vector<int>> chords = {   // Em9, Cmaj7, G6, D/F#
        {40, 47, 55, 62, 66, 59}, {48, 55, 59, 64, 67, 64}, {43, 50, 59, 64, 67, 62}, {42, 50, 57, 66}};
    for (size_t c = 0; c < chords.size(); ++c)
        for (size_t i = 0; i < chords[c].size(); ++i)
            pluck(c * 2.0 + i / 3.0, chords[c][i], i == 0 ? 0.32f : 0.22f + 0.04f * rnd.nextFloat(), i == 0 ? 3.5 : 2.2);
    const float pk = demo_.getMagnitude(0, 0, len);
    if (pk > 0) demo_.applyGain(0.5f / pk);
    demoPos_ = 0;
}

void MoodProcessor::prepareToPlay(double sampleRate, int samplesPerBlock) {
    core_.prepare(sampleRate);
    pullParams();
    lastWetSw_ = lastLoopSw_ = false;
    mono_.assign(static_cast<size_t>(std::max(samplesPerBlock, 32)), 0.f);
    if (demo_.getNumSamples() != static_cast<int>(8 * sampleRate)) makeDemo(sampleRate);
}

void MoodProcessor::pullParams() {
    auto& p = core_.p;
    p.time = param("time"); p.mix = param("mix"); p.length = param("length");
    p.modWet = param("modWet"); p.clock = param("clock"); p.modLoop = param("modLoop");
    p.balance = param("balance");
    p.wetMode = juce::roundToInt(param("wetMode"));
    p.routing = juce::roundToInt(param("routing"));
    p.loopMode = juce::roundToInt(param("loopMode"));
    p.trails = param("trails") > 0.5f; p.dryKill = param("dryKill") > 0.5f; p.latch = param("latch") > 0.5f;
    p.spread = param("spread") > 0.5f; p.smooth = param("smooth") > 0.5f;
}

bool MoodProcessor::loadFile(const juce::File& f) {
    std::unique_ptr<juce::AudioFormatReader> r(formats_.createReaderFor(f));
    if (!r || r->lengthInSamples <= 0 || r->lengthInSamples > 60 * 60 * 192000) return false;
    juce::AudioBuffer<float> buf(static_cast<int>(r->numChannels), static_cast<int>(r->lengthInSamples));
    r->read(&buf, 0, buf.getNumSamples(), 0, true, true);
    const juce::SpinLock::ScopedLockType lock(fileLock_);   // the audio thread only tries this lock
    file_ = std::move(buf); fileRate_ = r->sampleRate; filePos_ = 0; fileName_ = f.getFileName();
    return true;
}

bool MoodProcessor::copyWave(std::array<float, kWaveBins>& out) const {
    const juce::SpinLock::ScopedTryLockType lock(waveLock_);
    if (!lock.isLocked()) return false;
    out = wave_;
    return true;
}

void MoodProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) {
    juce::ScopedNoDenormals noDenormals;
    const auto t0 = juce::Time::getHighResolutionTicks();
    const int n = buffer.getNumSamples();
    const int nIn = getTotalNumInputChannels();
    if (buffer.getNumChannels() < 2 || n == 0) { buffer.clear(); return; }

    pullParams();
    const bool wetSw = param("wetSwitch") > 0.5f, loopSw = param("loopSwitch") > 0.5f;
    if (wetSw != lastWetSw_) { lastWetSw_ = wetSw; core_.footswitch(0, wetSw); }
    if (loopSw != lastLoopSw_) { lastLoopSw_ = loopSw; core_.footswitch(1, loopSw); }

    if (static_cast<int>(mono_.size()) < n) mono_.resize(static_cast<size_t>(n));   // only if the host exceeds its promised block size
    float* x = mono_.data();
    const float g = inputGain.load();
    const int src = source.load();
    if (src == Demo && demo_.getNumSamples() > 0) {
        const float* d = demo_.getReadPointer(0);
        const int len = demo_.getNumSamples();
        int pos = static_cast<int>(demoPos_);
        for (int i = 0; i < n; ++i) { x[i] = d[pos] * g; if (++pos >= len) pos = 0; }
        demoPos_ = pos;
    } else if (src == File) {
        std::fill(x, x + n, 0.f);
        const juce::SpinLock::ScopedTryLockType lock(fileLock_);
        if (lock.isLocked() && file_.getNumSamples() > 1 && fileRate_ > 0) {
            const double ratio = fileRate_ / getSampleRate();
            const int len = file_.getNumSamples(), nc = file_.getNumChannels();
            for (int i = 0; i < n; ++i) {
                const int i0 = static_cast<int>(filePos_), i1 = (i0 + 1) % len;
                const float fr = static_cast<float>(filePos_ - i0);
                float acc = 0;
                for (int c = 0; c < nc; ++c) { const float* s = file_.getReadPointer(c); acc += s[i0] + (s[i1] - s[i0]) * fr; }
                x[i] = acc / static_cast<float>(nc) * g;
                filePos_ += ratio; if (filePos_ >= len) filePos_ -= len;
            }
        }
    } else {
        const float* l = buffer.getReadPointer(0);
        const float* r = nIn > 1 ? buffer.getReadPointer(1) : l;
        const float k = nIn > 1 ? 0.5f * g : g;
        for (int i = 0; i < n; ++i) x[i] = (nIn > 1 ? l[i] + r[i] : l[i]) * k;
    }

    core_.process(x, buffer.getWritePointer(0), buffer.getWritePointer(1), n);
    for (int c = 2; c < buffer.getNumChannels(); ++c) buffer.clear(c, 0, n);

    const auto st = core_.state();
    ui.wetOn = st.wetOn; ui.freeze = st.freeze; ui.loopLed = static_cast<int>(st.loop);
    ui.lsrc = st.lsrc; ui.winStart = st.winStart; ui.winLen = st.winLen; ui.pos = static_cast<float>(st.pos);
    ui.clockHz = static_cast<float>(st.clockHz); ui.histCycles = st.histCycles;
    ui.peakIn = st.peakIn; ui.peakOut = st.peakOut;

    // refresh the waveform after a capture, and a few times a second while overdubbing
    bool wantWave = core_.takeLoopWaveDirty() || pendingWave_;
    if (st.loop == mood::LoopLed::Dub && ++dubBlocks_ % 40 == 0) wantWave = true;
    if (wantWave) {
        const juce::SpinLock::ScopedTryLockType lock(waveLock_);
        if (lock.isLocked()) { core_.loopPeaks(wave_.data(), kWaveBins); ui.waveVersion.fetch_add(1); pendingWave_ = false; }
        else pendingWave_ = true;
    }
    const double secs = juce::Time::highResolutionTicksToSeconds(juce::Time::getHighResolutionTicks() - t0);
    ui.load = static_cast<float>(100.0 * secs / (n / getSampleRate()));
}

void MoodProcessor::getStateInformation(juce::MemoryBlock& dest) {
    auto state = apvts.copyState();
    state.setProperty("source", source.load(), nullptr);
    state.setProperty("inputGain", inputGain.load(), nullptr);
    if (auto xml = state.createXml()) copyXmlToBinary(*xml, dest);
}

void MoodProcessor::setStateInformation(const void* data, int size) {
    if (auto xml = getXmlFromBinary(data, size)) {
        if (!xml->hasTagName(apvts.state.getType())) return;
        auto state = juce::ValueTree::fromXml(*xml);
        if (state.hasProperty("source")) source = juce::jlimit(0, 1, static_cast<int>(state["source"]));   // a file is not restored
        if (state.hasProperty("inputGain")) inputGain = static_cast<float>(state["inputGain"]);
        apvts.replaceState(state);
        // footswitches are momentary: never restore one as held down
        for (auto* id : {"wetSwitch", "loopSwitch"})
            if (auto* p = apvts.getParameter(id)) p->setValueNotifyingHost(0.f);
    }
}

juce::AudioProcessorEditor* MoodProcessor::createEditor() { return new MoodEditor(*this); }
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new MoodProcessor(); }
