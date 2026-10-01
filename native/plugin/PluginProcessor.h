#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include <array>
#include "dsp/mood.h"

// The pedal as an AudioProcessor. Every control (knobs, toggles, dips, and both
// footswitches as momentary switches) is a host parameter, so a DAW can automate
// it or map it to a MIDI controller.
class MoodProcessor final : public juce::AudioProcessor {
public:
    MoodProcessor();
    ~MoodProcessor() override = default;

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
    const juce::String getName() const override { return "MoodEmu"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 30.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return "Default"; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock& dest) override;
    void setStateInformation(const void* data, int size) override;

    juce::AudioProcessorValueTreeState apvts;

    // Readout text for a parameter in real-world units (depends on mode and clock).
    juce::String describe(const juce::String& id) const;
    juce::String describe(const juce::String& id, float value) const;

    // ---- input source ----
    enum Source { Live = 0, Demo = 1, File = 2 };
    std::atomic<int> source { Live };
    std::atomic<float> inputGain { 1.f };
    bool loadFile(const juce::File& f);           // message thread
    juce::String fileName() const { return fileName_; }

    // ---- UI snapshot (written by the audio thread, read by the editor) ----
    struct Ui {
        std::atomic<bool> wetOn { false }, freeze { false };
        std::atomic<int> loopLed { 0 }, lsrc { 0 }, winStart { 0 }, winLen { 0 };
        std::atomic<float> pos { 0 }, clockHz { 48000 }, peakIn { 0 }, peakOut { 0 }, load { 0 };
        std::atomic<uint32_t> histCycles { 0 };
        std::atomic<uint32_t> waveVersion { 0 };
    } ui;
    static constexpr int kWaveBins = 400;
    // copy of the loop's peak envelope; returns false if the audio thread holds the lock
    bool copyWave(std::array<float, kWaveBins>& out) const;

private:
    void pullParams();
    void makeDemo(double sr);
    float param(const char* id) const { return apvts.getRawParameterValue(id)->load(); }

    mood::Core core_;
    bool lastWetSw_ = false, lastLoopSw_ = false;
    std::vector<float> mono_;
    int dubBlocks_ = 0;
    bool pendingWave_ = false;

    juce::AudioBuffer<float> demo_;
    double demoPos_ = 0;

    juce::AudioFormatManager formats_;
    juce::AudioBuffer<float> file_;
    double fileRate_ = 0, filePos_ = 0;
    juce::String fileName_;
    juce::SpinLock fileLock_;

    std::array<float, kWaveBins> wave_ {};
    mutable juce::SpinLock waveLock_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MoodProcessor)
};
