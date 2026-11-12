#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace atelier
{
enum Mode { modeMix = 0, modeBus, modeMaster };

namespace format
{
    inline juce::String db (float v, int)       { return (v > 0.0f ? "+" : "") + juce::String (v, 1) + " dB"; }
    inline juce::String hz (float v, int)       { return v < 1000.0f ? juce::String (juce::roundToInt (v)) + " Hz"
                                                                     : juce::String (v / 1000.0f, 2) + " kHz"; }
    inline juce::String ms (float v, int)       { return v < 10.0f ? juce::String (v, 2) + " ms"
                                                       : v < 1000.0f ? juce::String (juce::roundToInt (v)) + " ms"
                                                                     : juce::String (v / 1000.0f, 2) + " s"; }
    inline juce::String pct (float v, int)      { return juce::String (juce::roundToInt (v)) + " %"; }
    inline juce::String ratio (float v, int)    { return juce::String (v, 1) + ":1"; }
    inline juce::String q (float v, int)        { return "Q " + juce::String (v, 2); }
    inline juce::String hpf (float v, int n)    { return v <= 20.5f ? juce::String ("OFF") : hz (v, n); }
    inline juce::String lpf (float v, int n)    { return v >= 21900.0f ? juce::String ("OFF") : hz (v, n); }
}

inline juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout layout;
    using Fmt = String (*) (float, int);

    auto addF = [&layout] (const char* id, const char* name, float lo, float hi, float def, Fmt fmt, float centre = 0.0f, float step = 0.0f)
    {
        NormalisableRange<float> range (lo, hi, step);
        if (centre > 0.0f) range.setSkewForCentre (centre);

        // On a skewed range the default may not survive the trip to the host's 0..1 value and back exactly
        // (auval checks this). Settle on the nearest default that does: a few rounds always find one.
        float norm = range.convertTo0to1 (def);
        for (int i = 0; i < 16; ++i)
        {
            const float next = range.convertTo0to1 (range.convertFrom0to1 (norm));
            if (next == norm) break;
            norm = next;
        }
        def = range.convertFrom0to1 (norm);

        layout.add (std::make_unique<AudioParameterFloat> (ParameterID { id, 1 }, name, range, def,
                        AudioParameterFloatAttributes().withStringFromValueFunction (fmt)));
    };
    auto addB = [&layout] (const char* id, const char* name, bool def)
    {
        layout.add (std::make_unique<AudioParameterBool> (ParameterID { id, 1 }, name, def));
    };
    auto addC = [&layout] (const char* id, const char* name, StringArray choices, int def)
    {
        layout.add (std::make_unique<AudioParameterChoice> (ParameterID { id, 1 }, name, choices, def));
    };

    addC ("mode", "Mode", { "Mix", "Bus", "Master" }, 0);
    addB ("bypass", "Bypass", false);
    addB ("lowLat", "Low Latency", false);
    addB ("match", "Level-Matched Bypass", false);

    // Input
    addF ("inGain", "Input Gain", -24.0f, 24.0f, 0.0f, format::db);
    addB ("phase", "Phase Invert", false);
    addF ("hpf", "High Pass", 20.0f, 600.0f, 20.0f, format::hpf, 110.0f);
    addF ("lpf", "Low Pass", 2000.0f, 22000.0f, 22000.0f, format::lpf, 9000.0f);

    // Gate (Mix)
    addB ("gateOn", "Gate On", false);
    addF ("gateThr", "Gate Threshold", -80.0f, 0.0f, -50.0f, format::db);
    addF ("gateRange", "Gate Range", 0.0f, 80.0f, 40.0f, format::db);
    addF ("gateRel", "Gate Release", 20.0f, 2000.0f, 150.0f, format::ms, 250.0f);
    addB ("gateExt", "Gate Ext Key", false);

    // Tube
    addB ("tubeOn", "Tube On", true);
    addF ("tubeDrive", "Tube Drive", 0.0f, 24.0f, 6.0f, format::db);
    addF ("tubeBias", "Tube Bias", 0.0f, 100.0f, 40.0f, format::pct);
    addC ("tubeType", "Tube Type", { "Triode", "Pentode", "Push-Pull" }, 0);

    // De-esser (Mix)
    addB ("dsOn", "De-Ess On", false);
    addF ("dsFreq", "De-Ess Freq", 2000.0f, 12000.0f, 6500.0f, format::hz, 5500.0f);
    addF ("dsThr", "De-Ess Threshold", -60.0f, 0.0f, -30.0f, format::db);

    // EQ
    addB ("eqOn", "EQ On", true);
    addC ("eqStyle", "EQ Style", { "Modern", "Vintage" }, 0);
    addC ("eqMS", "EQ Channel", { "Stereo", "Mid", "Side" }, 0);
    addB ("eqPre", "EQ Pre Comp", true);
    addF ("lfGain", "LF Gain", -15.0f, 15.0f, 0.0f, format::db);
    addF ("lfFreq", "LF Freq", 30.0f, 450.0f, 100.0f, format::hz, 110.0f);
    addF ("lmGain", "LMF Gain", -15.0f, 15.0f, 0.0f, format::db);
    addF ("lmFreq", "LMF Freq", 100.0f, 2500.0f, 400.0f, format::hz, 500.0f);
    addF ("lmQ", "LMF Q", 0.3f, 6.0f, 0.9f, format::q, 1.2f);
    addF ("hmGain", "HMF Gain", -15.0f, 15.0f, 0.0f, format::db);
    addF ("hmFreq", "HMF Freq", 600.0f, 15000.0f, 3000.0f, format::hz, 3000.0f);
    addF ("hmQ", "HMF Q", 0.3f, 6.0f, 0.9f, format::q, 1.2f);
    addF ("hfGain", "HF Gain", -15.0f, 15.0f, 0.0f, format::db);
    addF ("hfFreq", "HF Freq", 1500.0f, 18000.0f, 8000.0f, format::hz, 6000.0f);
    addF ("airGain", "Air Gain", 0.0f, 12.0f, 0.0f, format::db);
    addF ("airFreq", "Air Freq", 5000.0f, 20000.0f, 12000.0f, format::hz, 11000.0f);

    // Compressor
    addB ("compOn", "Comp On", false);
    addC ("compModel", "Comp Model", { "VCA", "FET", "Opto", "Vari-Mu" }, 0);
    addF ("compThr", "Comp Threshold", -50.0f, 0.0f, -18.0f, format::db);
    addF ("compRatio", "Comp Ratio", 1.0f, 20.0f, 4.0f, format::ratio, 4.0f);
    addF ("compAtt", "Comp Attack", 0.05f, 100.0f, 10.0f, format::ms, 8.0f);
    addF ("compRel", "Comp Release", 20.0f, 2000.0f, 200.0f, format::ms, 250.0f);
    addB ("compAuto", "Comp Auto Release", false);
    addF ("compMakeup", "Comp Makeup", 0.0f, 24.0f, 0.0f, format::db);
    addF ("compMix", "Comp Mix", 0.0f, 100.0f, 100.0f, format::pct);
    addF ("compSc", "Comp SC HPF", 20.0f, 400.0f, 20.0f, format::hpf, 100.0f);
    addB ("compExt", "Comp Ext Key", false);

    // Tape
    addB ("tapeOn", "Tape On", false);
    addF ("tapeDrive", "Tape Drive", 0.0f, 18.0f, 4.0f, format::db);
    addF ("tapeBias", "Tape Bias", -100.0f, 100.0f, 0.0f, format::pct);
    addF ("tapeBump", "Tape Head Bump", 0.0f, 100.0f, 50.0f, format::pct);
    addC ("tapeSpeed", "Tape Speed", { "7.5 ips", "15 ips", "30 ips" }, 1);

    // Iron
    addB ("ironOn", "Iron On", false);
    addF ("ironDrive", "Iron Drive", 0.0f, 18.0f, 4.0f, format::db);
    addC ("ironType", "Iron Type", { "Steel", "Nickel", "Console" }, 0);

    // Image (Bus / Master)
    addB ("imgOn", "Image On", false);
    addF ("width", "Width", 0.0f, 200.0f, 100.0f, format::pct);
    addF ("monoBass", "Mono Bass", 20.0f, 300.0f, 20.0f, format::hpf, 100.0f);

    // Limiter (Bus / Master)
    addB ("limOn", "Limiter On", false);
    addF ("limDrive", "Limiter Drive", 0.0f, 18.0f, 0.0f, format::db);
    addF ("limCeil", "Limiter Ceiling", -6.0f, 0.0f, -1.0f, format::db);
    addF ("limRel", "Limiter Release", 10.0f, 1000.0f, 120.0f, format::ms, 150.0f);
    addF ("limClip", "Limiter Clip", 0.0f, 100.0f, 0.0f, format::pct);
    addC ("dither", "Dither", { "No dither", "24 bit", "16 bit", "16 shaped" }, 0);

    // Output
    addF ("outGain", "Output Trim", -24.0f, 24.0f, 0.0f, format::db);

    return layout;
}
} // namespace atelier
