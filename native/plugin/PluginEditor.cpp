#include "PluginEditor.h"

namespace {
// palette shared with the web version (src/style.css)
const juce::Colour kBoard { 0xff16141a }, kEnclHi { 0xff5a4560 }, kEnclLo { 0xff2a1f2d };
const juce::Colour kInk { 0xfff3e7d6 }, kInkDim { 0xffc9b9b0 }, kPanel { 0xff221f26 }, kPanelLine { 0xff36313b };
const juce::Colour kText { 0xffded5cb }, kMuted { 0xff958d97 }, kKnobShade { 0xffb7a896 };
const juce::Colour kLedWet { 0xffffb347 }, kLedLoop { 0xffff5a4a }, kLedMid { 0xff5ee08a }, kAccent { 0xffe8a94c };
const juce::Colour kLcdBg { 0xff120f16 }, kLcdFg { 0xffc8f0b4 };

juce::Font font(float size, bool bold = false) {
    return juce::Font(juce::FontOptions(size).withStyle(bold ? "Bold" : "Regular"));
}
juce::Font mono(float size) { return juce::Font(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), size, juce::Font::plain)); }

constexpr int W = 1120, H = 740;
const char* const kMainIds[6] = {"time", "mix", "length", "modWet", "clock", "modLoop"};
const char* const kHiddenMain[6] = {"cross", "balance", "fade", "eq", "glue", "blend"};
const char* const kToggleIds[3] = {"wetMode", "routing", "loopMode"};
const char* const kHiddenToggles[3] = {"sync", "spreadSel", "inputMod"};

juce::StringArray toggleLabels(const juce::String& id) {
    if (id == "wetMode") return {"Soup", "Relay", "Flip"};
    if (id == "routing") return {"In", "In + Loop", "Loop"};
    if (id == "loopMode") return {"Burst", "Radio", "Mask"};
    if (id == "sync") return {"Loop>Wet", "Unsynced", "Wet>Loop"};
    if (id == "spreadSel") return {"Wet", "Both", "Loop"};
    if (id == "inputMod") return {"Wet>Loop", "Input", "Loop>Wet"};
    return {"Preset 1", "Live", "Preset 2"};
}
}  // namespace

// ───────── look and feel ─────────
BadMoodEditor::KnobLook::KnobLook() {
    setColour(juce::ComboBox::backgroundColourId, kPanel.brighter(0.08f));
    setColour(juce::ComboBox::outlineColourId, kPanelLine);
    setColour(juce::ComboBox::textColourId, kText);
    setColour(juce::PopupMenu::backgroundColourId, kPanel);
    setColour(juce::TextButton::buttonColourId, kAccent);
    setColour(juce::TextButton::textColourOffId, juce::Colour(0xff1d1608));
    setColour(juce::Slider::trackColourId, kAccent);
    setColour(juce::Slider::backgroundColourId, juce::Colour(0xff151317));
    setColour(juce::Slider::thumbColourId, kAccent);
}

void BadMoodEditor::KnobLook::drawRotarySlider(juce::Graphics& g, int x, int y, int w, int h, float pos, float a0, float a1, juce::Slider& s) {
    const bool small = w < 70;
    const auto area = juce::Rectangle<float>(static_cast<float>(x), static_cast<float>(y), static_cast<float>(w), static_cast<float>(h)).reduced(small ? 6.f : 10.f);
    const float r = std::min(area.getWidth(), area.getHeight()) / 2;
    const auto c = area.getCentre();
    g.setColour(kInk.withAlpha(0.55f));   // ticks
    for (int i = 0; i <= 10; ++i) {
        const float a = a0 + (a1 - a0) * static_cast<float>(i) / 10.f;
        const float r1 = r + 3, r2 = r + (i % 5 == 0 ? (small ? 6.f : 9.f) : (small ? 5.f : 7.f));
        g.drawLine(c.x + r1 * std::sin(a), c.y - r1 * std::cos(a), c.x + r2 * std::sin(a), c.y - r2 * std::cos(a), 1.2f);
    }
    g.setColour(juce::Colours::black.withAlpha(0.35f));
    g.fillEllipse(juce::Rectangle<float>(r * 2, r * 2).withCentre(c.translated(0, 3)));
    const bool hiddenLayer = s.getProperties()["hidden"];
    g.setGradientFill(juce::ColourGradient(hiddenLayer ? juce::Colour(0xffd9f5df) : juce::Colour(0xfffffaf0), c.x - r * 0.3f, c.y - r * 0.4f,
                                           hiddenLayer ? juce::Colour(0xff7fb08c) : kKnobShade, c.x + r, c.y + r, true));
    g.fillEllipse(juce::Rectangle<float>(r * 2, r * 2).withCentre(c));
    const float a = a0 + pos * (a1 - a0);
    juce::Path p;
    p.addRoundedRectangle(-2.f, -r * 0.86f, 4.f, r * 0.68f, 2.f);
    g.setColour(juce::Colour(0xff2a2a28));
    g.fillPath(p, juce::AffineTransform::rotation(a).translated(c));
}

void BadMoodEditor::KnobLook::drawToggleButton(juce::Graphics& g, juce::ToggleButton& b, bool, bool) {
    // yellow DIP switch with a white slider; label underneath
    const auto bounds = b.getLocalBounds().toFloat();
    const auto sw = juce::Rectangle<float>(16, 30).withCentre({bounds.getCentreX(), 18});
    g.setColour(juce::Colour(0xffd9b23a)); g.fillRoundedRectangle(sw, 3);
    g.setColour(juce::Colour(0xff8a6d17)); g.drawRoundedRectangle(sw, 3, 2);
    const auto knob = juce::Rectangle<float>(10, 12).withCentre({sw.getCentreX(), b.getToggleState() ? sw.getY() + 8.5f : sw.getBottom() - 8.5f});
    g.setColour(juce::Colour(0xfff2eee6)); g.fillRoundedRectangle(knob, 2);
    if (b.hasKeyboardFocus(true)) { g.setColour(kAccent); g.drawRoundedRectangle(sw.expanded(3), 4, 1.5f); }
    g.setColour(kMuted); g.setFont(font(10.5f, true));
    g.drawFittedText(b.getButtonText().toUpperCase(), bounds.withTop(36).toNearestInt(), juce::Justification::centredTop, 2, 0.8f);
}

// ───────── 3-way toggle ─────────
void BadMoodEditor::ThreeWay::bind(juce::RangedAudioParameter& p, juce::StringArray l) {
    attach.reset();
    param = &p;
    labels = std::move(l);
    attach = std::make_unique<juce::ParameterAttachment>(p, [this](float v) { index = juce::roundToInt(v); repaint(); if (changed) changed(); });
    attach->sendInitialUpdate();
    repaint();
}
void BadMoodEditor::ThreeWay::paint(juce::Graphics& g) {
    const auto lever = juce::Rectangle<float>(0, static_cast<float>(getHeight()) / 2.f - 32, 22, 64);
    g.setColour(juce::Colour(0xff3a343f)); g.fillRoundedRectangle(lever, 11);
    const auto bat = juce::Rectangle<float>(16, 22).withPosition(lever.getX() + 3, lever.getY() + 3 + static_cast<float>(index) * 18.f);
    g.setGradientFill(juce::ColourGradient(juce::Colours::white, bat.getX(), bat.getY(), juce::Colour(0xff8d8d88), bat.getRight(), bat.getBottom(), false));
    g.fillRoundedRectangle(bat, 8);
    for (int i = 0; i < 3; ++i) {
        const auto r = juce::Rectangle<float>(30, lever.getY() + 2 + static_cast<float>(i) * 20.f, static_cast<float>(getWidth() - 30), 20);
        g.setColour(i == index ? kInk : kInkDim.withAlpha(0.75f));
        g.setFont(font(12.5f, i == index));
        g.drawText(labels[i].toUpperCase(), r, juce::Justification::centredLeft);
        if (i == index) g.fillRect(r.getX(), r.getBottom() - 3, juce::GlyphArrangement::getStringWidth(font(12.5f, true), labels[i].toUpperCase()), 1.f);
    }
}
void BadMoodEditor::ThreeWay::mouseDown(const juce::MouseEvent& e) {
    if (!attach) return;
    int next;
    if (e.x < 26) next = (index + 1) % 3;   // click the lever to cycle
    else next = juce::jlimit(0, 2, (e.y - (getHeight() / 2 - 32) - 2) / 20);
    attach->setValueAsCompleteGesture(static_cast<float>(next));
}

// ───────── footswitch ─────────
void BadMoodEditor::Footswitch::press(bool down) {
    if (down == isDown) return;
    isDown = down;
    if (down) { param.beginChangeGesture(); param.setValueNotifyingHost(1.f); }
    else { param.setValueNotifyingHost(0.f); param.endChangeGesture(); }
    repaint();
}
void BadMoodEditor::Footswitch::paint(juce::Graphics& g) {
    auto r = getLocalBounds().toFloat().reduced(4);
    g.setColour(juce::Colour(0xff5d5660)); g.fillEllipse(r);
    g.setColour(juce::Colour(0xff2c282f)); g.fillEllipse(r.reduced(2));
    auto cap = r.reduced(9).translated(0, isDown ? 2.f : 0.f);
    if (isDown) cap = cap.reduced(1);
    g.setGradientFill(juce::ColourGradient(juce::Colour(0xfff4f4f0), cap.getX() + cap.getWidth() * 0.35f, cap.getY() + cap.getHeight() * 0.3f,
                                           juce::Colour(0xff7c7e79), cap.getRight(), cap.getBottom(), true));
    g.fillEllipse(cap);
    g.setColour(juce::Colour(0xff55574f)); g.setFont(mono(13));
    g.drawText(keyName, cap, juce::Justification::centred);
}

// ───────── loop display ─────────
void BadMoodEditor::LoopView::paint(juce::Graphics& g) {
    const auto b = getLocalBounds().toFloat();
    g.setColour(juce::Colour(0xff151317)); g.fillRoundedRectangle(b, 6);
    const int len = std::max(1, proc.ui.len.load());
    const int n = BadMoodProcessor::kWaveBins;
    const float bw = b.getWidth() / static_cast<float>(n), mid = b.getCentreY();
    const int led = proc.ui.loopLed.load();
    for (int i = 0; i < n; ++i) {
        const float a = std::min(1.f, wave[static_cast<size_t>(i)] * 1.6f) * (b.getHeight() / 2 - 4);
        g.setColour(led == 0 ? juce::Colour(0xff58505c) : kAccent);
        g.fillRect(b.getX() + static_cast<float>(i) * bw, mid - a, std::max(1.f, bw - 0.4f), 2 * a + 1);
    }
    auto xAt = [&](int s) { return b.getX() + static_cast<float>(s) / static_cast<float>(len) * b.getWidth(); };
    const int mode = juce::roundToInt(proc.apvts.getRawParameterValue("loopMode")->load());
    if (mode == 0 && led != 0) {   // Burst slices
        const int ns = proc.ui.nSlices.load(), step = proc.ui.step.load();
        for (int i = 0; i < ns; ++i) {
            g.setColour(i == step ? kLedMid : kInk.withAlpha(0.35f));
            g.fillRect(xAt(proc.ui.slices[static_cast<size_t>(i)].load()) - 0.5f, b.getY(), i == step ? 2.f : 1.f, b.getHeight());
        }
    }
    if (led != 0) { g.setColour(kInk.withAlpha(0.25f)); g.fillRect(xAt(proc.ui.seam.load()), b.getY() + 2, 1.f, b.getHeight() - 4); }
    g.setColour(led == 0 ? kLedLoop.withAlpha(0.6f) : kLedLoop);
    g.fillRect(xAt(proc.ui.pos.load()) - 1.5f, b.getY(), 3.f, b.getHeight());
    g.setColour(kMuted); g.setFont(mono(11));
    juce::String tag = led == 0 ? "listening (recording)" : led == 2 ? "overdubbing" : "playing";
    if (mode == 1 && led != 0) {
        const int a = proc.ui.station.load(), s2 = proc.ui.station2.load();
        tag << "  -  " << (s2 < 0 ? juce::String(badmood::kRadioNames[a]) : juce::String(badmood::kRadioNames[a]) + " / " + badmood::kRadioNames[s2] + " (static)");
    } else if (mode == 0 && led != 0) tag << "  -  " << proc.ui.nSlices.load() << " steps" << (proc.ui.scramble.load() ? ", scrambled" : "");
    else if (mode == 2 && led != 0) tag << "  -  mask " << juce::roundToInt(proc.ui.mask.load() * 100) << "%";
    g.drawText(tag, b.reduced(6, 3), juce::Justification::topLeft);
}

// ───────── editor ─────────
BadMoodEditor::BadMoodEditor(BadMoodProcessor& p) : AudioProcessorEditor(p), proc(p), loopView(p) {
    setLookAndFeel(&look);
    auto makeKnob = [this](bool main) {
        Knob k { {}, std::make_unique<juce::Slider>(juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox), nullptr };
        k.slider->setRotaryParameters(juce::degreesToRadians(-135.f), juce::degreesToRadians(135.f), true);
        k.slider->setMouseDragSensitivity(main ? 220 : 160);
        addAndMakeVisible(*k.slider);
        return k;
    };
    for (int i = 0; i < 6; ++i) {
        knobs.push_back(makeKnob(true));
        auto* s = knobs.back().slider.get();
        s->onValueChange = [this, i] { touch(knobs[static_cast<size_t>(i)].id); };
        s->onDragStart = [this, i] {
            // a knob moved while one switch is held: that switch does nothing (pedal "shift")
            if (!proc.ui.hidden.load()) {
                if (fsWet->isDown) proc.shiftFootswitch(0);
                if (fsLoop->isDown) proc.shiftFootswitch(1);
            }
            touch(knobs[static_cast<size_t>(i)].id);
        };
    }
    const char* hiddenPanelIds[8] = {"cross", "balance", "fade", "eq", "glue", "blend", "volume", "rampSpeed"};
    for (const char* id : hiddenPanelIds) {
        Knob k = makeKnob(false);
        k.id = id;
        k.slider->getProperties().set("hidden", true);
        k.slider->setTitle(labelFor(id));
        k.slider->setDoubleClickReturnValue(true, static_cast<double>(proc.apvts.getParameter(id)->convertFrom0to1(proc.apvts.getParameter(id)->getDefaultValue())));
        const juce::String sid = id;
        k.slider->onValueChange = [this, sid] { touch(sid); };
        k.att = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(proc.apvts, id, *k.slider);
        hiddenKnobs.push_back(std::move(k));
    }
    for (const char* id : kHiddenToggles) {
        Combo c { std::make_unique<juce::ComboBox>(), nullptr };
        c.box->addItemList(toggleLabels(id), 1);
        c.box->setTitle(labelFor(id));
        addAndMakeVisible(*c.box);
        c.att = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(proc.apvts, id, *c.box);
        hiddenCombos.push_back(std::move(c));
    }
    auto modeChanged = [this](int i) {
        return [this, i] {
            if (auto* t = toggles[i].get(); t != nullptr && t->param != nullptr)
                showReadout(labelFor(t->param->getParameterID()), t->param->getCurrentValueAsText().toLowerCase());
            repaint(pedalArea);
        };
    };
    for (int i = 0; i < 3; ++i) { toggles[i] = std::make_unique<ThreeWay>(modeChanged(i)); addAndMakeVisible(*toggles[i]); }
    presetToggle = std::make_unique<ThreeWay>([this] { showReadout("PRESET", proc.apvts.getParameter("preset")->getCurrentValueAsText().toLowerCase()); });
    presetToggle->bind(*proc.apvts.getParameter("preset"), toggleLabels("preset"));
    addAndMakeVisible(*presetToggle);
    fsWet = std::make_unique<Footswitch>(*proc.apvts.getParameter("wetSwitch"), "A");
    fsLoop = std::make_unique<Footswitch>(*proc.apvts.getParameter("loopSwitch"), "L");
    addAndMakeVisible(*fsWet); addAndMakeVisible(*fsLoop);
    addAndMakeVisible(loopView);

    sourceBox.addItem("Live input", 1);
    sourceBox.addItem("Demo guitar", 2);
    sourceBox.addItem("Audio file", 3);
    sourceBox.setSelectedId(proc.source.load() + 1, juce::dontSendNotification);
    sourceBox.onChange = [this] {
        const int s = sourceBox.getSelectedId() - 1;
        if (s == BadMoodProcessor::File && proc.fileName().isEmpty()) { loadButton.triggerClick(); return; }
        proc.source = s;
        showReadout("INPUT", sourceBox.getText().toLowerCase());
    };
    addAndMakeVisible(sourceBox);
    loadButton.onClick = [this] {
        chooser = std::make_unique<juce::FileChooser>("Choose a guitar recording", juce::File(), "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg");
        chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this](const juce::FileChooser& fc) {
            const auto f = fc.getResult();
            if (f == juce::File()) { sourceBox.setSelectedId(proc.source.load() + 1, juce::dontSendNotification); return; }
            if (proc.loadFile(f)) { proc.source = BadMoodProcessor::File; sourceBox.setSelectedId(3, juce::dontSendNotification); showReadout("INPUT", f.getFileName()); }
            else showReadout("INPUT", "could not read that file");
        });
    };
    addAndMakeVisible(loadButton);
    inputGain.setSliderStyle(juce::Slider::LinearHorizontal);
    inputGain.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    inputGain.setRange(0, 2); inputGain.setValue(proc.inputGain.load(), juce::dontSendNotification);
    inputGain.setTitle("Input level");
    inputGain.onValueChange = [this] { proc.inputGain = static_cast<float>(inputGain.getValue()); showReadout("INPUT LEVEL", juce::String(juce::Decibels::gainToDecibels(inputGain.getValue(), -60.0), 1) + " dB"); };
    addAndMakeVisible(inputGain);

    const std::pair<const char*, const char*> dipDefs[] = {
        {"miso", "MISO"}, {"spread", "Spread"}, {"dryKill", "Dry kill"}, {"latch", "Latch"}, {"trails", "Trails"}, {"smooth", "Smooth"},
        {"dryGlue", "Dry glue"}, {"half", "Half"},
        {"rampTime", "Time"}, {"rampModWet", "Modify wet"}, {"rampClock", "Clock"}, {"rampModLoop", "Modify loop"},
        {"rampLength", "Length"}, {"bounce", "Bounce"}, {"sweep", "Sweep top"}, {"polarity", "Polarity"}};
    for (const auto& [id, label] : dipDefs) {
        Dip d { id, std::make_unique<juce::ToggleButton>(label), nullptr };
        const juce::String lab = label;
        d.button->onClick = [this, lab, b = d.button.get()] { showReadout("DIP", lab.toLowerCase() + (b->getToggleState() ? " on" : " off")); };
        addAndMakeVisible(*d.button);
        d.att = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(proc.apvts, id, *d.button);
        dips.push_back(std::move(d));
    }
    save1.onClick = [this] { proc.savePreset(0); showReadout("PRESET", "saved to preset 1"); };
    save2.onClick = [this] { proc.savePreset(1); showReadout("PRESET", "saved to preset 2"); };
    resetHidden.onClick = [this] { proc.resetHiddenOptions(); showReadout("HIDDEN OPTIONS", "reset to defaults"); };
    for (auto* b : {&save1, &save2, &resetHidden}) addAndMakeVisible(*b);

    rebind();
    setWantsKeyboardFocus(true);
    setSize(W, H);
    startTimerHz(30);
}

BadMoodEditor::~BadMoodEditor() {
    stopTimer();
    fsWet->press(false); fsLoop->press(false);
    setLookAndFeel(nullptr);
}

juce::String BadMoodEditor::labelFor(const juce::String& id) {
    static const std::pair<const char*, const char*> names[] = {
        {"time", "TIME"}, {"mix", "MIX"}, {"length", "LENGTH"}, {"modWet", "MODIFY (WET)"}, {"clock", "CLOCK"}, {"modLoop", "MODIFY (LOOP)"},
        {"cross", "CROSS"}, {"balance", "BALANCE"}, {"fade", "FADE"}, {"eq", "EQ"}, {"glue", "GLUE"}, {"blend", "BLEND"},
        {"volume", "EFFECT VOLUME"}, {"rampSpeed", "RAMP SPEED"}, {"wetMode", "WET MODE"}, {"routing", "ROUTING"}, {"loopMode", "LOOP MODE"},
        {"sync", "SYNC"}, {"spreadSel", "SPREAD"}, {"inputMod", "INPUT MOD"}, {"preset", "PRESET"}};
    for (const auto& [k, v] : names) if (id == k) return v;
    return id.toUpperCase();
}

juce::String BadMoodEditor::mainKnobId(int i) const {
    if (proc.ui.hidden.load()) return kHiddenMain[i];
    if (i == 1) {
        if (fsLoop->isDown && !fsWet->isDown) return "volume";
        const auto rampOn = [this] { for (auto* id : {"rampTime", "rampModWet", "rampClock", "rampModLoop", "rampLength"}) if (proc.apvts.getRawParameterValue(id)->load() > 0.5f) return true; return false; };
        if (rampOn() && !fsWet->isDown) return "rampSpeed";
    }
    return kMainIds[i];
}

// Point the six knobs and three toggles at what they control right now.
void BadMoodEditor::rebind() {
    juce::String key;
    for (int i = 0; i < 6; ++i) key << mainKnobId(i) << ",";
    const bool hidden = proc.ui.hidden.load();
    key << (hidden ? "H" : "N");
    if (key == boundKey) return;
    boundKey = key;
    for (int i = 0; i < 6; ++i) {
        auto& k = knobs[static_cast<size_t>(i)];
        const juce::String id = mainKnobId(i);
        if (id == k.id) continue;
        k.att.reset();
        k.id = id;
        auto* prm = proc.apvts.getParameter(id);
        k.slider->getProperties().set("hidden", id != kMainIds[i]);
        k.slider->setTitle(labelFor(id));
        k.slider->setDoubleClickReturnValue(true, static_cast<double>(prm->convertFrom0to1(prm->getDefaultValue())));
        k.att = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(proc.apvts, id, *k.slider);
        k.slider->repaint();
    }
    if (hidden != togglesHidden || toggles[0]->param == nullptr) {
        togglesHidden = hidden;
        for (int i = 0; i < 3; ++i) {
            const char* id = hidden ? kHiddenToggles[i] : kToggleIds[i];
            toggles[i]->bind(*proc.apvts.getParameter(id), toggleLabels(id));
        }
    }
    repaint(pedalArea);
}

void BadMoodEditor::touch(const juce::String& id) {
    if (id.isEmpty()) return;
    lastTouched = id;
    showReadout(labelFor(id), proc.describe(id));
    if (id == "clock" || id == "smooth") repaint(pedalArea);
}

void BadMoodEditor::showReadout(const juce::String& label, const juce::String& value) {
    if (label == lcdLabel && value == lcdValue) return;
    lcdLabel = label; lcdValue = value;
    repaint(lcdArea);
}

void BadMoodEditor::resized() {
    pedalArea = {20, 20, 560, 690};
    const int cx[3] = {pedalArea.getX() + 100, pedalArea.getCentreX(), pedalArea.getRight() - 100};
    for (int i = 0; i < 6; ++i) {
        const int row = i / 3;
        knobs[static_cast<size_t>(i)].slider->setBounds(juce::Rectangle<int>(104, 104).withCentre({cx[i % 3], pedalArea.getY() + (row == 0 ? 178 : 410)}));
    }
    const int ty = pedalArea.getY() + 293;
    for (int i = 0; i < 3; ++i) toggles[i]->setBounds(cx[i] - 50, ty - 36, 136, 72);
    const int fy = pedalArea.getBottom() - 62;
    fsWet->setBounds(juce::Rectangle<int>(86, 86).withCentre({cx[0] + 10, fy}));
    fsLoop->setBounds(juce::Rectangle<int>(86, 86).withCentre({cx[2] - 10, fy}));
    presetToggle->setBounds(cx[1] - 34, fy - 36, 130, 72);
    ledWet = juce::Rectangle<int>(14, 14).withCentre({cx[0] + 10, fy - 62});
    ledLoop = juce::Rectangle<int>(14, 14).withCentre({cx[2] - 10, fy - 62});
    ledMid = juce::Rectangle<int>(12, 12).withCentre({cx[1], fy - 62});

    auto side = juce::Rectangle<int>(600, 20, 500, 690);
    lcdArea = side.removeFromTop(84);
    side.removeFromTop(10);
    auto src = side.removeFromTop(100).reduced(14, 10);
    src.removeFromTop(18);
    auto r1 = src.removeFromTop(26);
    sourceBox.setBounds(r1.removeFromLeft(180));
    r1.removeFromLeft(8);
    loadButton.setBounds(r1.removeFromLeft(130));
    src.removeFromTop(6);
    auto r2 = src.removeFromTop(22);
    inputGain.setBounds(r2.removeFromLeft(230).withTrimmedLeft(52));
    meterArea = r2.withTrimmedLeft(16).withHeight(22);
    side.removeFromTop(10);
    auto lv = side.removeFromTop(108).reduced(14, 10);
    lv.removeFromTop(16);
    loopView.setBounds(lv);
    side.removeFromTop(10);
    dipArea = side.removeFromTop(162);
    auto da = dipArea.reduced(14, 8);
    da.removeFromTop(16);
    for (int row = 0; row < 2; ++row) {
        auto rr = da.removeFromTop(66);
        rr.removeFromLeft(64);
        const int dw = rr.getWidth() / 8;
        for (int i = 0; i < 8; ++i) dips[static_cast<size_t>(row * 8 + i)].button->setBounds(rr.removeFromLeft(dw));
    }
    side.removeFromTop(10);
    hiddenArea = side.removeFromTop(150);
    auto ha = hiddenArea.reduced(14, 8);
    ha.removeFromTop(16);
    auto hk = ha.removeFromTop(64);
    const int kw = hk.getWidth() / 8;
    for (auto& k : hiddenKnobs) k.slider->setBounds(hk.removeFromLeft(kw).withTrimmedBottom(12));
    ha.removeFromTop(6);
    auto hc = ha.removeFromTop(26);
    for (auto& c : hiddenCombos) { auto cell = hc.removeFromLeft(hc.getWidth() / std::max(1, 3 - static_cast<int>(&c - hiddenCombos.data()))); c.box->setBounds(cell.withTrimmedLeft(62).withTrimmedRight(6)); }
    side.removeFromTop(10);
    presetArea = side.removeFromTop(44);
    auto pa = presetArea.reduced(10, 8);
    const int bw = (pa.getWidth() - 16) / 3;
    save1.setBounds(pa.removeFromLeft(bw)); pa.removeFromLeft(8);
    save2.setBounds(pa.removeFromLeft(bw)); pa.removeFromLeft(8);
    resetHidden.setBounds(pa);
}

void BadMoodEditor::paint(juce::Graphics& g) {
    g.fillAll(kBoard);
    // ── pedal enclosure ──
    const auto pa = pedalArea.toFloat();
    g.setColour(juce::Colours::black.withAlpha(0.5f));
    g.fillRoundedRectangle(pa.translated(0, 8), 14);
    g.setGradientFill(juce::ColourGradient(kEnclHi, pa.getX() + pa.getWidth() * 0.3f, pa.getY(), kEnclLo, pa.getX() + pa.getWidth() * 0.5f, pa.getBottom(), false));
    g.fillRoundedRectangle(pa, 14);
    for (auto pt : {pa.getTopLeft().translated(15, 15), pa.getTopRight().translated(-15, 15), pa.getBottomLeft().translated(15, -15), pa.getBottomRight().translated(-15, -15)}) {
        g.setColour(juce::Colour(0xff9a9b96)); g.fillEllipse(juce::Rectangle<float>(12, 12).withCentre(pt));
        g.setColour(juce::Colour(0xff555555)); g.drawLine(pt.x - 4, pt.y + 2, pt.x + 4, pt.y - 2, 2);
    }
    const bool hidden = proc.ui.hidden.load();
    g.setColour(kInk);
    g.setFont(font(46, true));
    g.drawText("BAD MOOD", pedalArea.getX() + 40, pedalArea.getY() + 24, 330, 52, juce::Justification::centredLeft);
    g.setColour(hidden ? kLedMid : kInkDim); g.setFont(font(11.5f, true));
    g.drawText(hidden ? "HIDDEN OPTIONS  -  release both switches to return" : "WET CHANNEL  -  MICRO-LOOPER  -  EMULATOR",
               pedalArea.getX() + 40, pedalArea.getY() + 76, 460, 16, juce::Justification::centredLeft);

    const int wm = juce::roundToInt(proc.apvts.getRawParameterValue("wetMode")->load());
    const int lm = juce::roundToInt(proc.apvts.getRawParameterValue("loopMode")->load());
    const char* wetSub[3][2] = {{"decay", "character"}, {"delay time", "repeats"}, {"lag", "harmony"}};
    const char* loopSub[3][2] = {{"step size", "sensitivity"}, {"station dependent", "scan"}, {"character", "threshold"}};
    for (size_t i = 0; i < knobs.size(); ++i) {
        const auto kb = knobs[i].slider->getBounds();
        const juce::String id = knobs[i].id;
        const bool alt = id != kMainIds[i];
        g.setColour(alt ? kLedMid : kInk); g.setFont(font(14, true));
        g.drawText(alt ? labelFor(id) : juce::String(i == 3 || i == 5 ? "MODIFY" : labelFor(id)), kb.withY(kb.getY() - 20).withHeight(18), juce::Justification::centred);
        juce::String sub;
        if (alt) sub = "hidden option";
        else if (i == 0) sub = wetSub[wm][0];
        else if (i == 3) sub = juce::String(wetSub[wm][1]) + "  -  wet";
        else if (i == 2) sub = loopSub[lm][0];
        else if (i == 5) sub = juce::String(loopSub[lm][1]) + "  -  loop";
        else if (i == 1) sub = "dry / fx";
        else sub = proc.apvts.getRawParameterValue("smooth")->load() > 0.5f ? "sample rate (smooth)" : "sample rate (stepped)";
        g.setColour(kInkDim); g.setFont(font(11.5f));
        g.drawText(sub.toUpperCase(), kb.withY(kb.getBottom() - 4).withHeight(16).withSizeKeepingCentre(180, 16), juce::Justification::centred);
    }
    g.setColour(kInk); g.setFont(font(13, true));
    g.drawText("WET", ledWet.getX() - 62, ledWet.getY() - 2, 54, 18, juce::Justification::centredRight);
    g.drawText("LOOP", ledLoop.getRight() + 8, ledLoop.getY() - 2, 54, 18, juce::Justification::centredLeft);

    auto led = [&](juce::Rectangle<int> r, bool on, juce::Colour c) {
        if (on) { g.setColour(c.withAlpha(0.35f)); g.fillEllipse(r.toFloat().expanded(6)); }
        g.setColour(on ? c : juce::Colour(0xff3a2428)); g.fillEllipse(r.toFloat());
    };
    led(ledWet, ledWetOn, kLedWet);
    led(ledLoop, ledLoopOn, kLedLoop);
    led(ledMid, ledMidState != 0, ledMidState == 2 ? kLedLoop : kLedMid);

    // ── side panels ──
    auto panel = [&](juce::Rectangle<int> r, const char* title) {
        g.setColour(kPanel); g.fillRoundedRectangle(r.toFloat(), 8);
        g.setColour(kPanelLine); g.drawRoundedRectangle(r.toFloat().reduced(0.5f), 8, 1);
        g.setColour(kMuted); g.setFont(font(12, true));
        g.drawText(title, r.reduced(14, 8).removeFromTop(14), juce::Justification::centredLeft);
    };
    panel({600, 114, 500, 100}, "INPUT");
    panel({600, 224, 500, 108}, "MICRO-LOOP BUFFER");
    panel(dipArea, "DIP SWITCHES");
    panel(hiddenArea, "HIDDEN OPTIONS  (or hold both footswitches)");
    g.setColour(kMuted); g.setFont(font(11, true));
    g.drawText("CUSTOMIZE", dipArea.getX() + 14, dipArea.getY() + 30, 60, 30, juce::Justification::centredLeft);
    g.drawText("RAMP", dipArea.getX() + 14, dipArea.getY() + 96, 60, 30, juce::Justification::centredLeft);
    for (auto& k : hiddenKnobs) {
        const auto kb = k.slider->getBounds();
        g.drawFittedText(labelFor(k.id).replace("EFFECT ", ""), kb.getX() - 4, kb.getBottom() - 2, kb.getWidth() + 8, 14, juce::Justification::centred, 1, 0.7f);
    }
    for (size_t i = 0; i < hiddenCombos.size(); ++i) {
        const auto cb = hiddenCombos[i].box->getBounds();
        g.drawText(labelFor(kHiddenToggles[i]), cb.getX() - 60, cb.getY(), 58, cb.getHeight(), juce::Justification::centredRight);
    }
    g.setColour(kText); g.setFont(font(13, true));
    g.drawText("Level", 614, inputGain.getY(), 50, inputGain.getHeight(), juce::Justification::centredLeft);
    g.setColour(kMuted); g.setFont(font(11.5f));
    g.drawFittedText("Unofficial emulation of Chase Bliss Audio's BAD MOOD for study and fun. Not affiliated with or endorsed by Chase Bliss; BAD MOOD is their trademark.",
                     20, H - 26, W - 40, 20, juce::Justification::centred, 1);

    // meters
    auto bar = [&](int y, const char* name, float pk) {
        g.setColour(kMuted); g.setFont(mono(10.5f));
        g.drawText(name, meterArea.getX(), y, 32, 8, juce::Justification::centredLeft);
        const auto track = juce::Rectangle<float>(static_cast<float>(meterArea.getX() + 34), static_cast<float>(y + 1), static_cast<float>(meterArea.getWidth() - 34), 6);
        g.setColour(juce::Colour(0xff151317)); g.fillRoundedRectangle(track, 3);
        const float db = juce::jlimit(0.f, 1.f, (juce::Decibels::gainToDecibels(pk, -60.f) + 60.f) / 60.f);
        g.setColour(db > 0.9f ? juce::Colour(0xffe5584a) : db > 0.7f ? juce::Colour(0xffe8c34c) : juce::Colour(0xff6fbf87));
        g.fillRoundedRectangle(track.withWidth(track.getWidth() * db), 3);
    };
    bar(meterArea.getY(), "IN", meterIn);
    bar(meterArea.getY() + 11, "OUT", meterOut);

    // LCD readout
    const auto la = lcdArea.toFloat();
    g.setColour(kLcdBg); g.fillRoundedRectangle(la, 8);
    g.setColour(juce::Colour(0xff0a080c)); g.drawRoundedRectangle(la.reduced(1), 8, 2);
    auto lr = lcdArea.reduced(16, 10);
    g.setColour(kLcdFg.withAlpha(0.7f)); g.setFont(mono(12));
    g.drawText(lcdLabel, lr.removeFromTop(16), juce::Justification::centredLeft);
    g.setColour(kLcdFg); g.setFont(mono(19));
    g.drawFittedText(lcdValue, lr.removeFromTop(30), juce::Justification::centredLeft, 1, 0.8f);
    g.setColour(kLcdFg.withAlpha(0.7f)); g.setFont(mono(12));
    g.drawText(lcdMeta, lr, juce::Justification::centredLeft);
}

void BadMoodEditor::timerCallback() {
    auto& ui = proc.ui;
    const double now = juce::Time::getMillisecondCounterHiRes();
    blink = static_cast<int>(now / 160.0) % 2 == 0;
    rebind();
    const bool tb = ui.trueBypass.load(), hidden = ui.hidden.load();
    if (hidden != lastHidden) { lastHidden = hidden; showReadout("HIDDEN OPTIONS", hidden ? "knobs and toggles now set hidden options" : "back to the main controls"); }
    if (tb != lastTb) { lastTb = tb; showReadout("TRUE BYPASS", tb ? "on (tap either switch to leave)" : "off"); }
    const uint32_t cycles = ui.cycles.load();
    if (cycles != lastCycles) { lastCycles = cycles; flashUntil = now + 120; }   // recording: blink once per loop length
    const uint32_t saved = ui.presetSaved.load();
    if (saved != lastSaved) { lastSaved = saved; savedUntil = now + 900; showReadout("PRESET", "saved"); }
    const int led = ui.loopLed.load();   // 0 rec (listening), 1 play, 2 dub
    const bool wetOn = tb ? blink : ui.wetOn.load() && !(ui.freeze.load() && !blink);
    const bool loopOn = tb ? blink : led == 1 || (led == 2 && blink) || (led == 0 && now < flashUntil);
    const int mid = tb ? (blink ? 2 : 0) : hidden ? 1 : now < savedUntil ? (blink ? 1 : 0) : 0;
    if (wetOn != ledWetOn) { ledWetOn = wetOn; repaint(ledWet.expanded(8)); }
    if (loopOn != ledLoopOn) { ledLoopOn = loopOn; repaint(ledLoop.expanded(8)); }
    if (mid != ledMidState) { ledMidState = mid; repaint(ledMid.expanded(8)); }
    if (led != lastLoopLed) {
        const char* msg[] = {"listening (tap to play)", "playing", "overdubbing..."};
        if (lastLoopLed >= 0) showReadout("MICRO-LOOPER", msg[juce::jlimit(0, 2, led)]);
        lastLoopLed = led;
    }
    const float mi = ui.peakIn.load(), mo = ui.peakOut.load();
    if (std::fabs(mi - meterIn) > 1e-3f || std::fabs(mo - meterOut) > 1e-3f) { meterIn = mi; meterOut = mo; repaint(meterArea.expanded(2)); }
    const float hz = ui.clockHz.load();
    const juce::String meta = juce::String(hz / 1000.f, 1) + " kHz    loop " + juce::String(static_cast<float>(ui.len.load()) / hz, 2) + " s"
        + "    cpu " + juce::String(ui.load.load(), 1) + "%";
    if (meta != lcdMeta) { lcdMeta = meta; repaint(lcdArea); }
    if (lastTouched.isNotEmpty() && (lcdLabel == labelFor(lastTouched))) {
        const juce::String v = proc.describe(lastTouched);   // follows automation and mode changes
        if (v != lcdValue) { lcdValue = v; repaint(lcdArea); }
    }
    const uint32_t ver = ui.waveVersion.load();
    if (ver != loopView.version && proc.copyWave(loopView.wave)) loopView.version = ver;
    loopView.repaint();
}

bool BadMoodEditor::keyPressed(const juce::KeyPress& k) {
    const auto c = juce::CharacterFunctions::toLowerCase(k.getTextCharacter());
    return c == 'a' || c == 'l';   // handled in keyStateChanged so holds work
}

bool BadMoodEditor::keyStateChanged(bool) {
    const bool a = juce::KeyPress::isKeyCurrentlyDown('a') || juce::KeyPress::isKeyCurrentlyDown('A');
    const bool l = juce::KeyPress::isKeyCurrentlyDown('l') || juce::KeyPress::isKeyCurrentlyDown('L');
    bool used = false;
    if (a != keyA) { keyA = a; fsWet->press(a); used = true; }
    if (l != keyL) { keyL = l; fsLoop->press(l); used = true; }
    return used;
}
