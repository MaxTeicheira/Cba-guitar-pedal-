#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include <array>
#include "dsp/badmood.h"

// The pedal as an AudioProcessor. Every control (knobs, toggles, dips, hidden options and
// both footswitches as momentary switches) is a host parameter, so a DAW can automate it
// or map it to a controller. The BAD MOOD MIDI CC map is also understood directly.
class BadMoodProcessor final : public juce::AudioProcessor, private juce::Timer {
public:
    BadMoodProcessor();
    ~BadMoodProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    bool isBusesLayoutSupported(const BusesLayout& l) const override {
        const auto in = l.getMainInputChannelSet();
        return l.getMainOutputChannelSet() == juce::AudioChannelSet::stereo()
            && (in == juce::AudioChannelSet::mono() || in == juce::AudioChannelSet::stereo());
    }
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return "BadMoodEmu"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 40.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return "Default"; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock& dest) override;
    void setStateInformation(const void* data, int size) override;

    juce::AudioProcessorValueTreeState apvts;

    // Readout text for a parameter in real-world units (depends on modes and clock).
    juce::String describe(const juce::String& id) const;
    juce::String describe(const juce::String& id, float value) const;

    // ---- presets (message thread) ----
    void savePreset(int slot);              // slot 0 = Preset 1 (left), 1 = Preset 2 (right)
    bool presetStored(int slot) const { return slots_[static_cast<size_t>(slot)].isValid(); }
    void resetHiddenOptions();
    // A knob moved while a footswitch is held: that switch then does nothing (pedal "shift").
    void shiftFootswitch(int which) { pendingShift_[static_cast<size_t>(which)].store(true); }

    // ---- input source ----
    enum Source { Live = 0, Demo = 1, File = 2 };
    std::atomic<int> source { Live };
    std::atomic<float> inputGain { 1.f };
    bool loadFile(const juce::File& f);           // message thread
    juce::String fileName() const { return fileName_; }

    // ---- UI snapshot (written by the audio thread, read by the editor) ----
    struct Ui {
        std::atomic<bool> wetOn { false }, freeze { false }, hidden { false }, trueBypass { false }, scramble { false };
        std::atomic<int> loopLed { 0 }, pos { 0 }, len { badmood::kLoopSamples }, seam { 0 };
        std::atomic<int> station { 0 }, station2 { -1 }, nSlices { 0 }, step { 0 };
        std::atomic<float> tune { 0 }, mask { 0 }, clockHz { 48000 }, peakIn { 0 }, peakOut { 0 }, load { 0 };
        std::atomic<uint32_t> cycles { 0 }, waveVersion { 0 }, presetSaved { 0 };
        std::array<std::atomic<int>, 8> slices {};
    } ui;
    static constexpr int kWaveBins = 400;
    // copy of the loop's peak envelope; returns false if the audio thread holds the lock
    bool copyWave(std::array<float, kWaveBins>& out) const;

private:
    void timerCallback() override;
    void pullParams();
    void makeDemo(double sr);
    void applySnapshot(const juce::ValueTree& t);
    juce::ValueTree snapshot() const;
    void setParamFromMidi(const char* id, float plain);
    float param(const char* id) const { return apvts.getRawParameterValue(id)->load(); }

    badmood::Core core_;
    std::array<std::atomic<float>*, badmood::kNumParams> raw_ {};
    std::atomic<float>* wetSw_ = nullptr;
    std::atomic<float>* loopSw_ = nullptr;
    bool lastWetSw_ = false, lastLoopSw_ = false;
    std::array<std::atomic<bool>, 2> pendingShift_ {};
    std::vector<float> monoL_, monoR_;
    int waveBlocks_ = 0;

    // MIDI: CC/PC messages for parameters go through a lock-free FIFO to the message thread,
    // so the parameter tree stays the single source of truth
    struct MidiMsg { int cc; int value; };   // cc < 0: program change -cc-1
    juce::AbstractFifo midiFifo_ { 256 };
    std::array<MidiMsg, 256> midiBuf_ {};
    std::atomic<int> pendingPresetSave_ { -1 };

    // presets
    std::array<juce::ValueTree, 2> slots_;
    juce::ValueTree live_;
    int lastPresetPos_ = 1;

    juce::AudioBuffer<float> demo_;
    double demoPos_ = 0;

    juce::AudioFormatManager formats_;
    juce::AudioBuffer<float> file_;
    double fileRate_ = 0, filePos_ = 0;
    juce::String fileName_;
    juce::SpinLock fileLock_;

    std::array<float, kWaveBins> wave_ {};
    mutable juce::SpinLock waveLock_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(BadMoodProcessor)
};
