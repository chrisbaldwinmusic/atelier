#pragma once

#include <juce_dsp/juce_dsp.h>

#include "../PluginProcessor.h"
#include "Widgets.h"

namespace atelier::ui
{
/** Spectrum analyser with the EQ curve drawn over it, plus mastering-grade metering.

    Left: input (faint line) and output (filled) spectra with a 3 dB/octave tilt, and the
    combined response of the filters and EQ. The five EQ bands can be dragged directly:
    horizontal = frequency, vertical = gain, scroll wheel = Q, double-click = flat.

    Right: integrated / short-term / momentary loudness, loudness range, true peak and
    stereo correlation. */
class AnalyserPanel final : public juce::Component
{
public:
    explicit AnalyserPanel (AtelierProcessor& p) : proc (p), state (p.apvts)
    {
        for (int i = 0; i < fftSize; ++i)
            window[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) (fftSize - 1));
        outDb.fill (-120.0f);
        inDb.fill (-120.0f);

        reset.onClick = [this] { proc.meters.resetLoudness.store (true); };
        addAndMakeVisible (reset);
    }

    /** Called by the editor's timer while the panel is showing. */
    void update()
    {
        analyse (proc.scopeOut, outDb);
        analyse (proc.scopeIn, inDb);
        repaint();
    }

    void resized() override
    {
        reset.setBounds (meterArea().removeFromBottom (22).removeFromRight (64));
    }

    //==============================================================================
    void paint (juce::Graphics& g) override
    {
        paintPanel (g, getLocalBounds().toFloat());
        const auto plot = plotArea().toFloat();
        sr = juce::jmax (8000.0, proc.getScopeSampleRate());

        g.setColour (col::well);
        g.fillRoundedRectangle (plot, 4.0f);

        // ---- grid ----
        g.setFont (font (8.5f, true));
        static constexpr float freqs[] = { 20, 30, 40, 50, 60, 80, 100, 200, 300, 400, 500, 600, 800, 1000, 2000, 3000, 4000, 5000, 6000, 8000, 10000, 20000 };
        for (float f : freqs)
        {
            const bool major = f == 100.0f || f == 1000.0f || f == 10000.0f;
            const float x = xFor (plot, f);
            g.setColour (col::wellEdge.withAlpha (major ? 0.55f : 0.22f));
            g.drawVerticalLine (juce::roundToInt (x), plot.getY(), plot.getBottom());
        }
        static constexpr std::pair<float, const char*> labels[] = { { 50, "50" }, { 100, "100" }, { 200, "200" }, { 500, "500" }, { 1000, "1k" },
                                                                    { 2000, "2k" }, { 5000, "5k" }, { 10000, "10k" } };
        g.setColour (col::creamDim.withAlpha (0.8f));
        for (auto& [f, text] : labels)
            g.drawText (text, juce::Rectangle<float> (30.0f, 10.0f).withCentre ({ xFor (plot, f), plot.getBottom() - 7.0f }), juce::Justification::centred, false);

        for (int db : { -12, -6, 0, 6, 12 })
        {
            const float y = yForGain (plot, (float) db);
            g.setColour (col::wellEdge.withAlpha (db == 0 ? 0.7f : 0.25f));
            g.drawHorizontalLine (juce::roundToInt (y), plot.getX(), plot.getRight());
            g.setColour (col::creamDim.withAlpha (0.7f));
            g.drawText ((db > 0 ? "+" : "") + juce::String (db), juce::Rectangle<float> (plot.getRight() - 26.0f, y - 6.0f, 22.0f, 12.0f),
                        juce::Justification::centredRight, false);
        }

        {
            juce::Graphics::ScopedSaveState clip (g);
            g.reduceClipRegion (plot.toNearestInt());

            // ---- spectra ----
            auto out = spectrumPath (plot, outDb);
            juce::Path fill (out);
            fill.lineTo (plot.getRight(), plot.getBottom());
            fill.lineTo (plot.getX(), plot.getBottom());
            fill.closeSubPath();
            g.setGradientFill (juce::ColourGradient (col::gold.withAlpha (0.42f), 0.0f, plot.getY(), col::gold.withAlpha (0.04f), 0.0f, plot.getBottom(), false));
            g.fillPath (fill);
            g.setColour (col::gold.withAlpha (0.85f));
            g.strokePath (out, juce::PathStrokeType (1.1f));
            g.setColour (col::cream.withAlpha (0.28f));
            g.strokePath (spectrumPath (plot, inDb), juce::PathStrokeType (1.0f));

            // ---- filter + EQ response ----
            designFilters();
            const bool eqOn = raw ("eqOn") > 0.5f;
            juce::Path curve;
            bool started = false;   // (Path::isEmpty() stays true until the first line is added)
            for (float x = plot.getX(); x <= plot.getRight(); x += 2.0f)
            {
                const float y = yForGain (plot, responseDb (freqFor (plot, x), eqOn));
                if (! started) { curve.startNewSubPath (x, y); started = true; } else curve.lineTo (x, y);
            }
            juce::Path area (curve);
            area.lineTo (plot.getRight(), yForGain (plot, 0.0f));
            area.lineTo (plot.getX(), yForGain (plot, 0.0f));
            area.closeSubPath();
            g.setColour (col::blue.withAlpha (eqOn ? 0.16f : 0.05f));
            g.fillPath (area);
            g.setColour (col::blue.brighter (0.5f).withAlpha (eqOn ? 1.0f : 0.4f));
            g.strokePath (curve, juce::PathStrokeType (2.0f));

            // ---- draggable band handles ----
            for (int i = 0; i < numNodes; ++i)
            {
                const auto p = nodePos (plot, i);
                const bool hot = i == dragging || i == hover;
                g.setColour (col::blue.brighter (hot ? 0.7f : 0.3f).withAlpha (eqOn ? 1.0f : 0.5f));
                g.fillEllipse (juce::Rectangle<float> (hot ? 17.0f : 14.0f, hot ? 17.0f : 14.0f).withCentre (p));
                g.setColour (col::ink);
                g.setFont (font (9.0f, true));
                g.drawText (juce::String (i + 1), juce::Rectangle<float> (16.0f, 12.0f).withCentre (p), juce::Justification::centred, false);
            }
        }

        if (const int n = dragging >= 0 ? dragging : hover; n >= 0)
        {
            auto text = juce::String (nodes[n].name) + "   " + paramText (nodes[n].freq) + "   " + paramText (nodes[n].gain);
            if (nodes[n].q != nullptr) text << "   " << paramText (nodes[n].q);
            g.setColour (col::cream);
            g.setFont (font (10.5f, true, 0.04f));
            g.drawText (text, plot.reduced (8.0f, 5.0f).removeFromTop (14.0f), juce::Justification::centredLeft, false);
        }
        else
        {
            g.setColour (col::creamDim.withAlpha (0.8f));
            g.setFont (font (9.0f, true, 0.08f));
            g.drawText ("DRAG A BAND  -  SCROLL FOR Q  -  DOUBLE-CLICK TO FLATTEN", plot.reduced (8.0f, 5.0f).removeFromTop (14.0f),
                        juce::Justification::centredLeft, false);
        }

        paintMeters (g);
    }

    //==============================================================================
    void mouseMove (const juce::MouseEvent& e) override
    {
        const int n = nodeAt (e.position);
        if (n != hover) { hover = n; repaint(); }
    }

    void mouseExit (const juce::MouseEvent&) override   { hover = -1; }

    void mouseDown (const juce::MouseEvent& e) override
    {
        dragging = nodeAt (e.position);
        if (dragging < 0) return;
        for (auto* id : { nodes[dragging].freq, nodes[dragging].gain })
            state.getParameter (id)->beginChangeGesture();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragging < 0) return;
        const auto plot = plotArea().toFloat();
        setParam (nodes[dragging].freq, freqFor (plot, juce::jlimit (plot.getX(), plot.getRight(), e.position.x)));
        setParam (nodes[dragging].gain, gainForY (plot, juce::jlimit (plot.getY(), plot.getBottom(), e.position.y)));
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (dragging < 0) return;
        for (auto* id : { nodes[dragging].freq, nodes[dragging].gain })
            state.getParameter (id)->endChangeGesture();
        dragging = -1;
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (const int n = nodeAt (e.position); n >= 0)
            if (auto* p = state.getParameter (nodes[n].gain))
            {
                p->beginChangeGesture();
                p->setValueNotifyingHost (p->getDefaultValue());
                p->endChangeGesture();
            }
    }

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override
    {
        const int n = nodeAt (e.position);
        if (n < 0 || nodes[n].q == nullptr) return;
        if (auto* p = state.getParameter (nodes[n].q))
        {
            p->beginChangeGesture();
            p->setValueNotifyingHost (juce::jlimit (0.0f, 1.0f, p->getValue() + wheel.deltaY * 0.35f));
            p->endChangeGesture();
        }
    }

private:
    static constexpr int fftOrder = 12, fftSize = 1 << fftOrder, numBins = fftSize / 2, numNodes = 5, meterW = 250;
    static constexpr float gainRange = 18.0f, specTop = 6.0f, specBottom = -84.0f;

    struct Node { const char* name; const char* gain; const char* freq; const char* q; };
    static constexpr Node nodes[numNodes] = { { "LOW SHELF", "lfGain", "lfFreq", nullptr }, { "LO MID", "lmGain", "lmFreq", "lmQ" },
                                              { "HI MID", "hmGain", "hmFreq", "hmQ" }, { "HIGH SHELF", "hfGain", "hfFreq", nullptr },
                                              { "AIR", "airGain", "airFreq", nullptr } };

    juce::Rectangle<int> plotArea() const   { return getLocalBounds().reduced (10).withTrimmedRight (meterW + 8); }
    juce::Rectangle<int> meterArea() const  { return getLocalBounds().reduced (10).removeFromRight (meterW); }

    static float xFor (juce::Rectangle<float> r, float f)       { return r.getX() + r.getWidth() * std::log (f / 20.0f) / std::log (1000.0f); }
    static float freqFor (juce::Rectangle<float> r, float x)    { return 20.0f * std::pow (1000.0f, (x - r.getX()) / r.getWidth()); }
    static float yForGain (juce::Rectangle<float> r, float db)  { return r.getCentreY() - db / gainRange * r.getHeight() * 0.5f; }
    static float gainForY (juce::Rectangle<float> r, float y)   { return (r.getCentreY() - y) / (r.getHeight() * 0.5f) * gainRange; }

    float raw (const char* id) const        { return state.getRawParameterValue (id)->load(); }
    juce::String paramText (const char* id) const   { return state.getParameter (id)->getCurrentValueAsText(); }

    void setParam (const char* id, float value)
    {
        if (auto* p = state.getParameter (id))
        {
            const auto& range = p->getNormalisableRange();
            p->setValueNotifyingHost (p->convertTo0to1 (juce::jlimit (range.start, range.end, value)));
        }
    }

    juce::Point<float> nodePos (juce::Rectangle<float> plot, int i) const
    {
        return { xFor (plot, raw (nodes[i].freq)), yForGain (plot, raw (nodes[i].gain)) };
    }

    int nodeAt (juce::Point<float> p) const
    {
        const auto plot = plotArea().toFloat();
        int best = -1; float bestDist = 13.0f;
        for (int i = 0; i < numNodes; ++i)
            if (const float d = nodePos (plot, i).getDistanceFrom (p); d < bestDist) { bestDist = d; best = i; }
        return best;
    }

    //==============================================================================
    void designFilters()
    {
        Equalizer::Params p;
        p.lfGain = raw ("lfGain");   p.lfFreq = raw ("lfFreq");
        p.lmGain = raw ("lmGain");   p.lmFreq = raw ("lmFreq");   p.lmQ = raw ("lmQ");
        p.hmGain = raw ("hmGain");   p.hmFreq = raw ("hmFreq");   p.hmQ = raw ("hmQ");
        p.hfGain = raw ("hfGain");   p.hfFreq = raw ("hfFreq");
        p.airGain = raw ("airGain"); p.airFreq = raw ("airFreq");
        p.style = (int) raw ("eqStyle");
        eq.sr = sr;
        eq.set (p);

        const bool isMix = (int) raw ("mode") == 0;
        hpOn = raw ("hpf") > 20.5f;
        lpOn = raw ("lpf") < 21900.0f;
        hpTwo = isMix;
        if (hpOn)
        {
            hpA.setHighPass (sr, raw ("hpf"), isMix ? 0.5412 : 0.707);
            hpB.setHighPass (sr, raw ("hpf"), 1.3066);
        }
        if (lpOn) lp.setLowPass (sr, raw ("lpf"), 0.707);
    }

    float responseDb (float f, bool eqOn) const
    {
        const double w = juce::MathConstants<double>::twoPi * juce::jmin ((double) f, 0.499 * sr) / sr;
        double mag = 1.0;
        if (eqOn) for (auto& b : eq.bands) mag *= b.magnitude (w);
        if (hpOn) mag *= hpA.magnitude (w) * (hpTwo ? hpB.magnitude (w) : 1.0);
        if (lpOn) mag *= lp.magnitude (w);
        return (float) juce::jlimit (-60.0, 40.0, 20.0 * std::log10 (juce::jmax (1.0e-6, mag)));
    }

    //==============================================================================
    void analyse (const ScopeFifo& fifo, std::array<float, numBins + 1>& dest)
    {
        fifo.copyLatest (fftData.data(), fftSize);
        for (int i = 0; i < fftSize; ++i) fftData[(size_t) i] *= window[(size_t) i];
        std::fill (fftData.begin() + fftSize, fftData.end(), 0.0f);
        fft.performFrequencyOnlyForwardTransform (fftData.data());

        const float norm = 4.0f / (float) fftSize;     // Hann window, single-sided
        for (int b = 0; b <= numBins; ++b)
        {
            const float db = 20.0f * std::log10 (juce::jmax (1.0e-7f, fftData[(size_t) b] * norm));
            dest[(size_t) b] = db > dest[(size_t) b] ? dest[(size_t) b] + 0.6f * (db - dest[(size_t) b])   // quick rise
                                                     : juce::jmax (db, dest[(size_t) b] - 1.3f);          // steady fall
        }
    }

    juce::Path spectrumPath (juce::Rectangle<float> plot, const std::array<float, numBins + 1>& db) const
    {
        juce::Path path;
        bool started = false;
        const double binHz = sr / (double) fftSize;

        for (float x = plot.getX(); x <= plot.getRight(); x += 2.0f)
        {
            const double f1 = freqFor (plot, x), f2 = freqFor (plot, x + 2.0f);
            const double b1 = f1 / binHz, b2 = f2 / binHz;
            float v;

            if (b2 - b1 < 1.0)      // fewer than one bin per step: interpolate
            {
                const int i = juce::jlimit (0, numBins - 1, (int) b1);
                const float t = (float) (b1 - (double) i);
                v = db[(size_t) i] + juce::jlimit (0.0f, 1.0f, t) * (db[(size_t) i + 1] - db[(size_t) i]);
            }
            else                    // several bins per step: show the loudest
            {
                v = -200.0f;
                for (int i = juce::jlimit (0, numBins, (int) b1); i <= juce::jlimit (0, numBins, (int) b2); ++i)
                    v = juce::jmax (v, db[(size_t) i]);
            }

            v += 3.0f * (float) std::log2 (juce::jmax (20.0, f1) / 1000.0);     // tilt so a balanced mix looks level
            const float y = juce::jmap (juce::jlimit (specBottom, specTop, v), specBottom, specTop, plot.getBottom(), plot.getY());
            if (! started) { path.startNewSubPath (x, y); started = true; } else path.lineTo (x, y);
        }
        return path;
    }

    //==============================================================================
    void paintMeters (juce::Graphics& g)
    {
        auto area = meterArea().toFloat();
        auto& m = proc.meters;
        auto lufs = [] (float v) { return v < -70.0f ? juce::String ("--.-") : juce::String (v, 1); };

        g.setColour (col::cream);
        g.setFont (font (12.5f, true, 0.16f));
        g.drawText ("LOUDNESS", area.removeFromTop (18.0f), juce::Justification::centredLeft, false);
        g.setColour (col::gold);
        g.fillRect (area.removeFromTop (4.0f).withTrimmedTop (2.0f).withHeight (1.5f));
        area.removeFromTop (4.0f);

        auto row = [&] (const juce::String& name, const juce::String& value, const juce::String& unit, juce::Colour valueColour, float h = 21.0f)
        {
            auto r = area.removeFromTop (h);
            g.setColour (col::creamDim);
            g.setFont (font (9.5f, true, 0.1f));
            g.drawText (name, r, juce::Justification::centredLeft, false);
            g.drawText (unit, r.removeFromRight (38.0f), juce::Justification::centredRight, false);
            g.setColour (valueColour);
            g.setFont (font (h > 22.0f ? 20.0f : 14.0f, true));
            g.drawText (value, r, juce::Justification::centredRight, false);
        };

        row ("INTEGRATED", lufs (m.lufsI.load()), "LUFS", col::gold.brighter (0.25f), 28.0f);
        row ("SHORT-TERM", lufs (m.lufsS.load()), "LUFS", col::cream);
        row ("MOMENTARY",  lufs (m.lufsM.load()), "LUFS", col::cream);
        row ("RANGE",      juce::String (m.lra.load(), 1), "LU", col::cream);

        const float tp = m.truePeak.load();
        const float tpDb = tp > 1.0e-5f ? 20.0f * std::log10 (tp) : -100.0f;
        row ("TRUE PEAK", tpDb < -90.0f ? juce::String ("-inf") : juce::String (tpDb, 1), "dBTP", tpDb > -0.05f ? col::red.brighter (0.3f) : col::cream);

        // correlation: -1 (cancels in mono) .. +1 (mono)
        area.removeFromTop (3.0f);
        auto label = area.removeFromTop (12.0f);
        g.setColour (col::creamDim);
        g.setFont (font (9.5f, true, 0.1f));
        g.drawText ("CORRELATION", label, juce::Justification::centredLeft, false);
        g.setFont (font (8.5f, true));
        auto bar = area.removeFromTop (10.0f).withTrimmedRight (72.0f);
        g.setColour (col::well);
        g.fillRoundedRectangle (bar, 2.0f);
        const float c = juce::jlimit (-1.0f, 1.0f, m.correlation.load());
        const float mid = bar.getCentreX(), x = mid + c * bar.getWidth() * 0.5f;
        g.setColour (c < 0.0f ? col::red : col::green);
        g.fillRect (juce::Rectangle<float> (juce::jmin (mid, x), bar.getY() + 2.0f, std::abs (x - mid), bar.getHeight() - 4.0f));
        g.setColour (col::cream);
        g.fillRect (mid - 0.5f, bar.getY(), 1.0f, bar.getHeight());
    }

    /** Small outlined button. */
    struct ResetButton final : juce::Button
    {
        ResetButton() : juce::Button ("RESET") {}
        void paintButton (juce::Graphics& g, bool over, bool down) override
        {
            auto r = getLocalBounds().toFloat().reduced (1.0f);
            g.setColour (col::button.brighter (down ? 0.2f : (over ? 0.1f : 0.0f)));
            g.fillRoundedRectangle (r, 3.0f);
            g.setColour (col::wellEdge);
            g.drawRoundedRectangle (r, 3.0f, 1.0f);
            g.setColour (col::creamDim);
            g.setFont (font (9.5f, true, 0.1f));
            g.drawText (getName(), getLocalBounds(), juce::Justification::centred, false);
        }
    };

    AtelierProcessor& proc;
    APVTS& state;
    ResetButton reset;

    juce::dsp::FFT fft { fftOrder };
    std::array<float, fftSize> window {};
    std::array<float, fftSize * 2> fftData {};
    std::array<float, numBins + 1> outDb {}, inDb {};

    Equalizer eq;
    Biquad hpA, hpB, lp;
    bool hpOn = false, lpOn = false, hpTwo = false;
    double sr = 48000.0;
    int dragging = -1, hover = -1;
};
} // namespace atelier::ui
