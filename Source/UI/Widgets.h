#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

namespace atelier::ui
{
using APVTS = juce::AudioProcessorValueTreeState;

namespace col
{
    // Warm blacks and bronzes for the bodywork, champagne gold for the metal. The module colours are kept
    // for lamps, scale dots and title rules only, so the strip reads as one instrument, not nine.
    const juce::Colour faceplate { 0xff161513 }, panel { 0xff23211e }, panelEdge { 0xff0a0908 }, panelHi { 0xff8a7448 };
    const juce::Colour well { 0xff0d0c0b }, wellEdge { 0xff4b4233 }, ledOff { 0xff2e2b27 }, button { 0xff141311 };
    const juce::Colour cream { 0xffece2ca }, creamDim { 0xffa69d89 }, ink { 0xff15120d };
    const juce::Colour gold { 0xffc8a45e }, goldLight { 0xfff0dcaa }, goldDark { 0xff6f5628 };
    const juce::Colour red { 0xffbd5441 }, blue { 0xff5f8cb4 }, green { 0xff6a9d72 };
    const juce::Colour orange { 0xffd58a45 }, purple { 0xff8a78b4 }, ivory { 0xffd6ccb2 }, teal { 0xff58a09a };
}

/** Top-to-bottom shading of polished metal: a bright upper edge, the body colour, a darker belly and a little
    light bounced back along the bottom. */
inline juce::ColourGradient metal (juce::Colour base, float top, float bottom)
{
    juce::ColourGradient g (base.brighter (0.55f), 0.0f, top, base.darker (0.1f), 0.0f, bottom, false);
    g.addColour (0.35, base);
    g.addColour (0.78, base.darker (0.38f));
    return g;
}

/** A disc of spun metal: the brightness goes round the dial, as it does on a lathe-turned knob, with two
    highlights opposite each other. */
inline void paintSpunMetal (juce::Graphics& g, juce::Rectangle<float> disc, juce::Colour light, juce::Colour dark)
{
    constexpr int slices = 72;
    const float step = juce::MathConstants<float>::twoPi / (float) slices;
    for (int i = 0; i < slices; ++i)
    {
        const float a = step * (float) i;
        const float shine = 0.5f + 0.5f * std::cos (2.0f * (a + step * 0.5f + 0.7f));
        juce::Path slice;
        slice.addPieSegment (disc, a, a + step * 1.35f, 0.0f);
        g.setColour (dark.interpolatedWith (light, shine * shine));
        g.fillPath (slice);
    }
    // overall light from above, so it still reads as a solid object
    g.setGradientFill (juce::ColourGradient (juce::Colours::white.withAlpha (0.16f), 0.0f, disc.getY(),
                                             juce::Colours::black.withAlpha (0.30f), 0.0f, disc.getBottom(), false));
    g.fillEllipse (disc);
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
    g.setGradientFill (juce::ColourGradient (col::panel.brighter (0.06f), 0.0f, r.getY(), col::panel.darker (0.28f), 0.0f, r.getBottom(), false));
    g.fillRoundedRectangle (r, 6.0f);
    g.setColour (col::panelEdge);
    g.drawRoundedRectangle (r, 6.0f, 1.2f);
    // a brass inlay just inside the edge, catching the light along the top
    g.setGradientFill (juce::ColourGradient (col::panelHi.withAlpha (0.55f), 0.0f, r.getY(), col::goldDark.withAlpha (0.22f), 0.0f, r.getBottom(), false));
    g.drawRoundedRectangle (r.reduced (1.4f), 5.0f, 0.8f);
}

//==============================================================================
/** How a control looks and behaves. The form says what the control does: one large knob per module for the
    control you reach for first, trim pots for calibration you set once, a centre-detent arc for cut and boost,
    and a chicken-head pointer over printed values for controls with natural steps. */
struct ControlStyle
{
    enum Size { hero, standard, small, trim };
    enum Shape { rotary, fader, horizontal };

    Size size = standard;
    Shape shape = rotary;
    bool bipolar = false;                                   // the scale grows either way from `origin`
    double origin = 0.0;
    std::vector<std::pair<double, juce::String>> detents;   // printed steps; the knob snaps to them while dragged
    double snapNear = 0.0;                                  // for a slider: snap to `origin` within this distance

    static ControlStyle make (Size s)                       { ControlStyle c; c.size = s; return c; }
    static ControlStyle centred (double o = 0.0)            { ControlStyle c; c.bipolar = true; c.origin = o; return c; }
    static ControlStyle stepped (std::vector<std::pair<double, juce::String>> d) { ControlStyle c; c.detents = std::move (d); return c; }
};

/** A slider that knows its style, and snaps to its steps while dragged (hold Option/Alt to move freely). */
class StyledSlider final : public juce::Slider
{
public:
    ControlStyle style;

    double snapValue (double attempted, DragMode mode) override
    {
        if (mode == notDragging || juce::ModifierKeys::currentModifiers.isAltDown()) return attempted;

        if (! style.detents.empty())
        {
            double best = attempted, distance = 1.0e9;
            const double at = valueToProportionOfLength (attempted);
            for (auto& d : style.detents)
                if (const double gap = std::abs (valueToProportionOfLength (d.first) - at); gap < distance) { distance = gap; best = d.first; }
            return best;
        }
        if (style.snapNear > 0.0 && std::abs (attempted - style.origin) < style.snapNear) return style.origin;
        return attempted;
    }
};

/** A chicken-head pointer knob: a dark enamel pointer with an engraved line, on a brass plate. */
inline void paintChickenHead (juce::Graphics& g, juce::Point<float> centre, float r, float angle)
{
    g.setColour (juce::Colours::black.withAlpha (0.45f));
    g.fillEllipse (juce::Rectangle<float> (r * 1.5f, r * 1.5f).withCentre (centre.translated (0.0f, 2.5f)));
    auto plate = juce::Rectangle<float> (r * 1.42f, r * 1.42f).withCentre (centre);
    paintSpunMetal (g, plate, col::goldLight.darker (0.15f), col::goldDark.darker (0.3f));
    g.setColour (col::ink.withAlpha (0.8f));
    g.drawEllipse (plate, 1.0f);

    juce::Path head;
    head.addEllipse (-r * 0.5f, -r * 0.5f, r, r);
    head.startNewSubPath (-r * 0.2f, -r * 0.3f);
    head.quadraticTo (-r * 0.12f, -r * 0.8f, 0.0f, -r * 1.0f);
    head.quadraticTo (r * 0.12f, -r * 0.8f, r * 0.2f, -r * 0.3f);
    head.closeSubPath();
    const auto turn = juce::AffineTransform::rotation (angle).translated (centre);

    g.setColour (juce::Colours::black.withAlpha (0.5f));
    g.fillPath (head, juce::AffineTransform::rotation (angle).translated (centre.translated (0.0f, 2.0f)));
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff3a332b), centre.x, centre.y - r, juce::Colour (0xff0c0a08), centre.x, centre.y + r, false));
    g.fillPath (head, turn);
    g.setColour (col::goldLight.withAlpha (0.25f));
    g.strokePath (head, juce::PathStrokeType (0.8f), turn);

    juce::Path line;
    line.startNewSubPath (0.0f, -r * 0.15f);
    line.lineTo (0.0f, -r * 0.88f);
    g.setColour (col::cream);
    g.strokePath (line, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded), turn);
}

/** Printed legends round a knob: a tick and a label at each step, the chosen one picked out. */
inline void paintLegends (juce::Graphics& g, juce::Point<float> centre, float r, const std::vector<std::pair<float, juce::String>>& marks, int chosen, juce::Colour accent)
{
    g.setFont (font (8.5f, true));
    for (int i = 0; i < (int) marks.size(); ++i)
    {
        const float a = marks[(size_t) i].first;
        const bool on = i == chosen;
        g.setColour (on ? accent.brighter (0.4f) : col::creamDim.withAlpha (0.7f));
        g.drawLine ({ centre.getPointOnCircumference (r + 2.0f, a), centre.getPointOnCircumference (r + (on ? 7.0f : 5.5f), a) }, on ? 1.6f : 1.0f);
        if (marks[(size_t) i].second.isNotEmpty())
        {
            g.setColour (on ? col::cream : col::creamDim);
            g.drawText (marks[(size_t) i].second, juce::Rectangle<float> (46.0f, 11.0f).withCentre (centre.getPointOnCircumference (r + 15.0f, a)),
                        juce::Justification::centred, false);
        }
    }
}

//==============================================================================
class AtelierLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    AtelierLookAndFeel()
    {
        setColour (juce::ComboBox::backgroundColourId, col::button);
        setColour (juce::ComboBox::textColourId, col::cream);
        setColour (juce::ComboBox::outlineColourId, col::wellEdge);
        setColour (juce::ComboBox::arrowColourId, col::creamDim);
        setColour (juce::PopupMenu::backgroundColourId, juce::Colour (0xff1d1b18));
        setColour (juce::PopupMenu::textColourId, col::cream);
        setColour (juce::PopupMenu::highlightedBackgroundColourId, col::gold);
        setColour (juce::PopupMenu::highlightedTextColourId, juce::Colours::black);
    }

    void drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos,
                           float startAngle, float endAngle, juce::Slider& slider) override
    {
        const auto* styled = dynamic_cast<StyledSlider*> (&slider);
        const ControlStyle style = styled != nullptr ? styled->style : ControlStyle();
        const auto accent = slider.findColour (juce::Slider::thumbColourId);
        const auto area = juce::Rectangle<int> (x, y, w, h).toFloat();
        const auto centre = area.getCentre();
        const float full = juce::jmin (area.getWidth(), area.getHeight()) * 0.5f - 9.0f;
        if (full < 6.0f) return;
        const float angle = startAngle + pos * (endAngle - startAngle);
        auto angleOf = [&] (double v) { return startAngle + (float) slider.valueToProportionOfLength (v) * (endAngle - startAngle); };

        if (! style.detents.empty())
        {
            // printed steps round a chicken-head pointer
            const float r = full * 0.62f;
            std::vector<std::pair<float, juce::String>> marks;
            int chosen = -1; double nearest = 1.0e9;
            for (auto& d : style.detents)
            {
                marks.push_back ({ angleOf (d.first), d.second });
                if (const double gap = std::abs (d.first - slider.getValue()); gap < nearest) { nearest = gap; chosen = (int) marks.size() - 1; }
            }
            if (nearest > 1.0e-3 * (slider.getMaximum() - slider.getMinimum())) chosen = -1;     // between steps (Option-dragged or automated)
            paintLegends (g, centre, r, marks, chosen, accent);
            paintChickenHead (g, centre, r * 0.92f, angle);
            return;
        }

        if (style.size == ControlStyle::trim)
        {
            // a recessed trim pot: set it once with a screwdriver
            const float r = full * 0.46f;
            for (int i = 0; i < 7; ++i)
            {
                const float t = (float) i / 6.0f;
                const auto p = centre.getPointOnCircumference (r * 1.55f, startAngle + t * (endAngle - startAngle));
                g.setColour (t <= pos + 0.001f ? accent.withMultipliedBrightness (1.1f) : col::ledOff.brighter (0.15f));
                g.fillEllipse (p.x - 1.1f, p.y - 1.1f, 2.2f, 2.2f);
            }
            auto well = juce::Rectangle<float> (r * 2.5f, r * 2.5f).withCentre (centre);
            g.setGradientFill (juce::ColourGradient (juce::Colours::black, centre.x, well.getY(), col::panel.brighter (0.1f), centre.x, well.getBottom(), false));
            g.fillEllipse (well);
            auto head = juce::Rectangle<float> (r * 2.0f, r * 2.0f).withCentre (centre);
            paintSpunMetal (g, head, col::goldLight.darker (0.1f), col::goldDark);
            g.setColour (col::ink.withAlpha (0.7f));
            g.drawEllipse (head, 0.8f);
            juce::Path slot;
            slot.addRoundedRectangle (-r * 0.8f, -r * 0.13f, r * 1.6f, r * 0.26f, r * 0.1f);
            const auto turn = juce::AffineTransform::rotation (angle + juce::MathConstants<float>::halfPi).translated (centre);
            g.setColour (col::goldLight.withAlpha (0.45f));
            g.fillPath (slot, juce::AffineTransform::translation (0.0f, 0.7f).followedBy (turn));
            g.setColour (col::ink);
            g.fillPath (slot, turn);
            // the pointer end of the slot, so the setting can be read
            g.setColour (accent.brighter (0.3f));
            g.fillEllipse (juce::Rectangle<float> (2.4f, 2.4f).withCentre (centre.getPointOnCircumference (r * 0.62f, angle)));
            return;
        }

        const float scale = style.size == ControlStyle::hero ? 1.0f : (style.size == ControlStyle::small ? 0.7f : 0.84f);
        const float radius = full * scale;

        // scale dots: from the start, or for a centre-detent control from its centre out to the setting
        const float originT = style.bipolar ? (float) slider.valueToProportionOfLength (style.origin) : 0.0f;
        for (int i = 0; i < 11; ++i)
        {
            const float t = (float) i / 10.0f;
            const auto p = centre.getPointOnCircumference (radius + 5.5f, startAngle + t * (endAngle - startAngle));
            const bool lit = style.bipolar ? (t >= juce::jmin (originT, pos) - 0.001f && t <= juce::jmax (originT, pos) + 0.001f)
                                           : t <= pos + 0.001f;
            const bool isOrigin = style.bipolar && std::abs (t - originT) < 0.05f;
            g.setColour (lit ? accent.withMultipliedBrightness (1.15f) : (isOrigin ? col::creamDim : col::ledOff.brighter (0.15f)));
            const float d = isOrigin ? 4.0f : 2.8f;
            g.fillEllipse (p.x - d * 0.5f, p.y - d * 0.5f, d, d);
        }

        g.setColour (juce::Colours::black.withAlpha (0.45f));
        g.fillEllipse (centre.x - radius, centre.y - radius + 3.0f, radius * 2.0f, radius * 2.0f);

        auto skirt = juce::Rectangle<float> (radius * 2.0f, radius * 2.0f).withCentre (centre);
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff4d4840), centre.x, skirt.getY(),
                                                 juce::Colour (0xff100f0d), centre.x, skirt.getBottom(), false));
        g.fillEllipse (skirt);

        if (style.size == ControlStyle::hero)
        {
            // knurled grip round the edge, turning with the knob
            constexpr int ridges = 40;
            for (int k = 0; k < ridges; ++k)
            {
                const float a = angle + juce::MathConstants<float>::twoPi * (float) k / (float) ridges;
                const float lightness = 0.5f + 0.5f * std::cos (a - juce::MathConstants<float>::pi * 1.25f);
                g.setColour (col::goldDark.interpolatedWith (col::goldLight, 0.15f + 0.5f * lightness).withAlpha (0.85f));
                g.drawLine ({ centre.getPointOnCircumference (radius * 0.84f, a), centre.getPointOnCircumference (radius * 0.98f, a) }, 1.4f);
            }
        }
        g.setColour (juce::Colours::black.withAlpha (0.8f));
        g.drawEllipse (skirt, 1.0f);

        // the cap is turned brass; the module colour lives in the scale dots around it
        auto cap = skirt.reduced (radius * (style.size == ControlStyle::hero ? 0.22f : 0.2f));
        g.setColour (col::goldDark.darker (0.6f));
        g.fillEllipse (cap.expanded (1.2f));
        paintSpunMetal (g, cap, col::goldLight, col::goldDark);
        g.setColour (col::goldLight.withAlpha (0.55f));
        g.drawEllipse (cap.reduced (0.5f), 0.9f);
        g.setColour (col::goldDark.darker (0.4f).withAlpha (0.55f));
        g.drawEllipse (cap.reduced (radius * 0.13f), 0.8f);

        // engraved pointer, filled with dark enamel
        const auto turn = juce::AffineTransform::rotation (angle).translated (centre);
        juce::Path pointer;
        pointer.addRoundedRectangle (-1.6f, -radius * 0.76f, 3.2f, radius * 0.48f, 1.6f);
        g.setColour (col::goldLight.withAlpha (0.5f));
        g.fillPath (pointer, juce::AffineTransform::translation (0.0f, 0.8f).followedBy (turn));
        g.setColour (col::ink);
        g.fillPath (pointer, turn);
    }

    void drawLinearSlider (juce::Graphics& g, int x, int y, int w, int h, float sliderPos, float, float,
                           juce::Slider::SliderStyle sliderStyle, juce::Slider& slider) override
    {
        const auto* styled = dynamic_cast<StyledSlider*> (&slider);
        const ControlStyle style = styled != nullptr ? styled->style : ControlStyle();
        const auto accent = slider.findColour (juce::Slider::thumbColourId);
        const bool vertical = sliderStyle == juce::Slider::LinearVertical;
        const auto area = juce::Rectangle<int> (x, y, w, h).toFloat();

        // the slot, with a scale printed beside it: a fader's to the right, a horizontal slider's underneath
        auto slot = vertical ? juce::Rectangle<float> (5.0f, area.getHeight()).withCentre ({ area.getX() + 17.0f, area.getCentreY() })
                             : juce::Rectangle<float> (area.getWidth(), 5.0f).withCentre ({ area.getCentreX(), area.getY() + 14.0f });
        g.setColour (juce::Colours::black);
        g.fillRoundedRectangle (slot, 2.5f);
        g.setColour (col::wellEdge);
        g.drawRoundedRectangle (slot, 2.5f, 0.8f);

        auto at = [&] (double v) { const float t = (float) slider.valueToProportionOfLength (v);
                                   return vertical ? area.getBottom() - t * area.getHeight() : area.getX() + t * area.getWidth(); };
        g.setFont (font (8.0f, true));
        for (auto& d : style.detents)
        {
            const float p = at (d.first);
            const bool origin = style.bipolar && std::abs (d.first - style.origin) < 1.0e-6;
            g.setColour (origin ? col::cream : col::creamDim.withAlpha (0.75f));
            if (vertical)
            {
                g.drawLine (slot.getRight() + 13.0f, p, slot.getRight() + (origin ? 21.0f : 18.0f), p, origin ? 1.4f : 1.0f);
                g.drawText (d.second, juce::Rectangle<float> (slot.getRight() + 23.0f, p - 5.5f, 26.0f, 11.0f), juce::Justification::centredLeft, false);
            }
            else
            {
                g.drawLine (p, slot.getBottom() + 12.0f, p, slot.getBottom() + (origin ? 18.0f : 15.0f), origin ? 1.4f : 1.0f);
                const auto just = &d == &style.detents.front() ? juce::Justification::centredLeft
                                : &d == &style.detents.back() ? juce::Justification::centredRight : juce::Justification::centred;
                auto box = juce::Rectangle<float> (40.0f, 10.0f).withCentre ({ p, slot.getBottom() + 24.0f });
                if (just == juce::Justification::centredLeft) box.setX (p - 2.0f);
                if (just == juce::Justification::centredRight) box.setRight (p + 2.0f);
                g.drawText (d.second, box, just, false);
            }
        }

        // the lit part of the slot, from the origin to the setting
        const float from = at (style.bipolar ? style.origin : slider.getMinimum());
        g.setColour (accent.withAlpha (0.8f));
        if (vertical) g.fillRect (juce::Rectangle<float> (slot.getX() + 1.0f, juce::jmin (from, sliderPos), 3.0f, std::abs (from - sliderPos)));
        else          g.fillRect (juce::Rectangle<float> (juce::jmin (from, sliderPos), slot.getY() + 1.0f, std::abs (from - sliderPos), 3.0f));

        // brass cap with grip grooves
        auto cap = vertical ? juce::Rectangle<float> (30.0f, 15.0f).withCentre ({ slot.getCentreX(), sliderPos })
                            : juce::Rectangle<float> (13.0f, 24.0f).withCentre ({ sliderPos, slot.getCentreY() });
        g.setColour (juce::Colours::black.withAlpha (0.45f));
        g.fillRoundedRectangle (cap.translated (0.0f, 2.0f), 2.5f);
        g.setGradientFill (metal (col::gold, cap.getY(), cap.getBottom()));
        g.fillRoundedRectangle (cap, 2.5f);
        g.setColour (col::ink.withAlpha (0.75f));
        g.drawRoundedRectangle (cap, 2.5f, 0.8f);
        for (int k = -1; k <= 1; ++k)
        {
            g.setColour (col::ink.withAlpha (k == 0 ? 0.9f : 0.45f));
            if (vertical) g.drawHorizontalLine (juce::roundToInt (cap.getCentreY()) + k * 3, cap.getX() + 4.0f, cap.getRight() - 4.0f);
            else          g.drawVerticalLine (juce::roundToInt (cap.getCentreX()) + k * 3, cap.getY() + 4.0f, cap.getBottom() - 4.0f);
        }
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
    Knob (APVTS& state, const juce::String& paramId, const juce::String& caption, juce::Colour accent, ControlStyle style = {})
        : text (caption)
    {
        slider.style = style;
        slider.setSliderStyle (style.shape == ControlStyle::fader ? juce::Slider::LinearVertical
                             : style.shape == ControlStyle::horizontal ? juce::Slider::LinearHorizontal
                                                                       : juce::Slider::RotaryHorizontalVerticalDrag);
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

    void resized() override
    {
        auto r = getLocalBounds().withTrimmedBottom (15);
        if (slider.style.shape == ControlStyle::fader)              r = r.withSizeKeepingCentre (juce::jmin (70, r.getWidth()), r.getHeight() - 10);
        else if (slider.style.shape == ControlStyle::horizontal)    r = r.withSizeKeepingCentre (r.getWidth() - 12, 44);
        slider.setBounds (r);
    }

    void paint (juce::Graphics& g) override
    {
        const bool active = slider.isMouseOverOrDragging();
        g.setFont (font (10.5f, true, active ? 0.0f : 0.08f));
        g.setColour (active ? col::cream : col::creamDim);
        g.drawFittedText (active ? slider.getTextFromValue (slider.getValue()) : text,
                          getLocalBounds().removeFromBottom (15), juce::Justification::centred, 1, 0.75f);
    }

    void mouseEnter (const juce::MouseEvent&) override  { repaint(); }
    void mouseExit (const juce::MouseEvent&) override   { repaint(); }
    void mouseUp (const juce::MouseEvent&) override     { repaint(); }

    StyledSlider slider;

private:
    juce::String text;
    std::unique_ptr<APVTS::SliderAttachment> attachment;
};

//==============================================================================
/** A rotary selector for a choice parameter: a chicken-head pointer over the printed choices.
    Click a legend to go straight to it; click the knob or drag to step through them. */
class RotarySwitch final : public juce::Component
{
public:
    RotarySwitch (APVTS& state, const juce::String& paramId, const juce::String& caption, juce::Colour accentColour, juce::StringArray shortNames = {})
        : text (caption), accent (accentColour), param (*state.getParameter (paramId)),
          attachment (param, [this] (float v) { index = juce::roundToInt (v); repaint(); })
    {
        if (auto* choice = dynamic_cast<juce::AudioParameterChoice*> (&param)) names = choice->choices;
        if (shortNames.size() == names.size()) names = shortNames;
        setHelpId (*this, paramId);
        attachment.sendInitialUpdate();
    }

    void paint (juce::Graphics& g) override
    {
        const auto lay = layout();
        g.setFont (legendFont());
        for (int i = 0; i < names.size(); ++i)
        {
            const bool on = i == index;
            const float a = angleOf (i);
            g.setColour (on ? accent.brighter (0.4f) : col::creamDim.withAlpha (0.6f));
            g.drawLine ({ lay.centre.getPointOnCircumference (lay.r + 2.0f, a), lay.centre.getPointOnCircumference (lay.r + (on ? 7.0f : 5.0f), a) }, on ? 1.6f : 1.0f);
            g.setColour (on ? col::cream : col::creamDim);
            g.drawText (names[i], lay.labels[(size_t) i], juce::Justification::centredLeft, false);
        }
        paintChickenHead (g, lay.centre, lay.r * 0.9f, angleOf (index));

        g.setFont (font (10.5f, true, 0.08f));
        g.setColour (col::creamDim);
        g.drawFittedText (text, getLocalBounds().removeFromBottom (15), juce::Justification::centred, 1, 0.8f);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        dragStart = e.position.y; startIndex = index; dragged = false;
        const auto lay = layout();
        for (int i = 0; i < names.size(); ++i)          // on a legend: go straight there
            if (lay.labels[(size_t) i].expanded (2.0f, 2.0f).contains (e.position)) { choose (i); dragged = true; return; }
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        const int steps = juce::roundToInt ((e.position.y - dragStart) / 16.0f);     // down the list, as the legends run
        if (steps != 0) dragged = true;
        choose (juce::jlimit (0, names.size() - 1, startIndex + steps));
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (! dragged && ! e.mouseWasDraggedSinceMouseDown())
            choose (e.mods.isShiftDown() ? (index + names.size() - 1) % names.size() : (index + 1) % names.size());
    }

    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w) override
    {
        if (w.deltaY != 0.0f) choose (juce::jlimit (0, names.size() - 1, index + (w.deltaY > 0.0f ? -1 : 1)));
    }

private:
    static juce::Font legendFont()      { return font (9.0f, true, 0.02f); }

    struct Layout { juce::Point<float> centre; float r; std::vector<juce::Rectangle<float>> labels; };

    /** The knob on the left, its legends stacked to its right, the whole group centred in the cell. */
    Layout layout() const
    {
        auto area = getLocalBounds().withTrimmedBottom (15).toFloat().reduced (2.0f, 4.0f);
        float widest = 0.0f;
        for (auto& n : names) widest = juce::jmax (widest, juce::GlyphArrangement::getStringWidth (legendFont(), n));
        const float r = juce::jlimit (9.0f, 17.0f, juce::jmin (area.getHeight() * 0.22f, (area.getWidth() - widest - 14.0f) * 0.5f));
        const float reach = r + 10.0f;
        const float groupW = 2.0f * r + 10.0f + widest;
        Layout lay;
        lay.r = r;
        lay.centre = { area.getCentreX() - groupW * 0.5f + r, area.getCentreY() };
        for (int i = 0; i < names.size(); ++i)
        {
            const auto tip = lay.centre.getPointOnCircumference (reach, angleOf (i));
            lay.labels.push_back ({ juce::jmax (tip.x, lay.centre.x + r + 6.0f), tip.y - 6.0f, widest + 4.0f, 12.0f });
        }
        return lay;
    }

    /** The pointer swings across the right-hand side, from up-right to down-right. */
    float angleOf (int i) const
    {
        const int n = juce::jmax (2, names.size());
        const float spread = juce::degreesToRadians (n <= 3 ? 90.0f : 120.0f);
        return juce::MathConstants<float>::halfPi - spread * 0.5f + spread * (float) i / (float) (n - 1);
    }

    void choose (int i)     { if (i != index) attachment.setValueAsCompleteGesture ((float) i); }

    juce::String text;
    juce::Colour accent;
    juce::RangedAudioParameter& param;
    juce::StringArray names;
    int index = 0, startIndex = 0;
    float dragStart = 0.0f;
    bool dragged = false;
    juce::ParameterAttachment attachment;
};

//==============================================================================
/** A needle gain-reduction meter: the needle rests at 0 on the right and swings left as the gain comes down. */
class NeedleMeter final : public juce::Component
{
public:
    explicit NeedleMeter (float rangeDb) : range (rangeDb) {}

    void set (float db)     { db = juce::jlimit (0.0f, range, db); if (std::abs (db - gr) > 0.02f) { gr = db; repaint(); } }

    void paint (juce::Graphics& g) override
    {
        auto face = getLocalBounds().toFloat().reduced (4.0f, 6.0f);
        g.setColour (juce::Colours::black.withAlpha (0.5f));
        g.fillRoundedRectangle (face.expanded (2.0f), 5.0f);
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xfff3e9cb), face.getCentreX(), face.getBottom(),
                                                 juce::Colour (0xffc9b98c), face.getX(), face.getY(), true));
        g.fillRoundedRectangle (face, 4.0f);

        juce::Graphics::ScopedSaveState save (g);
        g.reduceClipRegion (face.toNearestInt());
        const float sweep = juce::degreesToRadians (40.0f);
        const float R = juce::jmin (face.getHeight() * 0.95f, (face.getWidth() * 0.5f - 18.0f) / std::sin (sweep));
        const juce::Point<float> pivot (face.getCentreX(), face.getY() + R + 24.0f);
        auto angleFor = [&] (float db) { return sweep - 2.0f * sweep * std::log10 (1.0f + db) / std::log10 (1.0f + range); };
        const juce::Colour ink (0xff2a2520);

        juce::Path arc;
        arc.addCentredArc (pivot.x, pivot.y, R, R, 0.0f, angleFor (range), angleFor (0.0f), true);
        g.setColour (ink);
        g.strokePath (arc, juce::PathStrokeType (1.4f));
        g.setFont (font (8.5f, true));
        for (float m : { 0.0f, 1.0f, 2.0f, 3.0f, 5.0f, 7.0f, 10.0f, 20.0f })
        {
            if (m > range) continue;
            const float a = angleFor (m);
            g.drawLine ({ pivot.getPointOnCircumference (R, a), pivot.getPointOnCircumference (R + 6.0f, a) }, 1.2f);
            g.drawText (juce::String ((int) m), juce::Rectangle<float> (20.0f, 11.0f).withCentre (pivot.getPointOnCircumference (R + 14.0f, a)),
                        juce::Justification::centred, false);
        }
        g.setFont (font (8.0f, true, 0.2f));
        g.setColour (ink.withAlpha (0.8f));
        g.drawText ("GAIN REDUCTION  dB", juce::Rectangle<float> (face.getWidth(), 12.0f).withCentre ({ face.getCentreX(), pivot.y - R * 0.45f }), juce::Justification::centred, false);

        const float a = angleFor (gr);
        g.setColour (juce::Colours::black.withAlpha (0.18f));
        g.drawLine ({ pivot.translated (2.0f, 3.0f), pivot.getPointOnCircumference (R + 3.0f, a).translated (2.0f, 3.0f) }, 1.8f);
        g.setColour (juce::Colour (0xff151210));
        g.drawLine ({ pivot, pivot.getPointOnCircumference (R + 3.0f, a) }, 1.4f);

        g.setColour (juce::Colours::black.withAlpha (0.35f));
        g.drawRoundedRectangle (face.reduced (0.5f), 4.0f, 1.6f);
    }

private:
    float range, gr = 0.0f;
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

        if (lit) g.setGradientFill (metal (accent, r.getY(), r.getBottom()));
        else     g.setColour (col::button.brighter (over ? 0.08f : 0.0f));
        g.fillRoundedRectangle (r, 3.0f);
        g.setColour (lit ? accent.brighter (0.5f) : col::wellEdge);
        g.drawRoundedRectangle (r, 3.0f, 1.0f);
        g.setColour (lit ? col::ink : col::creamDim);
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
        if (lit) g.setGradientFill (metal (accent, r.getY(), r.getBottom()));
        else     g.setColour (col::button.brighter (over ? 0.08f : 0.0f));
        g.fillRoundedRectangle (r, 3.0f);
        g.setColour (lit ? accent.brighter (0.5f) : col::wellEdge);
        g.drawRoundedRectangle (r, 3.0f, 1.0f);
        g.setColour (lit ? col::ink : col::creamDim);
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
        g.setColour (juce::Colour (0xff0e0d0c));
        g.fillPath (glass);
        g.setGradientFill (juce::ColourGradient (glow.withAlpha (0.25f + 0.75f * level), r.getCentreX(), r.getBottom() - 14.0f,
                                                 glow.withAlpha (0.03f), r.getCentreX(), r.getY(), true));
        g.fillPath (glass);
        g.setColour (juce::Colours::white.withAlpha (0.22f));
        g.strokePath (glass, juce::PathStrokeType (1.0f));

        g.setColour (juce::Colour (0xff2b2925).withAlpha (0.9f));
        g.fillRoundedRectangle (r.getCentreX() - 7.0f, r.getY() + 16.0f, 14.0f, 30.0f, 2.0f);
        g.setColour (glow.brighter (0.6f).withAlpha (0.35f + 0.65f * level));
        g.fillRect (r.getCentreX() - 0.75f, r.getY() + 20.0f, 1.5f, 22.0f);
        g.setColour (juce::Colour (0xff4b4233));
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

    void addCell (juce::Component* c, int span = 1)     { cells.add (c); spans.add (span); if (c != nullptr) addAndMakeVisible (c); }
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

        for (int i = 0, slot = 0; i < cells.size(); slot += spans[i], ++i)
            if (auto* c = cells[i])
                c->setBounds (juce::Rectangle<float> ((float) grid.getX() + cw * (float) (slot % cols),
                                                      (float) grid.getY() + rh * (float) (slot / cols), cw * (float) spans[i], rh)
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
        // the title gives way to any header controls, tightening its letter-spacing rather than being cut off
        int titleRight = getWidth() - pad;
        for (auto& h : headers) if (h.comp->isVisible()) titleRight = juce::jmin (titleRight, h.comp->getX() - 4);
        const int titleX = pad + (power != nullptr ? 24 : 2);
        g.drawFittedText (title, titleX, 0, titleRight - titleX, titleH, juce::Justification::centredLeft, 1, 0.8f);

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
    juce::Array<int> spans;
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
                g.setGradientFill (metal (accent, b.getY(), b.getBottom()));
                g.fillRoundedRectangle (b, 3.5f);
            }
            g.setColour (lit ? col::ink : col::creamDim);
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
