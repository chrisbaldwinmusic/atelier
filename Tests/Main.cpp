// Offline checks for Atelier: runs audio through the processor and checks the results.
#include <functional>
#include "PluginEditor.h"
#include "PluginProcessor.h"

#include <CoreFoundation/CoreFoundation.h>
#include <iostream>
#include <time.h>
#include <pthread.h>
#include <sys/qos.h>

namespace
{
int failures = 0;

void check (bool ok, const juce::String& what)
{
    std::cout << (ok ? "  PASS  " : "  FAIL  ") << what << std::endl;
    if (! ok) ++failures;
}

void setParam (AtelierProcessor& p, const char* id, float value)
{
    auto* param = p.apvts.getParameter (id);
    jassert (param != nullptr);
    param->setValueNotifyingHost (param->convertTo0to1 (value));
}

void resetParams (AtelierProcessor& p)
{
    for (auto* param : p.getParameters())
        param->setValueNotifyingHost (param->getDefaultValue());
}

void run (AtelierProcessor& p, juce::AudioBuffer<float>& audio, int blockSize)
{
    juce::MidiBuffer midi;
    for (int pos = 0; pos < audio.getNumSamples(); pos += blockSize)
    {
        const int n = juce::jmin (blockSize, audio.getNumSamples() - pos);
        juce::AudioBuffer<float> view (audio.getArrayOfWritePointers(), audio.getNumChannels(), pos, n);
        p.processBlock (view, midi);
    }
}

juce::AudioBuffer<float> sine (int channels, int n, double sr, double freq, float gainDb)
{
    juce::AudioBuffer<float> b (channels, n);
    const float g = juce::Decibels::decibelsToGain (gainDb);
    for (int ch = 0; ch < channels; ++ch)
        for (int i = 0; i < n; ++i)
            b.setSample (ch, i, g * (float) std::sin (2.0 * juce::MathConstants<double>::pi * freq * i / sr));
    return b;
}

juce::AudioBuffer<float> noise (int channels, int n, float gainDb, int seed = 1)
{
    juce::AudioBuffer<float> b (channels, n);
    juce::Random rng (seed);
    const float g = juce::Decibels::decibelsToGain (gainDb);
    for (int ch = 0; ch < channels; ++ch)
        for (int i = 0; i < n; ++i)
            b.setSample (ch, i, g * (rng.nextFloat() * 2.0f - 1.0f));
    return b;
}

bool allFinite (const juce::AudioBuffer<float>& b)
{
    for (int ch = 0; ch < b.getNumChannels(); ++ch)
        for (int i = 0; i < b.getNumSamples(); ++i)
            if (! std::isfinite (b.getSample (ch, i))) return false;
    return true;
}

float rmsDb (const juce::AudioBuffer<float>& b, int start)
{
    return juce::Decibels::gainToDecibels (b.getRMSLevel (0, start, b.getNumSamples() - start));
}

/** Level of one frequency (dB) via a windowed single-bin DFT. */
float toneDb (const juce::AudioBuffer<float>& b, int start, double sr, double freq)
{
    double re = 0.0, im = 0.0, wsum = 0.0;
    const int n = b.getNumSamples() - start;
    for (int i = 0; i < n; ++i)
    {
        const double w = 0.5 - 0.5 * std::cos (2.0 * juce::MathConstants<double>::pi * i / (n - 1));
        const double ph = 2.0 * juce::MathConstants<double>::pi * freq * (start + i) / sr;
        const double x = b.getSample (0, start + i) * w;
        re += x * std::cos (ph); im += x * std::sin (ph); wsum += w;
    }
    return (float) (20.0 * std::log10 (std::max (1.0e-12, 2.0 * std::sqrt (re * re + im * im) / wsum)));
}

/** Independent true-peak reference: brute-force 16x sinc interpolation with a long window. */
float referenceTruePeakDb (const juce::AudioBuffer<float>& b, int start)
{
    const int os = 16, half = 64;
    const float* x = b.getReadPointer (0);
    double pk = 0.0;
    for (int i = start + half; i < b.getNumSamples() - half; ++i)
        for (int p = 0; p < os; ++p)
        {
            const double t = (double) p / os;
            double acc = 0.0;
            for (int k = -half + 1; k <= half; ++k)
            {
                const double d = t - k;
                const double sinc = std::abs (d) < 1.0e-12 ? 1.0 : std::sin (juce::MathConstants<double>::pi * d) / (juce::MathConstants<double>::pi * d);
                acc += x[i + k] * sinc * (0.5 + 0.5 * std::cos (juce::MathConstants<double>::pi * d / half));
            }
            pk = std::max (pk, std::abs (acc));
        }
    return (float) (20.0 * std::log10 (std::max (1.0e-9, pk)));
}

void allModulesOn (AtelierProcessor& p)
{
    for (auto* id : { "gateOn", "tubeOn", "dsOn", "eqOn", "compOn", "tapeOn", "ironOn", "imgOn", "limOn" })
        setParam (p, id, 1.0f);
}
}

//==============================================================================
int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui;

    const juce::File outDir = juce::File::getCurrentWorkingDirectory().getChildFile (argc > 1 ? argv[1] : "shots");
    outDir.createDirectory();

    const double sr = 48000.0;
    const int len = 96000;

    // ATELIER_ONLY=<text> runs just the sections whose heading contains the text
    auto want = [] (const char* section)
    {
        const char* only = std::getenv ("ATELIER_ONLY");
        return only == nullptr || juce::String (section).containsIgnoreCase (only);
    };

    if (want ("Bypass / latency"))
    {
        std::cout << "Bypass / latency" << std::endl;
        AtelierProcessor p;
        p.setPlayConfigDetails (2, 2, sr, 512);
        p.prepareToPlay (sr, 512);
        allModulesOn (p);
        setParam (p, "bypass", 1.0f);

        const auto in = noise (2, len, -12.0f);
        auto out = in;
        run (p, out, 512);
        const int lat = p.getLatencySamples();
        float maxErr = 0.0f;
        for (int i = 2000; i < len - lat; ++i)
            maxErr = std::max (maxErr, std::abs (out.getSample (0, i + lat) - in.getSample (0, i)));
        std::cout << "  latency " << lat << " samples, null error " << maxErr << std::endl;
        check (maxErr < 1.0e-5f, "bypass nulls against the input delayed by the reported latency");
    }

    if (want ("Active path alignment"))
    {
        std::cout << "Active path alignment" << std::endl;
        AtelierProcessor p;
        p.setPlayConfigDetails (2, 2, sr, 512);
        p.prepareToPlay (sr, 512);
        setParam (p, "mode", 2.0f);
        for (auto* id : { "tubeOn", "tapeOn", "ironOn", "limOn" }) setParam (p, id, 1.0f);
        for (auto* id : { "tubeDrive", "tapeDrive", "ironDrive", "tapeBump" }) setParam (p, id, 0.0f);

        // a low-level click should come out exactly `latency` samples later
        juce::AudioBuffer<float> b (2, 8192);
        b.clear();
        b.setSample (0, 1000, 0.01f); b.setSample (1, 1000, 0.01f);
        run (p, b, 512);
        int peakAt = 0; float pk = 0.0f;
        for (int i = 0; i < b.getNumSamples(); ++i)
            if (std::abs (b.getSample (0, i)) > pk) { pk = std::abs (b.getSample (0, i)); peakAt = i; }
        std::cout << "  impulse arrives at +" << (peakAt - 1000) << ", reported " << p.getLatencySamples() << std::endl;
        check (std::abs (peakAt - 1000 - p.getLatencySamples()) <= 1, "processed path matches reported latency");
    }

    if (want ("Default settings"))
    {
        std::cout << "Default settings" << std::endl;
        AtelierProcessor p;
        p.setPlayConfigDetails (2, 2, sr, 512);
        p.prepareToPlay (sr, 512);
        auto b = sine (2, len, sr, 1000.0, -18.0f);
        run (p, b, 512);
        const float fund = toneDb (b, 24000, sr, 1000.0);
        const float h2 = toneDb (b, 24000, sr, 2000.0), h3 = toneDb (b, 24000, sr, 3000.0);
        std::cout << "  1 kHz @ -18 dBFS -> " << fund << " dB, H2 " << h2 - fund << " dBc, H3 " << h3 - fund << " dBc" << std::endl;
        check (std::abs (fund + 18.0f) < 0.5f, "defaults are level-neutral");
        check (h2 - fund < -30.0f && h2 - fund > -90.0f, "default tube adds gentle 2nd harmonic");
    }

    if (want ("Colour stages (1 kHz @ -6 dBFS, drive 12 dB)"))
    {
        std::cout << "Colour stages (1 kHz @ -6 dBFS, drive 12 dB)" << std::endl;
        struct Case { const char* name; const char* on; const char* drive; };
        for (auto c : { Case { "tube", "tubeOn", "tubeDrive" }, Case { "tape", "tapeOn", "tapeDrive" }, Case { "iron", "ironOn", "ironDrive" } })
        {
            AtelierProcessor p;
            p.setPlayConfigDetails (2, 2, sr, 512);
            p.prepareToPlay (sr, 512);
            setParam (p, "tubeOn", 0.0f);
            setParam (p, c.on, 1.0f);
            setParam (p, c.drive, 12.0f);
            auto b = sine (2, len, sr, 1000.0, -6.0f);
            run (p, b, 512);
            const float f = toneDb (b, 24000, sr, 1000.0);
            std::cout << "  " << c.name << ": fund " << f << " dB, H2 " << toneDb (b, 24000, sr, 2000.0) - f
                      << " dBc, H3 " << toneDb (b, 24000, sr, 3000.0) - f << " dBc" << std::endl;
            check (allFinite (b) && f < -5.0f && f > -16.0f, juce::String (c.name) + " saturates without gaining level");

            // aliasing check: a 15 kHz tone has no legitimate product at 3 kHz, so anything there has folded back.
            // -18 dBFS at 15 kHz is already far hotter than real programme material; -6 dBFS is the torture case.
            for (float level : { -18.0f, -6.0f })
            {
                auto hi = sine (2, len, sr, 15000.0, level);
                p.prepareToPlay (sr, 512);
                run (p, hi, 512);
                const float alias = toneDb (hi, 24000, sr, 3000.0) - toneDb (hi, 24000, sr, 15000.0);
                std::cout << "    15 kHz @ " << level << " dBFS: alias at 3 kHz " << alias << " dBc" << std::endl;
                if (level < -10.0f)
                    check (alias < -70.0f, juce::String (c.name) + " aliasing is below -70 dBc at realistic HF level");
            }
        }
    }

    if (want ("EQ"))
    {
        std::cout << "EQ" << std::endl;
        AtelierProcessor p;
        p.setPlayConfigDetails (2, 2, sr, 512);
        p.prepareToPlay (sr, 512);
        setParam (p, "tubeOn", 0.0f);
        setParam (p, "hmGain", 6.0f); setParam (p, "hmFreq", 3000.0f);
        auto b = sine (2, len, sr, 3000.0, -18.0f);
        run (p, b, 512);
        const float g = toneDb (b, 24000, sr, 3000.0) + 18.0f;
        std::cout << "  +6 dB bell at 3 kHz measures " << g << " dB" << std::endl;
        check (std::abs (g - 6.0f) < 0.2f, "bell gain is accurate");
    }

    if (want ("Compressor"))
    {
        std::cout << "Compressor" << std::endl;
        for (int model = 0; model < 4; ++model)
        {
            AtelierProcessor p;
            p.setPlayConfigDetails (2, 2, sr, 512);
            p.prepareToPlay (sr, 512);
            setParam (p, "tubeOn", 0.0f);
            setParam (p, "compOn", 1.0f); setParam (p, "compModel", (float) model);
            setParam (p, "compThr", -26.0f); setParam (p, "compRatio", 4.0f);
            auto b = sine (2, len, sr, 1000.0, -6.0f);
            run (p, b, 512);
            const float out = toneDb (b, 48000, sr, 1000.0);
            std::cout << "  model " << model << ": -6 dBFS in -> " << out << " dB (ideal 4:1 = -21)" << std::endl;
            check (allFinite (b) && out < -14.0f && out > -24.0f, "model " + juce::String (model) + " compresses plausibly");
        }

        // The Vari-Mu's gain is turned down by valves, so its curve and its colour come from them
        struct Reading { float out, gr, h2, h3; };
        auto measure = [&] (int model, float rmsDb, bool bursts = false)
        {
            AtelierProcessor p;
            p.setPlayConfigDetails (2, 2, sr, 512);
            p.prepareToPlay (sr, 512);
            setParam (p, "tubeOn", 0.0f);
            setParam (p, "compOn", 1.0f); setParam (p, "compModel", (float) model);
            setParam (p, "compThr", bursts ? -30.0f : -26.0f); setParam (p, "compRatio", bursts ? 10.0f : 4.0f);
            if (bursts) setParam (p, "compAtt", 1.0f);
            auto b = sine (2, len, sr, 1000.0, rmsDb + 3.0103f);
            if (bursts)     // 100 ms loud, 100 ms 26 dB quieter: the gain swings hard and fast
                for (int i = 0; i < len; ++i)
                    if ((i / (int) (0.1 * sr)) % 2 == 1) { b.setSample (0, i, b.getSample (0, i) * 0.05f); b.setSample (1, i, b.getSample (1, i) * 0.05f); }
            run (p, b, 512);
            p.meters.grComp.store (0.0f);
            const float f = toneDb (b, 24000, sr, 1000.0);
            Reading r { f - 3.0103f, 0.0f, toneDb (b, 24000, sr, 2000.0) - f, toneDb (b, 24000, sr, 3000.0) - f };
            if (bursts)
            {
                // anything left below 20 Hz is thump from the gain moving
                double z[4] {}; const double k = 1.0 - std::exp (-2.0 * juce::MathConstants<double>::pi * 20.0 / sr);
                float lf = 0.0f;
                for (int i = 0; i < len; ++i)
                {
                    double x = b.getSample (0, i);
                    for (auto& s1 : z) { s1 += k * (x - s1); x = s1; }
                    if (i >= 24000) lf = std::max (lf, (float) std::abs (x));
                }
                r.out = juce::Decibels::gainToDecibels (lf, -200.0f);
            }
            return r;
        };
        const Reading vcaOnset = measure (0, -28.0f), muOnset = measure (3, -28.0f);
        const Reading muLight = measure (3, -22.0f), muMid = measure (3, -14.0f), muHigh = measure (3, -10.0f), muTop = measure (3, -2.0f), muTop2 = measure (3, 2.0f);
        std::cout << "  vari-mu, threshold -26 dB, ratio 4: -28 dB rms in -> " << muOnset.out << " dB (VCA " << vcaOnset.out << "); -22 -> " << muLight.out
                  << "; -14 -> " << muMid.out << "; -10 -> " << muHigh.out << "; -2 -> " << muTop.out << "; +2 -> " << muTop2.out << std::endl;
        std::cout << "    harmonics: H3 " << muLight.h3 << " dBc compressing lightly, " << muTop.h3 << " dBc compressing hard" << std::endl;
        check (muOnset.out < -28.1f && std::abs (vcaOnset.out + 28.0f) < 0.05f, "the vari-mu starts compressing earlier and more gently than the VCA");
        check (muTop2.out - muTop.out < muHigh.out - muMid.out, "the vari-mu's ratio rises as it is pushed harder");
        check (muLight.h3 < -70.0f && muTop.h3 > muLight.h3 + 20.0f, "the vari-mu is clean when compressing lightly and its colour grows as it works harder");

        const Reading muBursts = measure (3, -9.0f, true), vcaBursts = measure (0, -9.0f, true);
        std::cout << "  gain swinging 25 dB in 1 ms: below 20 Hz, vari-mu " << muBursts.out << " dBFS, VCA " << vcaBursts.out << " dBFS" << std::endl;
        check (muBursts.out < vcaBursts.out + 6.0f && muBursts.out < -50.0f, "the vari-mu's valve pair is balanced: fast gain changes don't thump");

        // The FET's colour is its channel's: loud material just engaging it saturates the channel, heavy reduction
        // turns the FET on so far that it is clean again
        auto fetAt = [&] (float thresholdDb)
        {
            AtelierProcessor p;
            p.setPlayConfigDetails (2, 2, sr, 512);
            p.prepareToPlay (sr, 512);
            setParam (p, "tubeOn", 0.0f);
            setParam (p, "compOn", 1.0f); setParam (p, "compModel", 1.0f);
            setParam (p, "compThr", thresholdDb); setParam (p, "compRatio", 4.0f);
            auto b = sine (2, len, sr, 1000.0, -3.0f);
            run (p, b, 512);
            const float f = toneDb (b, 24000, sr, 1000.0);
            return std::pair<float, float> { -3.0f - f, toneDb (b, 24000, sr, 3000.0) - f };
        };
        const auto fetLight = fetAt (-6.0f), fetHeavy = fetAt (-36.0f);
        const Reading fetBursts = measure (1, -9.0f, true);
        std::cout << "  FET, -3 dBFS sine: " << fetLight.first << " dB reduction, H3 " << fetLight.second << " dBc; "
                  << fetHeavy.first << " dB reduction, H3 " << fetHeavy.second << " dBc" << std::endl;
        std::cout << "  gain swinging 25 dB in 1 ms: below 20 Hz, FET " << fetBursts.out << " dBFS" << std::endl;
        check (fetLight.second > -50.0f && fetHeavy.second < fetLight.second - 20.0f, "the FET colours loud material as it starts to bite, and cleans up when it is turned on hard");
        check (fetBursts.out < -40.0f, "the FET's control voltage barely reaches the audio when the gain moves fast");

        // The opto's make-up amplifier is a real 12AX7 stage: mostly 2nd harmonic, growing with level
        auto optoColour = [&] (double freq, float levelDb, double at)
        {
            AtelierProcessor p;
            p.setPlayConfigDetails (2, 2, sr, 512);
            p.prepareToPlay (sr, 512);
            setParam (p, "tubeOn", 0.0f);
            setParam (p, "compOn", 1.0f); setParam (p, "compModel", 2.0f); setParam (p, "compThr", 0.0f);
            auto b = sine (2, len, sr, freq, levelDb);
            run (p, b, 512);
            return toneDb (b, 24000, sr, at) - toneDb (b, 24000, sr, freq);
        };
        const float optoH2Loud = optoColour (1000.0, -6.0f, 2000.0), optoH3Loud = optoColour (1000.0, -6.0f, 3000.0), optoH2Quiet = optoColour (1000.0, -18.0f, 2000.0);
        const float optoAlias = std::max (optoColour (15000.0, -6.0f, 18000.0), optoColour (15000.0, -6.0f, 3000.0));
        std::cout << "  opto make-up valve, 1 kHz: H2 " << optoH2Loud << " dBc, H3 " << optoH3Loud << " dBc at -6 dBFS; H2 " << optoH2Quiet << " dBc at -18 dBFS" << std::endl;
        std::cout << "    15 kHz at -6 dBFS: largest alias (18 kHz or 3 kHz) " << optoAlias << " dBc" << std::endl;
        check (optoH2Loud > -48.0f && optoH2Loud < -36.0f && optoH3Loud < optoH2Loud - 15.0f && optoH2Quiet < optoH2Loud - 9.0f,
               "the opto's 12AX7 make-up stage adds a touch of 2nd harmonic that grows with level");
        // it runs 2x oversampled, so its harmonics of the top octave are filtered out instead of folding back
        check (optoAlias < -90.0f, "the opto's make-up stage doesn't alias");

        // The compressed signal goes through the make-up valve's oversampler and the dry one through a delay: at half
        // mix the two must add without combing, right up to the top of the band
        auto mixLevel = [&] (double freq, float mixPercent)
        {
            AtelierProcessor p;
            p.setPlayConfigDetails (2, 2, sr, 512);
            p.prepareToPlay (sr, 512);
            setParam (p, "tubeOn", 0.0f);
            setParam (p, "compOn", 1.0f); setParam (p, "compModel", 2.0f); setParam (p, "compThr", 0.0f); setParam (p, "compMix", mixPercent);
            auto b = sine (2, len, sr, freq, -30.0f);
            run (p, b, 512);
            return toneDb (b, 24000, sr, freq);
        };
        float worstComb = 0.0f;
        for (double f : { 1000.0, 10000.0, 18000.0 })
            worstComb = std::max (worstComb, std::abs (mixLevel (f, 50.0f) - mixLevel (f, 100.0f)));
        std::cout << "  opto at half mix against full: largest level difference at 1, 10, 18 kHz " << worstComb << " dB" << std::endl;
        check (worstComb < 0.1f, "the compressed and dry signals stay in time, so the mix knob doesn't comb");

        // The opto's timing comes from its light-dependent cell: held at a reduction, then left in the dark
        struct Opto { double attack90, releaseHalf, after3s; };
        auto opto = [&] (double grDb, double hold, double attackScale = 1.0, double releaseScale = 1.0)
        {
            atelier::OptoCell c;
            c.prepare (sr);
            c.setTiming (attackScale, releaseScale);
            Opto o { -1.0, -1.0, 0.0 };
            const double v = atelier::OptoCell::voltsFor (grDb);
            for (int i = 1; i <= (int) (hold * sr); ++i) { c.step (v); if (o.attack90 < 0.0 && c.reduction() >= 0.9 * grDb) o.attack90 = 1000.0 * i / sr; }
            const double held = c.reduction();
            for (int i = 1; i <= (int) (3.0 * sr); ++i) { c.step (0.0); if (o.releaseHalf < 0.0 && c.reduction() <= 0.5 * held) o.releaseHalf = 1000.0 * i / sr; }
            o.after3s = c.reduction();
            return o;
        };
        const Opto own = opto (12.0, 0.5), heavy = opto (20.0, 0.5), light = opto (6.0, 0.5), brief = opto (20.0, 0.1), longHeavy = opto (20.0, 3.0), quick = opto (12.0, 0.5, 71.0 / 10.0, 115.0 / 50.0);
        std::cout << "  opto cell at its own pace: 12 dB reached (90 %) in " << own.attack90 << " ms, back to 6 dB in " << own.releaseHalf << " ms" << std::endl;
        std::cout << "    first half of the release: from 20 dB " << heavy.releaseHalf << " ms, from 12 dB " << own.releaseHalf << " ms, from 6 dB " << light.releaseHalf << " ms" << std::endl;
        std::cout << "    3 s after 20 dB: " << brief.after3s << " dB left after a 0.1 s passage, " << longHeavy.after3s << " dB after 3 s" << std::endl;
        std::cout << "    knobs at 10 ms attack, 50 ms release: " << quick.attack90 << " ms, " << quick.releaseHalf << " ms" << std::endl;
        check (std::abs (own.attack90 - 71.0) < 3.0 && std::abs (own.releaseHalf - 115.0) < 5.0, "the opto cell's own pace is what the knobs are calibrated to");
        check (std::abs (quick.attack90 - 10.0) < 1.0 && std::abs (quick.releaseHalf - 50.0) < 3.0, "the opto's attack and release knobs mean what they say at 12 dB");
        check (heavy.releaseHalf < own.releaseHalf && own.releaseHalf < light.releaseHalf, "the opto lets go of heavy reduction faster than light");
        check (longHeavy.after3s > 2.0 * brief.after3s && brief.after3s < 0.5, "the opto remembers: after a long, heavy passage the last of the reduction lingers");
    }

    if (want ("Tape hysteresis"))
    {
        std::cout << "Tape hysteresis" << std::endl;
        auto measure = [&] (float bias, float level)
        {
            AtelierProcessor p;
            p.setPlayConfigDetails (2, 2, sr, 512);
            p.prepareToPlay (sr, 512);
            setParam (p, "tubeOn", 0.0f); setParam (p, "tapeOn", 1.0f);
            setParam (p, "tapeDrive", 0.0f); setParam (p, "tapeBump", 0.0f); setParam (p, "tapeBias", bias);
            auto b = sine (2, len, sr, 1000.0, level);
            run (p, b, 512);
            const float f = toneDb (b, 48000, sr, 1000.0);
            return std::pair<float, float> { f - level, toneDb (b, 48000, sr, 3000.0) - f };
        };
        const auto quiet = measure (0.0f, -40.0f), loud = measure (0.0f, 0.0f), starved = measure (-100.0f, -18.0f), normal = measure (0.0f, -18.0f);
        std::cout << "  correct bias: -40 dBFS gain " << quiet.first << " dB, H3 " << quiet.second << " dBc;  0 dBFS gain "
                  << loud.first << " dB, H3 " << loud.second << " dBc" << std::endl;
        std::cout << "  at -18 dBFS: H3 " << normal.second << " dBc biased correctly, " << starved.second << " dBc under-biased" << std::endl;
        check (std::abs (quiet.first) < 1.0f && quiet.second < -70.0f, "tape is clean and near unity at low level");
        check (loud.first < -1.5f && loud.second > -30.0f, "tape compresses and saturates when hit hard");
        check (starved.second > normal.second + 10.0f, "under-biasing brings out the hysteresis grit");
    }

    if (want ("Limiter"))
    {
        std::cout << "Limiter" << std::endl;
        AtelierProcessor p;
        p.setPlayConfigDetails (2, 2, sr, 512);
        p.prepareToPlay (sr, 512);
        setParam (p, "mode", 2.0f);
        setParam (p, "limOn", 1.0f); setParam (p, "limDrive", 18.0f); setParam (p, "limCeil", -1.0f);
        auto b = noise (2, len, -3.0f, 7);
        // add some brutal transients
        for (int i = 5000; i < len; i += 7919) { b.setSample (0, i, 1.0f); b.setSample (1, i, -1.0f); }
        run (p, b, 480);
        const float pk = b.getMagnitude (0, len);
        std::cout << "  peak " << juce::Decibels::gainToDecibels (pk) << " dBFS (ceiling -1.0)" << std::endl;
        check (pk <= juce::Decibels::decibelsToGain (-1.0f) + 1.0e-6f, "output never exceeds the ceiling");

        // a decaying tone pushed 9 dB into the limiter: the loud start is held at the ceiling, the tail comes up by the drive
        setParam (p, "limDrive", 9.0f);
        auto t = sine (2, len, sr, 220.0, -3.0f);
        for (int i = 0; i < len; ++i) { const float e = std::exp (-4.0f * (float) i / (float) len); t.applyGain (i, 1, e); }
        const float inRms = rmsDb (t, 0);
        run (p, t, 512);
        std::cout << "  decaying tone: " << inRms << " dB RMS in -> " << rmsDb (t, 0) << " dB RMS out, peak "
                  << juce::Decibels::gainToDecibels (t.getMagnitude (0, len)) << std::endl;
        check (rmsDb (t, 0) > inRms + 3.0f && t.getMagnitude (0, len) <= juce::Decibels::decibelsToGain (-1.0f) + 1.0e-6f,
               "limiter raises loudness while holding the ceiling");
    }

    if (want ("True peak + loudness metering"))
    {
        std::cout << "True peak + loudness metering" << std::endl;
        AtelierProcessor p;
        p.setPlayConfigDetails (2, 2, sr, 512);
        p.prepareToPlay (sr, 512);
        setParam (p, "mode", 2.0f); setParam (p, "tubeOn", 0.0f);
        setParam (p, "limOn", 1.0f); setParam (p, "limDrive", 12.0f); setParam (p, "limCeil", -1.0f);

        // worst case for inter-sample peaks: a tone at fs/4 whose samples all miss the crest, plus two more high tones
        juce::AudioBuffer<float> b (2, 48000);
        for (int i = 0; i < b.getNumSamples(); ++i)
        {
            const double t = (double) i / sr;
            const float v = (float) (0.5 * std::sin (2.0 * juce::MathConstants<double>::pi * 12000.0 * t + 0.785)
                                   + 0.3 * std::sin (2.0 * juce::MathConstants<double>::pi * 9973.0 * t)
                                   + 0.3 * std::sin (2.0 * juce::MathConstants<double>::pi * 3001.0 * t + 1.0));
            b.setSample (0, i, v); b.setSample (1, i, v);
        }
        run (p, b, 512);
        const float samplePk = juce::Decibels::gainToDecibels (b.getMagnitude (4800, 43200));
        const float truePk = referenceTruePeakDb (b, 4800);
        const float metered = juce::Decibels::gainToDecibels (p.meters.truePeak.load());
        std::cout << "  limiter output: sample peak " << samplePk << " dBFS, true peak " << truePk << " dBTP (ceiling -1.0), meter reads " << metered << std::endl;
        check (truePk <= -0.85f, "limiter holds true peak to the ceiling (within 0.15 dB)");
        check (std::abs (metered - truePk) < 0.3f, "true-peak meter agrees with the reference");

        AtelierProcessor q;
        q.setPlayConfigDetails (2, 2, sr, 512);
        q.prepareToPlay (sr, 512);
        setParam (q, "tubeOn", 0.0f);
        juce::AudioBuffer<float> prog (2, (int) sr * 20);
        for (int i = 0; i < prog.getNumSamples(); ++i)
        {
            const float g = juce::Decibels::decibelsToGain (i < prog.getNumSamples() / 2 ? -20.0f : -30.0f);
            const float v = g * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 997.0 * i / sr);
            prog.setSample (0, i, v); prog.setSample (1, i, -v);   // right channel inverted
        }
        run (q, prog, 512);
        std::cout << "  10 s at -20 then 10 s at -30 LUFS: integrated " << q.meters.lufsI.load() << " LUFS, range " << q.meters.lra.load()
                  << " LU, correlation " << q.meters.correlation.load() << std::endl;
        check (std::abs (q.meters.lufsI.load() + 22.6f) < 0.5f, "integrated loudness is gated and correct");
        check (std::abs (q.meters.lra.load() - 10.0f) < 1.5f, "loudness range is correct");
        check (q.meters.correlation.load() < -0.95f, "correlation meter reads -1 for out-of-phase channels");
    }

    if (want ("A/B"))
    {
        std::cout << "A/B" << std::endl;
        AtelierProcessor p;
        p.setPlayConfigDetails (2, 2, sr, 512);
        p.prepareToPlay (sr, 512);
        setParam (p, "tapeDrive", 3.0f);
        p.switchAB (1);
        setParam (p, "tapeDrive", 15.0f);
        p.switchAB (0);
        const float a = p.apvts.getRawParameterValue ("tapeDrive")->load();
        p.switchAB (1);
        const float bv = p.apvts.getRawParameterValue ("tapeDrive")->load();
        check (std::abs (a - 3.0f) < 0.01f && std::abs (bv - 15.0f) < 0.01f, "A and B hold separate settings");
    }

    if (want ("Gate / de-esser"))
    {
        std::cout << "Gate / de-esser" << std::endl;
        AtelierProcessor p;
        p.setPlayConfigDetails (2, 2, sr, 512);
        p.prepareToPlay (sr, 512);
        setParam (p, "tubeOn", 0.0f);
        setParam (p, "gateOn", 1.0f); setParam (p, "gateThr", -40.0f); setParam (p, "gateRange", 60.0f);
        auto quiet = sine (2, len, sr, 1000.0, -60.0f);
        run (p, quiet, 512);
        check (toneDb (quiet, 48000, sr, 1000.0) < -110.0f, "gate closes on signal below threshold");
        auto loud = sine (2, len, sr, 1000.0, -20.0f);
        run (p, loud, 512);
        check (std::abs (rmsDb (loud, 48000) + 23.0f) < 0.5f, "gate passes signal above threshold");

        setParam (p, "gateOn", 0.0f); setParam (p, "dsOn", 1.0f); setParam (p, "dsThr", -40.0f);
        auto ess = sine (2, len, sr, 8000.0, -12.0f);
        run (p, ess, 512);
        auto low = sine (2, len, sr, 300.0, -12.0f);
        run (p, low, 512);
        std::cout << "  8 kHz: " << toneDb (ess, 48000, sr, 8000.0) << " dB, 300 Hz: " << toneDb (low, 48000, sr, 300.0) << " dB" << std::endl;
        check (toneDb (ess, 48000, sr, 8000.0) < -17.0f, "de-esser reduces sibilance band");
        check (std::abs (toneDb (low, 48000, sr, 300.0) + 12.0f) < 0.3f, "de-esser leaves the body alone");
    }

    if (want ("Low-latency mode"))
    {
        std::cout << "Low-latency mode" << std::endl;
        AtelierProcessor p;
        p.setPlayConfigDetails (2, 2, sr, 512);
        p.prepareToPlay (sr, 512);
        const int normalLat = p.getLatencySamples();
        setParam (p, "lowLat", 1.0f);
        p.prepareToPlay (sr, 512);
        const int lat = p.getLatencySamples();
        std::cout << "  latency " << lat << " samples (" << 1000.0 * lat / sr << " ms) against " << normalLat << " normally" << std::endl;
        check (lat <= 16 && lat < normalLat / 8, "low-latency mode reports a fraction of the normal latency");

        allModulesOn (p);
        setParam (p, "bypass", 1.0f);
        const auto in = noise (2, len, -12.0f);
        auto out = in;
        run (p, out, 512);
        float maxErr = 0.0f;
        for (int i = 2000; i < len - lat; ++i)
            maxErr = std::max (maxErr, std::abs (out.getSample (0, i + lat) - in.getSample (0, i)));
        check (maxErr < 1.0e-5f, "low-latency bypass nulls against the input delayed by the reported latency");

        // processed path: a 1 kHz tone through tube, tape and iron should arrive in step with the reported latency
        setParam (p, "bypass", 0.0f); setParam (p, "mode", 2.0f);
        for (auto* id : { "gateOn", "dsOn", "eqOn", "compOn", "imgOn" }) setParam (p, id, 0.0f);
        for (auto* id : { "tubeDrive", "tapeDrive", "ironDrive", "tapeBump" }) setParam (p, id, 0.0f);
        p.prepareToPlay (sr, 512);
        const auto toneIn = sine (2, len, sr, 1000.0, -30.0f);
        auto toneOut = toneIn;
        run (p, toneOut, 512);
        double best = -1.0; int bestLag = 0;
        for (int lag = 0; lag < 40; ++lag)
        {
            double acc = 0.0;
            for (int i = 24000; i < 72000; ++i) acc += (double) toneIn.getSample (0, i) * toneOut.getSample (0, i + lag);
            if (acc > best) { best = acc; bestLag = lag; }
        }
        std::cout << "  processed 1 kHz tone lags by " << bestLag << " samples, reported " << lat << std::endl;
        check (std::abs (bestLag - lat) <= 1, "low-latency processed path matches reported latency");

        // colour stages stay alias-free on the minimum-phase oversamplers
        setParam (p, "tapeDrive", 12.0f); setParam (p, "tubeDrive", 12.0f); setParam (p, "limOn", 0.0f);
        p.prepareToPlay (sr, 512);
        auto hi = sine (2, len, sr, 15000.0, -18.0f);
        run (p, hi, 512);
        const float alias = toneDb (hi, 24000, sr, 3000.0) - toneDb (hi, 24000, sr, 15000.0);
        std::cout << "  15 kHz @ -18 dBFS through tube + tape + iron: alias at 3 kHz " << alias << " dBc" << std::endl;
        check (alias < -60.0f, "low-latency colour stages keep aliasing below -60 dBc");

        // zero look-ahead limiter still holds the ceiling
        setParam (p, "limOn", 1.0f); setParam (p, "limDrive", 18.0f); setParam (p, "limCeil", -1.0f);
        p.prepareToPlay (sr, 512);
        auto b = noise (2, len, -3.0f, 7);
        for (int i = 5000; i < len; i += 7919) { b.setSample (0, i, 1.0f); b.setSample (1, i, -1.0f); }
        run (p, b, 480);
        check (allFinite (b) && b.getMagnitude (0, len) <= juce::Decibels::decibelsToGain (-1.0f) + 1.0e-6f,
               "low-latency limiter never exceeds the ceiling");
    }

    if (want ("External sidechain"))
    {
        std::cout << "External sidechain" << std::endl;
        auto withKey = [&] (AtelierProcessor& p)
        {
            juce::AudioProcessor::BusesLayout layout;
            layout.inputBuses.add (juce::AudioChannelSet::stereo());
            layout.inputBuses.add (juce::AudioChannelSet::stereo());
            layout.outputBuses.add (juce::AudioChannelSet::stereo());
            const bool ok = p.setBusesLayout (layout);
            p.setRateAndBufferSizeDetails (sr, 512);
            p.prepareToPlay (sr, 512);
            return ok;
        };
        // channels 0-1 carry the audio, 2-3 the key; the key plays during [keyFrom, keyTo)
        auto material = [&] (float mainDb, float keyDb, int keyFrom, int keyTo)
        {
            juce::AudioBuffer<float> b (4, len);
            const auto main = sine (2, len, sr, 1000.0, mainDb), key = sine (2, len, sr, 180.0, keyDb);
            for (int ch = 0; ch < 2; ++ch)
            {
                b.copyFrom (ch, 0, main, ch, 0, len);
                b.clear (ch + 2, 0, len);
                b.copyFrom (ch + 2, keyFrom, key, ch, keyFrom, keyTo - keyFrom);
            }
            return b;
        };
        auto levelIn = [&] (const juce::AudioBuffer<float>& b, int from, int to)
        {
            juce::AudioBuffer<float> part (1, to - from);
            part.copyFrom (0, 0, b, 0, from, to - from);
            return toneDb (part, 0, sr, 1000.0);
        };

        for (int model = 0; model < 4; ++model)
        {
            AtelierProcessor p;
            const bool ok = withKey (p);
            if (model == 0) check (ok, "host can enable a stereo sidechain input");
            setParam (p, "tubeOn", 0.0f);
            setParam (p, "compOn", 1.0f); setParam (p, "compExt", 1.0f); setParam (p, "compModel", (float) model);
            setParam (p, "compThr", -30.0f); setParam (p, "compRatio", 4.0f); setParam (p, "compRel", 100.0f);
            auto b = material (-12.0f, -6.0f, 0, len / 2);
            run (p, b, 512);
            const float ducked = levelIn (b, 24000, 44000), open = levelIn (b, 72000, 92000);
            std::cout << "  model " << model << ": audio at -12 dBFS -> " << ducked << " dB while the key plays, " << open << " dB after it stops" << std::endl;
            // the opto cell is meant to let go slowly after long, heavy reduction
            check (allFinite (b) && ducked < -20.0f && std::abs (open + 12.0f) < (model == 2 ? 3.0f : 1.0f),
                   "model " + juce::String (model) + " ducks to the key and recovers when it stops");
        }

        {
            AtelierProcessor p;
            withKey (p);
            setParam (p, "tubeOn", 0.0f);
            setParam (p, "gateOn", 1.0f); setParam (p, "gateExt", 1.0f); setParam (p, "gateThr", -30.0f); setParam (p, "gateRange", 60.0f);
            auto b = material (-20.0f, -10.0f, len / 2, len);
            run (p, b, 512);
            const float shut = levelIn (b, 24000, 44000), open = levelIn (b, 72000, 92000);
            std::cout << "  keyed gate: " << shut << " dB with no key, " << open << " dB with the key playing" << std::endl;
            check (shut < -70.0f && std::abs (open + 20.0f) < 0.5f, "gate opens from the key, not from its own audio");
        }

        {
            // no sidechain connected: the switch must fall back to normal behaviour
            AtelierProcessor p;
            p.setPlayConfigDetails (2, 2, sr, 512);
            p.prepareToPlay (sr, 512);
            setParam (p, "tubeOn", 0.0f);
            setParam (p, "compOn", 1.0f); setParam (p, "compExt", 1.0f); setParam (p, "compThr", -26.0f);
            auto b = sine (2, len, sr, 1000.0, -6.0f);
            run (p, b, 512);
            check (toneDb (b, 48000, sr, 1000.0) < -14.0f && ! p.meters.keyConnected.load(),
                   "with no sidechain connected the compressor listens to its own audio");
        }
    }

    if (want ("Click-free switching (100 Hz tone, largest sample-to-sample step)"))
    {
        std::cout << "Click-free switching (100 Hz tone, largest sample-to-sample step)" << std::endl;
        struct Flip { const char* id; float a, b; };
        static const Flip flips[] = {
            { "gateOn", 1, 0 }, { "tubeOn", 1, 0 }, { "dsOn", 1, 0 }, { "eqOn", 1, 0 }, { "compOn", 1, 0 }, { "tapeOn", 1, 0 },
            { "ironOn", 1, 0 }, { "imgOn", 1, 0 }, { "limOn", 1, 0 }, { "bypass", 0, 1 }, { "phase", 0, 1 }, { "eqPre", 1, 0 },
            { "tubeType", 0, 1 }, { "tubeType", 0, 2 }, { "compModel", 0, 1 }, { "compModel", 0, 3 }, { "tapeSpeed", 1, 0 },
            { "ironType", 0, 2 }, { "eqStyle", 0, 1 }, { "eqMS", 0, 2 }, { "hpf", 150, 20 }, { "lpf", 3000, 22000 },
            { "mode", 0, 2 }, { "mode", 2, 1 }, { "lowLat", 0, 1 }, { "match", 0, 1 }, { "limClip", 0, 100 } };

        float worst = 0.0f, steadyWorst = 0.0f; juce::String worstName;
        for (int baseMode : { 0, 2 })
            for (const auto& f : flips)
            {
                AtelierProcessor p;
                p.setPlayConfigDetails (2, 2, sr, 512);
                p.prepareToPlay (sr, 512);
                allModulesOn (p);
                setParam (p, "mode", (float) baseMode);
                setParam (p, "hpf", 150.0f); setParam (p, "lpf", 3000.0f);
                setParam (p, "lfGain", 10.0f); setParam (p, "lfFreq", 150.0f); setParam (p, "lmGain", -8.0f); setParam (p, "lmFreq", 120.0f);
                setParam (p, "compThr", -36.0f); setParam (p, "compMakeup", 10.0f);
                setParam (p, "tubeDrive", 18.0f); setParam (p, "tapeDrive", 12.0f); setParam (p, "ironDrive", 12.0f);
                setParam (p, "width", 160.0f); setParam (p, "limDrive", 12.0f); setParam (p, "dsThr", -50.0f);
                if (const char* off = std::getenv ("ATELIER_OFF"))
                    for (const auto& id : juce::StringArray::fromTokens (off, ",", ""))
                        setParam (p, id.toRawUTF8(), 0.0f);
                setParam (p, f.id, f.a);

                // settle, then flip the control back and forth every quarter of a second
                auto b = sine (2, 12000 * 6, sr, 100.0, -12.0f);
                for (int ch = 0; ch < 2; ++ch)      // slightly different channels so the stereo stages have work to do
                    for (int i = 0; i < b.getNumSamples(); ++i)
                        b.setSample (ch, i, b.getSample (ch, i) * (ch == 0 ? 1.0f : 0.8f));
                for (int seg = 0; seg < 6; ++seg)
                {
                    if (seg >= 2) setParam (p, f.id, seg % 2 == 0 ? f.b : f.a);
                    juce::AudioBuffer<float> view (b.getArrayOfWritePointers(), 2, seg * 12000, 12000);
                    run (p, view, 480);
                }

                float steady = 0.0f, step = 0.0f;
                int stepAt = 0, stepCh = 0;
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 6000; i < b.getNumSamples(); ++i)
                    {
                        const float d = std::abs (b.getSample (ch, i) - b.getSample (ch, i - 1));
                        if (i < 24000) steady = std::max (steady, d);
                        else if (d > step) { step = d; stepAt = i; stepCh = ch; }
                    }
                if (std::getenv ("ATELIER_DEBUG") != nullptr && step >= std::max (0.03f, 2.5f * steady))
                {
                    std::cout << "    " << f.id << " mode " << baseMode << " at " << stepAt << ":";
                    for (int i = stepAt - 4; i <= stepAt + 3; ++i) std::cout << " " << b.getSample (stepCh, i);
                    std::cout << std::endl;
                }
                steadyWorst = std::max (steadyWorst, steady);
                if (step > worst) { worst = step; worstName = juce::String (f.id) + " (mode " + juce::String (baseMode) + ")"; }
                if (! (allFinite (b) && step < std::max (0.03f, 2.5f * steady)))
                    check (false, juce::String (f.id) + " in mode " + juce::String (baseMode) + ": step " + juce::String (step, 4)
                                      + " against " + juce::String (steady, 4) + " when left alone");
            }
        std::cout << "  largest step while switching " << worst << " at " << worstName << "; largest in steady playback " << steadyWorst << std::endl;
        check (true, "switching matrix completed");
    }

    if (want ("Level-matched bypass / delta listen"))
    {
        std::cout << "Level-matched bypass / delta listen" << std::endl;
        AtelierProcessor p;
        p.setPlayConfigDetails (2, 2, sr, 512);
        p.prepareToPlay (sr, 512);
        setParam (p, "tubeOn", 0.0f);
        setParam (p, "compOn", 1.0f); setParam (p, "compThr", -30.0f); setParam (p, "compMakeup", 14.0f);   // clearly louder

        const auto in = noise (2, (int) sr * 5, -20.0f, 31);
        auto processed = in;
        run (p, processed, 512);
        const float gain = rmsDb (processed, (int) sr * 3) - rmsDb (in, (int) sr * 3);

        setParam (p, "bypass", 1.0f);
        auto plain = in;
        run (p, plain, 512);
        const float plainGain = rmsDb (plain, (int) sr * 3) - rmsDb (in, (int) sr * 3);

        setParam (p, "match", 1.0f);
        auto matched = in;
        run (p, matched, 512);
        const float matchedGain = rmsDb (matched, (int) sr * 3) - rmsDb (in, (int) sr * 3);
        std::cout << "  processing is " << gain << " dB louder; bypass plays at " << plainGain << " dB, matched bypass at " << matchedGain
                  << " dB (meter says " << p.meters.matchDb.load() << ")" << std::endl;
        check (std::abs (plainGain) < 0.05f, "ordinary bypass is still exactly the input");
        check (std::abs (matchedGain - gain) < 0.75f, "matched bypass plays the input at the processed loudness");

        // delta: nothing changed -> silence; a +6 dB bell on its own frequency -> about the input level again (2x - 1x)
        AtelierProcessor q;
        q.setPlayConfigDetails (2, 2, sr, 512);
        q.prepareToPlay (sr, 512);
        setParam (q, "tubeOn", 0.0f);
        q.deltaListen = true;
        q.prepareToPlay (sr, 512);
        auto quiet = noise (2, len, -12.0f, 33);
        run (q, quiet, 512);
        setParam (q, "hmGain", 6.0f); setParam (q, "hmFreq", 3000.0f);
        auto bell = sine (2, len, sr, 3000.0, -18.0f);
        run (q, bell, 512);
        std::cout << "  delta with nothing engaged: " << rmsDb (quiet, 24000) << " dB; with a +6 dB bell on a -18 dB tone: " << toneDb (bell, 48000, sr, 3000.0) << " dB" << std::endl;
        check (rmsDb (quiet, 24000) < -95.0f, "delta is silent when the strip changes nothing");
        check (std::abs (toneDb (bell, 48000, sr, 3000.0) + 18.0f) < 0.6f, "delta plays exactly what the EQ added");

        // delta lines up in low-latency mode and with every colour stage working gently
        setParam (q, "hmGain", 0.0f); setParam (q, "lowLat", 1.0f);
        q.prepareToPlay (sr, 512);
        auto ll = noise (2, len, -12.0f, 34);
        run (q, ll, 512);
        check (rmsDb (ll, 24000) < -95.0f, "delta is silent in low-latency mode too");
    }

    if (want ("Mastering finishers: clipper, dither, dense material"))
    {
        std::cout << "Mastering finishers: clipper, dither, dense material" << std::endl;
        auto master = [&] (AtelierProcessor& p)
        {
            p.setPlayConfigDetails (2, 2, sr, 512);
            p.prepareToPlay (sr, 512);
            setParam (p, "mode", 2.0f); setParam (p, "tubeOn", 0.0f);
            setParam (p, "limOn", 1.0f); setParam (p, "limCeil", -1.0f);
        };

        // drum-like material: short loud hits over a quiet bed, pushed 9 dB into the limiter
        auto drums = [&]
        {
            auto b = noise (2, len, -40.0f, 41);
            for (int i = 0; i < len; ++i)
            {
                const float t = (float) (i % 12000) / (float) sr;
                const float hit = 0.9f * std::exp (-t * 60.0f) * std::sin (2.0f * juce::MathConstants<float>::pi * (60.0f + 140.0f * std::exp (-t * 80.0f)) * t);
                b.addSample (0, i, hit); b.addSample (1, i, hit);
            }
            return b;
        };
        float gr[2] {}, loud[2] {}, tp[2] {};
        for (int c = 0; c < 2; ++c)
        {
            AtelierProcessor p;
            master (p);
            setParam (p, "limDrive", 9.0f); setParam (p, "limClip", c == 0 ? 0.0f : 100.0f);
            auto b = drums();
            run (p, b, 512);
            p.meters.grLim.store (0.0f);
            auto again = drums();
            run (p, again, 512);
            gr[c] = p.meters.grLim.load(); loud[c] = p.meters.lufsI.load(); tp[c] = referenceTruePeakDb (again, 4800);
        }
        std::cout << "  9 dB of drive on drum hits: limiter alone removes " << gr[0] << " dB (" << loud[0] << " LUFS, " << tp[0]
                  << " dBTP); with Clip at 100 % it removes " << gr[1] << " dB (" << loud[1] << " LUFS, " << tp[1] << " dBTP)" << std::endl;
        check (gr[0] > 5.0f && gr[1] < 1.5f, "the clipper takes the peaks so the limiter barely works");
        check (tp[0] <= -0.8f && tp[1] <= -0.8f, "true peak stays at the ceiling with and without the clipper");
        check (loud[1] > loud[0] + 0.3f, "clipping the hits ends up louder than limiting them");

        {
            // clipper aliasing: 15 kHz driven hard into it; anything at 3 kHz has folded back
            AtelierProcessor p;
            master (p);
            setParam (p, "limDrive", 12.0f); setParam (p, "limClip", 100.0f);
            auto hi = sine (2, len, sr, 15000.0, -6.0f);
            run (p, hi, 512);
            const float alias = toneDb (hi, 24000, sr, 3000.0) - toneDb (hi, 24000, sr, 15000.0);
            std::cout << "  15 kHz clipped by about 5 dB: alias at 3 kHz " << alias << " dBc" << std::endl;
            check (alias < -70.0f, "clipper aliasing is below -70 dBc");
        }

        {
            // the case that used to overshoot: full-band noise driven hard
            AtelierProcessor p;
            master (p);
            setParam (p, "limDrive", 18.0f);
            auto b = noise (2, len, -3.0f, 7);
            run (p, b, 512);
            const float truePk = referenceTruePeakDb (b, 24000);
            // White noise right up to the top of the band is the hardest possible case: its inter-sample peaks come
            // from content no converter passes. A standard meter must read under the ceiling; the ideal-reconstruction
            // reference (stricter than any meter) may sit a little above it.
            const float metered = juce::Decibels::gainToDecibels (p.meters.truePeak.load());
            std::cout << "  full-band noise, 18 dB of drive: meter reads " << metered << " dBTP against a -1.0 ceiling; ideal reconstruction "
                      << truePk << " dBTP" << std::endl;
            check (metered <= -1.0f + 0.05f, "dense material reads at or under the ceiling on a standard true-peak meter");
            check (truePk <= 0.0f, "dense material stays within 1 dB of the ceiling even by ideal reconstruction");
        }

        for (int mode = 1; mode <= 3; ++mode)
        {
            AtelierProcessor p;
            master (p);
            setParam (p, "limOn", 0.0f); setParam (p, "dither", (float) mode);
            juce::AudioBuffer<float> b (2, len);
            b.clear();
            run (p, b, 512);
            const double lsb = mode == 1 ? 1.0 / 8388608.0 : 1.0 / 32768.0;
            bool onGrid = true;
            for (int i = 0; i < len; ++i)
            {
                const double v = (double) b.getSample (0, i) / lsb;
                if (std::abs (v - std::round (v)) > 1.0e-3) onGrid = false;
            }
            // noise in the band the ear cares about most, 0.5 to 4 kHz
            double band = 0.0; int bins = 0;
            for (double f = 500.0; f <= 4000.0; f += 250.0) { band += std::pow (10.0, toneDb (b, 24000, sr, f) / 10.0); ++bins; }
            const float bandDb = (float) (10.0 * std::log10 (band / bins));
            double sq = 0.0;
            for (int i = 24000; i < len; ++i) sq += (double) b.getSample (0, i) * b.getSample (0, i);
            const float noiseDb = (float) (10.0 * std::log10 (std::max (1.0e-30, sq / (len - 24000))));
            static float flatBand = 0.0f;
            if (mode == 2) flatBand = bandDb;
            std::cout << "  dither mode " << mode << " on silence: noise " << noiseDb << " dBFS, 0.5 to 4 kHz content " << bandDb << " dB" << std::endl;
            const float expected = mode == 1 ? -144.5f : -96.3f;
            if (mode < 3) check (onGrid && std::abs (noiseDb - expected) < 1.5f, "dither mode " + juce::String (mode) + " rounds to its bit depth with the right noise level");
            else          check (onGrid && bandDb < flatBand - 6.0f, "shaped dither moves the noise out of the midrange");
        }

        {
            AtelierProcessor p;
            master (p);
            setParam (p, "dither", 2.0f); setParam (p, "bypass", 1.0f);
            const auto in = noise (2, len, -12.0f);
            auto out = in;
            run (p, out, 512);
            const int lat = p.getLatencySamples();
            float maxErr = 0.0f;
            for (int i = 2000; i < len - lat; ++i) maxErr = std::max (maxErr, std::abs (out.getSample (0, i + lat) - in.getSample (0, i)));
            check (maxErr < 1.0e-5f, "bypass stays bit-transparent with dither selected");
        }
    }

    if (want ("Valve types"))
    {
        std::cout << "Valve types" << std::endl;
        static const char* names[] = { "triode", "pentode", "push-pull" };
        struct Reading { float fund, h2, h3; };
        auto measure = [&] (int type, float level, float drive)
        {
            AtelierProcessor p;
            p.setPlayConfigDetails (2, 2, sr, 512);
            p.prepareToPlay (sr, 512);
            setParam (p, "tubeType", (float) type); setParam (p, "tubeDrive", drive);
            auto b = sine (2, len, sr, 1000.0, level);
            run (p, b, 512);
            const float f = toneDb (b, 24000, sr, 1000.0);
            return Reading { f, toneDb (b, 24000, sr, 2000.0) - f, toneDb (b, 24000, sr, 3000.0) - f };
        };

        Reading gentle[3], hard[3];
        for (int type = 0; type < 3; ++type)
        {
            gentle[type] = measure (type, -18.0f, 6.0f);
            hard[type] = measure (type, -6.0f, 12.0f);
            std::cout << "  " << names[type] << ": 0 VU at 6 dB of drive -> " << gentle[type].fund << " dB, H2 " << gentle[type].h2 << ", H3 " << gentle[type].h3
                      << " dBc;  -6 dBFS at 12 dB -> " << hard[type].fund << " dB, H2 " << hard[type].h2 << ", H3 " << hard[type].h3 << " dBc" << std::endl;
            check (std::abs (gentle[type].fund + 18.0f) < 0.5f, juce::String (names[type]) + " is at unity gain for a 0 VU signal");
            // 18 dB hotter than the stage is calibrated for, the level must not rise; a saturating valve loses a good deal
            check (hard[type].fund < -5.0f && hard[type].fund > -20.0f, juce::String (names[type]) + " saturates without gaining level");

            AtelierProcessor p;
            p.setPlayConfigDetails (2, 2, sr, 512);
            p.prepareToPlay (sr, 512);
            setParam (p, "tubeType", (float) type); setParam (p, "tubeDrive", 12.0f);
            auto hi = sine (2, len, sr, 15000.0, -18.0f);
            run (p, hi, 512);
            const float alias = toneDb (hi, 24000, sr, 3000.0) - toneDb (hi, 24000, sr, 15000.0);
            std::cout << "    15 kHz @ -18 dBFS: alias at 3 kHz " << alias << " dBc" << std::endl;
            check (alias < -70.0f, juce::String (names[type]) + " aliasing is below -70 dBc at realistic HF level");

            // Bias with nothing playing: the operating point moves a long way, and none of it should reach the output
            p.prepareToPlay (sr, 512);
            float thump = 0.0f;
            for (int block = 0; block < 200; ++block)
            {
                if (block == 20) setParam (p, "tubeBias", 100.0f);
                if (block == 100) setParam (p, "tubeBias", 0.0f);
                juce::AudioBuffer<float> quiet (2, 512);
                quiet.clear();
                run (p, quiet, 512);
                thump = std::max (thump, quiet.getMagnitude (0, 512));
            }
            std::cout << "    Bias swept end to end and back in silence: output peak " << juce::Decibels::gainToDecibels (thump, -200.0f) << " dBFS" << std::endl;
            check (thump < 1.0e-4f, juce::String (names[type]) + " can be re-biased without a thump");
        }
        check (gentle[0].h2 > gentle[0].h3 + 10.0f, "the triode's colour is mostly 2nd harmonic");
        // the pair is matched to 5 %, so a trace of 2nd harmonic survives the cancellation, as it does in hardware
        check (gentle[2].h2 < gentle[0].h2 - 10.0f && hard[2].h3 > hard[2].h2 + 10.0f, "the push-pull pair cancels the 2nd harmonic and leaves the 3rd");
        // "more bite": pushed, the pentode's distortion is far richer in odd harmonics than the triode's
        check (gentle[1].h2 < gentle[0].h2 - 6.0f && hard[1].h3 - hard[1].h2 > hard[0].h3 - hard[0].h2 + 6.0f,
               "the pentode is cleaner than the triode when driven gently, and its distortion has more bite when pushed");
    }

    if (want ("Iron cores"))
    {
        std::cout << "Iron cores" << std::endl;
        struct Reading { float h2, h3, thd; };
        auto measure = [&] (int core, double freq)
        {
            AtelierProcessor p;
            p.setPlayConfigDetails (2, 2, sr, 512);
            p.prepareToPlay (sr, 512);
            setParam (p, "tubeOn", 0.0f); setParam (p, "ironOn", 1.0f); setParam (p, "ironType", (float) core);
            auto b = sine (2, len, sr, freq, -18.0f + 3.0103f);     // 0 VU, at the default drive
            run (p, b, 512);
            const float f = toneDb (b, 24000, sr, freq);
            double harm = 0.0;
            for (int h = 2; h <= 5; ++h) harm += std::pow (10.0, (toneDb (b, 24000, sr, freq * h) - f) / 10.0);
            return Reading { toneDb (b, 24000, sr, 2.0 * freq) - f, toneDb (b, 24000, sr, 3.0 * freq) - f, (float) (10.0 * std::log10 (harm + 1.0e-30)) };
        };
        static const char* coreNames[] = { "steel", "nickel", "console" };
        Reading bass[3], mid[3];
        for (int c = 0; c < 3; ++c)
        {
            bass[c] = measure (c, 40.0); mid[c] = measure (c, 1000.0);
            std::cout << "  " << coreNames[c] << ", 0 VU at the default drive: harmonics " << bass[c].thd << " dBc at 40 Hz, " << mid[c].thd
                      << " dBc at 1 kHz (H2 " << bass[c].h2 << ", H3 " << bass[c].h3 << " at 40 Hz)" << std::endl;
            check (bass[c].thd > mid[c].thd + 25.0f, juce::String (coreNames[c]) + " saturates the bass long before the midrange");
        }
        check (bass[1].thd < bass[0].thd - 4.0f && bass[1].thd < bass[2].thd - 4.0f, "nickel is the cleanest core");
        check (bass[2].h2 > bass[0].h2 + 20.0f && bass[2].h2 > bass[1].h2 + 20.0f, "the console core's standing current gives it the 2nd harmonic");

        // Hysteresis: at low levels the loop leaves a floor of odd harmonics that no longer falls with level
        auto quiet = [&] (const atelier::IronCircuit::Core& core, double volts)
        {
            const double rate = 384000.0;
            atelier::IronCircuit ic;
            ic.prepare (rate);
            ic.setCore (core);
            const int n = (int) (0.75 * rate), from = (int) (0.25 * rate);
            juce::AudioBuffer<float> b (1, n - from);
            for (int i = 0; i < n; ++i)
            {
                const double y = ic.processVolts (0, volts * std::sin (2.0 * juce::MathConstants<double>::pi * 40.0 * i / rate));
                if (i >= from) b.setSample (0, i - from, (float) (y / volts));
            }
            return toneDb (b, 0, rate, 120.0) - toneDb (b, 0, rate, 40.0);
        };
        const float steel20 = quiet (atelier::IronCircuit::steel, 2.0), steel40 = quiet (atelier::IronCircuit::steel, 0.2),
                    nickel40 = quiet (atelier::IronCircuit::nickel, 0.2);
        auto noLoop = atelier::IronCircuit::steel;
        noLoop.hc = 0.0;
        const float steel40NoLoop = quiet (noLoop, 0.2);
        std::cout << "  40 Hz, H3 at 2 V and 0.2 V: steel " << steel20 << ", " << steel40 << " dBc (" << steel40NoLoop << " without the loop); nickel at 0.2 V "
                  << nickel40 << " dBc" << std::endl;
        check (steel40 > steel40NoLoop + 3.0f && std::abs (steel40 - steel20) < 8.0f, "the core's hysteresis loop leaves a low-level floor that stops falling with level");
        check (nickel40 < steel40 - 15.0f, "nickel's loop is far narrower than steel's");
    }

    if (want ("Valve circuits against ngspice"))
    {
        std::cout << "Valve circuits against ngspice" << std::endl;
        juce::File ngspice (juce::SystemStats::getEnvironmentVariable ("NGSPICE", {}));
        const auto ngHome = juce::File::getSpecialLocation (juce::File::userHomeDirectory).getChildFile ("Library/Caches/ngspice-44.2");
        if (! ngspice.existsAsFile()) ngspice = ngHome.getChildFile ("bin/ngspice");

        if (! ngspice.existsAsFile())
        {
            std::cout << "  SKIPPED: ngspice not found (set NGSPICE=/path/to/ngspice)" << std::endl;
        }
        else
        {
            const double osRate = 192000.0;
            const int total = (int) (0.3 * osRate), from = (int) (0.1 * osRate);
            const juce::String grid = "Bg G K I=V(G,K)>0 ? pwr(V(G,K),1.5)/1300 : 0\n";
            const juce::String triodeModel = juce::String (".subckt TRIODE P G K\n")
                + "Bp P K I=V(E1)>0 ? 2*pwr(V(E1),1.4)/1060 : 0\n"
                + "Be E1 0 V=V(P,K)/600*ln(1+exp(600*(1/100+V(G,K)/sqrt(300+V(P,K)*V(P,K)))))\n" + grid + ".ends\n";
            const juce::String pentodeModel = juce::String (".subckt PENTODE P G K S\n")
                + "Be E1 0 V=V(S,K)/222.06*ln(1+exp(222.06*(1/34.9+V(G,K)/V(S,K))))\n"
                + "Bp P K I=(V(E1)>0 && V(P,K)>0) ? 2*pwr(V(E1),1.35)/2648.1*atan(V(P,K)/4.7) : 0\n"
                + "Bs S K I=(V(G,K)+V(S,K)/34.9)>0 ? pwr(V(G,K)+V(S,K)/34.9,1.35)/4627 : 0\n" + grid + ".ends\n";
            auto half = [&] (const char* name, double kg1)
            {
                return juce::String (".subckt ") + name + " P G K\n"
                    + "Bp P K I=V(E1)>0 ? 2*pwr(V(E1),1.3)/" + juce::String (kg1) + " : 0\n"
                    + "Be E1 0 V=V(P,K)/84*ln(1+exp(84*(1/21.5+V(G,K)/sqrt(300+V(P,K)*V(P,K)))))\n" + grid + ".ends\n";
            };

            // each deck is the same circuit as the C++ model, part for part; AMP is the test tone's amplitude
            const juce::String triodeDeck = triodeModel
                + "Vb bp 0 250\nVin in 0 SIN(0 AMP 220)\n"
                + "Cin in g1 47n\nRgl g1 0 1Meg\nRgs g1 g 33k\nX1 p g k TRIODE\n"
                + "Rp bp p 100k\nRk k 0 1.5k\nCk k 0 22u\nCout p out 100n\nRl out 0 1Meg\n";
            const juce::String pentodeDeck = pentodeModel
                + "Vb bp 0 250\nVin in 0 SIN(0 AMP 220)\n"
                + "Cin in g1 47n\nRgl g1 0 1Meg\nRgs g1 g 33k\nX1 p g k s PENTODE\n"
                + "Rp bp p 220k\nRs bp s 1Meg\nCs s 0 100n\nRk k 0 2.2k\nCk k 0 22u\nCout p out 100n\nRl out 0 1Meg\n";
            // the output transformer is ideal: each plate sits at the supply minus or plus the winding voltage,
            // and the winding is driven by the difference between the two plate currents
            const juce::String pairDeck = half ("HALFA", 1180.0) + half ("HALFB", 1180.0 * atelier::PushPullCircuit::mismatch)
                + "Vin in 0 SIN(0 AMP 220)\nEinv inb 0 in 0 -1\n"
                + "CinA in ja 47n\nRglA ja 0 470k\nRgsA ja ga " + juce::String (atelier::PushPullCircuit::Rgs) + "\nCinB inb jb 47n\nRglB jb 0 470k\nRgsB jb gb " + juce::String (atelier::PushPullCircuit::Rgs) + "\n"
                + "B1 pax 0 V=250-V(out)\nVs1 pax pa 0\nB2 pbx 0 V=250+V(out)\nVs2 pbx pb 0\n"
                + "XA pa ga k HALFA\nXB pb gb k HALFB\nRk k 0 820\nCk k 0 100u\n"
                + "Bw 0 out I=I(Vs1)-I(Vs2)\nRh out 0 5k\nLh out 0 40\n";

            auto reference = [&] (const juce::String& circuit, double amp)
            {
                const auto deck = outDir.getChildFile ("valve.cir"), data = outDir.getChildFile ("valve.txt");
                data.deleteFile();
                deck.replaceWithText ("Valve stage\n" + circuit.replace ("AMP", juce::String (amp))
                                      + ".options reltol=1e-5\n.control\ntran 5.2083333333u 0.3 0 2.6u\nlinearize v(out)\nwrdata valve.txt v(out)\n.endc\n.end\n");

                juce::ChildProcess proc;
                // ngspice writes its data file relative to where it is run, and takes file names literally
                juce::StringArray cmd { "/bin/sh", "-c", "cd " + outDir.getFullPathName().quoted() + " && SPICE_LIB_DIR="
                                                             + ngspice.getParentDirectory().getSiblingFile ("share/ngspice").getFullPathName().quoted()
                                                             + " " + ngspice.getFullPathName().quoted() + " -b valve.cir > /dev/null 2>&1" };
                std::vector<double> ref;
                if (proc.start (cmd) && proc.waitForProcessToFinish (120000))
                {
                    juce::StringArray lines;
                    lines.addLines (data.loadFileAsString());
                    for (const auto& line : lines)
                    {
                        const auto parts = juce::StringArray::fromTokens (line.trim(), " \t", "");
                        if (parts.size() >= 2) ref.push_back (parts[parts.size() - 1].getDoubleValue());
                    }
                }
                return ref;
            };

            auto against = [&] (const char* name, const juce::String& circuit, std::initializer_list<double> amps, auto&& model)
            {
                float worst = -200.0f;
                for (double amp : amps)
                {
                    const auto ref = reference (circuit, amp);
                    if ((int) ref.size() < total - 2) { check (false, juce::String ("ngspice produced a reference for the ") + name + " at " + juce::String (amp) + " V"); continue; }

                    model.prepare (osRate);
                    double err = 0.0, sig = 0.0;
                    for (int i = 0; i < total - 2; ++i)
                    {
                        const double y = model.processVolts (0, amp * std::sin (2.0 * juce::MathConstants<double>::pi * 220.0 * i / osRate));
                        if (i >= from) { err += (y - ref[(size_t) i]) * (y - ref[(size_t) i]); sig += ref[(size_t) i] * ref[(size_t) i]; }
                    }
                    const float diff = (float) (10.0 * std::log10 (std::max (1.0e-30, err / sig)));
                    worst = std::max (worst, diff);
                    std::cout << "  " << name << ", " << amp << " V at the grid: difference from ngspice " << diff << " dB" << std::endl;
                }
                check (worst < -60.0f, juce::String (name) + " matches the ngspice simulation of the same circuit to better than -60 dB");
            };

            // from a whisper to several times what it takes to clip each stage
            against ("triode", triodeDeck, { 0.05, 0.5, 2.0, 8.0 }, atelier::TubeCircuit());
            against ("pentode", pentodeDeck, { 0.01, 0.1, 0.5, 2.0, 8.0 }, atelier::PentodeCircuit());
            against ("push-pull pair", pairDeck, { 0.2, 2.0, 8.0, 20.0, 40.0 }, atelier::PushPullCircuit());

            {
                // The vari-mu pair at the compressor's own rate, with the side chain's control voltage both held
                // (ramped up over 10 ms, then steady) and swinging fast, which is when a mismatched pair would thump.
                using VM = atelier::VariMuCircuit;
                const double rate = 48000.0;
                auto remote = [] (const char* name, double strength)
                {
                    juce::String d = juce::String (".subckt ") + name + " P G K\n";
                    for (int k = 0; k < VM::sections; ++k)
                    {
                        const double mu = VM::sectionMu[k], kp = mu * 6.0, kg1 = VM::sectionKg1[k] / strength;
                        const juce::String e = "E" + juce::String (k);
                        d << "Be" << k << " " << e << " 0 V=V(P,K)/" << juce::String (kp, 6) << "*ln(1+exp(" << juce::String (kp, 6) << "*(1/"
                          << juce::String (mu, 6) << "+V(G,K)/sqrt(300+V(P,K)*V(P,K)))))\n"
                          << "Bp" << k << " P K I=V(" << e << ")>0 ? 2*pwr(V(" << e << "),1.4)/" << juce::String (kg1, 6) << " : 0\n";
                    }
                    return d + "Bg G K I=V(G,K)>0 ? pwr(V(G,K),1.5)/1300 : 0\n.ends\n";
                };
                float worst = -200.0f;
                struct Case { double amp; juce::String control; std::function<double (double)> at; const char* what; };
                const auto ramp = [] (double v) { return [v] (double t) { return v * std::min (1.0, t / 0.01); }; };
                const auto swing = [] (double t) { return t < 0.1 ? 0.0 : t < 0.101 ? 15000.0 * (t - 0.1) : t < 0.25 ? 15.0 : t < 0.251 ? 15.0 - 15000.0 * (t - 0.25) : 0.0; };
                const Case cases[] = { { 0.1, "PWL(0 0 0.01 20)", ramp (20.0), "20 V on the grids" }, { 2.0, "PWL(0 0 0.01 0)", ramp (0.0), "no control voltage" },
                                       { 2.0, "PWL(0 0 0.01 8)", ramp (8.0), "8 V on the grids" }, { 0.5, "PWL(0 0 0.1 0 0.101 15 0.25 15 0.251 0)", swing, "control swinging 0 to 15 V in 1 ms" } };
                for (const auto& c : cases)
                {
                    const auto deck = outDir.getChildFile ("valve.cir"), data = outDir.getChildFile ("valve.txt");
                    data.deleteFile();
                    deck.replaceWithText ("Vari-mu pair\n" + remote ("VA", 1.0) + remote ("VB", 1.0 / VM::mismatch)
                        + "Vin in 0 SIN(0 " + juce::String (c.amp) + " 220)\nVc c 0 " + c.control + "\n"
                        + "BsA sa 0 V=V(in)-V(c)-" + juce::String (VM::restBias) + "\nBsB sb 0 V=-V(in)-V(c)-" + juce::String (VM::restBias) + "\n"
                        + "RgA sa ga " + juce::String (VM::Rg) + "\nRgB sb gb " + juce::String (VM::Rg) + "\n"
                        + "B1 pax 0 V=" + juce::String (VM::supply) + "-V(out)\nVs1 pax pa 0\nB2 pbx 0 V=" + juce::String (VM::supply) + "+V(out)\nVs2 pbx pb 0\n"
                        + "XA pa ga 0 VA\nXB pb gb 0 VB\nBw 0 out I=I(Vs1)-I(Vs2)\nRh out 0 " + juce::String (VM::Rh) + "\nLh out 0 " + juce::String (VM::Lh) + "\n"
                        // the time step is written in full: rounded to six figures, the two simulations slowly drift apart
                        + ".options reltol=1e-5\n.control\ntran " + juce::String (1.0 / rate, 15) + " 0.4 0 " + juce::String (0.5 / rate, 15)
                        + "\nlinearize v(out)\nwrdata valve.txt v(out)\n.endc\n.end\n");
                    juce::ChildProcess proc;
                    juce::StringArray cmd { "/bin/sh", "-c", "cd " + outDir.getFullPathName().quoted() + " && SPICE_LIB_DIR="
                                                                 + ngspice.getParentDirectory().getSiblingFile ("share/ngspice").getFullPathName().quoted()
                                                                 + " " + ngspice.getFullPathName().quoted() + " -b valve.cir > /dev/null 2>&1" };
                    std::vector<double> ref;
                    if (proc.start (cmd) && proc.waitForProcessToFinish (120000))
                    {
                        juce::StringArray lines;
                        lines.addLines (data.loadFileAsString());
                        for (const auto& line : lines)
                        {
                            const auto parts = juce::StringArray::fromTokens (line.trim(), " \t", "");
                            if (parts.size() >= 2) ref.push_back (parts[parts.size() - 1].getDoubleValue());
                        }
                    }
                    const int n = (int) (0.4 * rate) - 2;
                    if ((int) ref.size() < n) { check (false, juce::String ("ngspice produced a reference for the vari-mu pair, ") + c.what); continue; }

                    VM v;
                    v.prepare (rate);
                    double err = 0.0, sig = 0.0;
                    for (int i = 0; i < n; ++i)
                    {
                        const double t = i / rate;
                        const double y = v.processVolts (0, c.amp * std::sin (2.0 * juce::MathConstants<double>::pi * 220.0 * t), c.at (t));
                        if (t >= 0.05) { err += (y - ref[(size_t) i]) * (y - ref[(size_t) i]); sig += ref[(size_t) i] * ref[(size_t) i]; }
                    }
                    const float diff = (float) (10.0 * std::log10 (std::max (1.0e-30, err / sig)));
                    worst = std::max (worst, diff);
                    std::cout << "  vari-mu pair, " << c.amp << " V, " << c.what << ": difference from ngspice " << diff << " dB" << std::endl;
                }
                check (worst < -60.0f, "vari-mu pair matches the ngspice simulation of the same circuit to better than -60 dB");
            }

            {
                // The opto cell at the compressor's rate: the panel driven as the side chain drives it (held, then
                // released; hard and long; and in quick bursts), with the knobs at the cell's own pace and sped up.
                using OC = atelier::OptoCell;
                const double rate = 48000.0, length = 1.5;
                struct Case { std::vector<std::pair<double, double>> drive; double sa, sr; const char* what; };
                const double v12 = OC::voltsFor (12.0), v20 = OC::voltsFor (20.0), v30 = OC::voltsFor (30.0);
                std::vector<std::pair<double, double>> bursts { { 0.0, 0.0 } };
                for (int k = 0; k < 6; ++k)
                {
                    const double t0 = 0.05 + 0.1 * k;
                    bursts.insert (bursts.end(), { { t0, 0.0 }, { t0 + 0.001, v20 }, { t0 + 0.05, v20 }, { t0 + 0.051, 0.0 } });
                }
                const Case cases[] = { { { { 0, 0 }, { 0.005, v12 }, { 0.5, v12 }, { 0.501, 0 } }, 1.0, 1.0, "12 dB for half a second, own pace" },
                                       { { { 0, 0 }, { 0.005, v12 }, { 0.5, v12 }, { 0.501, 0 } }, 4.0, 2.0, "12 dB for half a second, knobs faster" },
                                       { { { 0, 0 }, { 0.005, v30 }, { 1.0, v30 }, { 1.001, 0 } }, 1.0, 1.0, "30 dB for a second" },
                                       { bursts, 8.0, 3.0, "20 dB in 50 ms bursts" } };
                float worst = -200.0f;
                for (const auto& c : cases)
                {
                    auto at = [&c] (double t)
                    {
                        const auto& d = c.drive;
                        for (size_t k = 1; k < d.size(); ++k)
                            if (t <= d[k].first) return d[k - 1].second + (d[k].second - d[k - 1].second) * (t - d[k - 1].first) / (d[k].first - d[k - 1].first);
                        return d.back().second;
                    };
                    juce::String pwl ("PWL(");
                    for (const auto& [t, v] : c.drive) pwl << juce::String (t, 6) << " " << juce::String (v, 9) << " ";
                    pwl = pwl.trimEnd() + ")";

                    const auto deck = outDir.getChildFile ("opto.cir"), data = outDir.getChildFile ("opto.txt");
                    data.deleteFile();
                    auto num = [] (double v) { return juce::String (v, 12); };
                    deck.replaceWithText (juce::String ("Opto cell\n")
                        + "Vv v 0 " + pwl + "\n"
                        + "BA a 0 V=V(v)>1e-3 ? exp(-" + num (OC::panelB) + "/sqrt(V(v))) : 0\n"
                        + "BL 0 l I=(V(a)-V(l))/" + num (OC::panelTau) + "\nCL l 0 1\n"
                        + "Br r 0 V=" + num (OC::gain) + "*V(l)-V(x)/" + num (OC::lifetime) + "-" + num (OC::pairRate) + "*V(x)*V(x)-"
                            + num (OC::capture) + "*V(x)*(1-V(z))+" + num (OC::trapCap) + "*V(z)/" + num (OC::trapTime) + "\n"
                        + "Bx 0 x I=V(r)>0 ? " + num (c.sa) + "*V(r) : " + num (c.sr) + "*V(r)\nCx x 0 1\n"
                        + "Bz 0 z I=(" + num (OC::capture) + "*V(x)*(1-V(z))-" + num (OC::trapCap) + "*V(z)/" + num (OC::trapTime) + ")/" + num (OC::trapCap) + "\nCz z 0 1\n"
                        + "Vin in 0 SIN(0 1 220)\nRs in out " + num (OC::Rs) + "\n"
                        + "Bc out 0 I=V(out)*(V(x)+" + num (OC::xd) + ")/" + num (OC::Rs) + "\n"
                        + ".ic V(l)=0 V(x)=0 V(z)=0\n.options reltol=1e-6\n.control\ntran " + juce::String (1.0 / rate, 15) + " " + juce::String (length)
                        + " 0 " + juce::String (0.5 / rate, 15) + " uic\nlinearize v(out)\nwrdata opto.txt v(out)\n.endc\n.end\n");
                    juce::ChildProcess proc;
                    juce::StringArray cmd { "/bin/sh", "-c", "cd " + outDir.getFullPathName().quoted() + " && SPICE_LIB_DIR="
                                                                 + ngspice.getParentDirectory().getSiblingFile ("share/ngspice").getFullPathName().quoted()
                                                                 + " " + ngspice.getFullPathName().quoted() + " -b opto.cir > /dev/null 2>&1" };
                    std::vector<double> ref;
                    if (proc.start (cmd) && proc.waitForProcessToFinish (300000))
                    {
                        juce::StringArray lines;
                        lines.addLines (data.loadFileAsString());
                        for (const auto& line : lines)
                        {
                            const auto parts = juce::StringArray::fromTokens (line.trim(), " \t", "");
                            if (parts.size() >= 2) ref.push_back (parts[parts.size() - 1].getDoubleValue());
                        }
                    }
                    const int n = (int) (length * rate) - 2;
                    if ((int) ref.size() < n) { check (false, juce::String ("ngspice produced a reference for the opto cell, ") + c.what); continue; }

                    OC cell;
                    cell.prepare (rate);
                    cell.setTiming (c.sa, c.sr);
                    double err = 0.0, sig = 0.0, peakGr = 0.0;
                    for (int i = 0; i < n; ++i)
                    {
                        const double t = i / rate;
                        if (i > 0) cell.step (at (t));
                        peakGr = std::max (peakGr, cell.reduction());
                        const double y = cell.divide (std::sin (2.0 * juce::MathConstants<double>::pi * 220.0 * t));
                        if (t >= 0.01) { err += (y - ref[(size_t) i]) * (y - ref[(size_t) i]); sig += ref[(size_t) i] * ref[(size_t) i]; }
                    }
                    const float diff = (float) (10.0 * std::log10 (std::max (1.0e-30, err / sig)));
                    worst = std::max (worst, diff);
                    std::cout << "  opto cell, " << c.what << " (peak " << peakGr << " dB): difference from ngspice " << diff << " dB" << std::endl;
                }
                check (worst < -60.0f, "the opto cell matches the ngspice simulation of the same circuit to better than -60 dB");
            }

            {
                // The FET gain cell against ngspice's own JFET model: held at light, medium and heavy reduction with
                // a signal from small to well into the channel's saturation, and with the control swinging fast.
                using FC = atelier::FetCircuit;
                const double rate = 48000.0, length = 0.2;
                struct Case { double amp; juce::String control; std::function<double (double)> at; juce::String what; };
                std::vector<Case> cases;
                for (double gr : { 6.0, 15.0, 30.0 })
                    for (double amp : { 0.05, 0.3, 1.0 })
                    {
                        const double v = FC::gateFor (gr);
                        cases.push_back ({ amp, "DC " + juce::String (v, 12), [v] (double) { return v; }, juce::String (gr) + " dB, " + juce::String (amp) + " V" });
                    }
                const double v0 = FC::restGate, v1 = FC::gateFor (25.0);
                cases.push_back ({ 0.5, "PWL(0 " + juce::String (v0, 12) + " 0.05 " + juce::String (v0, 12) + " 0.0505 " + juce::String (v1, 12) + " 0.12 "
                                        + juce::String (v1, 12) + " 0.1205 " + juce::String (v0, 12) + ")",
                                  [v0, v1] (double t) { return t < 0.05 ? v0 : t < 0.0505 ? v0 + (v1 - v0) * (t - 0.05) / 0.0005 : t < 0.12 ? v1
                                                             : t < 0.1205 ? v1 + (v0 - v1) * (t - 0.12) / 0.0005 : v0; },
                                  "control swinging to 25 dB in 0.5 ms, 0.5 V" });
                float worst = -200.0f;
                for (const auto& c : cases)
                {
                    const auto deck = outDir.getChildFile ("fet.cir"), data = outDir.getChildFile ("fet.txt");
                    data.deleteFile();
                    auto num = [] (double v) { return juce::String (v, 12); };
                    deck.replaceWithText (juce::String ("FET gain cell\n")
                        + "Vin in 0 SIN(0 " + juce::String (c.amp) + " 220)\nVc c 0 " + c.control + "\n"
                        + "Rs in d " + num (FC::Rs) + "\nCf d m " + num (FC::Cf) + "\nRf m g " + num (FC::Rf) + "\nRc c g " + num (FC::Rc) + "\n"
                        + "J1 d g 0 JM\n.model JM NJF(Beta=" + num (FC::beta) + " Vto=" + num (FC::vto) + " Lambda=" + num (FC::lambda) + ")\n"
                        // ngspice's gmin, a conductance it adds across every junction to help it converge, would leak
                        // a few picoamps through the 10 M gate resistors; the junction's own leakage is kept
                        + ".options gmin=1e-20 reltol=1e-7 abstol=1e-15 vntol=1e-9\n.control\ntran " + juce::String (1.0 / rate, 15) + " " + juce::String (length)
                        + " 0 " + juce::String (0.5 / rate, 15) + "\nlinearize v(d)\nwrdata fet.txt v(d)\n.endc\n.end\n");
                    juce::ChildProcess proc;
                    juce::StringArray cmd { "/bin/sh", "-c", "cd " + outDir.getFullPathName().quoted() + " && SPICE_LIB_DIR="
                                                                 + ngspice.getParentDirectory().getSiblingFile ("share/ngspice").getFullPathName().quoted()
                                                                 + " " + ngspice.getFullPathName().quoted() + " -b fet.cir > /dev/null 2>&1" };
                    std::vector<double> ref;
                    if (proc.start (cmd) && proc.waitForProcessToFinish (120000))
                    {
                        juce::StringArray lines;
                        lines.addLines (data.loadFileAsString());
                        for (const auto& line : lines)
                        {
                            const auto parts = juce::StringArray::fromTokens (line.trim(), " \t", "");
                            if (parts.size() >= 2) ref.push_back (parts[parts.size() - 1].getDoubleValue());
                        }
                    }
                    const int n = (int) (length * rate) - 2;
                    if ((int) ref.size() < n) { check (false, "ngspice produced a reference for the FET gain cell, " + c.what); continue; }

                    FC f;
                    f.prepare (rate);
                    f.reset (c.at (0.0));
                    double err = 0.0, sig = 0.0;
                    for (int i = 0; i < n; ++i)
                    {
                        const double t = i / rate;
                        const double y = f.processVolts (0, c.amp * std::sin (2.0 * juce::MathConstants<double>::pi * 220.0 * t), c.at (t));
                        err += (y - ref[(size_t) i]) * (y - ref[(size_t) i]); sig += ref[(size_t) i] * ref[(size_t) i];
                    }
                    const float diff = (float) (10.0 * std::log10 (std::max (1.0e-30, err / sig)));
                    worst = std::max (worst, diff);
                    std::cout << "  FET gain cell, " << c.what << ": difference from ngspice " << diff << " dB" << std::endl;
                }
                check (worst < -60.0f, "the FET gain cell matches ngspice's own JFET to better than -60 dB");
            }

            {
                // The output transformers at the rate they run at, from quiet to well into saturation, at 40 Hz
                // (where the core saturates) and 1 kHz (where leakage and winding capacitance start to matter).
                using IC = atelier::IronCircuit;
                const double rate = 384000.0;
                const IC::Core cores[] = { IC::steel, IC::nickel, IC::console };
                const char* coreNames[] = { "steel", "nickel", "console" };
                float worst = -200.0f;
                for (int t = 0; t < 3; ++t)
                    for (double freq : { 40.0, 1000.0 })
                        for (double amp : { 1.0, 30.0 })
                        {
                            const auto& c = cores[t];
                            IC model;
                            model.prepare (rate);
                            model.setCore (c);
                            const double ll = c.RL / (c.q * 2.0 * juce::MathConstants<double>::pi * c.f0), cw = c.q / (c.RL * 2.0 * juce::MathConstants<double>::pi * c.f0);
                            const double flux0 = c.bias * c.lambdaS;
                            double slope;
                            const double idc = model.magnetising (flux0, slope);
                            auto num = [] (double v) { return juce::String (v, 12); };
                            const juce::String core = "(V(fl)/" + num (c.L0) + ")*(1+" + num (c.lowLevel) + "/pwr(1+(V(fl)/" + num (c.lambda1) + ")*(V(fl)/" + num (c.lambda1)
                                                      + "),2)+pwr(abs(V(fl))/" + num (c.lambdaS) + "," + juce::String (c.knee) + "))-" + num (idc);

                            const auto deck = outDir.getChildFile ("iron.cir"), data = outDir.getChildFile ("iron.txt");
                            data.deleteFile();
                            // the core's flux is the integral of the voltage across it: a 1 F capacitor charged by that voltage
                            deck.replaceWithText ("Output transformer\nVin in 0 SIN(0 " + juce::String (amp) + " " + juce::String (freq) + ")\n"
                                + "R1 in m " + num (c.R1) + "\nRc m 0 " + num (c.Rc) + "\nBfl 0 fl I=V(m)\nCfl fl 0 1\nRfl fl 0 1e15\n"
                                // the hysteresis loop's current, from its own equation, alongside the core's curve
                                + "Bh 0 hy I=(V(m)*" + num (c.hc) + "-abs(V(m))*V(hy))/" + num (c.width * c.lambdaS) + "\nChy hy 0 1\nRhy hy 0 1e15\n"
                                + "Bm m 0 I=" + core + "+V(hy)\nLl m o " + num (ll) + "\nC1 o 0 " + num (cw) + "\nRL o 0 " + num (c.RL) + "\n.ic V(fl)=" + num (flux0) + " V(hy)=0\n"
                                + ".options reltol=1e-6\n.control\ntran " + juce::String (1.0 / rate, 15) + " 0.25 0 " + juce::String (0.5 / rate, 15)
                                + " uic\nlinearize v(o)\nwrdata iron.txt v(o)\n.endc\n.end\n");
                            juce::ChildProcess proc;
                            juce::StringArray cmd { "/bin/sh", "-c", "cd " + outDir.getFullPathName().quoted() + " && SPICE_LIB_DIR="
                                                                         + ngspice.getParentDirectory().getSiblingFile ("share/ngspice").getFullPathName().quoted()
                                                                         + " " + ngspice.getFullPathName().quoted() + " -b iron.cir > /dev/null 2>&1" };
                            std::vector<double> ref;
                            if (proc.start (cmd) && proc.waitForProcessToFinish (120000))
                            {
                                juce::StringArray lines;
                                lines.addLines (data.loadFileAsString());
                                for (const auto& line : lines)
                                {
                                    const auto parts = juce::StringArray::fromTokens (line.trim(), " \t", "");
                                    if (parts.size() >= 2) ref.push_back (parts[parts.size() - 1].getDoubleValue());
                                }
                            }
                            const int n = (int) (0.25 * rate) - 2;
                            if ((int) ref.size() < n) { check (false, juce::String ("ngspice produced a reference for the ") + coreNames[t] + " transformer"); continue; }

                            double err = 0.0, sig = 0.0;
                            for (int i = 0; i < n; ++i)
                            {
                                const double y = model.processVolts (0, amp * std::sin (2.0 * juce::MathConstants<double>::pi * freq * i / rate));
                                if (i >= (int) (0.05 * rate)) { err += (y - ref[(size_t) i]) * (y - ref[(size_t) i]); sig += ref[(size_t) i] * ref[(size_t) i]; }
                            }
                            const float diff = (float) (10.0 * std::log10 (std::max (1.0e-30, err / sig)));
                            worst = std::max (worst, diff);
                            std::cout << "  " << coreNames[t] << " transformer, " << freq << " Hz, " << amp << " V: difference from ngspice " << diff << " dB" << std::endl;
                        }
                check (worst < -60.0f, "the output transformers match the ngspice simulation of the same circuit to better than -60 dB");
            }
        }

        // behaviour a static curve cannot have: after a loud burst the bias has shifted and takes time to come back
        atelier::TubeCircuit t;
        t.prepare (192000.0);
        auto level = [&] (double amp, int n)
        {
            double sq = 0.0;
            for (int i = 0; i < n; ++i) { const double y = t.processVolts (0, amp * std::sin (2.0 * juce::MathConstants<double>::pi * 220.0 * i / 192000.0)); sq += y * y; }
            return 10.0 * std::log10 (sq / n);
        };
        const double before = level (0.05, 19200);
        level (8.0, 96000);
        const double justAfter = level (0.05, 1920), later = (level (0.05, 192000), level (0.05, 19200));
        std::cout << "  quiet tone: " << before << " dB before a loud burst, " << justAfter << " dB in the 10 ms after it, " << later << " dB a second later" << std::endl;
        check (std::abs (justAfter - before) > 0.5 && std::abs (later - before) < 0.2, "the valve's bias shifts after a loud burst and then recovers");
    }

    if (want ("Starting points"))
    {
        std::cout << "Starting points" << std::endl;
        int count = 0; bool valid = true, sane = true;
        for (int mode = 0; mode < 3; ++mode)
            for (const auto& src : atelier::guide::sourcesFor (mode))
            {
                ++count;
                AtelierProcessor p;
                p.setPlayConfigDetails (2, 2, sr, 512);
                p.prepareToPlay (sr, 512);
                setParam (p, "mode", (float) mode);

                for (const auto& [id, value] : src.settings)
                {
                    auto* param = p.apvts.getParameter (id);
                    const bool ok = param != nullptr && value >= param->getNormalisableRange().start && value <= param->getNormalisableRange().end;
                    if (! ok) { valid = false; std::cout << "    bad setting in " << src.name << ": " << id << " = " << value << std::endl; }
                }
                if (src.tips.size() != 4) { valid = false; std::cout << "    " << src.name << " has " << src.tips.size() << " tips" << std::endl; }

                // music-like test signal peaking near -8 dBFS: the level the starting points are written for
                atelier::ui::applyStartingPoint (p, src, -8.0f);
                auto b = noise (2, 48000, -30.0f, 21);
                for (int i = 0; i < b.getNumSamples(); ++i)
                {
                    const float env = 0.25f + 0.75f * std::exp (-6.0f * (float) (i % 12000) / 12000.0f);
                    const float v = 0.35f * env * (float) (std::sin (2.0 * juce::MathConstants<double>::pi * 110.0 * i / sr)
                                                         + 0.5 * std::sin (2.0 * juce::MathConstants<double>::pi * 1760.0 * i / sr));
                    b.addSample (0, i, v); b.addSample (1, i, v * 0.9f);
                }
                const float inRms = rmsDb (b, 12000);
                run (p, b, 512);
                const float change = rmsDb (b, 12000) - inRms, gr = p.meters.grComp.load();
                const bool on = p.apvts.getRawParameterValue ("compOn")->load() > 0.5f;
                std::cout << "  " << juce::String (src.name).paddedRight (' ', 30) << " level " << (change >= 0 ? "+" : "") << juce::String (change, 1)
                          << " dB" << (on ? ", compressor up to " + juce::String (gr, 1) + " dB" : juce::String()) << std::endl;
                // a master's limiter is meant to bring the peaks up to the ceiling, so those get louder by design
                const bool limiting = p.apvts.getRawParameterValue ("limOn")->load() > 0.5f;
                if (! (allFinite (b) && change > -9.0f && change < (limiting ? 13.0f : 4.0f))) sane = false;
            }
        check (valid, juce::String (count) + " starting points use real controls, legal values and four tips each");
        check (sane, "every starting point leaves the level within reason on a typical signal");

        // the same starting point on a much quieter track: thresholds follow the level down
        AtelierProcessor p;
        p.setPlayConfigDetails (2, 2, sr, 512);
        p.prepareToPlay (sr, 512);
        const auto& vocal = atelier::guide::sourcesFor (0).front();
        atelier::ui::applyStartingPoint (p, vocal, -8.0f);
        const float normalThr = p.apvts.getRawParameterValue ("compThr")->load();
        atelier::ui::applyStartingPoint (p, vocal, -20.0f);
        const float quietThr = p.apvts.getRawParameterValue ("compThr")->load();
        atelier::ui::applyStartingPoint (p, vocal, -100.0f);
        const float silentThr = p.apvts.getRawParameterValue ("compThr")->load();
        std::cout << "  " << vocal.name << " compressor threshold: " << normalThr << " dB at the reference level, " << quietThr
                  << " dB on a track 12 dB quieter, " << silentThr << " dB with nothing playing" << std::endl;
        check (std::abs (quietThr - (normalThr - 12.0f)) < 0.1f && std::abs (silentThr - normalThr) < 0.1f,
               "Apply fits thresholds to the level that is playing");
    }

    if (want ("Stress: all modules, every mode / rate / block size / layout"))
    {
        std::cout << "Stress: all modules, every mode / rate / block size / layout" << std::endl;
        for (double rate : { 44100.0, 48000.0, 96000.0, 192000.0 })
            for (int mode = 0; mode < 3; ++mode)
                for (int chans : { 1, 2 })
                    for (int block : { 17, 64, 1024 })
                    {
                        AtelierProcessor p;
                        p.setPlayConfigDetails (chans, chans, rate, block);
                        p.prepareToPlay (rate, block);
                        allModulesOn (p);
                        setParam (p, "mode", (float) mode);
                        setParam (p, "hpf", 80.0f); setParam (p, "lpf", 16000.0f);
                        setParam (p, "lfGain", 4.0f); setParam (p, "airGain", 6.0f); setParam (p, "hmGain", -5.0f);
                        setParam (p, "compThr", -30.0f); setParam (p, "tubeDrive", 18.0f); setParam (p, "tapeDrive", 12.0f);
                        setParam (p, "ironDrive", 12.0f); setParam (p, "width", 150.0f); setParam (p, "monoBass", 120.0f);
                        setParam (p, "eqMS", 1.0f); setParam (p, "limDrive", 6.0f);
                        auto b = noise (chans, 30000, -6.0f, mode + 3);
                        run (p, b, block);
                        const float pk = b.getMagnitude (0, b.getNumSamples());
                        if (! (allFinite (b) && pk < 4.0f && pk > 0.01f))
                            check (false, "rate " + juce::String (rate) + " mode " + juce::String (mode) + " ch " + juce::String (chans)
                                              + " block " + juce::String (block) + " peak " + juce::String (pk));
                    }
        check (true, "stress matrix completed");
    }

    if (want ("CPU"))
    {
        std::cout << "CPU" << std::endl;
        // Twenty seconds of audio, timed as four five-second stretches. The time is this thread's own processor time,
        // which leaves out the time it spends waiting while other programs run, and the quickest stretch is the
        // estimate: other work on the machine can only slow a stretch (through shared caches), never speed it up.
        // Ask for the performance cores, as a host does for its audio thread; left to itself, a busy machine may run
        // this thread on an efficiency core, where the same work takes markedly longer.
        pthread_set_qos_class_self_np (QOS_CLASS_USER_INTERACTIVE, 0);
        auto threadMs = []
        {
            timespec t {};
            clock_gettime (CLOCK_THREAD_CPUTIME_ID, &t);
            return (double) t.tv_sec * 1000.0 + (double) t.tv_nsec * 1.0e-6;
        };
        auto cost = [&] (int mode, int valve, int compModel = 0)
        {
            AtelierProcessor p;
            p.setPlayConfigDetails (2, 2, sr, 256);
            p.prepareToPlay (sr, 256);
            allModulesOn (p);
            setParam (p, "mode", (float) mode); setParam (p, "tubeType", (float) valve);
            if (compModel != 0) setParam (p, "compModel", (float) compModel);
            const int stretch = (int) sr * 5;
            double best = 1.0e9;
            for (int k = 0; k < 4; ++k)
            {
                auto b = noise (2, stretch, -12.0f, 5 + k);
                const double start = threadMs();
                run (p, b, 256);
                best = std::min (best, threadMs() - start);
            }
            return best / (stretch / sr * 1000.0) * 100.0;
        };
        for (int mode = 0; mode < 3; ++mode)
        {
            const double percent = cost (mode, 0);
            std::cout << "  mode " << mode << ", every module on: " << percent << " % of one core at 48 kHz" << std::endl;
            check (percent < 15.0, "CPU use is reasonable");
        }
        {
            const double percent = cost (1, 0, 3);
            std::cout << "  mode 1 with the Vari-Mu compressor: " << percent << " % of one core at 48 kHz" << std::endl;
            check (percent < 18.0, "CPU use is reasonable with the Vari-Mu compressor");
        }
        {
            const double percent = cost (1, 0, 2);
            std::cout << "  mode 1 with the Opto compressor: " << percent << " % of one core at 48 kHz" << std::endl;
            check (percent < 18.0, "CPU use is reasonable with the Opto compressor");     // a valve stage, 2x oversampled
        }
        {
            const double percent = cost (1, 0, 1);
            std::cout << "  mode 1 with the FET compressor: " << percent << " % of one core at 48 kHz" << std::endl;
            check (percent < 15.0, "CPU use is reasonable with the FET compressor");
        }
        // the pentode has a third electrode to solve for and the push-pull pair a second valve, so both cost more than the triode
        static const char* valves[] = { "triode", "pentode", "push-pull" };
        for (int valve = 1; valve < 3; ++valve)
        {
            const double percent = cost (0, valve);
            std::cout << "  mode 0 with the " << valves[valve] << ": " << percent << " % of one core at 48 kHz" << std::endl;
            check (percent < 18.0, juce::String ("CPU use is reasonable with the ") + valves[valve]);
        }
    }

    if (want ("State round-trip"))
    {
        std::cout << "State round-trip" << std::endl;
        AtelierProcessor p;
        p.setPlayConfigDetails (2, 2, sr, 512);
        p.prepareToPlay (sr, 512);
        setParam (p, "mode", 1.0f); setParam (p, "tapeDrive", 9.0f);
        juce::MemoryBlock state;
        p.getStateInformation (state);
        resetParams (p);
        p.setStateInformation (state.getData(), (int) state.getSize());
        check (std::abs (p.apvts.getRawParameterValue ("tapeDrive")->load() - 9.0f) < 0.01f
                   && (int) p.apvts.getRawParameterValue ("mode")->load() == 1, "state restores");

        // UI snapshots, with audio running so the meters and the coach have something to show
        allModulesOn (p);
        setParam (p, "compThr", -30.0f); setParam (p, "limDrive", 8.0f); setParam (p, "lfGain", 3.0f); setParam (p, "airGain", 4.0f);

        static const char* names[] = { "mix", "bus", "master" };
        for (int mode = 0; mode < 3; ++mode)
        {
            setParam (p, "mode", (float) mode);
            p.apvts.state.setProperty ("ui_bottom", mode == 2 ? 2 : 1, nullptr);    // master shot shows the analyser
            if (mode == 2) { setParam (p, "hpf", 30.0f); setParam (p, "hmGain", -3.0f); setParam (p, "lmGain", 2.0f); }
            std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditor());
            for (int pass = 0; pass < 6; ++pass)
            {
                auto audio = noise (2, 24000, -16.0f, 11 + pass);
                run (p, audio, 512);
                CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.12, false);   // lets the editor timer run
            }

            const auto img = ed->createComponentSnapshot (ed->getLocalBounds(), true, 2.0f);
            const auto file = outDir.getChildFile (juce::String ("atelier-") + names[mode] + ".png");
            file.deleteFile();
            juce::FileOutputStream os (file);
            juce::PNGImageFormat png;
            check (os.openedOk() && png.writeImageToStream (img, os), "rendered " + file.getFullPathName());
        }

    }

    std::cout << (failures == 0 ? "\nALL CHECKS PASSED" : "\nFAILURES: " + juce::String (failures)) << std::endl;
    return failures == 0 ? 0 : 1;
}
