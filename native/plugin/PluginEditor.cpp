#include "PluginEditor.h"

namespace {
// palette shared with the web version (src/style.css)
const juce::Colour kBoard { 0xff17191a }, kEncl { 0xff4f7d72 }, kEnclHi { 0xff6a9a8d }, kEnclLo { 0xff345a51 };
const juce::Colour kInk { 0xfff1e8d4 }, kInkDim { 0xffc9c0aa }, kPanel { 0xff222627 }, kPanelLine { 0xff33393a };
const juce::Colour kText { 0xffddd6c6 }, kMuted { 0xff8f9692 }, kKnob { 0xffefe6d0 }, kKnobShade { 0xffb9ae95 };
const juce::Colour kLedWet { 0xffffb347 }, kLedLoop { 0xffff5a4a }, kAccent { 0xffe8a94c };
const juce::Colour kLcdBg { 0xff0f1a14 }, kLcdFg { 0xff9fe3b0 };

juce::Font font(float size, bool bold = false) {
    return juce::Font(juce::FontOptions(size).withStyle(bold ? "Bold" : "Regular"));
}
juce::Font mono(float size) { return juce::Font(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), size, juce::Font::plain)); }

constexpr int W = 900, H = 640;
const char* kKnobSub[6][3] = {
    {"decay", "delay", "refresh"}, {"dry / fx", "dry / fx", "dry / fx"}, {"slice", "loop length", "slice"},
    {"smear", "feedback", "speed"}, {"sample rate", "sample rate", "sample rate"}, {"sensitivity", "speed", "stretch"},
};
}  // namespace

// ───────── look and feel ─────────
MoodEditor::KnobLook::KnobLook() {
    setColour(juce::ComboBox::backgroundColourId, kPanel.brighter(0.08f));
    setColour(juce::ComboBox::outlineColourId, kPanelLine);
    setColour(juce::ComboBox::textColourId, kText);
    setColour(juce::PopupMenu::backgroundColourId, kPanel);
    setColour(juce::TextButton::buttonColourId, kAccent);
    setColour(juce::TextButton::textColourOffId, juce::Colour(0xff1d1608));
    setColour(juce::Slider::trackColourId, kAccent);
    setColour(juce::Slider::backgroundColourId, juce::Colour(0xff151818));
    setColour(juce::Slider::thumbColourId, kAccent);
}

void MoodEditor::KnobLook::drawRotarySlider(juce::Graphics& g, int x, int y, int w, int h, float pos, float a0, float a1, juce::Slider&) {
    const auto area = juce::Rectangle<float>(static_cast<float>(x), static_cast<float>(y), static_cast<float>(w), static_cast<float>(h)).reduced(10);
    const float r = std::min(area.getWidth(), area.getHeight()) / 2;
    const auto c = area.getCentre();
    g.setColour(kInk.withAlpha(0.55f));   // ticks
    for (int i = 0; i <= 10; ++i) {
        const float a = a0 + (a1 - a0) * i / 10.f;
        const float r1 = r + 4, r2 = r + (i % 5 == 0 ? 9.f : 7.f);
        g.drawLine(c.x + r1 * std::sin(a), c.y - r1 * std::cos(a), c.x + r2 * std::sin(a), c.y - r2 * std::cos(a), 1.4f);
    }
    g.setColour(juce::Colours::black.withAlpha(0.35f));
    g.fillEllipse(juce::Rectangle<float>(r * 2, r * 2).withCentre(c.translated(0, 3)));
    g.setGradientFill(juce::ColourGradient(juce::Colour(0xfffffaf0), c.x - r * 0.3f, c.y - r * 0.4f, kKnobShade, c.x + r, c.y + r, true));
    g.fillEllipse(juce::Rectangle<float>(r * 2, r * 2).withCentre(c));
    const float a = a0 + pos * (a1 - a0);
    juce::Path p;
    p.addRoundedRectangle(-2.f, -r * 0.86f, 4.f, r * 0.68f, 2.f);
    g.setColour(juce::Colour(0xff2a2a28));
    g.fillPath(p, juce::AffineTransform::rotation(a).translated(c));
}

void MoodEditor::KnobLook::drawToggleButton(juce::Graphics& g, juce::ToggleButton& b, bool, bool) {
    // red DIP switch with a white slider; label underneath
    const auto bounds = b.getLocalBounds().toFloat();
    const auto sw = juce::Rectangle<float>(18, 34).withCentre({bounds.getCentreX(), 20});
    g.setColour(juce::Colour(0xffb33a2f)); g.fillRoundedRectangle(sw, 3);
    g.setColour(juce::Colour(0xff7d2219)); g.drawRoundedRectangle(sw, 3, 2);
    const auto knob = juce::Rectangle<float>(12, 13).withCentre({sw.getCentreX(), b.getToggleState() ? sw.getY() + 9.5f : sw.getBottom() - 9.5f});
    g.setColour(juce::Colour(0xfff2eee6)); g.fillRoundedRectangle(knob, 2);
    if (b.hasKeyboardFocus(true)) { g.setColour(kAccent); g.drawRoundedRectangle(sw.expanded(3), 4, 1.5f); }
    g.setColour(kMuted); g.setFont(font(11.5f, true));
    g.drawText(b.getButtonText().toUpperCase(), bounds.withTop(42), juce::Justification::centredTop);
}

// ───────── 3-way toggle ─────────
MoodEditor::ThreeWay::ThreeWay(juce::RangedAudioParameter& p, juce::StringArray l, std::function<void()> onChange)
    : param(p), labels(std::move(l)), attach(p, [this](float v) { index = juce::roundToInt(v); repaint(); if (changed) changed(); }), changed(std::move(onChange)) {
    attach.sendInitialUpdate();
}
void MoodEditor::ThreeWay::paint(juce::Graphics& g) {
    const auto lever = juce::Rectangle<float>(0, getHeight() / 2.f - 32, 22, 64);
    g.setGradientFill(juce::ColourGradient::horizontal(juce::Colour(0xff2b2f2f), juce::Colour(0xff2b2f2f), lever));
    g.setColour(juce::Colour(0xff3a3f3f)); g.fillRoundedRectangle(lever, 11);
    const auto bat = juce::Rectangle<float>(16, 22).withPosition(lever.getX() + 3, lever.getY() + 3 + index * 18.f);
    g.setGradientFill(juce::ColourGradient(juce::Colours::white, bat.getX(), bat.getY(), juce::Colour(0xff8d8d88), bat.getRight(), bat.getBottom(), false));
    g.fillRoundedRectangle(bat, 8);
    for (int i = 0; i < 3; ++i) {
        const auto r = juce::Rectangle<float>(30, lever.getY() + 2 + i * 20.f, static_cast<float>(getWidth() - 30), 20);
        g.setColour(i == index ? kInk : kInkDim.withAlpha(0.75f));
        g.setFont(font(12.5f, i == index));
        g.drawText(labels[i].toUpperCase(), r, juce::Justification::centredLeft);
        if (i == index) g.fillRect(r.getX(), r.getBottom() - 3, juce::GlyphArrangement::getStringWidth(font(12.5f, true), labels[i].toUpperCase()), 1.f);
    }
}
void MoodEditor::ThreeWay::mouseDown(const juce::MouseEvent& e) {
    int next;
    if (e.x < 26) next = (index + 1) % 3;   // click the lever to cycle
    else next = juce::jlimit(0, 2, (e.y - (getHeight() / 2 - 32) - 2) / 20);
    attach.setValueAsCompleteGesture(static_cast<float>(next));
}

// ───────── footswitch ─────────
void MoodEditor::Footswitch::press(bool down) {
    if (down == isDown) return;
    isDown = down;
    if (down) { param.beginChangeGesture(); param.setValueNotifyingHost(1.f); }
    else { param.setValueNotifyingHost(0.f); param.endChangeGesture(); }
    repaint();
}
void MoodEditor::Footswitch::paint(juce::Graphics& g) {
    auto r = getLocalBounds().toFloat().reduced(4);
    g.setColour(juce::Colour(0xff555b59)); g.fillEllipse(r);
    g.setColour(juce::Colour(0xff2c302f)); g.fillEllipse(r.reduced(2));
    auto cap = r.reduced(9).translated(0, isDown ? 2.f : 0.f);
    if (isDown) cap = cap.reduced(1);
    g.setGradientFill(juce::ColourGradient(juce::Colour(0xfff4f4f0), cap.getX() + cap.getWidth() * 0.35f, cap.getY() + cap.getHeight() * 0.3f,
                                           juce::Colour(0xff7c7e79), cap.getRight(), cap.getBottom(), true));
    g.fillEllipse(cap);
    g.setColour(juce::Colour(0xff55574f)); g.setFont(mono(13));
    g.drawText(keyName, cap, juce::Justification::centred);
}

// ───────── loop display ─────────
void MoodEditor::LoopView::paint(juce::Graphics& g) {
    const auto b = getLocalBounds().toFloat();
    g.setColour(juce::Colour(0xff151818)); g.fillRoundedRectangle(b, 6);
    const int lsrc = proc.ui.lsrc.load();
    if (lsrc <= 0) {
        g.setColour(juce::Colour(0xff5d6562)); g.setFont(mono(13));
        g.drawText("no loop captured yet", b, juce::Justification::centred);
        return;
    }
    const float ws = proc.ui.winStart.load() / static_cast<float>(lsrc);
    g.setColour(kAccent.withAlpha(0.14f));
    g.fillRect(b.withLeft(b.getX() + ws * b.getWidth()));
    const int n = MoodProcessor::kWaveBins;
    const float bw = b.getWidth() / n, mid = b.getCentreY();
    for (int i = 0; i < n; ++i) {
        const float a = std::min(1.f, wave[static_cast<size_t>(i)] * 1.6f) * (b.getHeight() / 2 - 4);
        g.setColour(i / static_cast<float>(n) >= ws ? kAccent : juce::Colour(0xff4c5552));
        g.fillRect(b.getX() + i * bw, mid - a, std::max(1.f, bw - 0.4f), 2 * a + 1);
    }
    const float px = b.getX() + (proc.ui.winStart.load() + proc.ui.pos.load()) / lsrc * b.getWidth();
    g.setColour(proc.ui.loopLed.load() == 0 ? juce::Colour(0xff5d6562) : kLedLoop);
    g.fillRect(px - 1.5f, b.getY(), 3.f, b.getHeight());
}

// ───────── editor ─────────
MoodEditor::MoodEditor(MoodProcessor& p) : AudioProcessorEditor(p), proc(p), loopView(p) {
    setLookAndFeel(&look);
    const std::pair<const char*, const char*> defs[] = {
        {"time", "TIME"}, {"mix", "MIX"}, {"length", "LENGTH"}, {"modWet", "MODIFY"}, {"clock", "CLOCK"}, {"modLoop", "MODIFY"}};
    for (const auto& [id, label] : defs) {
        Knob k { id, label, std::make_unique<juce::Slider>(juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox), nullptr };
        k.slider->setRotaryParameters(juce::degreesToRadians(-135.f), juce::degreesToRadians(135.f), true);
        k.slider->setMouseDragSensitivity(220);
        k.slider->setDoubleClickReturnValue(true, static_cast<double>(proc.apvts.getParameter(id)->getDefaultValue()));
        k.slider->setTitle(juce::String(label) + (juce::String(id) == "modWet" ? " (wet)" : juce::String(id) == "modLoop" ? " (loop)" : ""));
        const juce::String sid = id;
        k.slider->onValueChange = [this, sid] { touch(sid); };
        k.slider->onDragStart = [this, sid] { touch(sid); };
        addAndMakeVisible(*k.slider);
        k.att = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(proc.apvts, id, *k.slider);
        knobs.push_back(std::move(k));
    }
    auto modeChanged = [this](const char* label, const char* id) {
        return [this, label, id] {
            auto* prm = proc.apvts.getParameter(id);
            if (lastTouched.isNotEmpty() || isVisible()) showReadout(label, prm->getCurrentValueAsText().toLowerCase());
            repaint(pedalArea);
        };
    };
    wetMode = std::make_unique<ThreeWay>(*proc.apvts.getParameter("wetMode"), juce::StringArray{"Reverb", "Delay", "Slip"}, modeChanged("WET MODE", "wetMode"));
    routing = std::make_unique<ThreeWay>(*proc.apvts.getParameter("routing"), juce::StringArray{"In", "In + Loop", "Loop"}, modeChanged("ROUTING", "routing"));
    loopMode = std::make_unique<ThreeWay>(*proc.apvts.getParameter("loopMode"), juce::StringArray{"Env", "Tape", "Stretch"}, modeChanged("LOOP MODE", "loopMode"));
    for (auto* t : {wetMode.get(), routing.get(), loopMode.get()}) addAndMakeVisible(*t);
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
        if (s == MoodProcessor::File && proc.fileName().isEmpty()) { loadButton.triggerClick(); return; }
        proc.source = s;
        showReadout("INPUT", sourceBox.getText().toLowerCase());
    };
    addAndMakeVisible(sourceBox);
    loadButton.onClick = [this] {
        chooser = std::make_unique<juce::FileChooser>("Choose a guitar recording", juce::File(), "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg");
        chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this](const juce::FileChooser& fc) {
            const auto f = fc.getResult();
            if (f == juce::File()) { sourceBox.setSelectedId(proc.source.load() + 1, juce::dontSendNotification); return; }
            if (proc.loadFile(f)) { proc.source = MoodProcessor::File; sourceBox.setSelectedId(3, juce::dontSendNotification); showReadout("INPUT", f.getFileName()); }
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
    balance.setSliderStyle(juce::Slider::LinearHorizontal);
    balance.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    balance.setTitle("Balance");
    balance.onValueChange = [this] { touch("balance"); };
    addAndMakeVisible(balance);
    balanceAtt = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(proc.apvts, "balance", balance);
    const std::pair<const char*, const char*> dipDefs[] = {{"trails", "Trails"}, {"dryKill", "Dry kill"}, {"latch", "Latch"}, {"spread", "Spread"}, {"smooth", "Smooth"}};
    for (const auto& [id, label] : dipDefs) {
        Dip d { std::make_unique<juce::ToggleButton>(label), nullptr };
        const juce::String lab = label;
        d.button->onClick = [this, lab, b = d.button.get()] { showReadout("DIP", lab.toLowerCase() + (b->getToggleState() ? " on" : " off")); };
        addAndMakeVisible(*d.button);
        d.att = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(proc.apvts, id, *d.button);
        dips.push_back(std::move(d));
    }
    setWantsKeyboardFocus(true);
    setSize(W, H);
    startTimerHz(30);
}

MoodEditor::~MoodEditor() {
    stopTimer();
    fsWet->press(false); fsLoop->press(false);
    setLookAndFeel(nullptr);
}

void MoodEditor::touch(const juce::String& id) {
    lastTouched = id;
    const juce::String label = id == "modWet" ? "MODIFY (WET)" : id == "modLoop" ? "MODIFY (LOOP)" : id.toUpperCase();
    showReadout(label, proc.describe(id));
    if (id == "clock") repaint(pedalArea);
}

void MoodEditor::showReadout(const juce::String& label, const juce::String& value) {
    if (label == lcdLabel && value == lcdValue) return;
    lcdLabel = label; lcdValue = value;
    repaint(lcdArea);
}

void MoodEditor::resized() {
    pedalArea = {20, 20, 520, 600};
    const int cx[3] = {pedalArea.getX() + 95, pedalArea.getCentreX(), pedalArea.getRight() - 95};
    for (int i = 0; i < 6; ++i) {
        const int row = i / 3;
        knobs[static_cast<size_t>(i)].slider->setBounds(juce::Rectangle<int>(104, 104).withCentre({cx[i % 3], pedalArea.getY() + (row == 0 ? 172 : 394)}));
    }
    const int ty = pedalArea.getY() + 282;
    wetMode->setBounds(cx[0] - 48, ty - 36, 130, 72);
    routing->setBounds(cx[1] - 48, ty - 36, 130, 72);
    loopMode->setBounds(cx[2] - 48, ty - 36, 130, 72);
    const int fy = pedalArea.getBottom() - 52;
    fsWet->setBounds(juce::Rectangle<int>(84, 84).withCentre({cx[0] + 20, fy}));
    fsLoop->setBounds(juce::Rectangle<int>(84, 84).withCentre({cx[2] - 20, fy}));
    ledWet = juce::Rectangle<int>(14, 14).withCentre({cx[0] + 20, fy - 56});
    ledLoop = juce::Rectangle<int>(14, 14).withCentre({cx[2] - 20, fy - 56});

    auto side = juce::Rectangle<int>(560, 20, 320, 600);
    lcdArea = side.removeFromTop(92);
    side.removeFromTop(14);
    auto src = side.removeFromTop(118).reduced(14, 10);
    src.removeFromTop(18);
    auto r1 = src.removeFromTop(28);
    sourceBox.setBounds(r1.removeFromLeft(170));
    r1.removeFromLeft(8);
    loadButton.setBounds(r1);
    src.removeFromTop(8);
    inputGain.setBounds(src.removeFromTop(22).withTrimmedLeft(52));
    meterArea = src.removeFromTop(22).translated(0, 2);
    side.removeFromTop(14);
    auto lv = side.removeFromTop(118).reduced(14, 10);
    lv.removeFromTop(18);
    loopView.setBounds(lv);
    side.removeFromTop(14);
    auto dipArea = side.removeFromTop(132).reduced(14, 10);
    dipArea.removeFromTop(18);
    auto dipRow = dipArea.removeFromTop(64);
    const int dw = dipRow.getWidth() / 5;
    for (auto& d : dips) d.button->setBounds(dipRow.removeFromLeft(dw));
    dipArea.removeFromTop(4);
    balance.setBounds(dipArea.removeFromTop(24).withTrimmedLeft(66).withTrimmedRight(34));
}

void MoodEditor::paint(juce::Graphics& g) {
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
    g.setColour(kInk);
    g.setFont(font(52, true));
    g.drawText("MOOD", pedalArea.getX() + 40, pedalArea.getY() + 22, 220, 56, juce::Justification::centredLeft);
    g.setColour(kInkDim); g.setFont(font(11.5f, true));
    g.drawText("WET CHANNEL  -  MICRO-LOOPER  -  EMULATOR", pedalArea.getX() + 40, pedalArea.getY() + 78, 440, 16, juce::Justification::centredLeft);

    const int wm = juce::roundToInt(proc.apvts.getRawParameterValue("wetMode")->load());
    const int lm = juce::roundToInt(proc.apvts.getRawParameterValue("loopMode")->load());
    for (size_t i = 0; i < knobs.size(); ++i) {
        const auto kb = knobs[i].slider->getBounds();
        g.setColour(kInk); g.setFont(font(14, true));
        g.drawText(knobs[i].label, kb.withY(kb.getY() - 20).withHeight(18), juce::Justification::centred);
        g.setColour(kInkDim); g.setFont(font(11.5f));
        const int mode = i == 5 || i == 2 ? lm : wm;
        juce::String sub = kKnobSub[i][mode];
        if (i == 3) sub += "  -  wet";
        if (i == 5) sub += "  -  loop";
        g.drawText(sub.toUpperCase(), kb.withY(kb.getBottom() - 4).withHeight(16).withSizeKeepingCentre(170, 16), juce::Justification::centred);
    }
    g.setColour(kInk); g.setFont(font(14, true));
    g.drawText("WET", ledWet.getX() - 70, ledWet.getY() - 2, 60, 18, juce::Justification::centredRight);
    g.drawText("LOOP", ledLoop.getRight() + 10, ledLoop.getY() - 2, 60, 18, juce::Justification::centredLeft);

    auto led = [&](juce::Rectangle<int> r, bool on, juce::Colour c) {
        if (on) { g.setColour(c.withAlpha(0.35f)); g.fillEllipse(r.toFloat().expanded(6)); }
        g.setColour(on ? c : juce::Colour(0xff3a2420)); g.fillEllipse(r.toFloat());
    };
    led(ledWet, ledWetOn, kLedWet);
    led(ledLoop, ledLoopOn, kLedLoop);

    // ── side panels ──
    auto panel = [&](juce::Rectangle<int> r, const char* title) {
        g.setColour(kPanel); g.fillRoundedRectangle(r.toFloat(), 8);
        g.setColour(kPanelLine); g.drawRoundedRectangle(r.toFloat().reduced(0.5f), 8, 1);
        g.setColour(kMuted); g.setFont(font(12, true));
        g.drawText(title, r.reduced(14, 10).removeFromTop(14), juce::Justification::centredLeft);
    };
    panel({560, 126, 320, 118}, "INPUT");
    panel({560, 258, 320, 118}, "LOOP BUFFER");
    panel({560, 390, 320, 132}, "DIP SWITCHES");
    g.setColour(kText); g.setFont(font(13, true));
    g.drawText("Level", 574, inputGain.getY(), 50, inputGain.getHeight(), juce::Justification::centredLeft);
    g.drawText("Balance", 574, balance.getY(), 64, balance.getHeight(), juce::Justification::centredLeft);
    g.setColour(kMuted); g.setFont(font(12));
    g.drawText("wet", balance.getX() - 26, balance.getY(), 24, balance.getHeight(), juce::Justification::centredRight);
    g.drawText("loop", balance.getRight() + 4, balance.getY(), 34, balance.getHeight(), juce::Justification::centredLeft);
    g.drawFittedText("Unofficial emulation for study and fun. Not affiliated with or endorsed by Chase Bliss.",
                     560, 540, 320, 36, juce::Justification::topLeft, 2);

    // meters
    auto bar = [&](int y, const char* name, float pk) {
        g.setColour(kMuted); g.setFont(mono(10.5f));
        g.drawText(name, meterArea.getX(), y, 32, 8, juce::Justification::centredLeft);
        const auto track = juce::Rectangle<float>(static_cast<float>(meterArea.getX() + 34), static_cast<float>(y + 1), static_cast<float>(meterArea.getWidth() - 34), 6);
        g.setColour(juce::Colour(0xff151818)); g.fillRoundedRectangle(track, 3);
        const float db = juce::jlimit(0.f, 1.f, (juce::Decibels::gainToDecibels(pk, -60.f) + 60.f) / 60.f);
        g.setColour(db > 0.9f ? juce::Colour(0xffe5584a) : db > 0.7f ? juce::Colour(0xffe8c34c) : juce::Colour(0xff6fbf87));
        g.fillRoundedRectangle(track.withWidth(track.getWidth() * db), 3);
    };
    bar(meterArea.getY(), "IN", meterIn);
    bar(meterArea.getY() + 11, "OUT", meterOut);

    // LCD readout
    const auto la = lcdArea.toFloat();
    g.setColour(kLcdBg); g.fillRoundedRectangle(la, 8);
    g.setColour(juce::Colour(0xff0a0f0c)); g.drawRoundedRectangle(la.reduced(1), 8, 2);
    auto lr = lcdArea.reduced(16, 12);
    g.setColour(kLcdFg.withAlpha(0.7f)); g.setFont(mono(12));
    g.drawText(lcdLabel, lr.removeFromTop(16), juce::Justification::centredLeft);
    g.setColour(kLcdFg); g.setFont(mono(19));
    g.drawFittedText(lcdValue, lr.removeFromTop(30), juce::Justification::centredLeft, 1, 0.8f);
    g.setColour(kLcdFg.withAlpha(0.7f)); g.setFont(mono(12));
    g.drawText(lcdMeta, lr, juce::Justification::centredLeft);
}

void MoodEditor::timerCallback() {
    auto& ui = proc.ui;
    const double now = juce::Time::getMillisecondCounterHiRes();
    blink = static_cast<int>(now / 160.0) % 2 == 0;
    const uint32_t cycles = ui.histCycles.load();
    if (cycles != lastCycles) { lastCycles = cycles; flashUntil = now + 120; }   // off: blink each new loop recorded
    const int led = ui.loopLed.load();   // 0 off, 1 play, 2 rec, 3 dub
    const bool wetOn = ui.wetOn.load() && !(ui.freeze.load() && !blink);
    const bool loopOn = led == 1 || ((led == 2 || led == 3) && blink) || (led == 0 && now < flashUntil);
    if (wetOn != ledWetOn) { ledWetOn = wetOn; repaint(ledWet.expanded(8)); }
    if (loopOn != ledLoopOn) { ledLoopOn = loopOn; repaint(ledLoop.expanded(8)); }
    if (led != lastLoopLed) {
        const char* msg[] = {nullptr, "playing", "recording...", "overdubbing..."};
        if (msg[led] != nullptr) showReadout("LOOP", msg[led]);
        else if (lastLoopLed == 1) showReadout("LOOP", "stopped");
        lastLoopLed = led;
    }
    const float mi = ui.peakIn.load(), mo = ui.peakOut.load();
    if (std::fabs(mi - meterIn) > 1e-3f || std::fabs(mo - meterOut) > 1e-3f) { meterIn = mi; meterOut = mo; repaint(meterArea.expanded(2)); }
    const int lsrc = ui.lsrc.load();
    const juce::String meta = juce::String(ui.clockHz.load() / 1000.f, 1) + " kHz    "
        + (lsrc > 0 ? "loop " + juce::String(ui.winLen.load() / ui.clockHz.load(), 2) + " s" : juce::String("no loop"))
        + "    cpu " + juce::String(ui.load.load(), 1) + "%";
    if (meta != lcdMeta) { lcdMeta = meta; repaint(lcdArea); }
    if (lastTouched.isNotEmpty() && lcdLabel != "LOOP" && lcdLabel != "DIP" && lcdLabel != "INPUT") {
        const juce::String v = proc.describe(lastTouched);   // follows automation and mode changes
        if (v != lcdValue && lcdLabel.isNotEmpty()) { lcdValue = v; repaint(lcdArea); }
    }
    const uint32_t ver = ui.waveVersion.load();
    if (ver != loopView.version && proc.copyWave(loopView.wave)) loopView.version = ver;
    loopView.repaint();
}

bool MoodEditor::keyPressed(const juce::KeyPress& k) {
    const auto c = juce::CharacterFunctions::toLowerCase(k.getTextCharacter());
    return c == 'a' || c == 'l';   // handled in keyStateChanged so holds work
}

bool MoodEditor::keyStateChanged(bool) {
    const bool a = juce::KeyPress::isKeyCurrentlyDown('a') || juce::KeyPress::isKeyCurrentlyDown('A');
    const bool l = juce::KeyPress::isKeyCurrentlyDown('l') || juce::KeyPress::isKeyCurrentlyDown('L');
    bool used = false;
    if (a != keyA) { keyA = a; fsWet->press(a); used = true; }
    if (l != keyL) { keyL = l; fsLoop->press(l); used = true; }
    return used;
}
