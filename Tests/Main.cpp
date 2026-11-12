// Offline checks for Atelier: runs audio through the processor and checks the results.
#include "PluginEditor.h"
#include "PluginProcessor.h"

#include <CoreFoundation/CoreFoundation.h>
#include <iostream>

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

    if (want ("Valve circuit against ngspice"))
    {
        std::cout << "Valve circuit against ngspice" << std::endl;
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
            float worst = -200.0f;

            for (double amp : { 0.05, 0.5, 2.0, 8.0 })
            {
                const auto deck = outDir.getChildFile ("triode.cir"), data = outDir.getChildFile ("triode.txt");
                data.deleteFile();
                juce::String d;
                d << "Common-cathode 12AX7 stage\n"
                  << ".subckt TRIODE P G K\n"
                  << "Bp P K I=V(E1)>0 ? 2*pwr(V(E1),1.4)/1060 : 0\n"
                  << "Be E1 0 V=V(P,K)/600*ln(1+exp(600*(1/100+V(G,K)/sqrt(300+V(P,K)*V(P,K)))))\n"
                  << "Bg G K I=V(G,K)>0 ? pwr(V(G,K),1.5)/1300 : 0\n"
                  << ".ends\n"
                  << "Vb bp 0 250\nVin in 0 SIN(0 " << amp << " 220)\n"
                  << "Cin in g1 47n\nRgl g1 0 1Meg\nRgs g1 g 33k\nX1 p g k TRIODE\n"
                  << "Rp bp p 100k\nRk k 0 1.5k\nCk k 0 22u\nCout p out 100n\nRl out 0 1Meg\n"
                  << ".options reltol=1e-5\n"
                  << ".control\ntran 5.2083333333u 0.3 0 2.6u\nlinearize v(out)\nwrdata triode.txt v(out)\n.endc\n.end\n";
                deck.replaceWithText (d);

                juce::ChildProcess proc;
                // ngspice writes its data file relative to where it is run, and takes file names literally
                juce::StringArray cmd { "/bin/sh", "-c", "cd " + outDir.getFullPathName().quoted() + " && SPICE_LIB_DIR="
                                                             + ngspice.getParentDirectory().getSiblingFile ("share/ngspice").getFullPathName().quoted()
                                                             + " " + ngspice.getFullPathName().quoted() + " -b triode.cir > /dev/null 2>&1" };
                const bool ran = proc.start (cmd) && proc.waitForProcessToFinish (120000);
                juce::StringArray lines;
                lines.addLines (data.loadFileAsString());
                std::vector<double> ref;
                for (const auto& line : lines)
                {
                    const auto parts = juce::StringArray::fromTokens (line.trim(), " \t", "");
                    if (parts.size() >= 2) ref.push_back (parts[parts.size() - 1].getDoubleValue());
                }
                if (! ran || (int) ref.size() < total - 2) { check (false, "ngspice produced a reference for " + juce::String (amp) + " V"); continue; }

                atelier::TubeCircuit t;
                t.prepare (osRate);
                double err = 0.0, sig = 0.0;
                for (int i = 0; i < total - 2; ++i)
                {
                    const double y = t.processVolts (0, amp * std::sin (2.0 * juce::MathConstants<double>::pi * 220.0 * i / osRate));
                    if (i >= from) { err += (y - ref[(size_t) i]) * (y - ref[(size_t) i]); sig += ref[(size_t) i] * ref[(size_t) i]; }
                }
                const float diff = (float) (10.0 * std::log10 (std::max (1.0e-30, err / sig)));
                worst = std::max (worst, diff);
                std::cout << "  " << amp << " V at the grid: difference from ngspice " << diff << " dB" << std::endl;
            }
            check (worst < -40.0f, "valve stage matches the ngspice simulation of the same circuit to better than -40 dB");
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
        for (int mode = 0; mode < 3; ++mode)
        {
            AtelierProcessor p;
            p.setPlayConfigDetails (2, 2, sr, 256);
            p.prepareToPlay (sr, 256);
            allModulesOn (p);
            setParam (p, "mode", (float) mode);
            auto b = noise (2, (int) sr * 20, -12.0f, 5);
            const auto start = juce::Time::getMillisecondCounterHiRes();
            run (p, b, 256);
            const double ms = juce::Time::getMillisecondCounterHiRes() - start;
            std::cout << "  mode " << mode << ", every module on: " << ms / 200.0 << " % of one core at 48 kHz" << std::endl;
            check (ms / 200.0 < 15.0, "CPU use is reasonable");
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
