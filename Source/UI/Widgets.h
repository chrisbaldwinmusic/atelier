#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

namespace atelier::ui
{
using APVTS = juce::AudioProcessorValueTreeState;

namespace col
{
    const juce::Colour faceplate { 0xff17181b }, panel { 0xff232529 }, panelEdge { 0xff0c0d0e }, panelHi { 0xff34373d };
    const juce::Colour well { 0xff0d0e0f }, wellEdge { 0xff3a3d43 }, ledOff { 0xff2c2e33 };
    const juce::Colour cream { 0xffe9dfc6 }, creamDim { 0xffa39d8d };
    const juce::Colour gold { 0xffc9a25a }, red { 0xffc0503f }, blue { 0xff4f86b8 }, green { 0xff62a070 };
    const juce::Colour orange { 0xffd9853b }, purple { 0xff8672b8 }, ivory { 0xffcfc7b2 }, teal { 0xff4fa3a0 };
}

inline juce::Font font (float size, bool bold = false, float tracking = 0.0f)
{
    return juce::Font (juce::FontOptions (size, bold ? juce::Font::bold : juce::Font::plain)).withExtraKerningFactor (tracking);
}

/** Every control carries the id of its Guide entry so the editor can explain whatever is under the pointer. */
inline void setHelpId (juce::Component& c, const juce::String& id)  { c.getProperties().set ("help", id); }

inline juce::String findHelpId (juce::Component* c)
{
    for (; c != nullptr; c = c->getParentComponent())
        if (c->getProperties().contains ("help"))
            return c->getProperties()["help"].toString();
    return {};
}

/** Draws word-wrapped text and returns the height it used. */
inline float drawWrapped (juce::Graphics& g, const juce::String& text, const juce::Font& f, juce::Colour c, juce::Rectangle<float> area)
{
    juce::AttributedString s;
    s.append (text, f, c);
    s.setWordWrap (juce::AttributedString::byWord);
    juce::TextLayout layout;
    layout.createLayout (s, area.getWidth());
    layout.draw (g, area);
    return layout.getHeight();
}

inline void paintPanel (juce::Graphics& g, juce::Rectangle<float> r)
{
    r = r.reduced (0.5f);
    g.setGradientFill (juce::ColourGradient (col::panel.brighter (0.05f), 0.0f, r.getY(), col::panel.darker (0.25f), 0.0f, r.getBottom(), false));
    g.fillRoundedRectangle (r, 6.0f);
    g.setColour (col::panelEdge);
    g.drawRoundedRectangle (r, 6.0f, 1.2f);
    g.setColour (col::panelHi.withAlpha (0.6f));
    g.drawHorizontalLine ((int) r.getY() + 1, r.getX() + 6.0f, r.getRight() - 6.0f);
}

//==============================================================================
class AtelierLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    AtelierLookAndFeel()
    {
        setColour (juce::ComboBox::backgroundColourId, juce::Colour (0xff131416));
        setColour (juce::ComboBox::textColourId, col::cream);
        setColour (juce::ComboBox::outlineColourId, col::wellEdge);
        setColour (juce::ComboBox::arrowColourId, col::creamDim);
        setColour (juce::PopupMenu::backgroundColourId, juce::Colour (0xff1c1d20));
        setColour (juce::PopupMenu::textColourId, col::cream);
        setColour (juce::PopupMenu::highlightedBackgroundColourId, col::gold);
        setColour (juce::PopupMenu::highlightedTextColourId, juce::Colours::black);
    }

    void drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos,
                           float startAngle, float endAngle, juce::Slider& slider) override
    {
        const auto accent = slider.findColour (juce::Slider::thumbColourId);
        const auto area = juce::Rectangle<int> (x, y, w, h).toFloat();
        const auto centre = area.getCentre();
        const float radius = juce::jmin (area.getWidth(), area.getHeight()) * 0.5f - 9.0f;
        if (radius < 6.0f) return;

        for (int i = 0; i < 11; ++i)
        {
            const float t = (float) i / 10.0f;
            const auto p = centre.getPointOnCircumference (radius + 5.5f, startAngle + t * (endAngle - startAngle));
            g.setColour (t <= pos + 0.001f ? accent.withMultipliedBrightness (1.15f) : col::ledOff.brighter (0.15f));
            g.fillEllipse (p.x - 1.4f, p.y - 1.4f, 2.8f, 2.8f);
        }

        g.setColour (juce::Colours::black.withAlpha (0.45f));
        g.fillEllipse (centre.x - radius, centre.y - radius + 3.0f, radius * 2.0f, radius * 2.0f);

        auto skirt = juce::Rectangle<float> (radius * 2.0f, radius * 2.0f).withCentre (centre);
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff4a4d54), centre.x, skirt.getY(),
                                                 juce::Colour (0xff101113), centre.x, skirt.getBottom(), false));
        g.fillEllipse (skirt);
        g.setColour (juce::Colours::black.withAlpha (0.8f));
        g.drawEllipse (skirt, 1.0f);

        auto cap = skirt.reduced (radius * 0.24f);
        g.setGradientFill (juce::ColourGradient (accent.brighter (0.35f), centre.x, cap.getY(),
                                                 accent.darker (0.75f), centre.x, cap.getBottom(), false));
        g.fillEllipse (cap);
        g.setColour (juce::Colours::white.withAlpha (0.18f));
        g.drawEllipse (cap.reduced (0.5f), 1.0f);

        juce::Path pointer;
        pointer.addRoundedRectangle (-1.5f, -radius * 0.98f, 3.0f, radius * 0.62f, 1.5f);
        g.setColour (juce::Colour (0xfff6f0de));
        g.fillPath (pointer, juce::AffineTransform::rotation (startAngle + pos * (endAngle - startAngle)).translated (centre));
    }

    void drawComboBox (juce::Graphics& g, int w, int h, bool, int, int, int, int, juce::ComboBox& box) override
    {
        auto r = juce::Rectangle<float> (0.0f, 0.0f, (float) w, (float) h).reduced (0.5f);
        g.setColour (box.findColour (juce::ComboBox::backgroundColourId));
        g.fillRoundedRectangle (r, 3.0f);
        g.setColour (box.findColour (juce::ComboBox::outlineColourId));
        g.drawRoundedRectangle (r, 3.0f, 1.0f);

        juce::Path arrow;
        const float ax = (float) w - 11.0f, ay = (float) h * 0.5f;
        arrow.addTriangle (ax - 3.5f, ay - 2.0f, ax + 3.5f, ay - 2.0f, ax, ay + 2.5f);
        g.setColour (box.findColour (juce::ComboBox::arrowColourId).withAlpha (box.isEnabled() ? 1.0f : 0.3f));
        g.fillPath (arrow);
    }

    juce::Font getComboBoxFont (juce::ComboBox&) override      { return font (11.5f, true, 0.04f); }
    juce::Font getPopupMenuFont() override                     { return font (13.0f); }

    void positionComboBoxText (juce::ComboBox& box, juce::Label& label) override
    {
        label.setBounds (4, 1, box.getWidth() - 20, box.getHeight() - 2);
        label.setFont (getComboBoxFont (box));
        label.setJustificationType (juce::Justification::centred);
    }
};

//==============================================================================
/** Rotary control with its caption; shows the value while hovered or dragged.
    Shift-drag for fine adjustment, double-click to reset. */
class Knob final : public juce::Component
{
public:
    Knob (APVTS& state, const juce::String& paramId, const juce::String& caption, juce::Colour accent)
        : text (caption)
    {
        slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        slider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        slider.setRotaryParameters (juce::MathConstants<float>::pi * 1.25f, juce::MathConstants<float>::pi * 2.75f, true);
        slider.setColour (juce::Slider::thumbColourId, accent);
        slider.setMouseDragSensitivity (220);
        slider.addMouseListener (this, false);
        slider.onValueChange = [this] { repaint(); };
        addAndMakeVisible (slider);
        attachment = std::make_unique<APVTS::SliderAttachment> (state, paramId, slider);
        setHelpId (*this, paramId);
    }

    void resized() override     { slider.setBounds (getLocalBounds().withTrimmedBottom (15)); }

    void paint (juce::Graphics& g) override
    {
        const bool active = slider.isMouseOverOrDragging();
        g.setFont (font (10.5f, true, active ? 0.0f : 0.08f));
        g.setColour (active ? col::cream : col::creamDim);
        g.drawText (active ? slider.getTextFromValue (slider.getValue()) : text,
                    getLocalBounds().removeFromBottom (15), juce::Justification::centred, false);
    }

    void mouseEnter (const juce::MouseEvent&) override  { repaint(); }
    void mouseExit (const juce::MouseEvent&) override   { repaint(); }
    void mouseUp (const juce::MouseEvent&) override     { repaint(); }

    juce::Slider slider;

private:
    juce::String text;
    std::unique_ptr<APVTS::SliderAttachment> attachment;
};

//==============================================================================
/** Illuminated push button bound to a bool parameter. With no text it is a power LED. */
class LedButton final : public juce::Button
{
public:
    LedButton (APVTS& state, const juce::String& paramId, const juce::String& caption, juce::Colour accentColour)
        : juce::Button (caption), accent (accentColour)
    {
        setClickingTogglesState (true);
        attachment = std::make_unique<APVTS::ButtonAttachment> (state, paramId, *this);
        setHelpId (*this, paramId);
    }

    void paintButton (juce::Graphics& g, bool over, bool) override
    {
        const bool lit = getToggleState();
        auto r = getLocalBounds().toFloat().reduced (1.0f);

        if (getName().isEmpty())
        {
            const float d = juce::jmin (r.getWidth(), r.getHeight()) - 4.0f;
            auto led = r.withSizeKeepingCentre (d, d);
            if (lit)
            {
                g.setColour (accent.withAlpha (0.25f));
                g.fillEllipse (led.expanded (3.0f));
            }
            g.setColour (lit ? accent.brighter (0.3f) : col::ledOff.brighter (over ? 0.3f : 0.0f));
            g.fillEllipse (led);
            g.setColour (juce::Colours::black.withAlpha (0.7f));
            g.drawEllipse (led, 1.0f);
            return;
        }

        g.setColour (lit ? accent : juce::Colour (0xff131416).brighter (over ? 0.08f : 0.0f));
        g.fillRoundedRectangle (r, 3.0f);
        g.setColour (lit ? accent.brighter (0.4f) : col::wellEdge);
        g.drawRoundedRectangle (r, 3.0f, 1.0f);
        g.setColour (lit ? juce::Colour (0xff141414) : col::creamDim);
        g.setFont (font (10.5f, true, 0.06f));
        g.drawText (getName(), getLocalBounds(), juce::Justification::centred, false);
    }

private:
    juce::Colour accent;
    std::unique_ptr<APVTS::ButtonAttachment> attachment;
};

//==============================================================================
/** Latching text button that isn't tied to a parameter (interface options such as showing the Guide). */
class TextToggle final : public juce::Button
{
public:
    TextToggle (const juce::String& caption, juce::Colour accentColour) : juce::Button (caption), accent (accentColour)
    {
        setClickingTogglesState (true);
    }

    void paintButton (juce::Graphics& g, bool over, bool) override
    {
        const bool lit = getToggleState();
        auto r = getLocalBounds().toFloat().reduced (1.0f);
        g.setColour (lit ? accent : juce::Colour (0xff131416).brighter (over ? 0.08f : 0.0f));
        g.fillRoundedRectangle (r, 3.0f);
        g.setColour (lit ? accent.brighter (0.4f) : col::wellEdge);
        g.drawRoundedRectangle (r, 3.0f, 1.0f);
        g.setColour (lit ? juce::Colour (0xff141414) : col::creamDim);
        g.setFont (font (10.5f, true, 0.06f));
        g.drawText (getName(), getLocalBounds(), juce::Justification::centred, false);
    }

private:
    juce::Colour accent;
};

//==============================================================================
/** A grid cell holding a small control (combo or button) above a caption. */
class CaptionCell final : public juce::Component
{
public:
    CaptionCell (std::unique_ptr<juce::Component> control, const juce::String& caption, int controlW, int controlH)
        : child (std::move (control)), text (caption), cw (controlW), ch (controlH)
    {
        addAndMakeVisible (*child);
    }

    void resized() override
    {
        child->setBounds (getLocalBounds().withTrimmedBottom (15).withSizeKeepingCentre (juce::jmin (cw, getWidth() - 6), ch));
    }

    void paint (juce::Graphics& g) override
    {
        g.setFont (font (10.5f, true, 0.08f));
        g.setColour (col::creamDim);
        g.drawText (text, getLocalBounds().removeFromBottom (15), juce::Justification::centred, false);
    }

private:
    std::unique_ptr<juce::Component> child;
    juce::String text;
    int cw, ch;
};

inline std::unique_ptr<juce::ComboBox> makeCombo (APVTS& state, const juce::String& paramId,
                                                  juce::OwnedArray<APVTS::ComboBoxAttachment>& attachments)
{
    auto box = std::make_unique<juce::ComboBox>();
    if (auto* choice = dynamic_cast<juce::AudioParameterChoice*> (state.getParameter (paramId)))
        box->addItemList (choice->choices, 1);
    attachments.add (new APVTS::ComboBoxAttachment (state, paramId, *box));
    setHelpId (*box, paramId);
    return box;
}

//==============================================================================
/** Numeric readout (gain reduction, peak). */
class Readout final : public juce::Component
{
public:
    Readout (const juce::String& caption, juce::Colour c) : text (caption), colour (c) {}

    void set (const juce::String& v)    { if (v != value) { value = v; repaint(); } }

    void paint (juce::Graphics& g) override
    {
        auto box = getLocalBounds().withTrimmedBottom (15).withSizeKeepingCentre (juce::jmin (74, getWidth() - 6), 30).toFloat();
        g.setColour (col::well);
        g.fillRoundedRectangle (box, 3.0f);
        g.setColour (col::wellEdge);
        g.drawRoundedRectangle (box, 3.0f, 1.0f);
        g.setColour (colour.brighter (0.3f));
        g.setFont (font (14.0f, true));
        g.drawText (value, box, juce::Justification::centred, false);

        g.setFont (font (10.5f, true, 0.08f));
        g.setColour (col::creamDim);
        g.drawText (text, getLocalBounds().removeFromBottom (15), juce::Justification::centred, false);
    }

private:
    juce::String text, value { "0.0" };
    juce::Colour colour;
};

//==============================================================================
/** Valve glow: brightens as the tube stage is driven harder. */
class TubeLamp final : public juce::Component
{
public:
    void set (float v)  { v = juce::jlimit (0.0f, 1.0f, v); if (std::abs (v - level) > 0.01f) { level = v; repaint(); } }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat().withSizeKeepingCentre (34.0f, 62.0f);
        const auto glow = juce::Colour (0xffff8a2a);

        g.setGradientFill (juce::ColourGradient (glow.withAlpha (0.10f + 0.5f * level), r.getCentreX(), r.getCentreY() + 8.0f,
                                                 glow.withAlpha (0.0f), r.getCentreX(), r.getCentreY() - 48.0f, true));
        g.fillEllipse (r.expanded (22.0f));

        juce::Path glass;
        glass.addRoundedRectangle (r.getX(), r.getY(), r.getWidth(), r.getHeight(), 15.0f, 15.0f, true, true, false, false);
        g.setColour (juce::Colour (0xff0e0f10));
        g.fillPath (glass);
        g.setGradientFill (juce::ColourGradient (glow.withAlpha (0.25f + 0.75f * level), r.getCentreX(), r.getBottom() - 14.0f,
                                                 glow.withAlpha (0.03f), r.getCentreX(), r.getY(), true));
        g.fillPath (glass);
        g.setColour (juce::Colours::white.withAlpha (0.22f));
        g.strokePath (glass, juce::PathStrokeType (1.0f));

        g.setColour (juce::Colour (0xff2a2b2e).withAlpha (0.9f));
        g.fillRoundedRectangle (r.getCentreX() - 7.0f, r.getY() + 16.0f, 14.0f, 30.0f, 2.0f);
        g.setColour (glow.brighter (0.6f).withAlpha (0.35f + 0.65f * level));
        g.fillRect (r.getCentreX() - 0.75f, r.getY() + 20.0f, 1.5f, 22.0f);
        g.setColour (juce::Colour (0xff3b3d42));
        g.fillRect (r.getX() - 3.0f, r.getBottom(), r.getWidth() + 6.0f, 5.0f);
    }

private:
    float level = 0.0f;
};

//==============================================================================
/** One module of the strip: title bar with power LED, a 2-row grid of controls
    and an optional gain-reduction strip along the bottom. */
class Section final : public juce::Component
{
public:
    Section (const juce::String& name, juce::Colour accentColour, int numCols)
        : accent (accentColour), cols (numCols), title (name) {}

    void setPower (APVTS& state, const juce::String& paramId)
    {
        power = std::make_unique<LedButton> (state, paramId, "", accent);
        power->onStateChange = [this] { repaint(); };
        addAndMakeVisible (*power);
    }

    void addCell (juce::Component* c)                   { cells.add (c); if (c != nullptr) addAndMakeVisible (c); }
    void addHeader (juce::Component* c, int width)      { headers.add ({ c, width }); addAndMakeVisible (c); }
    void enableGrMeter (float rangeDb, bool showValue = false)  { grRange = rangeDb; showGrValue = showValue; }
    void setGr (float db)                               { if (std::abs (db - gr) > 0.02f) { gr = db; repaint (grBounds().expanded (2)); } }

    static constexpr int titleH = 28, pad = 8, grH = 14;
    int preferredCols() const   { return cols; }

    void resized() override
    {
        auto top = getLocalBounds().reduced (pad, 0).removeFromTop (titleH);
        if (power != nullptr) power->setBounds (top.removeFromLeft (18).withSizeKeepingCentre (16, 16));

        for (int i = headers.size(); --i >= 0;)
        {
            auto h = headers.getReference (i);
            if (! h.comp->isVisible()) continue;
            h.comp->setBounds (top.removeFromRight (h.width).withSizeKeepingCentre (h.width, 18));
            top.removeFromRight (5);
        }

        // the GR strip's space is reserved in every section so knobs line up across the row
        auto grid = getLocalBounds().reduced (pad, 0).withTrimmedTop (titleH + 4).withTrimmedBottom (grH + 8);
        const float cw = (float) grid.getWidth() / (float) cols, rh = (float) grid.getHeight() * 0.5f;

        for (int i = 0; i < cells.size(); ++i)
            if (auto* c = cells[i])
                c->setBounds (juce::Rectangle<float> ((float) grid.getX() + cw * (float) (i % cols),
                                                      (float) grid.getY() + rh * (float) (i / cols), cw, rh)
                                  .reduced (2.0f, 3.0f).toNearestInt());
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        paintPanel (g, r);

        const bool lit = power == nullptr || power->getToggleState();
        g.setColour (accent.withAlpha (lit ? 0.95f : 0.35f));
        g.fillRect (juce::Rectangle<float> ((float) pad, (float) titleH - 1.5f, r.getWidth() - 2.0f * (float) pad, 1.5f));
        g.setColour (lit ? col::cream : col::creamDim.withAlpha (0.7f));
        g.setFont (font (12.5f, true, 0.16f));
        g.drawText (title, pad + (power != nullptr ? 24 : 2), 0, getWidth(), titleH, juce::Justification::centredLeft, false);

        if (grRange > 0.0f)
        {
            auto bar = grBounds().toFloat();
            g.setColour (col::well);
            g.fillRoundedRectangle (bar, 2.0f);
            g.setFont (font (8.5f, true));
            g.setColour (col::creamDim);
            g.drawText ("GR", bar.removeFromLeft (20.0f), juce::Justification::centred, false);

            const int segs = juce::jmax (8, (int) bar.getWidth() / 7);
            const float sw = bar.getWidth() / (float) segs;
            const int litSegs = juce::roundToInt (juce::jlimit (0.0f, 1.0f, gr / grRange) * (float) segs);
            for (int i = 0; i < segs; ++i)
            {
                g.setColour (i < litSegs ? accent.brighter (0.2f) : col::ledOff);
                g.fillRect (bar.getX() + sw * (float) i + 1.0f, bar.getY() + 3.0f, sw - 2.0f, bar.getHeight() - 6.0f);
            }

            if (showGrValue && gr > 0.05f)      // the figure, over the unlit end of the strip
            {
                auto label = bar.removeFromRight (34.0f);
                g.setColour (col::well.withAlpha (0.85f));
                g.fillRect (label);
                g.setColour (col::cream);
                g.setFont (font (9.0f, true));
                g.drawText ("-" + juce::String (gr, 1), label, juce::Justification::centred, false);
            }
        }
    }

    const juce::Colour accent;

private:
    juce::Rectangle<int> grBounds() const
    {
        return getLocalBounds().reduced (pad, 0).removeFromBottom (grH + 6).withTrimmedBottom (6);
    }

    struct Header { juce::Component* comp; int width; };
    int cols;
    juce::String title;
    std::unique_ptr<LedButton> power;
    juce::Array<juce::Component*> cells;
    juce::Array<Header> headers;
    bool showGrValue = false;
    float grRange = 0.0f, gr = 0.0f;
};

//==============================================================================
/** Radio buttons bound to a choice parameter (the Mix / Bus / Master selector). */
class ChoiceButtons final : public juce::Component
{
public:
    ChoiceButtons (APVTS& state, const juce::String& paramId, juce::Colour accentColour)
        : accent (accentColour), param (*state.getParameter (paramId)),
          attachment (param, [this] (float v) { setIndex (juce::roundToInt (v)); })
    {
        if (auto* choice = dynamic_cast<juce::AudioParameterChoice*> (&param))
            names = choice->choices;
        setHelpId (*this, paramId);
        attachment.sendInitialUpdate();
    }

    std::function<void (int)> onChange;
    int getIndex() const    { return index; }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat().reduced (0.5f);
        g.setColour (col::well);
        g.fillRoundedRectangle (r, 5.0f);
        const float w = r.getWidth() / (float) juce::jmax (1, names.size());

        for (int i = 0; i < names.size(); ++i)
        {
            auto b = juce::Rectangle<float> (r.getX() + w * (float) i, r.getY(), w, r.getHeight()).reduced (3.0f);
            const bool lit = i == index;
            if (lit)
            {
                g.setGradientFill (juce::ColourGradient (accent.brighter (0.25f), 0.0f, b.getY(), accent.darker (0.25f), 0.0f, b.getBottom(), false));
                g.fillRoundedRectangle (b, 3.5f);
            }
            g.setColour (lit ? juce::Colour (0xff15130f) : col::creamDim);
            g.setFont (font (13.0f, true, 0.18f));
            g.drawText (names[i].toUpperCase(), b, juce::Justification::centred, false);
        }
        g.setColour (col::wellEdge);
        g.drawRoundedRectangle (r, 5.0f, 1.0f);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        attachment.setValueAsCompleteGesture ((float) juce::jlimit (0, names.size() - 1, e.x * names.size() / juce::jmax (1, getWidth())));
    }

private:
    void setIndex (int i)
    {
        if (i == index) return;
        index = i;
        repaint();
        if (onChange) onChange (i);
    }

    juce::Colour accent;
    juce::RangedAudioParameter& param;
    juce::StringArray names;
    int index = -1;
    juce::ParameterAttachment attachment;
};

//==============================================================================
/** Analog VU face with needle, stereo peak ladders and loudness readout. Click the VU to show gain reduction. */
class MeterPanel final : public juce::Component
{
public:
    float vuDb = -40.0f, grDb = 0.0f;
    float inDb[2] { -100.0f, -100.0f }, outDb[2] { -100.0f, -100.0f };
    float lufsS = -100.0f, lufsM = -100.0f;
    bool showGr = false;

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (vuBounds().contains (e.getPosition())) { showGr = ! showGr; repaint(); }
    }

    /** Which Guide entry describes the meter under this point. */
    juce::String helpIdAt (juce::Point<int> p) const
    {
        const int below = p.y - vuBounds().getBottom();
        return below < 6 ? "meter.vu" : (below < 44 ? "meter.in" : (below < 82 ? "meter.out" : "meter.lufs"));
    }

    void paint (juce::Graphics& g) override
    {
        paintPanel (g, getLocalBounds().toFloat());
        paintVu (g, vuBounds().toFloat());

        auto area = getLocalBounds().reduced (12, 0).withTrimmedTop (vuBounds().getBottom() + 10).withTrimmedBottom (10);
        paintLadder (g, area.removeFromTop (30), "IN", inDb);
        area.removeFromTop (6);
        paintLadder (g, area.removeFromTop (30), "OUT", outDb);
        area.removeFromTop (8);

        auto lufs = area.removeFromTop (26).toFloat();
        g.setColour (col::well);
        g.fillRoundedRectangle (lufs, 3.0f);
        auto text = [] (float v) { return v < -70.0f ? juce::String ("--.-") : juce::String (v, 1); };
        auto half = lufs.removeFromLeft (lufs.getWidth() * 0.56f);
        g.setFont (font (9.5f, true, 0.08f));
        g.setColour (col::creamDim);
        g.drawText ("LUFS S", half.removeFromLeft (56.0f), juce::Justification::centred, false);
        g.drawText ("M", lufs.removeFromLeft (22.0f), juce::Justification::centred, false);
        g.setFont (font (14.0f, true));
        g.setColour (col::gold.brighter (0.2f));
        g.drawText (text (lufsS), half, juce::Justification::centredLeft, false);
        g.drawText (text (lufsM), lufs, juce::Justification::centredLeft, false);
    }

private:
    juce::Rectangle<int> vuBounds() const   { return getLocalBounds().reduced (12, 0).withTrimmedTop (12).withHeight (150); }

    static float vuFraction (float vu)      { return juce::jlimit (0.0f, 1.0f, std::pow (10.0f, vu / 20.0f) / std::pow (10.0f, 3.0f / 20.0f)); }

    void paintVu (juce::Graphics& g, juce::Rectangle<float> face)
    {
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xfff3e9cb), face.getCentreX(), face.getBottom(),
                                                 juce::Colour (0xffc9b98c), face.getX(), face.getY(), true));
        g.fillRoundedRectangle (face, 5.0f);

        juce::Graphics::ScopedSaveState save (g);
        g.reduceClipRegion (face.toNearestInt());

        const juce::Point<float> pivot (face.getCentreX(), face.getBottom() + face.getHeight() * 0.32f);
        const float R = face.getHeight() * 1.08f;
        const float sweep = juce::degreesToRadians (44.0f);
        auto angleFor = [sweep] (float vu) { return -sweep + 2.0f * sweep * vuFraction (vu); };
        const juce::Colour ink (0xff2a2520), redInk (0xffb5352a);

        juce::Path arc, redArc;
        arc.addCentredArc (pivot.x, pivot.y, R, R, 0.0f, angleFor (-20.0f), angleFor (0.0f), true);
        redArc.addCentredArc (pivot.x, pivot.y, R, R, 0.0f, angleFor (0.0f), angleFor (3.0f), true);
        g.setColour (ink);    g.strokePath (arc, juce::PathStrokeType (1.6f));
        g.setColour (redInk); g.strokePath (redArc, juce::PathStrokeType (4.0f));

        static constexpr int marks[] = { -20, -10, -7, -5, -3, -2, -1, 0, 1, 2, 3 };
        g.setFont (font (9.0f, true));
        for (int m : marks)
        {
            const float a = angleFor ((float) m);
            g.setColour (m > 0 ? redInk : ink);
            g.drawLine ({ pivot.getPointOnCircumference (R, a), pivot.getPointOnCircumference (R + 7.0f, a) }, 1.3f);
            g.drawText (juce::String (std::abs (m)), juce::Rectangle<float> (22.0f, 12.0f).withCentre (pivot.getPointOnCircumference (R + 16.0f, a)),
                        juce::Justification::centred, false);
        }

        g.setColour (ink.withAlpha (0.8f));
        g.setFont (font (15.0f, true, 0.2f));
        g.drawText (showGr ? "GR" : "VU", face.withTrimmedTop (face.getHeight() * 0.56f).withHeight (20.0f), juce::Justification::centred, false);
        g.setFont (font (8.0f, true, 0.2f));
        g.drawText (showGr ? "COMPRESSOR GAIN REDUCTION" : "OUTPUT   0 VU = -18 dBFS",
                    face.withTrimmedTop (face.getHeight() * 0.56f + 20.0f).withHeight (12.0f), juce::Justification::centred, false);

        const float a = angleFor (showGr ? -grDb : vuDb);
        g.setColour (juce::Colours::black.withAlpha (0.18f));
        g.drawLine ({ pivot.translated (2.0f, 3.0f), pivot.getPointOnCircumference (R + 4.0f, a).translated (2.0f, 3.0f) }, 2.0f);
        g.setColour (juce::Colour (0xff151210));
        g.drawLine ({ pivot, pivot.getPointOnCircumference (R + 4.0f, a) }, 1.6f);

        g.setColour (juce::Colours::black.withAlpha (0.35f));
        g.drawRoundedRectangle (face.reduced (0.5f), 5.0f, 2.0f);
    }

    void paintLadder (juce::Graphics& g, juce::Rectangle<int> area, const juce::String& name, const float* db)
    {
        g.setFont (font (9.5f, true, 0.1f));
        g.setColour (col::creamDim);
        g.drawText (name, area.removeFromLeft (30), juce::Justification::centredLeft, false);

        constexpr float lo = -48.0f, hi = 3.0f;
        const int segs = juce::jmax (10, area.getWidth() / 6);
        const float sw = (float) area.getWidth() / (float) segs;

        for (int ch = 0; ch < 2; ++ch)
        {
            auto row = area.removeFromTop (area.getHeight() / (2 - ch)).toFloat().reduced (0.0f, 2.0f);
            const int lit = juce::roundToInt (juce::jlimit (0.0f, 1.0f, (db[ch] - lo) / (hi - lo)) * (float) segs);
            for (int i = 0; i < segs; ++i)
            {
                const float segDb = lo + (hi - lo) * ((float) i + 0.5f) / (float) segs;
                const auto c = segDb > 0.0f ? col::red : (segDb > -9.0f ? col::gold : col::green);
                g.setColour (i < lit ? c.brighter (0.2f) : col::ledOff);
                g.fillRect (row.getX() + sw * (float) i + 0.5f, row.getY(), sw - 1.5f, row.getHeight());
            }
        }
    }
};
} // namespace atelier::ui
