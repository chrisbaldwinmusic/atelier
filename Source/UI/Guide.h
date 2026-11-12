#pragma once

#include "GuideData.h"
#include "Widgets.h"

namespace atelier::ui
{
//==============================================================================
/** Live feedback: looks at the meters and settings and says the single most useful thing. */
struct Coach
{
    enum Level { ok, note, warn };

    struct Advice
    {
        Level level = ok;
        juce::String text { "Play some audio and I'll comment on what the strip is doing." };
        bool operator!= (const Advice& o) const   { return level != o.level || text != o.text; }
    };

    struct Readings
    {
        float inPeakDb = -100, outPeakDb = -100, vu = -40, grComp = 0, grDeEss = 0, grLim = 0, grClip = 0, heat = 0, lufsS = -100;
        float keyPeakDb = -100, matchDb = 0;
        bool keyConnected = false, delta = false;
    };

    static Advice advise (APVTS& s, const Readings& m)
    {
        auto val = [&s] (const char* id) { return s.getRawParameterValue (id)->load(); };
        auto on  = [&val] (const char* id) { return val (id) > 0.5f; };
        const int mode = juce::jlimit (0, 2, (int) val ("mode"));
        const bool isMix = mode == 0;

        if (m.delta)
            return { warn, "Delta Listen is on: you are hearing only what the strip changes, not the signal itself. Switch it off before you bounce." };
        if (on ("bypass"))
        {
            if (on ("match"))
                return { note, "Bypassed and level-matched: the untouched signal is playing " + juce::String (std::abs (m.matchDb), 1) + " dB "
                                   + (m.matchDb >= 0.0f ? "louder" : "quieter") + " so both sound equally loud. Which one is actually better?" };
            return { note, "Bypassed. Is the processed version better, or just louder? Light MATCH in the Output section for a fair comparison." };
        }
        if (m.inPeakDb < -80.0f)
            return { ok, "Waiting for audio. Play the track and I'll comment on what the strip is doing." };
        if ((on ("compExt") && on ("compOn")) || (isMix && on ("gateExt") && on ("gateOn")))
        {
            if (! m.keyConnected)
                return { warn, "External Key is on, but the host has not connected a sidechain input, so the detector is listening to this track instead. Route a key track to the plugin's sidechain." };
            if (m.keyPeakDb < -70.0f)
                return { note, "External Key is on but nothing is arriving at the sidechain input yet. Check the key track is routed and playing." };
        }
        if (m.outPeakDb > 0.0f)
            return { warn, "Output is peaking at +" + juce::String (m.outPeakDb, 1) + " dBFS. Pull Output Trim down"
                               + (isMix ? "." : " or switch the Limiter on.") };
        if (m.inPeakDb > -1.0f)
            return { warn, "Input is very hot. The colour stages are calibrated for about 0 VU (-18 dBFS average); lower Input Gain for a cleaner, more controllable sound." };

        if (! isMix && on ("limOn") && m.grClip > 4.0f)
            return { warn, "The clipper is shaving " + juce::String (m.grClip, 1) + " dB off the peaks. Up to 2 or 3 dB is usually inaudible on drums; beyond that listen for a crunchy edge and lower Clip or Drive." };
        if (! isMix && on ("limOn") && m.grLim > (mode == 2 ? 6.0f : 4.0f))
            return { warn, "Limiter is removing " + juce::String (m.grLim, 1) + " dB. Past about 5 dB, kicks and snares lose punch; back off Drive or add density earlier with the compressor." };

        if (on ("compOn"))
        {
            static constexpr float heavy[3] = { 12.0f, 6.0f, 3.0f };
            static const char* why[3] = {
                "That is heavy control for one track. If it sounds squashed, raise the threshold or blend with Mix.",
                "That is a lot for a bus, where 1 to 4 dB usually glues. Raise the threshold or use Mix for parallel.",
                "That is a lot for a master, where 1 to 2 dB is typical. More than that means the mix needs attention." };
            if (m.grComp > heavy[mode])
                return { warn, "Compressor is at " + juce::String (m.grComp, 1) + " dB of gain reduction. " + why[mode] };
            if (m.grComp < 0.3f)
                return { note, "The compressor is on but barely moving. Lower the Threshold until the GR strip shows a few dB on the loud moments." };
        }

        if (isMix && on ("dsOn") && m.grDeEss > 8.0f)
            return { warn, "De-esser is cutting " + juce::String (m.grDeEss, 1) + " dB. Beyond about 6 dB voices start to lisp; raise its threshold." };
        if (isMix && on ("gateOn") && val ("gateRange") > 50.0f)
            return { note, "Gate Range is very deep. Fully silent gaps can sound unnatural; try 15 to 30 dB so a little room tone remains." };
        if (on ("tubeOn") && m.heat > 2.2f)
            return { note, "The valve is deep into saturation (see the lamp). Great for attitude; lower Tube Drive or Input Gain if you wanted warmth rather than grit." };
        if (! isMix && on ("imgOn") && val ("width") > 150.0f)
            return { note, "Width is above 150 %. Check the mix in mono; the centre (vocal, kick, bass) may be getting weak." };
        if (mode == 2 && m.lufsS > -8.0f)
            return { note, juce::String (m.lufsS, 1) + " LUFS short-term is very loud. Streaming services will turn it down to about -14, so the extra limiting costs dynamics for nothing." };
        if (m.vu < -12.0f)
            return { note, "Level through the strip is low. Raise Input Gain towards 0 VU so the tube, tape and compressor work in their intended range." };

        static const char* fine[3] = {
            "Levels look healthy. Hover any control to see what it does and what to listen for.",
            "The bus is sitting comfortably. Bypass now and then to check you're improving it, not just changing it.",
            "The master looks healthy. Compare against a reference track at matched loudness." };
        return { ok, fine[mode] };
    }
};

//==============================================================================
/** Resets the processing to neutral, then applies a source's starting point.
    Mode, bypass, input gain, output trim, polarity and the routing switches are left alone.

    Starting points are written for a track that peaks around -8 dBFS. If audio has been
    playing, `recentPeakDb` says how loud it really is, and everything that depends on level
    is moved to match: gate, de-esser and compressor thresholds follow the level, and the
    limiter drive is set so that it removes the intended amount. That way the starting point
    does roughly what its tips describe on any material, instead of nothing at all or far too much. */
inline void applyStartingPoint (juce::AudioProcessor& processor, const guide::Source& source, float recentPeakDb)
{
    static const juce::StringArray untouched { "mode", "bypass", "inGain", "outGain", "phase", "lowLat", "gateExt", "compExt", "match", "dither" };
    static const juce::StringArray switches { "gateOn", "tubeOn", "dsOn", "eqOn", "compOn", "tapeOn", "ironOn", "imgOn", "limOn" };
    static const juce::StringArray thresholds { "gateThr", "dsThr", "compThr" };

    constexpr float referencePeakDb = -8.0f;
    const bool heard = recentPeakDb > -60.0f;
    const float shift = heard ? juce::jlimit (-30.0f, 8.0f, recentPeakDb - referencePeakDb) : 0.0f;

    auto setting = [&source] (const char* id, float fallback)
    {
        for (auto& [key, value] : source.settings)
            if (juce::String (key) == id) return value;
        return fallback;
    };

    for (auto* base : processor.getParameters())
    {
        auto* p = dynamic_cast<juce::RangedAudioParameter*> (base);
        if (p == nullptr || untouched.contains (p->paramID)) continue;

        float target = p->getDefaultValue();
        if (switches.contains (p->paramID)) target = 0.0f;

        for (auto& [id, value] : source.settings)
            if (p->paramID == id)
            {
                float v = value;
                if (thresholds.contains (p->paramID))
                    v += shift;
                else if (p->paramID == "limDrive" && heard)     // written as "how much the limiter should remove"
                    v += setting ("limCeil", -1.0f) - recentPeakDb;

                const auto& range = p->getNormalisableRange();
                target = p->convertTo0to1 (juce::jlimit (range.start, range.end, v));
            }

        if (std::abs (target - p->getValue()) > 1.0e-6f)
        {
            p->beginChangeGesture();
            p->setValueNotifyingHost (target);
            p->endChangeGesture();
        }
    }
}

//==============================================================================
/** The teaching strip along the bottom: choose what you're working on, get tips and a
    starting point for it, and read about whichever control is under the pointer. */
class GuidePanel final : public juce::Component
{
public:
    GuidePanel (juce::AudioProcessor& p, APVTS& s) : processor (p), state (s)
    {
        for (auto& st : guide::styles) styleBox.addItem (st.name, styleBox.getNumItems() + 1);
        styleBox.setSelectedItemIndex (juce::jlimit (0, (int) guide::styles.size() - 1, (int) state.state.getProperty ("guide_style", 0)), juce::dontSendNotification);
        styleBox.onChange = [this] { state.state.setProperty ("guide_style", styleBox.getSelectedItemIndex(), nullptr); repaint(); };

        sourceBox.onChange = [this]
        {
            state.state.setProperty ("guide_source" + juce::String (mode), sourceBox.getSelectedItemIndex(), nullptr);
            repaint();
        };

        apply.onClick = [this] { applyStartingPoint (processor, source(), recentPeakDb); };
        setHelpId (apply, "guide.apply");

        addAndMakeVisible (sourceBox);
        addAndMakeVisible (styleBox);
        addAndMakeVisible (apply);
        setMode (0);
    }

    void setMode (int newMode)
    {
        mode = juce::jlimit (0, 2, newMode);
        auto& list = guide::sourcesFor (mode);
        sourceBox.clear (juce::dontSendNotification);
        for (auto& src : list) sourceBox.addItem (src.name, sourceBox.getNumItems() + 1);
        sourceBox.setSelectedItemIndex (juce::jlimit (0, (int) list.size() - 1, (int) state.state.getProperty ("guide_source" + juce::String (mode), 0)),
                                        juce::dontSendNotification);
        repaint();
    }

    /** Loudest input peak of the last several seconds (dBFS), kept up to date by the editor. */
    float recentPeakDb = -100.0f;

    void setHovered (const juce::String& id)        { if (id != hovered) { hovered = id; repaint(); } }
    void setCoach (const Coach::Advice& a)          { if (coach != a) { coach = a; repaint(); } }
    void refreshValue()                             { if (hovered.isNotEmpty()) repaint (lessonArea()); }

    void resized() override
    {
        auto left = getLocalBounds().reduced (12, 10).removeFromLeft (leftW - 24);
        left.removeFromTop (44);
        sourceBox.setBounds (left.removeFromTop (24));
        left.removeFromTop (19);
        styleBox.setBounds (left.removeFromTop (24));
        apply.setBounds (left.removeFromBottom (28));
    }

    void paint (juce::Graphics& g) override
    {
        paintPanel (g, getLocalBounds().toFloat());
        g.setColour (col::wellEdge.withAlpha (0.6f));
        g.fillRect (leftW, 12, 1, getHeight() - 24);
        g.fillRect (getWidth() - rightW, 12, 1, getHeight() - 24);

        // ---- left: what are you working on? ----
        auto left = getLocalBounds().reduced (12, 10).removeFromLeft (leftW - 24);
        g.setColour (col::cream);
        g.setFont (font (12.5f, true, 0.16f));
        g.drawText ("GUIDE", left.removeFromTop (18), juce::Justification::centredLeft, false);
        g.setColour (col::gold);
        g.fillRect (left.removeFromTop (5).withTrimmedTop (3).withHeight (2).toFloat().withHeight (1.5f));
        g.setColour (col::creamDim);
        g.setFont (font (9.0f, true, 0.12f));
        static const char* kinds[3] = { "SOURCE", "GROUP", "TARGET" };
        g.drawText (kinds[mode], left.removeFromTop (21).withTrimmedTop (6), juce::Justification::centredLeft, false);
        left.removeFromTop (24);
        g.drawText ("STYLE", left.removeFromTop (19).withTrimmedTop (4), juce::Justification::centredLeft, false);

        // ---- middle: tips for this source and style ----
        auto mid = getLocalBounds().reduced (0, 10).withTrimmedLeft (leftW + 14).withTrimmedRight (rightW + 14).toFloat();
        auto& src = source();
        auto& style = guide::styles[(size_t) juce::jlimit (0, (int) guide::styles.size() - 1, styleBox.getSelectedItemIndex())];

        auto head = mid.removeFromTop (20.0f);
        g.setColour (col::gold);
        g.setFont (font (12.5f, true, 0.16f));
        g.drawText (juce::String (src.name).toUpperCase(), head, juce::Justification::centredLeft, false);
        if (styleBox.getSelectedItemIndex() > 0)
        {
            g.setColour (col::creamDim);
            g.setFont (font (9.5f, true, 0.12f));
            g.drawText (juce::String (style.name).toUpperCase(), head, juce::Justification::centredRight, false);
        }
        mid.removeFromTop (3.0f);

        auto bullet = [&] (const char* text, juce::Colour textCol, juce::Colour dot)
        {
            g.setColour (dot);
            g.fillEllipse (mid.getX(), mid.getY() + 5.5f, 4.0f, 4.0f);
            const float h = drawWrapped (g, text, font (11.5f), textCol, mid.withTrimmedLeft (11.0f));
            mid.removeFromTop (h + 4.5f);
        };
        for (auto* tip : src.tips) bullet (tip, col::cream.withAlpha (0.92f), col::gold);
        if (style.notes[mode][0] != 0) bullet (style.notes[mode], col::gold.brighter (0.15f), col::cream);

        // ---- right: what does this control do? ----
        auto right = lessonArea().toFloat();
        guide::Help help = guide::modeOverview[mode];
        juce::String value;

        if (hovered == "guide.apply")
        {
            applyHelp = "Resets the processing and dials in a sensible starting point for " + juce::String (src.name)
                      + ". Input Gain, Output Trim, polarity and the mode are left alone."
                      + (mode == 2 ? " On a master the limiter is driven until your peaks reach the ceiling, so expect it to get louder." : "");
            help = { "", "Apply Starting Point", applyHelp.toRawUTF8(),
                     "It is a starting point, not a preset. Play the track for a few seconds first: thresholds and limiter drive are then fitted to how loud it really is, so the meters should land near the amounts in the tips. Tweak from there." };
        }
        else if (auto* h = guide::findHelp (hovered.toRawUTF8()))
        {
            help = *h;
            if (auto* p = state.getParameter (hovered); p != nullptr && hovered != "mode")
                value = dynamic_cast<juce::AudioParameterBool*> (p) != nullptr ? (p->getValue() > 0.5f ? "ON" : "OFF")
                                                                                 : p->getCurrentValueAsText();
        }

        auto titleRow = right.removeFromTop (18.0f);
        g.setColour (col::gold);
        g.setFont (font (11.5f, true));
        g.drawText (value, titleRow, juce::Justification::centredRight, false);
        g.setColour (col::cream);
        g.setFont (font (11.5f, true, 0.12f));
        g.drawText (juce::String (help.title).toUpperCase(), titleRow.withTrimmedRight (value.isEmpty() ? 0.0f : 70.0f), juce::Justification::centredLeft, true);
        right.removeFromTop (3.0f);

        auto coachBox = right.removeFromBottom (48.0f);
        right.removeFromBottom (4.0f);
        right.removeFromTop (drawWrapped (g, help.what, font (11.5f), col::cream.withAlpha (0.9f), right) + 5.0f);
        drawWrapped (g, help.tip, font (11.5f).italicised(), col::gold.brighter (0.15f), right);

        // ---- live coach ----
        const auto level = coach.level == Coach::warn ? col::red : (coach.level == Coach::note ? col::gold : col::green);
        g.setColour (col::well);
        g.fillRoundedRectangle (coachBox, 4.0f);
        g.setColour (level.withAlpha (0.6f));
        g.drawRoundedRectangle (coachBox.reduced (0.5f), 4.0f, 1.0f);
        auto inner = coachBox.reduced (7.0f, 6.0f);
        auto tag = inner.removeFromLeft (34.0f).removeFromTop (14.0f);
        g.setColour (level);
        g.fillRoundedRectangle (tag, 2.0f);
        g.setColour (juce::Colour (0xff141414));
        g.setFont (font (8.5f, true, 0.12f));
        g.drawText ("LIVE", tag, juce::Justification::centred, false);
        drawWrapped (g, coach.text, font (11.0f), col::cream, inner.withTrimmedLeft (8.0f));
    }

private:
    static constexpr int leftW = 214, rightW = 430;

    juce::Rectangle<int> lessonArea() const
    {
        return getLocalBounds().reduced (0, 10).removeFromRight (rightW).reduced (14, 0);
    }

    const guide::Source& source() const
    {
        auto& list = guide::sourcesFor (mode);
        return list[(size_t) juce::jlimit (0, (int) list.size() - 1, sourceBox.getSelectedItemIndex())];
    }

    /** Gold call-to-action button. */
    struct ApplyButton final : juce::Button
    {
        ApplyButton() : juce::Button ("APPLY STARTING POINT") {}
        void paintButton (juce::Graphics& g, bool over, bool down) override
        {
            auto r = getLocalBounds().toFloat().reduced (0.5f);
            const auto base = col::gold.brighter (over ? 0.2f : 0.0f).darker (down ? 0.2f : 0.0f);
            g.setGradientFill (juce::ColourGradient (base.brighter (0.25f), 0.0f, r.getY(), base.darker (0.25f), 0.0f, r.getBottom(), false));
            g.fillRoundedRectangle (r, 4.0f);
            g.setColour (juce::Colour (0xff15130f));
            g.setFont (font (10.5f, true, 0.12f));
            g.drawText (getName(), getLocalBounds(), juce::Justification::centred, false);
        }
    };

    juce::AudioProcessor& processor;
    APVTS& state;
    juce::ComboBox sourceBox, styleBox;
    ApplyButton apply;
    juce::String hovered, applyHelp;
    Coach::Advice coach;
    int mode = 0;
};
} // namespace atelier::ui
