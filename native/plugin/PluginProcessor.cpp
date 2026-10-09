#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace {
using APF = juce::AudioParameterFloat;
using APC = juce::AudioParameterChoice;
using APB = juce::AudioParameterBool;

juce::String fmtTime(double s) { return s < 1 ? juce::String(juce::roundToInt(s * 1000)) + " ms" : juce::String(s, 2) + " s"; }
juce::String fmtSpeed(double s) { return (s < 0 ? "reverse " : "") + juce::String(std::fabs(s), 2) + "x"; }
juce::String pct(double x) { return juce::String(juce::roundToInt(x * 100)) + "%"; }

struct Def { const char* id; const char* name; int kind; const char* choices; };   // kind 0 float, 1 choice, 2 bool
const Def kDefs[] = {
    {"time", "Time", 0, nullptr}, {"mix", "Mix", 0, nullptr}, {"length", "Length", 0, nullptr},
    {"modWet", "Modify (wet)", 0, nullptr}, {"clock", "Clock", 0, nullptr}, {"modLoop", "Modify (loop)", 0, nullptr},
    {"wetMode", "Wet mode", 1, "Soup|Relay|Flip"}, {"routing", "Routing", 1, "Input only|Input + Loop|Loop only"},
    {"loopMode", "Loop mode", 1, "Burst|Radio|Mask"},
    {"miso", "Dip: MISO", 2, nullptr}, {"spread", "Dip: Spread", 2, nullptr}, {"dryKill", "Dip: Dry kill", 2, nullptr},
    {"latch", "Dip: Latch", 2, nullptr}, {"trails", "Dip: Trails", 2, nullptr}, {"smooth", "Dip: Smooth", 2, nullptr},
    {"dryGlue", "Dip: Dry glue", 2, nullptr}, {"half", "Dip: Half", 2, nullptr},
    {"rampTime", "Ramp dip: Time", 2, nullptr}, {"rampModWet", "Ramp dip: Modify (wet)", 2, nullptr},
    {"rampClock", "Ramp dip: Clock", 2, nullptr}, {"rampModLoop", "Ramp dip: Modify (loop)", 2, nullptr},
    {"rampLength", "Ramp dip: Length", 2, nullptr}, {"bounce", "Ramp dip: Bounce", 2, nullptr},
    {"sweep", "Ramp dip: Sweep (top)", 2, nullptr}, {"polarity", "Ramp dip: Polarity (CV only)", 2, nullptr},
    {"rampSpeed", "Ramp speed", 0, nullptr},
    {"cross", "Hidden: Cross", 0, nullptr}, {"balance", "Hidden: Level balance", 0, nullptr}, {"fade", "Hidden: Fade", 0, nullptr},
    {"eq", "Hidden: EQ", 0, nullptr}, {"glue", "Hidden: Glue", 0, nullptr}, {"blend", "Hidden: Blend", 0, nullptr},
    {"sync", "Hidden: Sync", 1, "Loop synced to wet|Unsynced|Wet synced to loop"},
    {"spreadSel", "Hidden: Spread", 1, "Wet only|Both|Loop only"},
    {"inputMod", "Hidden: Input mod", 1, "Wet mods loop|Input|Loop mods wet"},
    {"volume", "Hidden: Effect volume", 0, nullptr},
};
static_assert(sizeof(kDefs) / sizeof(kDefs[0]) == static_cast<size_t>(badmood::kNumParams), "one Def per engine parameter");

// BAD MOOD MIDI CC map (MIDI manual p.3-7). kind: 0 knob (0-127), 1 three-way toggle, 2 on/off.
struct CcMap { int cc; const char* id; int kind; };
const CcMap kCc[] = {
    {14, "time", 0}, {15, "mix", 0}, {16, "length", 0}, {17, "modWet", 0}, {18, "clock", 0}, {19, "modLoop", 0}, {20, "rampSpeed", 0},
    {21, "wetMode", 1}, {22, "routing", 1}, {23, "loopMode", 1},
    {24, "cross", 0}, {25, "balance", 0}, {26, "fade", 0}, {27, "eq", 0}, {28, "glue", 0}, {29, "blend", 0}, {30, "volume", 0},
    {31, "sync", 1}, {32, "spreadSel", 1}, {33, "inputMod", 1},
    {61, "rampTime", 2}, {62, "rampModWet", 2}, {63, "rampClock", 2}, {64, "rampModLoop", 2}, {65, "rampLength", 2},
    {66, "bounce", 2}, {67, "sweep", 2}, {68, "polarity", 2},
    {71, "miso", 2}, {72, "spread", 2}, {73, "dryKill", 2}, {74, "trails", 2}, {75, "latch", 2}, {76, "half", 2},
    {77, "smooth", 2}, {78, "dryGlue", 2},
};
const char* const kHiddenIds[] = {"cross", "balance", "fade", "eq", "glue", "blend", "sync", "spreadSel", "inputMod", "volume"};
}  // namespace

BadMoodProcessor::BadMoodProcessor()
    : AudioProcessor(BusesProperties().withInput("Input", juce::AudioChannelSet::stereo(), true)
                                      .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      apvts(*this, nullptr, "BADMOOD", [this] {
          juce::AudioProcessorValueTreeState::ParameterLayout layout;
          badmood::Params defaults;
          for (const auto& d : kDefs) {
              const juce::String id = d.id;
              const auto def = static_cast<float>(badmood::getParam(defaults, d.id));
              if (d.kind == 0)
                  layout.add(std::make_unique<APF>(juce::ParameterID(id, 1), d.name, juce::NormalisableRange<float>(0.f, 1.f), def,
                      juce::AudioParameterFloatAttributes().withStringFromValueFunction(
                          [this, id](float v, int) { return describe(id, v); })));
              else if (d.kind == 1)
                  layout.add(std::make_unique<APC>(juce::ParameterID(id, 1), d.name, juce::StringArray::fromTokens(d.choices, "|", ""), juce::roundToInt(def)));
              else
                  layout.add(std::make_unique<APB>(juce::ParameterID(id, 1), d.name, def > 0.5f));
          }
          layout.add(std::make_unique<APC>(juce::ParameterID("preset", 1), "Preset", juce::StringArray{"Preset 1", "Live", "Preset 2"}, 1));
          layout.add(std::make_unique<APB>(juce::ParameterID("wetSwitch", 1), "Left footswitch (wet: tap on/off, hold freeze)", false));
          layout.add(std::make_unique<APB>(juce::ParameterID("loopSwitch", 1), "Right footswitch (looper: tap play/listen, hold overdub)", false));
          return layout;
      }()) {
    for (int i = 0; i < badmood::kNumParams; ++i) raw_[static_cast<size_t>(i)] = apvts.getRawParameterValue(badmood::kParamNames[i]);
    wetSw_ = apvts.getRawParameterValue("wetSwitch");
    loopSw_ = apvts.getRawParameterValue("loopSwitch");
    formats_.registerBasicFormats();
    // The standalone starts on the built-in phrase so it makes sound with nothing plugged in;
    // in a DAW the plugin processes the track.
    if (wrapperType == wrapperType_Standalone) source = Demo;
    core_.prepare(48000.0);
    live_ = snapshot();
    startTimerHz(30);
}

BadMoodProcessor::~BadMoodProcessor() { stopTimer(); }

juce::String BadMoodProcessor::describe(const juce::String& id, float v) const {
    auto get = [this](const char* pid, float fallback) { const auto* r = apvts.getRawParameterValue(pid); return r ? r->load() : fallback; };
    if (apvts.getRawParameterValue("volume") == nullptr) return juce::String(v, 3);   // still constructing
    const int wetMode = juce::roundToInt(get("wetMode", 0)), loopMode = juce::roundToInt(get("loopMode", 1));
    const double sr = getSampleRate() > 0 ? getSampleRate() : 48000.0;
    const bool smooth = get("smooth", 0) > 0.5f;
    const double c = std::min(1.0, badmood::clockHz(get("clock", 1), smooth) / sr);
    const double fv = v;
    if (id == "time") {
        if (wetMode == 0) return fv > 0.985 ? juce::String("decay infinite") : "decay " + fmtTime(badmood::expMap(fv, 0.4, 40) / c);
        if (wetMode == 1) return "delay " + fmtTime(badmood::expMap(fv, 0.03, badmood::kRelayTmaxSec) / c);
        return "lag " + fmtTime(badmood::kFlipLagMaxSec * fv * fv / c);
    }
    if (id == "mix") {
        const bool dk = get("dryKill", 0) > 0.5f;
        return "dry " + pct(dk ? 0 : std::min(1.0, 2.0 * (1 - fv))) + " / fx " + pct(std::min(1.0, 2.0 * fv));
    }
    if (id == "clock") {
        const double hz = badmood::clockHz(fv, smooth);
        return hz > sr ? juce::String(hz / 1000, 1) + " kHz (capped at " + juce::String(sr / 1000, 1) + ")"
                       : juce::String(hz / 1000, 1) + " kHz";
    }
    if (id == "modWet") {
        if (wetMode == 0) return "character " + pct(fv) + (fv > 0.6 ? " (synthetic)" : fv < 0.3 ? " (diffuse)" : "");
        if (wetMode == 1) { const int n = badmood::relayRepeats(fv); return n > badmood::kRelayTaps ? juce::String("infinite repeats") : juce::String(n) + (n == 1 ? " repeat" : " repeats"); }
        const auto& ch = badmood::kFlipChords[badmood::flipChordIndex(fv)];
        juce::String s = "chord " + juce::String(badmood::flipChordIndex(fv) + 1) + ":";
        for (int k = 0; k < ch.n; ++k) s << " " << badmood::kIntervalNames[ch.note[k]];
        return s;
    }
    const double L = (get("half", 0) > 0.5f ? badmood::kLoopSamplesHalf : badmood::kLoopSamples) / (c * sr);
    if (id == "length") {
        if (loopMode == 0) return "step " + fmtTime(L / badmood::kBurstDivs[juce::jlimit(0, 8, juce::roundToInt(fv * 8))]);
        if (loopMode == 2) {
            const int t = juce::jlimit(0, 5, static_cast<int>(std::floor(fv * 5)));
            const double fr = fv * 5 - t;
            return juce::String("mask: ") + badmood::kMaskNames[t] + (fr > 0.05 && t < 5 ? juce::String(" > ") + badmood::kMaskNames[t + 1] : juce::String());
        }
        const auto tn = badmood::radioTuning(get("modLoop", 0.5));
        switch (tn.a) {
            case 0: return "tape " + fmtSpeed(badmood::speedMap(fv, 0.04));
            case 1: return "ambient " + juce::String(std::pow(2.0, -4 * (1 - fv)), 2) + "x, same pitch";
            case 2: return "orchestral: " + juce::String(1 + static_cast<int>(std::floor(fv * 5.999))) + " voices";
            case 3: return "shoegaze: moment " + juce::String(fv * 7 + 1, 1) + " of 8";
            default: return "dance: rotate every " + fmtTime(L / (1 << juce::jlimit(0, 4, juce::roundToInt(fv * 4))));
        }
    }
    if (id == "modLoop") {
        if (loopMode == 0) return "sensitivity " + pct(fv);
        if (loopMode == 2) return fv <= 0.005 ? juce::String("off: pure loop") : "threshold " + pct(fv);
        const auto tn = badmood::radioTuning(fv);
        if (tn.b < 0) return juce::String("station: ") + badmood::kRadioNames[tn.a];
        return juce::String("static: ") + badmood::kRadioNames[tn.a] + " / " + badmood::kRadioNames[tn.b];
    }
    if (id == "rampSpeed") return "ramp period " + fmtTime(badmood::expMap(fv, 20, 0.25));
    if (id == "cross") return fv < 1e-3 ? juce::String("off") : pct(fv);
    if (id == "balance") return "wet " + pct(std::min(1.0, 2.0 * (1 - fv))) + " / loop " + pct(std::min(1.0, 2.0 * fv));
    if (id == "fade") return fv > 0.97 ? juce::String("no fade") : "keeps " + pct(0.25 + 0.75 * fv) + " per pass";
    if (id == "eq") return std::fabs(fv - 0.5) <= 0.01 ? juce::String("flat") : fv < 0.5 ? "darker " + pct((0.5 - fv) * 2) : "brighter " + pct((fv - 0.5) * 2);
    if (id == "glue") return fv < 1e-3 ? juce::String("off") : pct(fv) + (fv > 0.55 ? " (destroy)" : "");
    if (id == "blend") return "clean loop " + pct(fv);
    if (id == "volume") return juce::String((fv - 0.5) * 24, 1) + " dB";
    return juce::String(v, 3);
}

juce::String BadMoodProcessor::describe(const juce::String& id) const {
    const auto* p = apvts.getRawParameterValue(id);
    return p ? describe(id, p->load()) : juce::String();
}

void BadMoodProcessor::makeDemo(double sr) {
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
            pluck(static_cast<double>(c) * 2.0 + static_cast<double>(i) / 3.0, chords[c][i],
                  i == 0 ? 0.32f : 0.22f + 0.04f * rnd.nextFloat(), i == 0 ? 3.5 : 2.2);
    const float pk = demo_.getMagnitude(0, 0, len);
    if (pk > 0) demo_.applyGain(0.5f / pk);
    demoPos_ = 0;
}

void BadMoodProcessor::prepareToPlay(double sampleRate, int samplesPerBlock) {
    core_.prepare(sampleRate);
    pullParams();
    lastWetSw_ = lastLoopSw_ = false;
    monoL_.assign(static_cast<size_t>(std::max(samplesPerBlock, 32)), 0.f);
    monoR_.assign(static_cast<size_t>(std::max(samplesPerBlock, 32)), 0.f);
    if (demo_.getNumSamples() != static_cast<int>(8 * sampleRate)) makeDemo(sampleRate);
}

void BadMoodProcessor::pullParams() {
    for (int i = 0; i < badmood::kNumParams; ++i)
        badmood::setParam(core_.p, badmood::kParamNames[i], static_cast<double>(raw_[static_cast<size_t>(i)]->load()));
}

bool BadMoodProcessor::loadFile(const juce::File& f) {
    std::unique_ptr<juce::AudioFormatReader> r(formats_.createReaderFor(f));
    if (!r || r->lengthInSamples <= 0 || r->lengthInSamples > 60 * 60 * 192000) return false;
    juce::AudioBuffer<float> buf(static_cast<int>(r->numChannels), static_cast<int>(r->lengthInSamples));
    r->read(&buf, 0, buf.getNumSamples(), 0, true, true);
    const juce::SpinLock::ScopedLockType lock(fileLock_);   // the audio thread only tries this lock
    file_ = std::move(buf); fileRate_ = r->sampleRate; filePos_ = 0; fileName_ = f.getFileName();
    return true;
}

bool BadMoodProcessor::copyWave(std::array<float, kWaveBins>& out) const {
    const juce::SpinLock::ScopedTryLockType lock(waveLock_);
    if (!lock.isLocked()) return false;
    out = wave_;
    return true;
}

void BadMoodProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) {
    juce::ScopedNoDenormals noDenormals;
    const auto t0 = juce::Time::getHighResolutionTicks();
    const int n = buffer.getNumSamples();
    const int nIn = getTotalNumInputChannels();

    // MIDI: footswitch functions act at once; everything else goes to the message thread
    for (const auto meta : midi) {
        const auto m = meta.getMessage();
        if (m.isController()) {
            const int cc = m.getControllerNumber(), v = m.getControllerValue();
            if (cc == 102) core_.command("wet", v > 0);
            else if (cc == 103) core_.command("loop", v > 0);
            else if (cc == 105) core_.command("freeze", v > 0);
            else if (cc == 106) core_.command("overdub", v > 0);
            else if (cc == 55) core_.command("trueBypass", v > 0);
            else {
                const auto scope = midiFifo_.write(1);
                if (scope.blockSize1 > 0) midiBuf_[static_cast<size_t>(scope.startIndex1)] = {cc, v};
            }
        } else if (m.isProgramChange()) {
            const auto scope = midiFifo_.write(1);
            if (scope.blockSize1 > 0) midiBuf_[static_cast<size_t>(scope.startIndex1)] = {-1 - m.getProgramChangeNumber(), 0};
        }
    }
    midi.clear();

    if (buffer.getNumChannels() < 2 || n == 0) { buffer.clear(); return; }

    pullParams();
    const bool wetSw = wetSw_->load() > 0.5f, loopSw = loopSw_->load() > 0.5f;
    if (wetSw != lastWetSw_) { lastWetSw_ = wetSw; core_.footswitch(0, wetSw ? badmood::FsDown : badmood::FsUp); }
    if (loopSw != lastLoopSw_) { lastLoopSw_ = loopSw; core_.footswitch(1, loopSw ? badmood::FsDown : badmood::FsUp); }
    for (int w = 0; w < 2; ++w)
        if (pendingShift_[static_cast<size_t>(w)].exchange(false)) core_.footswitch(w, badmood::FsShift);

    if (static_cast<int>(monoL_.size()) < n) { monoL_.resize(static_cast<size_t>(n)); monoR_.resize(static_cast<size_t>(n)); }   // only if the host exceeds its promised block size
    float* xl = monoL_.data();
    float* xr = monoR_.data();
    bool stereoIn = false;
    const float g = inputGain.load();
    const int src = source.load();
    if (src == Demo && demo_.getNumSamples() > 0) {
        const float* d = demo_.getReadPointer(0);
        const int len = demo_.getNumSamples();
        int pos = static_cast<int>(demoPos_);
        for (int i = 0; i < n; ++i) { xl[i] = d[pos] * g; if (++pos >= len) pos = 0; }
        demoPos_ = pos;
    } else if (src == File) {
        std::fill(xl, xl + n, 0.f);
        const juce::SpinLock::ScopedTryLockType lock(fileLock_);
        if (lock.isLocked() && file_.getNumSamples() > 1 && fileRate_ > 0) {
            const double ratio = fileRate_ / getSampleRate();
            const int len = file_.getNumSamples(), nc = file_.getNumChannels();
            stereoIn = nc > 1;
            for (int i = 0; i < n; ++i) {
                const int i0 = static_cast<int>(filePos_), i1 = (i0 + 1) % len;
                const float fr = static_cast<float>(filePos_ - i0);
                const float* s0 = file_.getReadPointer(0);
                const float* s1 = file_.getReadPointer(nc > 1 ? 1 : 0);
                xl[i] = (s0[i0] + (s0[i1] - s0[i0]) * fr) * g;
                xr[i] = (s1[i0] + (s1[i1] - s1[i0]) * fr) * g;
                filePos_ += ratio; if (filePos_ >= len) filePos_ -= len;
            }
        }
    } else {
        const float* l = buffer.getReadPointer(0);
        for (int i = 0; i < n; ++i) xl[i] = l[i] * g;
        if (nIn > 1) {
            stereoIn = true;
            const float* r = buffer.getReadPointer(1);
            for (int i = 0; i < n; ++i) xr[i] = r[i] * g;
        }
    }

    core_.process(xl, stereoIn ? xr : nullptr, buffer.getWritePointer(0), buffer.getWritePointer(1), n);
    for (int c = 2; c < buffer.getNumChannels(); ++c) buffer.clear(c, 0, n);

    badmood::Event evs[16];
    const int ne = core_.takeEvents(evs, 16);
    for (int i = 0; i < ne; ++i) {
        if (evs[i] == badmood::Event::PresetSave1) pendingPresetSave_ = 0;
        else if (evs[i] == badmood::Event::PresetSave2) pendingPresetSave_ = 1;
    }

    const auto st = core_.state();
    ui.wetOn = st.wetOn; ui.freeze = st.freeze; ui.hidden = st.hidden; ui.trueBypass = st.trueBypass; ui.scramble = st.scramble;
    ui.loopLed = static_cast<int>(st.loop);
    ui.pos = st.pos; ui.len = st.len; ui.seam = st.seam; ui.cycles = st.cycles;
    ui.station = st.station; ui.station2 = st.station2; ui.tune = static_cast<float>(st.tune); ui.mask = static_cast<float>(st.mask);
    ui.nSlices = st.nSlices; ui.step = st.step;
    for (int i = 0; i < 8; ++i) ui.slices[static_cast<size_t>(i)] = i < st.nSlices ? core_.sliceStart(i) : 0;
    ui.clockHz = static_cast<float>(st.clockHz);
    ui.peakIn = st.peakIn; ui.peakOut = st.peakOut;

    // refresh the waveform a few times a second
    if (++waveBlocks_ % 16 == 0) {
        const juce::SpinLock::ScopedTryLockType lock(waveLock_);
        if (lock.isLocked()) { core_.loopPeaks(wave_.data(), kWaveBins); ui.waveVersion.fetch_add(1); }
    }
    const double secs = juce::Time::highResolutionTicksToSeconds(juce::Time::getHighResolutionTicks() - t0);
    ui.load = static_cast<float>(100.0 * secs / (n / getSampleRate()));
}

// ---- message thread: MIDI parameters, presets ----
void BadMoodProcessor::setParamFromMidi(const char* id, float plain) {
    if (auto* p = apvts.getParameter(id)) {
        p->beginChangeGesture();
        p->setValueNotifyingHost(p->convertTo0to1(plain));
        p->endChangeGesture();
    }
}

void BadMoodProcessor::timerCallback() {
    const auto scope = midiFifo_.read(midiFifo_.getNumReady());
    auto handle = [this](const MidiMsg& m) {
        if (m.cc < 0) {   // program change: 0 = live, 1 = right slot, 2 = left slot (MIDI manual p.2)
            const int pc = -1 - m.cc;
            if (pc <= 2) setParamFromMidi("preset", pc == 0 ? 1.f : pc == 1 ? 2.f : 0.f);
            return;
        }
        for (const auto& c : kCc) {
            if (c.cc != m.cc) continue;
            const float v = c.kind == 0 ? static_cast<float>(m.value) / 127.f
                          : c.kind == 1 ? (m.value <= 1 ? 0.f : m.value == 2 ? 1.f : 2.f)
                          : (m.value > 0 ? 1.f : 0.f);
            setParamFromMidi(c.id, v);
            return;
        }
    };
    for (int i = 0; i < scope.blockSize1; ++i) handle(midiBuf_[static_cast<size_t>(scope.startIndex1 + i)]);
    for (int i = 0; i < scope.blockSize2; ++i) handle(midiBuf_[static_cast<size_t>(scope.startIndex2 + i)]);

    const int save = pendingPresetSave_.exchange(-1);
    if (save >= 0) savePreset(save);

    // PRESET toggle: leaving Live stores the live controls; returning restores them
    const int pos = juce::roundToInt(param("preset"));
    if (pos != lastPresetPos_) {
        if (lastPresetPos_ == 1) live_ = snapshot();
        if (pos == 1) applySnapshot(live_);
        else if (slots_[static_cast<size_t>(pos / 2)].isValid()) applySnapshot(slots_[static_cast<size_t>(pos / 2)]);
        lastPresetPos_ = pos;
    }
}

juce::ValueTree BadMoodProcessor::snapshot() const {
    juce::ValueTree t("SNAPSHOT");
    for (const char* id : badmood::kParamNames)
        if (const auto* p = apvts.getRawParameterValue(id)) t.setProperty(id, p->load(), nullptr);
    return t;
}

void BadMoodProcessor::applySnapshot(const juce::ValueTree& t) {
    if (!t.isValid()) return;
    for (const char* id : badmood::kParamNames)
        if (t.hasProperty(id)) setParamFromMidi(id, static_cast<float>(t[id]));
}

void BadMoodProcessor::savePreset(int slot) {
    slots_[static_cast<size_t>(juce::jlimit(0, 1, slot))] = snapshot();
    ui.presetSaved.fetch_add(1);
}

void BadMoodProcessor::resetHiddenOptions() {
    badmood::Params defaults;
    for (const char* id : kHiddenIds) setParamFromMidi(id, static_cast<float>(badmood::getParam(defaults, id)));
}

void BadMoodProcessor::getStateInformation(juce::MemoryBlock& dest) {
    auto state = apvts.copyState();
    state.setProperty("source", source.load(), nullptr);
    state.setProperty("inputGain", inputGain.load(), nullptr);
    state.removeChild(state.getChildWithName("PRESETS"), nullptr);
    juce::ValueTree presets("PRESETS");
    for (int i = 0; i < 2; ++i)
        if (slots_[static_cast<size_t>(i)].isValid()) { auto c = slots_[static_cast<size_t>(i)].createCopy(); c.setProperty("slot", i, nullptr); presets.appendChild(c, nullptr); }
    if (lastPresetPos_ != 1 && live_.isValid()) { auto c = live_.createCopy(); c.setProperty("slot", -1, nullptr); presets.appendChild(c, nullptr); }
    state.appendChild(presets, nullptr);
    if (auto xml = state.createXml()) copyXmlToBinary(*xml, dest);
}

void BadMoodProcessor::setStateInformation(const void* data, int size) {
    if (auto xml = getXmlFromBinary(data, size)) {
        if (!xml->hasTagName(apvts.state.getType())) return;
        auto state = juce::ValueTree::fromXml(*xml);
        if (state.hasProperty("source")) source = juce::jlimit(0, 1, static_cast<int>(state["source"]));   // a file is not restored
        if (state.hasProperty("inputGain")) inputGain = static_cast<float>(state["inputGain"]);
        const auto presets = state.getChildWithName("PRESETS");
        slots_[0] = slots_[1] = juce::ValueTree();
        juce::ValueTree storedLive;
        for (const auto& c : presets) {
            const int slot = c["slot"];
            if (slot == 0 || slot == 1) slots_[static_cast<size_t>(slot)] = c.createCopy();
            else storedLive = c.createCopy();
        }
        state.removeChild(presets, nullptr);
        apvts.replaceState(state);
        lastPresetPos_ = juce::roundToInt(param("preset"));
        live_ = lastPresetPos_ != 1 && storedLive.isValid() ? storedLive : snapshot();
        // footswitches are momentary: never restore one as held down
        for (auto* id : {"wetSwitch", "loopSwitch"})
            if (auto* p = apvts.getParameter(id)) p->setValueNotifyingHost(0.f);
    }
}

juce::AudioProcessorEditor* BadMoodProcessor::createEditor() { return new BadMoodEditor(*this); }
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new BadMoodProcessor(); }
