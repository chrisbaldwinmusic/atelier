#include "PluginEditor.h"

using namespace atelier;
using namespace atelier::ui;

namespace
{
    constexpr int headerH = 74, rowH = 300, guideH = 200, margin = 10, gap = 8, meterW = 262;

    float toDb (float gain)     { return gain > 1.0e-5f ? 20.0f * std::log10 (gain) : -100.0f; }
}

//==============================================================================
AtelierEditor::AtelierEditor (AtelierProcessor& p)
    : AudioProcessorEditor (&p), proc (p), apvts (p.apvts),
      input   ("INPUT",      col::ivory,  2),
      gate    ("GATE",       col::green,  2),
      tube    ("TUBE",       col::orange, 2),
      deEss   ("DE-ESS",     col::purple, 1),
      eq      ("EQUALISER",  col::blue,   6),
      comp    ("COMPRESSOR", col::red,    5),
      tape    ("TAPE",       col::gold,   2),
      iron    ("IRON",       col::teal,   1),
      image   ("IMAGE",      col::purple, 1),
      limiter ("LIMITER",    col::green,  2),
      output  ("OUTPUT",     col::ivory,  2),
      guidePanel (p, p.apvts),
      analyserPanel (p)
{
    setLookAndFeel (&lnf);
    addAndMakeVisible (content);

    for (auto* s : { &input, &gate, &tube, &deEss, &eq, &comp, &tape, &iron, &image, &limiter, &output })
        content.addChildComponent (s);
    content.addAndMakeVisible (meterPanel);
    content.addAndMakeVisible (guidePanel);
    content.addChildComponent (analyserPanel);

    using CS = ControlStyle;
    const auto hero = CS::make (CS::hero), small = CS::make (CS::small), trim = CS::make (CS::trim);

    // ---- INPUT ----
    knob (input, "inGain", "GAIN", [] { auto c = CS::centred(); c.size = CS::hero; return c; }());
    {
        auto* cell = new CaptionCell (std::make_unique<LedButton> (apvts, "phase", juce::String::fromUTF8 ("\xc3\x98"), col::ivory), "PHASE", 44, 26);
        owned.add (cell); input.addCell (cell);
    }
    knob (input, "hpf", "HIGH PASS");
    knob (input, "lpf", "LOW PASS");

    // ---- GATE ----
    gate.setPower (apvts, "gateOn");
    knob (gate, "gateThr", "THRESHOLD", hero);
    knob (gate, "gateRange", "RANGE");
    knob (gate, "gateRel", "RELEASE");
    gateRead = readout (gate, "REDUCTION");
    gate.enableGrMeter (40.0f);
    {
        auto* b = new LedButton (apvts, "gateExt", "KEY", col::green);
        owned.add (b); gate.addHeader (b, 44);
    }

    // ---- TUBE ----  (Bias is calibration: a trim pot)
    tube.setPower (apvts, "tubeOn");
    knob (tube, "tubeDrive", "DRIVE", hero);
    knob (tube, "tubeBias", "BIAS", trim);
    selector (tube, "tubeType", "VALVE", { "TRIODE", "PENTODE", "PUSH-PULL" });
    lamp = new TubeLamp();
    setHelpId (*lamp, "tubeOn");
    owned.add (lamp); tube.addCell (lamp);

    // ---- DE-ESS ----
    deEss.setPower (apvts, "dsOn");
    knob (deEss, "dsFreq", "FREQ", small);
    knob (deEss, "dsThr", "THRESHOLD", hero);
    deEss.enableGrMeter (12.0f);

    // ---- EQ ----  (top row: gains, cut or boost from the centre; bottom row: frequencies; the Qs share the middle column)
    eq.setPower (apvts, "eqOn");
    knob (eq, "lfGain", "LOW", CS::centred());
    knob (eq, "lmGain", "LO MID", CS::centred());
    knob (eq, "lmQ", "LO MID Q", small);
    knob (eq, "hmGain", "HI MID", CS::centred());
    knob (eq, "hfGain", "HIGH", CS::centred());
    knob (eq, "airGain", "AIR");
    knob (eq, "lfFreq", "FREQ", small);
    knob (eq, "lmFreq", "FREQ", small);
    knob (eq, "hmQ", "HI MID Q", small);
    knob (eq, "hmFreq", "FREQ", small);
    knob (eq, "hfFreq", "FREQ", small);
    knob (eq, "airFreq", "FREQ", small);
    headerCombo (eq, "eqStyle", 86);
    eqMsBox = headerCombo (eq, "eqMS", 78);
    {
        auto* pre = new LedButton (apvts, "eqPre", "PRE", col::blue);
        pre->onStateChange = [this] { updateFlowText(); };
        owned.add (pre); eq.addHeader (pre, 40);
    }

    // ---- COMPRESSOR ----  (a needle meter, the threshold, ratio in steps, and the four designs on a selector)
    comp.setPower (apvts, "compOn");
    compMeter = new NeedleMeter (20.0f);
    setHelpId (*compMeter, "compOn");
    owned.add (compMeter); comp.addCell (compMeter, 2);
    knob (comp, "compThr", "THRESHOLD", hero);
    knob (comp, "compRatio", "RATIO", CS::stepped ({ { 1.0, "1" }, { 1.5, "1.5" }, { 2.0, "2" }, { 3.0, "3" }, { 4.0, "4" },
                                                      { 6.0, "6" }, { 10.0, "10" }, { 20.0, "20" } }));
    selector (comp, "compModel", "MODEL", { "VCA", "FET", "OPTO", "V-MU" });
    knob (comp, "compAtt", "ATTACK");
    knob (comp, "compRel", "RELEASE");
    knob (comp, "compMakeup", "MAKEUP");
    knob (comp, "compMix", "MIX", small);
    knob (comp, "compSc", "SC HPF", trim);
    {
        auto* b = new LedButton (apvts, "compAuto", "AUTO REL", col::red);
        owned.add (b); comp.addHeader (b, 66);
    }
    {
        auto* b = new LedButton (apvts, "compExt", "EXT KEY", col::red);
        owned.add (b); comp.addHeader (b, 62);
    }

    // ---- TAPE ----  (bias and head bump are alignment: trim pots)
    tape.setPower (apvts, "tapeOn");
    knob (tape, "tapeDrive", "DRIVE", hero);
    knob (tape, "tapeBias", "BIAS", trim);
    knob (tape, "tapeBump", "HEAD BUMP", trim);
    selector (tape, "tapeSpeed", "SPEED", { juce::String::fromUTF8 ("7\xc2\xbd"), "15", "30" });

    // ---- IRON ----
    iron.setPower (apvts, "ironOn");
    knob (iron, "ironDrive", "DRIVE", hero);
    selector (iron, "ironType", "CORE", { "STEEL", "NICKEL", "CONSOLE" });

    // ---- IMAGE ----  (width reads as narrow to wide, with a detent at unchanged)
    image.setPower (apvts, "imgOn");
    {
        auto c = CS::centred (100.0);
        c.shape = CS::horizontal; c.snapNear = 4.0;
        c.detents = { { 0.0, "MONO" }, { 100.0, "" }, { 200.0, "WIDE" } };
        knob (image, "width", "WIDTH", c);
    }
    knob (image, "monoBass", "MONO BASS", small);

    // ---- LIMITER ----
    limiter.setPower (apvts, "limOn");
    knob (limiter, "limDrive", "DRIVE", hero);
    knob (limiter, "limCeil", "CEILING", CS::stepped ({ { -6.0, "-6" }, { -3.0, "-3" }, { -2.0, "" }, { -1.0, "-1" }, { -0.5, "" },
                                                         { -0.3, "" }, { -0.1, "" }, { 0.0, "0" } }));
    knob (limiter, "limRel", "RELEASE");
    knob (limiter, "limClip", "CLIP", small);
    limiter.enableGrMeter (12.0f, true);

    // ---- OUTPUT ----
    ditherBox = headerCombo (output, "dither", 74);
    {
        // a short fader, as on a console channel, with a detent at unity
        auto c = CS::centred();
        c.shape = CS::fader; c.snapNear = 0.4;
        c.detents = { { 24.0, "+24" }, { 12.0, "+12" }, { 0.0, "0" }, { -12.0, "-12" }, { -24.0, "-24" } };
        knob (output, "outGain", "TRIM", c);
    }
    {
        auto* cell = new CaptionCell (std::make_unique<LedButton> (apvts, "match", "=", col::ivory), "MATCH BYP", 44, 26);
        owned.add (cell); output.addCell (cell);
    }
    peakRead = readout (output, "PEAK dBFS");
    setHelpId (*peakRead, "meter.out");
    {
        auto toggle = std::make_unique<TextToggle> (juce::String::fromUTF8 ("\xce\x94"), col::red);
        deltaButton = toggle.get();
        setHelpId (*deltaButton, "delta");
        deltaButton->setToggleState (proc.deltaListen.load(), juce::dontSendNotification);
        deltaButton->onClick = [this] { proc.deltaListen.store (deltaButton->getToggleState()); };
        auto* cell = new CaptionCell (std::move (toggle), "DELTA", 44, 26);
        owned.add (cell); output.addCell (cell);
    }

    // ---- header ----
    bypassButton = std::make_unique<LedButton> (apvts, "bypass", "BYPASS", col::red);
    content.addAndMakeVisible (*bypassButton);
    lowLatButton = std::make_unique<LedButton> (apvts, "lowLat", "LOW LAT", col::gold);
    content.addAndMakeVisible (*lowLatButton);

    modeButtons = std::make_unique<ChoiceButtons> (apvts, "mode", col::gold);
    modeButtons->onChange = [this] (int m) { modeChanged (m); };
    content.addAndMakeVisible (*modeButtons);

    // The bottom strip shows the Guide, the Analyser, or folds away; the choice is saved with the session.
    bottomView = juce::jlimit (0, 2, (int) apvts.state.getProperty ("ui_bottom", (int) viewGuide));
    guideButton.onClick = [this] { setBottomView (guideButton.getToggleState() ? viewGuide : viewNone); };
    scopeButton.onClick = [this] { setBottomView (scopeButton.getToggleState() ? viewAnalyser : viewNone); };
    guideButton.setToggleState (bottomView == viewGuide, juce::dontSendNotification);
    scopeButton.setToggleState (bottomView == viewAnalyser, juce::dontSendNotification);

    // A/B: two complete settings snapshots to flip between
    auto showAB = [this]
    {
        aButton.setToggleState (proc.getABSlot() == 0, juce::dontSendNotification);
        bButton.setToggleState (proc.getABSlot() == 1, juce::dontSendNotification);
    };
    aButton.onClick = [this, showAB] { proc.switchAB (0); showAB(); };
    bButton.onClick = [this, showAB] { proc.switchAB (1); showAB(); };
    showAB();

    for (auto* b : { &undoButton, &redoButton }) b->setClickingTogglesState (false);
    undoButton.onClick = [this] { proc.undo.undo(); };
    redoButton.onClick = [this] { proc.undo.redo(); };

    presetBox.setTextWhenNothingSelected ("PRESETS");
    presetBox.onChange = [this] { presetChosen(); };
    refreshPresets();

    for (auto* c : std::initializer_list<juce::Component*> { &guideButton, &scopeButton, &aButton, &bButton, &undoButton, &redoButton, &presetBox })
        content.addAndMakeVisible (c);

    content.addMouseListener (this, true);
    setResizable (true, true);

    // Open at a size that fits this display, leaving room for the host's own window chrome.
    int width = (int) apvts.state.getProperty ("ui_width", 0);
    if (auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
    {
        const auto screen = display->userBounds;
        const double fit = juce::jmin (1.0, (double) (screen.getHeight() - 110) / (double) baseH(),
                                            (double) (screen.getWidth() - 40) / (double) baseW);
        const int fitWidth = juce::roundToInt (fit * baseW);
        width = width > 0 ? juce::jmin (width, fitWidth) : fitWidth;
    }
    applyWindowSize (width > 0 ? width : baseW);

    modeChanged (modeButtons->getIndex());
    startTimerHz (30);
}

AtelierEditor::~AtelierEditor()
{
    stopTimer();
    content.removeMouseListener (this);
    setLookAndFeel (nullptr);
}

//==============================================================================
void AtelierEditor::applyWindowSize (int width)
{
    const double aspect = (double) baseW / (double) baseH();
    width = juce::jlimit (baseW / 2, baseW * 2, width);
    getConstrainer()->setFixedAspectRatio (aspect);
    setResizeLimits (baseW / 2, juce::roundToInt (baseW / 2 / aspect), baseW * 2, juce::roundToInt (baseW * 2 / aspect));
    setSize (width, juce::roundToInt (width / aspect));
    resized();
}

void AtelierEditor::setBottomView (int view)
{
    bottomView = view;
    apvts.state.setProperty ("ui_bottom", view, nullptr);
    guideButton.setToggleState (view == viewGuide, juce::dontSendNotification);
    scopeButton.setToggleState (view == viewAnalyser, juce::dontSendNotification);
    applyWindowSize (getWidth());
}

//==============================================================================
juce::File AtelierEditor::presetFolder()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
               .getChildFile ("Audio/Presets/Channel Audio/Atelier");
}

void AtelierEditor::refreshPresets()
{
    presetFiles = presetFolder().findChildFiles (juce::File::findFiles, false, "*.atelier");
    presetFiles.sort();

    presetBox.clear (juce::dontSendNotification);
    presetBox.addItem ("Init (reset everything)", 1);
    if (! presetFiles.isEmpty()) presetBox.addSeparator();
    for (int i = 0; i < presetFiles.size(); ++i)
        presetBox.addItem (presetFiles[i].getFileNameWithoutExtension(), 100 + i);
    presetBox.addSeparator();
    presetBox.addItem ("Save preset...", 2);
    presetBox.addItem ("Show presets folder", 3);
}

void AtelierEditor::presetChosen()
{
    const int id = presetBox.getSelectedId();
    if (id == 0) return;

    if (id >= 100)
    {
        loadPreset (presetFiles[id - 100]);
        return;     // leave the preset's name showing
    }

    presetBox.setSelectedId (0, juce::dontSendNotification);
    if (id == 1) resetToInit();
    if (id == 2) savePreset();
    if (id == 3) { presetFolder().createDirectory(); presetFolder().revealToUser(); }
}

void AtelierEditor::resetToInit()
{
    proc.undo.beginNewTransaction ("Init");
    for (auto* base : proc.getParameters())
        if (auto* p = dynamic_cast<juce::RangedAudioParameter*> (base); p != nullptr && p->paramID != "mode")
        {
            p->beginChangeGesture();
            p->setValueNotifyingHost (p->getDefaultValue());
            p->endChangeGesture();
        }
}

void AtelierEditor::savePreset()
{
    presetFolder().createDirectory();
    chooser = std::make_unique<juce::FileChooser> ("Save Atelier preset", presetFolder().getChildFile ("My Preset.atelier"), "*.atelier");
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::warnAboutOverwriting,
                          [this] (const juce::FileChooser& fc)
    {
        auto file = fc.getResult();
        if (file == juce::File()) return;
        file = file.withFileExtension ("atelier");

        // a preset is the sound only: window and Guide choices stay with the session
        auto tree = apvts.copyState();
        for (int i = tree.getNumProperties(); --i >= 0;)
            if (const auto name = tree.getPropertyName (i).toString(); name.startsWith ("ui_") || name.startsWith ("guide_"))
                tree.removeProperty (tree.getPropertyName (i), nullptr);

        if (auto xml = tree.createXml())
            xml->writeTo (file);

        refreshPresets();
        presetBox.setSelectedId (100 + presetFiles.indexOf (file), juce::dontSendNotification);
    });
}

void AtelierEditor::loadPreset (const juce::File& file)
{
    auto xml = juce::parseXML (file);
    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType())) return;

    auto tree = juce::ValueTree::fromXml (*xml);
    for (int i = 0; i < apvts.state.getNumProperties(); ++i)    // keep this session's window and Guide choices
        if (const auto name = apvts.state.getPropertyName (i); name.toString().startsWith ("ui_") || name.toString().startsWith ("guide_"))
            tree.setProperty (name, apvts.state.getProperty (name), nullptr);

    apvts.replaceState (tree);
}

//==============================================================================
void AtelierEditor::knob (Section& s, const char* paramId, const char* caption, ControlStyle style)
{
    auto* k = new Knob (apvts, paramId, caption, s.accent, std::move (style));
    owned.add (k);
    s.addCell (k);
}

void AtelierEditor::selector (Section& s, const char* paramId, const char* caption, juce::StringArray shortNames)
{
    auto* sw = new RotarySwitch (apvts, paramId, caption, s.accent, std::move (shortNames));
    owned.add (sw);
    s.addCell (sw);
}

juce::ComboBox* AtelierEditor::headerCombo (Section& s, const char* paramId, int width)
{
    auto* box = makeCombo (apvts, paramId, comboAttachments).release();
    owned.add (box);
    s.addHeader (box, width);
    return box;
}

Readout* AtelierEditor::readout (Section& s, const char* caption)
{
    auto* r = new Readout (caption, s.accent);
    owned.add (r);
    s.addCell (r);
    return r;
}

//==============================================================================
void AtelierEditor::updateHover (const juce::MouseEvent& e)
{
    if (e.eventComponent == &meterPanel)
        guidePanel.setHovered (meterPanel.helpIdAt (e.getPosition()));
    else
        guidePanel.setHovered (findHelpId (e.eventComponent));
}

void AtelierEditor::mouseEnter (const juce::MouseEvent& e)  { updateHover (e); }
void AtelierEditor::mouseMove (const juce::MouseEvent& e)   { updateHover (e); }

void AtelierEditor::mouseExit (const juce::MouseEvent& e)
{
    if (e.eventComponent == &meterPanel || findHelpId (e.eventComponent).isNotEmpty())
        guidePanel.setHovered ({});
}

//==============================================================================
void AtelierEditor::modeChanged (int newMode)
{
    mode = juce::jlimit (0, 2, newMode);
    const bool isMix = mode == modeMix;

    for (auto* s : { &input, &tube, &eq, &comp, &tape, &iron, &output })
        s->setVisible (true);

    gate.setVisible (isMix);
    deEss.setVisible (isMix);
    image.setVisible (! isMix);
    limiter.setVisible (! isMix);
    eqMsBox->setVisible (! isMix);      // mid/side EQ only applies to busses and the master
    ditherBox->setVisible (! isMix);    // dither belongs at the end of a bus print or a master
    output.resized();

    guidePanel.setMode (mode);
    eq.resized();
    layoutRows();
    updateFlowText();
}

void AtelierEditor::updateFlowText()
{
    const bool isMix = mode == modeMix;
    const bool eqPre = apvts.getRawParameterValue ("eqPre")->load() > 0.5f;

    juce::StringArray flow { "IN", "FILTERS" };
    if (isMix) flow.add ("GATE");
    flow.add ("TUBE");
    if (isMix) flow.add ("DE-ESS");
    flow.add (eqPre ? "EQ" : "COMP");
    flow.add (eqPre ? "COMP" : "EQ");
    flow.add ("TAPE");
    flow.add ("IRON");
    if (! isMix) { flow.add ("IMAGE"); flow.add ("LIMITER"); }
    flow.add ("OUT");

    content.flowText = flow.joinIntoString (juce::String::fromUTF8 (" \xe2\x80\xba "));
    content.repaint();
}

void AtelierEditor::layoutRows()
{
    auto layoutRow = [] (juce::Rectangle<int> area, std::initializer_list<Section*> all)
    {
        juce::Array<Section*> row;
        int cols = 0;
        for (auto* s : all)
            if (s->isVisible()) { row.add (s); cols += s->preferredCols(); }
        if (row.isEmpty()) return;

        const float fixed = (float) (row.size() * 2 * Section::pad + (row.size() - 1) * gap);
        const float cellW = ((float) area.getWidth() - fixed) / (float) cols;
        float x = (float) area.getX();

        for (auto* s : row)
        {
            const float w = cellW * (float) s->preferredCols() + 2.0f * (float) Section::pad;
            s->setBounds (juce::roundToInt (x), area.getY(), juce::roundToInt (x + w) - juce::roundToInt (x), area.getHeight());
            x += w + (float) gap;
        }
    };

    auto area = juce::Rectangle<int> (baseW, baseH()).reduced (margin);
    area.removeFromTop (headerH);

    layoutRow (area.removeFromTop (rowH), { &input, &gate, &tube, &deEss, &eq });
    area.removeFromTop (gap);

    auto rowB = area.removeFromTop (rowH);
    meterPanel.setBounds (rowB.removeFromRight (meterW));
    rowB.removeFromRight (gap);
    layoutRow (rowB, { &comp, &tape, &iron, &image, &limiter, &output });

    guidePanel.setVisible (bottomView == viewGuide);
    analyserPanel.setVisible (bottomView == viewAnalyser);
    if (bottomView != viewNone)
    {
        area.removeFromTop (gap);
        const auto strip = area.removeFromTop (guideH);
        guidePanel.setBounds (strip);
        analyserPanel.setBounds (strip);
    }
}

void AtelierEditor::resized()
{
    content.setTransform (juce::AffineTransform::scale ((float) getWidth() / (float) baseW));
    content.setBounds (0, 0, baseW, baseH());
    apvts.state.setProperty ("ui_width", getWidth(), nullptr);

    modeButtons->setBounds (baseW / 2 - 170, 10, 340, 38);
    presetBox.setBounds (296, 17, 160, 24);

    int x = baseW - margin;
    auto place = [&x] (juce::Component& c, int w, int gapAfter = 6) { x -= w; c.setBounds (x, 17, w, 24); x -= gapAfter; };
    place (*bypassButton, 70);
    place (guideButton, 54);
    place (scopeButton, 74, 12);
    place (bButton, 26, 2);
    place (aButton, 26, 12);
    place (redoButton, 44, 2);
    place (undoButton, 44);
    place (*lowLatButton, 66);
    layoutRows();
}

void AtelierEditor::paint (juce::Graphics& g)   { g.fillAll (col::faceplate); }

void AtelierEditor::Content::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff25221e), 0.0f, 0.0f, juce::Colour (0xff11100e), 0.0f, r.getBottom(), false));
    g.fillAll();

    // brushed finish: fine horizontal grain, and a broad soft sheen across the middle of the plate
    g.setColour (col::goldLight.withAlpha (0.016f));
    for (int y = 0; y < getHeight(); y += 2)
        g.drawHorizontalLine (y, 0.0f, r.getRight());
    {
        juce::ColourGradient sheen (juce::Colours::transparentBlack, 0.0f, 0.0f, juce::Colours::transparentBlack, r.getRight(), 0.0f, false);
        sheen.addColour (0.45, col::gold.withAlpha (0.045f));
        g.setGradientFill (sheen);
        g.fillAll();
    }

    // the name plate, in gold leaf
    {
        juce::ColourGradient leaf (col::goldLight, 0.0f, 14.0f, col::gold, 0.0f, 44.0f, false);
        leaf.addColour (0.45, col::gold);
        leaf.addColour (0.62, col::goldDark.brighter (0.25f));
        leaf.addColour (0.8, col::goldLight.darker (0.1f));
        g.setGradientFill (leaf);
        g.setFont (font (30.0f, true, 0.32f));
        g.drawText ("ATELIER", margin + 6, 12, 300, 34, juce::Justification::centredLeft, false);
    }
    g.setColour (col::creamDim);
    g.setFont (font (10.0f, true, 0.34f));
    g.drawText ("BOUTIQUE CHANNEL STRIP", margin + 8, 46, 300, 14, juce::Justification::centredLeft, false);

    // signal flow, under the mode selector
    g.setColour (col::creamDim);
    g.setFont (font (8.5f, true, 0.06f));
    g.drawFittedText (flowText, getWidth() / 2 - 260, 52, 520, 12, juce::Justification::centred, 1, 0.7f);

    for (auto p : { juce::Point<float> (7.0f, 7.0f), { r.getRight() - 7.0f, 7.0f }, { 7.0f, r.getBottom() - 7.0f }, { r.getRight() - 7.0f, r.getBottom() - 7.0f } })
    {
        g.setGradientFill (juce::ColourGradient (col::goldLight.darker (0.2f), 0.0f, p.y - 3.5f, col::goldDark, 0.0f, p.y + 3.5f, false));
        g.fillEllipse (p.x - 3.5f, p.y - 3.5f, 7.0f, 7.0f);
        g.setColour (juce::Colours::black.withAlpha (0.7f));
        g.drawLine (p.x - 2.2f, p.y - 2.2f, p.x + 2.2f, p.y + 2.2f, 1.0f);
    }
}

//==============================================================================
void AtelierEditor::timerCallback()
{
    auto& m = proc.meters;
    auto take = [] (std::atomic<float>& a) { return a.exchange (0.0f, std::memory_order_relaxed); };
    auto fall = [] (float& disp, float target, float rate) { disp = target > disp ? target : juce::jmax (target, disp - rate); };

    for (int ch = 0; ch < 2; ++ch)
    {
        fall (inHold[ch],  toDb (take (m.inPeak[ch])),  1.6f);
        fall (outHold[ch], toDb (take (m.outPeak[ch])), 1.6f);
        meterPanel.inDb[ch] = inHold[ch];
        meterPanel.outDb[ch] = outHold[ch];
    }

    // loudest recent input peak, falling slowly: starting points are fitted to it
    fall (recentIn, juce::jmax (inHold[0], inHold[1]), 0.02f);
    guidePanel.recentPeakDb = recentIn;

    const float outPk = juce::jmax (outHold[0], outHold[1]);
    if (outPk >= peakHold || ++peakHoldTicks > 60) { peakHold = outPk; peakHoldTicks = 0; }
    peakRead->set (peakHold < -90.0f ? juce::String ("-inf") : juce::String (peakHold, 1));

    fall (grCompDisp, take (m.grComp), 0.6f);
    fall (grGateDisp, take (m.grGate), 2.0f);
    fall (grDsDisp,   take (m.grDeEss), 0.5f);
    fall (grLimDisp,  take (m.grLim), 0.4f);
    fall (heatDisp,   take (m.heat), 0.06f);
    fall (keyHold,    toDb (take (m.keyPeak)), 1.6f);

    compMeter->set (grCompDisp);
    gate.setGr (grGateDisp);
    deEss.setGr (grDsDisp);
    limiter.setGr (grLimDisp);
    gateRead->set (juce::String (-grGateDisp, 1));
    fall (grClipDisp, take (m.grClip), 0.4f);
    lamp->set (apvts.getRawParameterValue ("tubeOn")->load() > 0.5f ? 0.12f + heatDisp * 0.6f : 0.0f);

    const float vuTarget = juce::jlimit (-40.0f, 6.0f, toDb (m.vuRms.load (std::memory_order_relaxed)) + 3.0f + 18.0f);
    vuDisp += 0.3f * (vuTarget - vuDisp);
    meterPanel.vuDb = vuDisp;
    meterPanel.grDb = grCompDisp;
    meterPanel.lufsS = m.lufsS.load (std::memory_order_relaxed);
    meterPanel.lufsM = m.lufsM.load (std::memory_order_relaxed);
    meterPanel.repaint();

    if (bottomView == viewAnalyser)
        analyserPanel.update();

    if (++ticks % 6 == 0)
        guidePanel.refreshValue();

    // group edits into undo steps: a new step begins whenever the mouse is released
    if (ticks % 10 == 0 && ! juce::ModifierKeys::currentModifiers.isAnyMouseButtonDown())
        proc.undo.beginNewTransaction();

    if (ticks % 12 == 0)
    {
        Coach::Readings r;
        r.inPeakDb = juce::jmax (inHold[0], inHold[1]);
        r.outPeakDb = peakHold;
        r.vu = vuDisp;          r.grComp = grCompDisp;  r.grDeEss = grDsDisp;
        r.grClip = grClipDisp;
        r.grLim = grLimDisp;    r.heat = heatDisp;      r.lufsS = meterPanel.lufsS;
        r.matchDb = m.matchDb.load (std::memory_order_relaxed);   r.delta = proc.deltaListen.load();
        r.keyPeakDb = keyHold;  r.keyConnected = m.keyConnected.load (std::memory_order_relaxed);
        guidePanel.setCoach (Coach::advise (apvts, r));
    }
}
