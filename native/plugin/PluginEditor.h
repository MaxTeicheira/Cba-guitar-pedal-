#pragma once
#include "PluginProcessor.h"

// Pedal-style front panel. Repaints are limited to the small regions that change
// (LEDs, readout, meters, loop display): a full-window repaint at 30 Hz can starve
// the UI thread on high-DPI screens.
//
// As on the pedal, the six knobs and three toggles take other jobs while footswitches
// are held: both switches = Hidden Options; right switch + MIX = effect volume; with a
// ramp dip on, MIX sets the ramp speed unless the left switch is held.
class BadMoodEditor final : public juce::AudioProcessorEditor, private juce::Timer {
public:
    explicit BadMoodEditor(BadMoodProcessor&);
    ~BadMoodEditor() override;
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
        explicit ThreeWay(std::function<void()> onChange) : changed(std::move(onChange)) {}
        void bind(juce::RangedAudioParameter& p, juce::StringArray labels);
        void paint(juce::Graphics&) override;
        void mouseDown(const juce::MouseEvent&) override;
        juce::RangedAudioParameter* param = nullptr;
        juce::StringArray labels;
        std::unique_ptr<juce::ParameterAttachment> attach;
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
        explicit LoopView(BadMoodProcessor& p) : proc(p) {}
        void paint(juce::Graphics&) override;
        BadMoodProcessor& proc;
        std::array<float, BadMoodProcessor::kWaveBins> wave {};
        uint32_t version = 0;
    };

private:
    void timerCallback() override;
    void showReadout(const juce::String& label, const juce::String& value);
    void touch(const juce::String& id);
    void rebind();
    juce::String mainKnobId(int i) const;
    static juce::String labelFor(const juce::String& id);

    BadMoodProcessor& proc;
    KnobLook look;

    struct Knob { juce::String id; std::unique_ptr<juce::Slider> slider; std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> att; };
    std::vector<Knob> knobs;          // TIME MIX LENGTH / MODIFY CLOCK MODIFY (rebindable)
    std::unique_ptr<ThreeWay> toggles[3], presetToggle;
    bool togglesHidden = false;
    std::unique_ptr<Footswitch> fsWet, fsLoop;
    LoopView loopView;

    juce::ComboBox sourceBox;
    juce::TextButton loadButton { "Load file..." };
    juce::Slider inputGain;
    struct Dip { juce::String id; std::unique_ptr<juce::ToggleButton> button; std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> att; };
    std::vector<Dip> dips;
    std::vector<Knob> hiddenKnobs;    // fixed bindings in the Hidden Options panel
    struct Combo { std::unique_ptr<juce::ComboBox> box; std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> att; };
    std::vector<Combo> hiddenCombos;
    juce::TextButton save1 { "Save Preset 1" }, save2 { "Save Preset 2" }, resetHidden { "Reset hidden" };
    std::unique_ptr<juce::FileChooser> chooser;

    juce::Rectangle<int> pedalArea, lcdArea, ledWet, ledMid, ledLoop, meterArea, dipArea, hiddenArea, presetArea;
    juce::String lcdLabel { "READY" }, lcdValue { "Tap L to loop, A for wet" }, lastTouched;
    juce::String lcdMeta;
    bool ledWetOn = false, ledLoopOn = false, blink = false;
    int ledMidState = 0;              // 0 off, 1 green, 2 red
    juce::String boundKey;
    uint32_t lastCycles = 0, lastSaved = 0; double flashUntil = 0, savedUntil = 0; int lastLoopLed = -1;
    bool lastHidden = false, lastTb = false;
    float meterIn = 0, meterOut = 0;
    bool keyA = false, keyL = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(BadMoodEditor)
};
