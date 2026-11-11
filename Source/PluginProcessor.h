#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>

#include "DSP/Common.h"
#include "DSP/Dynamics.h"
#include "DSP/Equalizer.h"
#include "DSP/Saturation.h"
#include "Parameters.h"

class AtelierProcessor final : public juce::AudioProcessor,
                               private juce::AudioProcessorValueTreeState::Listener,
                               private juce::AsyncUpdater
{
public:
    AtelierProcessor();
    ~AtelierProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using juce::AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override                         { return true; }

    const juce::String getName() const override             { return "Atelier"; }
    bool acceptsMidi() const override                       { return false; }
    bool producesMidi() const override                      { return false; }
    bool isMidiEffect() const override                      { return false; }
    double getTailLengthSeconds() const override            { return 0.0; }
    juce::AudioProcessorParameter* getBypassParameter() const override  { return bypassParam; }

    int getNumPrograms() override                           { return 1; }
    int getCurrentProgram() override                        { return 0; }
    void setCurrentProgram (int) override                   {}
    const juce::String getProgramName (int) override        { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::UndoManager undo;
    juce::AudioProcessorValueTreeState apvts;

    /** A/B comparison: two complete settings snapshots held for the life of the plugin instance. */
    int  getABSlot() const noexcept     { return abSlot; }
    void switchAB (int slot);

    /** Delta listen: the output becomes "processed minus input", so you hear only what the strip
        changes. A monitoring aid, deliberately not saved with the session or exposed to automation. */
    std::atomic<bool> deltaListen { false };

    /** Recent audio for the analyser: after the input gain, and at the output. */
    atelier::ScopeFifo scopeIn, scopeOut;
    double getScopeSampleRate() const noexcept  { return sr; }

    /** Written on the audio thread, read (and peak-reset) by the editor. */
    struct Meters
    {
        std::atomic<float> inPeak[2] { { 0.0f }, { 0.0f } }, outPeak[2] { { 0.0f }, { 0.0f } };
        std::atomic<float> vuRms { 0.0f }, heat { 0.0f };
        std::atomic<float> grComp { 0.0f }, grGate { 0.0f }, grDeEss { 0.0f }, grLim { 0.0f };
        std::atomic<float> lufsS { -100.0f }, lufsM { -100.0f }, lufsI { -100.0f }, lra { 0.0f };
        std::atomic<float> truePeak { 0.0f };       // largest inter-sample peak since the last reset (linear)
        std::atomic<float> correlation { 1.0f };    // -1 (out of phase) .. +1 (mono)
        std::atomic<bool>  resetLoudness { false }; // set by the UI to restart integrated / LRA / true peak
        std::atomic<float> keyPeak { 0.0f };        // level arriving at the sidechain input
        std::atomic<bool>  keyConnected { false };  // the host has given us a sidechain bus
        std::atomic<float> matchDb { 0.0f };        // how much louder the processed signal is than the input
    } meters;

private:
    // chunk-rate smoothed parameters
    enum Sm
    {
        sHpf, sLpf, sGateThr, sGateRange, sGateRel, sTubeDrive, sTubeBias, sDsFreq, sDsThr,
        sLfGain, sLfFreq, sLmGain, sLmFreq, sLmQ, sHmGain, sHmFreq, sHmQ, sHfGain, sHfFreq, sAirGain, sAirFreq,
        sCompThr, sCompRatio, sCompAtt, sCompRel, sCompMakeup, sCompMix, sCompSc,
        sTapeDrive, sTapeBias, sTapeBump, sIronDrive, sWidth, sMonoBass, sLimDrive, sLimCeil, sLimRel,
        numSmoothed
    };

    void processChunk (float* l, float* r, const float* keyL, const float* keyR, int n);
    void resetLatencyPath();
    void parameterChanged (const juce::String&, float) override     { triggerAsyncUpdate(); }
    void handleAsyncUpdate() override;
    float raw (const char* id) const    { return apvts.getRawParameterValue (id)->load(); }

    std::atomic<float>* smSrc[numSmoothed] {};
    float sm[numSmoothed] {};
    float smCoef = 0.1f;
    bool smPrimed = false;

    std::atomic<float> *pMode {}, *pBypass {}, *pInGain {}, *pPhase {}, *pOutGain {};
    std::atomic<float> *pGateOn {}, *pTubeOn {}, *pTubeType {}, *pDsOn {}, *pEqOn {}, *pEqStyle {}, *pEqMS {}, *pEqPre {};
    std::atomic<float> *pCompOn {}, *pCompModel {}, *pCompAuto {}, *pTapeOn {}, *pTapeSpeed {}, *pIronOn {}, *pIronType {};
    std::atomic<float> *pImgOn {}, *pLimOn {}, *pLowLat {}, *pGateExt {}, *pCompExt {}, *pMatch {};
    juce::AudioParameterBool* bypassParam = nullptr;

    double sr = 48000.0;
    int chunk = 32;

    juce::SmoothedValue<float> inGain, outGain;
    atelier::Biquad hpfA, hpfB, lpf;
    atelier::Gate gate;
    atelier::DeEsser deEsser;
    atelier::Equalizer eq;
    atelier::Compressor comp;
    atelier::TubeStage tube;
    atelier::TapeStage tape;
    atelier::IronStage iron;
    atelier::StereoImage image;
    atelier::Limiter limiter;
    atelier::LoudnessMeter loudness;
    atelier::TruePeak outTruePeak;
    float corrLR = 0.0f, corrLL = 0.0f, corrRR = 0.0f, corrCoef = 0.001f;
    juce::ValueTree abState[2];
    int abSlot = 0;

    // Index 0 is the normal linear-phase path, index 1 the minimum-phase low-latency path.
    std::unique_ptr<juce::dsp::Oversampling<float>> tubeOS[2], colourOS[2];
    atelier::StereoDelay tubeDelay[2], colourDelay[2], keyDelay[2];
    int latencyFor[2] {};
    bool prepared = false, llActive = false;
    int duckHold = 0;
    float duck = 1.0f, duckStep = 0.004f;       // output dip while the latency path is swapped

    // level-matched bypass and delta listen: the untouched input, delayed to line up with the output,
    // and a loudness-weighted measure of input against processed output
    atelier::StereoDelay dryDelay[2];
    atelier::Biquad kInShelf, kInHp, kOutShelf, kOutHp;
    double msIn = 0.0, msOut = 0.0;
    juce::SmoothedValue<float> matchGain, deltaMix;
    std::vector<float> dlyL, dlyR;

    // click-free switching
    atelier::Fade hpfFade, lpfFade, gateFade, tubeFade, dsFade, eqFade, compFade, tapeFade, ironFade, colourFade, imgFade;
    bool eqPreActive = true, orderPrimed = false;
    float headroomSm = 1.0f, kneeSm = 0.0f;

    std::vector<float> monoR, tmpL, tmpR, dryL, dryR, keyL, keyR, osDryL, osDryR;
    float vuState = 0.0f, vuCoef = 0.001f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AtelierProcessor)
};
