#pragma once
#include "PluginProcessor.h"

// Pedal-style front panel. Repaints are limited to the small regions that change
// (LEDs, readout, meters, loop display): a full-window repaint at 30 Hz can starve
// the UI thread on high-DPI screens.
class MoodEditor final : public juce::AudioProcessorEditor, private juce::Timer {
public:
    explicit MoodEditor(MoodProcessor&);
    ~MoodEditor() override;
    void paint(juce::Graphics&) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress&) override;
    bool keyStateChanged(bool isKeyDown) override;

    // ---- small components (public so the .cpp can define them) ----
    struct KnobLook : juce::LookAndFeel_V4 {
        KnobLook();
        void drawRotarySlider(juce::Graphics&, int x, int y, int w, int h, float pos, float a0, float a1, juce::Slider&) override;
        void drawToggleButton(juce::Graphics&, juce::ToggleButton&, bool, bool) override;
    };
    struct ThreeWay : juce::Component {
        ThreeWay(juce::RangedAudioParameter& p, juce::StringArray labels, std::function<void()> onChange);
        void paint(juce::Graphics&) override;
        void mouseDown(const juce::MouseEvent&) override;
        juce::RangedAudioParameter& param;
        juce::StringArray labels;
        juce::ParameterAttachment attach;
        int index = 0;
        std::function<void()> changed;
    };
    struct Footswitch : juce::Component {
        Footswitch(juce::RangedAudioParameter& p, juce::String key) : param(p), keyName(std::move(key)) {}
        void paint(juce::Graphics&) override;
        void mouseDown(const juce::MouseEvent&) override { press(true); }
        void mouseUp(const juce::MouseEvent&) override { press(false); }
        void press(bool down);
        juce::RangedAudioParameter& param;
        juce::String keyName;
        bool isDown = false;
    };
    struct LoopView : juce::Component {
        explicit LoopView(MoodProcessor& p) : proc(p) {}
        void paint(juce::Graphics&) override;
        MoodProcessor& proc;
        std::array<float, MoodProcessor::kWaveBins> wave {};
        uint32_t version = 0;
    };

private:
    void timerCallback() override;
    void showReadout(const juce::String& label, const juce::String& value);
    void touch(const juce::String& id);

    MoodProcessor& proc;
    KnobLook look;

    struct Knob { juce::String id, label; std::unique_ptr<juce::Slider> slider; std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> att; };
    std::vector<Knob> knobs;
    std::unique_ptr<ThreeWay> wetMode, routing, loopMode;
    std::unique_ptr<Footswitch> fsWet, fsLoop;
    LoopView loopView;

    juce::ComboBox sourceBox;
    juce::TextButton loadButton { "Load file..." };
    juce::Slider inputGain;
    juce::Slider balance;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> balanceAtt;
    struct Dip { std::unique_ptr<juce::ToggleButton> button; std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> att; };
    std::vector<Dip> dips;
    std::unique_ptr<juce::FileChooser> chooser;

    juce::Rectangle<int> pedalArea, lcdArea, ledWet, ledLoop, meterArea;
    juce::String lcdLabel { "READY" }, lcdValue { "Tap L to loop, A for wet" }, lastTouched;
    juce::String lcdMeta;
    bool ledWetOn = false, ledLoopOn = false, blink = false;
    uint32_t lastCycles = 0; double flashUntil = 0; int lastLoopLed = 0;
    float meterIn = 0, meterOut = 0;
    bool keyA = false, keyL = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MoodEditor)
};
