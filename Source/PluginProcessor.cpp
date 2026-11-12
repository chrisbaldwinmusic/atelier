#include "PluginProcessor.h"
#include "PluginEditor.h"

using namespace atelier;

namespace
{
    constexpr int tubeOsOrder = 2;      // 4x for the valve stage
    constexpr int colourOsOrder = 3;    // 8x for tape + iron: tape pre-emphasis drives the top end hardest
    constexpr int compOsOrder = 1;      // 2x for the compressor's make-up valve: its 2nd and 3rd harmonics fit below the new Nyquist

    void pushPeak (std::atomic<float>& dest, float v) noexcept
    {
        if (v > dest.load (std::memory_order_relaxed))
            dest.store (v, std::memory_order_relaxed);
    }

    /** Low-frequency delay of an oversampler's up/down round trip, in whole samples.
        A steady ramp comes out of any unity-gain filter as the same ramp, shifted by its delay. */
    int measureDelay (juce::dsp::Oversampling<float>& os, int chunk)
    {
        std::vector<float> l ((size_t) chunk), r ((size_t) chunk);
        const float slope = 1.0e-4f;
        int n = 0;
        float last = 0.0f;
        for (int pass = 0; pass < 64; ++pass)
        {
            for (int i = 0; i < chunk; ++i) l[(size_t) i] = r[(size_t) i] = slope * (float) n++;
            float* chans[2] = { l.data(), r.data() };
            juce::dsp::AudioBlock<float> block (chans, 2, (size_t) chunk);
            os.processSamplesUp (block);
            os.processSamplesDown (block);
            last = l[(size_t) chunk - 1];
        }
        os.reset();
        return juce::jlimit (0, 64, juce::roundToInt ((float) (n - 1) - last / slope));
    }

    float blockPeak (const float* x, int n) noexcept
    {
        float p = 0.0f;
        for (int i = 0; i < n; ++i) p = std::max (p, std::abs (x[i]));
        return p;
    }
}

//==============================================================================
AtelierProcessor::AtelierProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",     juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output",    juce::AudioChannelSet::stereo(), true)
                          .withInput  ("Sidechain", juce::AudioChannelSet::stereo(), false)),
      apvts (*this, &undo, "ATELIER", createParameterLayout())
{
    static const char* smoothedIds[numSmoothed] =
    {
        "hpf", "lpf", "gateThr", "gateRange", "gateRel", "tubeDrive", "tubeBias", "dsFreq", "dsThr",
        "lfGain", "lfFreq", "lmGain", "lmFreq", "lmQ", "hmGain", "hmFreq", "hmQ", "hfGain", "hfFreq", "airGain", "airFreq",
        "compThr", "compRatio", "compAtt", "compRel", "compMakeup", "compMix", "compSc",
        "tapeDrive", "tapeBias", "tapeBump", "ironDrive", "width", "monoBass", "limDrive", "limCeil", "limRel", "limClip"
    };

    for (int i = 0; i < numSmoothed; ++i)
    {
        smSrc[i] = apvts.getRawParameterValue (smoothedIds[i]);
        jassert (smSrc[i] != nullptr);
    }

    auto p = [this] (const char* id) { auto* v = apvts.getRawParameterValue (id); jassert (v != nullptr); return v; };
    pMode = p ("mode");         pBypass = p ("bypass");     pInGain = p ("inGain");     pPhase = p ("phase");
    pOutGain = p ("outGain");   pGateOn = p ("gateOn");     pTubeOn = p ("tubeOn");     pTubeType = p ("tubeType");
    pDsOn = p ("dsOn");         pEqOn = p ("eqOn");         pEqStyle = p ("eqStyle");   pEqMS = p ("eqMS");
    pEqPre = p ("eqPre");       pCompOn = p ("compOn");     pCompModel = p ("compModel"); pCompAuto = p ("compAuto");
    pTapeOn = p ("tapeOn");     pTapeSpeed = p ("tapeSpeed"); pIronOn = p ("ironOn");   pIronType = p ("ironType");
    pImgOn = p ("imgOn");       pLimOn = p ("limOn");       pLowLat = p ("lowLat");
    pGateExt = p ("gateExt");   pCompExt = p ("compExt");   pMatch = p ("match");       pDither = p ("dither");

    bypassParam = dynamic_cast<juce::AudioParameterBool*> (apvts.getParameter ("bypass"));
    apvts.addParameterListener ("lowLat", this);
}

AtelierProcessor::~AtelierProcessor()
{
    apvts.removeParameterListener ("lowLat", this);
    cancelPendingUpdate();
}

bool AtelierProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto mono = juce::AudioChannelSet::mono(), stereo = juce::AudioChannelSet::stereo();
    const auto out = layouts.getMainOutputChannelSet();
    if ((out != mono && out != stereo) || out != layouts.getMainInputChannelSet())
        return false;

    const auto key = layouts.getChannelSet (true, 1);
    return key.isDisabled() || key == mono || key == stereo;
}

/** The host is told about a latency change from the message thread, never from the audio thread. */
void AtelierProcessor::handleAsyncUpdate()
{
    if (prepared)
        setLatencySamples (latencyFor[pLowLat->load() > 0.5f ? 1 : 0]);
}

//==============================================================================
void AtelierProcessor::prepareToPlay (double sampleRate, int)
{
    sr = sampleRate;
    chunk = 32 * juce::jmax (1, juce::roundToInt (sr / 48000.0));

    // Normal path: linear-phase oversampling, sample-exact against the dry signal.
    // Low-latency path: minimum-phase oversampling, a few samples instead of over a hundred.
    using OS = juce::dsp::Oversampling<float>;
    tubeOS[0]   = std::make_unique<OS> (2, tubeOsOrder,   OS::filterHalfBandFIREquiripple, true, true);
    colourOS[0] = std::make_unique<OS> (2, colourOsOrder, OS::filterHalfBandFIREquiripple, true, true);
    tubeOS[1]   = std::make_unique<OS> (2, tubeOsOrder,   OS::filterHalfBandPolyphaseIIR,  true, false);
    colourOS[1] = std::make_unique<OS> (2, colourOsOrder, OS::filterHalfBandPolyphaseIIR,  true, false);
    compOS[0]   = std::make_unique<OS> (2, compOsOrder,   OS::filterHalfBandFIREquiripple, true, true);
    compOS[1]   = std::make_unique<OS> (2, compOsOrder,   OS::filterHalfBandPolyphaseIIR,  true, false);

    limiter.prepare (sr);

    for (int i = 0; i < 2; ++i)
    {
        tubeOS[i]->initProcessing ((size_t) chunk);
        colourOS[i]->initProcessing ((size_t) chunk);
        compOS[i]->initProcessing ((size_t) chunk);
        // The linear-phase filters have an exact delay. The minimum-phase ones are measured, because
        // what matters for staying in time is their delay across the audible band, not a nominal figure.
        const int tubeLatency   = i == 0 ? juce::roundToInt (tubeOS[i]->getLatencyInSamples())   : measureDelay (*tubeOS[i], chunk);
        const int colourLatency = i == 0 ? juce::roundToInt (colourOS[i]->getLatencyInSamples()) : measureDelay (*colourOS[i], chunk);
        const int compLatency   = i == 0 ? juce::roundToInt (compOS[i]->getLatencyInSamples())   : measureDelay (*compOS[i], chunk);
        tubeDelay[i].prepare (tubeLatency);
        keyDelay[i].prepare (tubeLatency);      // the compressor sits after the valve stage
        colourDelay[i].prepare (colourLatency);
        compDelay[i].prepare (compLatency);
        latencyFor[i] = tubeLatency + compLatency + colourLatency + (i == 0 ? limiter.fullLatency() : 0);
        dryDelay[i].prepare (latencyFor[i]);
    }

    // roughly how the ear weighs loudness: less bass, a little more presence (after BS.1770)
    for (auto* f : { &kInShelf, &kInHp, &kOutShelf, &kOutHp }) f->reset();
    kInShelf.setHighShelf (sr, 1500.0, 0.707, 4.0);     kOutShelf.setHighShelf (sr, 1500.0, 0.707, 4.0);
    kInHp.setHighPass (sr, 38.0, 0.5);                  kOutHp.setHighPass (sr, 38.0, 0.5);
    msIn = msOut = 0.0;
    meters.matchDb.store (0.0f);
    matchGain.reset (sr, 0.03);     matchGain.setCurrentAndTargetValue (1.0f);
    deltaMix.reset (sr, 0.03);      deltaMix.setCurrentAndTargetValue (deltaListen.load() ? 1.0f : 0.0f);

    llActive = pLowLat->load() > 0.5f;
    limiter.setLowLatency (llActive);
    duck = 1.0f; duckHold = 0;
    duckStep = (float) (1.0 / (0.005 * sr));

    tube.prepare (sr * (double) (1 << tubeOsOrder));
    tape.prepare (sr * (double) (1 << colourOsOrder));
    iron.prepare (sr * (double) (1 << colourOsOrder));

    hpfA.reset(); hpfB.reset(); lpf.reset();
    gate.prepare (sr);
    deEsser.prepare (sr);
    eq.prepare (sr);
    comp.prepare (sr, sr * (double) (1 << compOsOrder));
    image.prepare (sr);
    loudness.prepare (sr);
    outTruePeak.reset();
    corrLR = corrLL = corrRR = 0.0f;
    corrCoef = msToCoef (300.0f, sr);
    meters.truePeak.store (0.0f);

    for (auto* f : { &hpfFade, &lpfFade, &gateFade, &tubeFade, &dsFade, &eqFade, &compFade, &colourFade, &imgFade })
        f->prepare (sr);
    tapeFade.prepare (sr * (double) (1 << colourOsOrder));      // these two blend inside the oversampled domain
    ironFade.prepare (sr * (double) (1 << colourOsOrder));
    clipFade.prepare (sr * (double) (1 << colourOsOrder));
    dither.reset();
    orderPrimed = false;

    inGain.reset (sr, 0.02);
    outGain.reset (sr, 0.02);
    inGain.setCurrentAndTargetValue (dbToGain (pInGain->load()));
    outGain.setCurrentAndTargetValue (dbToGain (pOutGain->load()));

    smCoef = 1.0f - (float) std::exp (-(double) chunk / (0.008 * sr));
    smCoefSlow = 1.0f - (float) std::exp (-(double) chunk / (0.03 * sr));
    smPrimed = false;

    for (auto* v : { &monoR, &tmpL, &tmpR, &dryL, &dryR, &keyL, &keyR, &dlyL, &dlyR })
        v->assign ((size_t) chunk, 0.0f);
    osDryL.assign ((size_t) (chunk << colourOsOrder), 0.0f);
    osDryR.assign ((size_t) (chunk << colourOsOrder), 0.0f);

    vuState = 0.0f;
    vuCoef = msToCoef (150.0f, sr);

    // Latency is constant within each path: disabled stages are replaced by matched delays.
    prepared = true;
    setLatencySamples (latencyFor[llActive ? 1 : 0]);
}

/** Called with the output faded to silence, when the latency path is swapped. */
void AtelierProcessor::resetLatencyPath()
{
    for (int i = 0; i < 2; ++i)
    {
        tubeOS[i]->reset(); compOS[i]->reset(); colourOS[i]->reset();
        tubeDelay[i].reset(); compDelay[i].reset(); colourDelay[i].reset(); keyDelay[i].reset(); dryDelay[i].reset();
    }
    limiter.setLowLatency (llActive);
    tube.reset(); tape.reset(); iron.reset(); comp.reset();
}

//==============================================================================
void AtelierProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int mainCh = juce::jmin (getMainBusNumInputChannels(), buffer.getNumChannels());
    const int n = buffer.getNumSamples();

    if (mainCh < 1 || n == 0 || ! prepared)
        return;

    if (meters.resetLoudness.exchange (false))
    {
        loudness.resetIntegrated();
        meters.truePeak.store (0.0f, std::memory_order_relaxed);
    }

    const bool mono = mainCh < 2;
    float* l = buffer.getWritePointer (0);
    float* r = mono ? nullptr : buffer.getWritePointer (1);

    // sidechain input, when the host has connected one: its channels follow the main input's
    const int keyCh = getBusCount (true) > 1 ? juce::jmin (getChannelCountOfBus (true, 1), buffer.getNumChannels() - mainCh) : 0;
    const float* kl = keyCh > 0 ? buffer.getReadPointer (mainCh) : nullptr;
    const float* kr = keyCh > 1 ? buffer.getReadPointer (mainCh + 1) : kl;
    meters.keyConnected.store (kl != nullptr, std::memory_order_relaxed);

    for (int pos = 0; pos < n; pos += chunk)
    {
        const int m = juce::jmin (chunk, n - pos);
        const float* ckl = kl != nullptr ? kl + pos : nullptr;
        const float* ckr = kl != nullptr ? kr + pos : nullptr;

        if (mono)
        {
            std::copy (l + pos, l + pos + m, monoR.begin());
            processChunk (l + pos, monoR.data(), ckl, ckr, m);
        }
        else
        {
            processChunk (l + pos, r + pos, ckl, ckr, m);
        }
    }

    meters.lufsS.store (loudness.shortTerm, std::memory_order_relaxed);
    meters.lufsM.store (loudness.momentary, std::memory_order_relaxed);
    meters.lufsI.store (loudness.integrated, std::memory_order_relaxed);
    meters.lra.store (loudness.range, std::memory_order_relaxed);
    meters.correlation.store (corrLL > 1.0e-9f && corrRR > 1.0e-9f ? corrLR / std::sqrt (corrLL * corrRR) : 1.0f,
                              std::memory_order_relaxed);
}

void AtelierProcessor::processChunk (float* l, float* r, const float* kl, const float* kr, int n)
{
    const int  mode     = juce::jlimit (0, 2, (int) pMode->load());
    const bool bypassed = pBypass->load() > 0.5f;
    const bool isMix    = mode == modeMix;
    auto on = [bypassed] (const std::atomic<float>* p) { return ! bypassed && p->load() > 0.5f; };

    // ---- parameter smoothing --------------------------------------------------
    // Mode voicing: how hard the colour stages are hit for a given level.
    static constexpr float headroomForMode[3] = { 1.0f, 0.7f, 0.5f };
    static constexpr float kneeForMode[3]     = { 0.0f, 2.0f, 4.0f };

    for (int i = 0; i < numSmoothed; ++i)
    {
        const float target = smSrc[i]->load (std::memory_order_relaxed);
        // filter cut-offs glide more slowly: a fast sweep of a filter's corner is audible as a thump
        const float coef = (i == sHpf || i == sLpf) ? smCoefSlow : smCoef;
        sm[i] = smPrimed ? sm[i] + coef * (target - sm[i]) : target;
    }
    headroomSm = smPrimed ? headroomSm + smCoef * (headroomForMode[mode] - headroomSm) : headroomForMode[mode];
    kneeSm     = smPrimed ? kneeSm + smCoef * (kneeForMode[mode] - kneeSm) : kneeForMode[mode];
    smPrimed = true;
    const float headroom = headroomSm;

    // ---- latency path ---------------------------------------------------------
    // Swapping between the normal and low-latency paths moves the audio in time, so the
    // output dips to silence for a few milliseconds while it happens.
    const bool wantLL = pLowLat->load() > 0.5f;
    if (wantLL != llActive && duck <= 0.0f)
    {
        llActive = wantLL;
        resetLatencyPath();
        duckHold = latencyFor[llActive ? 1 : 0] + 2 * chunk;   // stay silent until audio has filled the new path
    }
    duckHold = std::max (0, duckHold - n);
    const float duckTarget = wantLL == llActive && duckHold == 0 ? 1.0f : 0.0f;
    const int path = llActive ? 1 : 0;

    // ---- the untouched input: kept for delta listen, and measured for level matching ----
    dryDelay[path].process (l, r, dlyL.data(), dlyR.data(), n);
    double eIn = 0.0, eOut = 0.0;
    for (int i = 0; i < n; ++i)
    {
        const float a = kInHp.process (0, kInShelf.process (0, l[i]));
        const float b = kInHp.process (1, kInShelf.process (1, r[i]));
        eIn += (double) (a * a + b * b);
    }

    // Runs one in-place stage with click-free on/off and choice switching.
    auto stage = [&] (Fade& f, bool want, int choice, auto&& resetFn, auto&& processFn)
    {
        bool cold = false;
        if (! f.begin (want, choice, cold)) return;
        if (cold) resetFn();

        const bool fading = ! f.steady();
        if (fading)
        {
            std::copy (l, l + n, dryL.begin());
            std::copy (r, r + n, dryR.begin());
        }
        processFn();
        if (fading) f.mix (l, r, dryL.data(), dryR.data(), n);
    };

    // ---- input ----------------------------------------------------------------
    // polarity is part of the smoothed gain, so flipping it passes through zero instead of jumping
    inGain.setTargetValue (bypassed ? 1.0f : dbToGain (pInGain->load()) * (pPhase->load() > 0.5f ? -1.0f : 1.0f));
    for (int i = 0; i < n; ++i)
    {
        const float g = inGain.getNextValue();
        l[i] *= g; r[i] *= g;
    }
    pushPeak (meters.inPeak[0], blockPeak (l, n));
    pushPeak (meters.inPeak[1], blockPeak (r, n));
    scopeIn.push (l, r, n);
    if (kl != nullptr)
        pushPeak (meters.keyPeak, std::max (blockPeak (kl, n), blockPeak (kr, n)));

    // ---- filters --------------------------------------------------------------
    stage (hpfFade, ! bypassed && sm[sHpf] > 20.5f, isMix ? 1 : 0,
           [&] { hpfA.reset(); hpfB.reset(); },
           [&]
           {
               if (hpfFade.active == 1)     // 24 dB/oct on channels, a gentler 12 dB/oct on busses and the master
               {
                   hpfA.setHighPass (sr, sm[sHpf], 0.5412);
                   hpfB.setHighPass (sr, sm[sHpf], 1.3066);
                   for (int i = 0; i < n; ++i)
                   {
                       l[i] = hpfB.process (0, hpfA.process (0, l[i]));
                       r[i] = hpfB.process (1, hpfA.process (1, r[i]));
                   }
               }
               else
               {
                   hpfA.setHighPass (sr, sm[sHpf], 0.707);
                   for (int i = 0; i < n; ++i) { l[i] = hpfA.process (0, l[i]); r[i] = hpfA.process (1, r[i]); }
               }
           });

    stage (lpfFade, ! bypassed && sm[sLpf] < 21900.0f, 0,
           [&] { lpf.reset(); },
           [&]
           {
               lpf.setLowPass (sr, sm[sLpf], 0.707);
               for (int i = 0; i < n; ++i) { l[i] = lpf.process (0, l[i]); r[i] = lpf.process (1, r[i]); }
           });

    // ---- gate (Mix) -----------------------------------------------------------
    const bool gateKeyed = kl != nullptr && pGateExt->load() > 0.5f;
    stage (gateFade, isMix && on (pGateOn), gateKeyed ? 1 : 0,
           [&] { gate.reset(); },
           [&]
           {
               gate.set (sm[sGateThr], sm[sGateRange], sm[sGateRel]);
               const bool keyed = gateFade.active == 1 && kl != nullptr;
               pushPeak (meters.grGate, gate.process (l, r, n, keyed ? kl : nullptr, keyed ? kr : nullptr));
           });

    // ---- tube preamp ----------------------------------------------------------
    tubeDelay[path].process (l, r, tmpL.data(), tmpR.data(), n);
    {
        bool cold = false;
        if (tubeFade.begin (on (pTubeOn), (int) pTubeType->load(), cold))
        {
            auto& os = *tubeOS[path];
            if (cold)
            {
                // the oversampler's output is empty until its filters have filled: wait for it before fading in
                os.reset(); tube.reset();
                tubeFade.hold = 2 * tubeDelay[path].len + 8;
            }

            tube.set (tubeFade.active, sm[sTubeDrive], sm[sTubeBias] * 0.01f, headroom);
            pushPeak (meters.heat, std::max (blockPeak (l, n), blockPeak (r, n)) * tube.drive);

            float* chans[2] = { l, r };
            juce::dsp::AudioBlock<float> block (chans, 2, (size_t) n);
            auto up = os.processSamplesUp (block);
            tube.process (up.getChannelPointer (0), up.getChannelPointer (1), (int) up.getNumSamples());
            os.processSamplesDown (block);
            tubeFade.mix (l, r, tmpL.data(), tmpR.data(), n);
        }
        else
        {
            std::copy (tmpL.begin(), tmpL.begin() + n, l);
            std::copy (tmpR.begin(), tmpR.begin() + n, r);
        }
    }

    // the key reaches the compressor in step with the audio, which the valve stage has just delayed
    if (kl != nullptr)
        keyDelay[path].process (kl, kr, keyL.data(), keyR.data(), n);

    // ---- de-esser (Mix) -------------------------------------------------------
    stage (dsFade, isMix && on (pDsOn), 0,
           [&] { deEsser.reset(); },
           [&]
           {
               deEsser.set (sm[sDsFreq], sm[sDsThr]);
               pushPeak (meters.grDeEss, deEsser.process (l, r, n));
           });

    // ---- EQ <-> compressor ----------------------------------------------------
    // Changing the order fades both out, swaps them, and fades them back in.
    const bool wantPre = pEqPre->load() > 0.5f;
    if (! orderPrimed) { orderPrimed = true; eqPreActive = wantPre; }
    if (wantPre != eqPreActive && eqFade.value <= 0.0f && compFade.value <= 0.0f)
        eqPreActive = wantPre;
    const bool swapping = wantPre != eqPreActive;

    auto runEq = [&]
    {
        const int ms = isMix ? 0 : juce::jlimit (0, 2, (int) pEqMS->load());
        const int style = (int) pEqStyle->load() > 0 ? 1 : 0;
        stage (eqFade, on (pEqOn) && ! swapping, style | (ms << 1),
               [&] { eq.reset(); },
               [&]
               {
                   Equalizer::Params p;
                   p.lfGain = sm[sLfGain];   p.lfFreq = sm[sLfFreq];
                   p.lmGain = sm[sLmGain];   p.lmFreq = sm[sLmFreq];   p.lmQ = sm[sLmQ];
                   p.hmGain = sm[sHmGain];   p.hmFreq = sm[sHmFreq];   p.hmQ = sm[sHmQ];
                   p.hfGain = sm[sHfGain];   p.hfFreq = sm[sHfFreq];
                   p.airGain = sm[sAirGain]; p.airFreq = sm[sAirFreq];
                   p.style = eqFade.active & 1;
                   eq.set (p);
                   eq.process (l, r, n, eqFade.active >> 1);
               });
    };

    // The compressor turns the gain down at the base rate; its make-up valve runs 2x oversampled, and the dry
    // signal is delayed to match, whether the compressor is on or not, so the timing never changes.
    auto runComp = [&]
    {
        const bool keyed = kl != nullptr && pCompExt->load() > 0.5f;
        compDelay[path].process (l, r, tmpL.data(), tmpR.data(), n);
        bool cold = false;
        if (compFade.begin (on (pCompOn) && ! swapping, juce::jlimit (0, 3, (int) pCompModel->load()) | (keyed ? 4 : 0), cold))
        {
            auto& os = *compOS[path];
            if (cold)
            {
                os.reset(); comp.reset();
                compFade.hold = 2 * compDelay[path].len + 8;
            }

            const bool useKey = (compFade.active & 4) != 0 && kl != nullptr;
            Compressor::Params p;
            p.model = compFade.active & 3;
            p.thresholdDb = sm[sCompThr];   p.ratio = sm[sCompRatio];
            p.attackMs = sm[sCompAtt];      p.releaseMs = sm[sCompRel];
            p.makeupDb = sm[sCompMakeup];   p.mix = sm[sCompMix] * 0.01f;
            p.scHpf = sm[sCompSc];          p.autoRelease = pCompAuto->load() > 0.5f;
            p.extraKnee = kneeSm;           p.external = useKey;
            comp.set (p);
            pushPeak (meters.grComp, comp.process (l, r, n, useKey ? keyL.data() : nullptr, useKey ? keyR.data() : nullptr));

            float* chans[2] = { l, r };
            juce::dsp::AudioBlock<float> block (chans, 2, (size_t) n);
            auto up = os.processSamplesUp (block);
            comp.colour (up.getChannelPointer (0), up.getChannelPointer (1), (int) up.getNumSamples());
            os.processSamplesDown (block);
            comp.finish (l, r, tmpL.data(), tmpR.data(), n);
            compFade.mix (l, r, tmpL.data(), tmpR.data(), n);
        }
        else
        {
            std::copy (tmpL.begin(), tmpL.begin() + n, l);
            std::copy (tmpR.begin(), tmpR.begin() + n, r);
        }
    };

    if (eqPreActive) { runEq(); runComp(); }
    else             { runComp(); runEq(); }

    // ---- tape + iron + clipper (shared oversampler) ------------------------------
    const bool limOn = ! isMix && on (pLimOn);
    colourDelay[path].process (l, r, tmpL.data(), tmpR.data(), n);
    {
        bool coldTape = false, coldIron = false, coldOs = false;
        const bool runTape = tapeFade.begin (on (pTapeOn), (int) pTapeSpeed->load(), coldTape);
        const bool runIron = ironFade.begin (on (pIronOn), (int) pIronType->load(), coldIron);

        // The clipper belongs to the limiter but runs here, inside the oversampler, so it doesn't alias.
        bool coldClip = false;
        const bool runClip = clipFade.begin (limOn && sm[sLimClip] > 0.05f, 0, coldClip);

        if (colourFade.begin (runTape || runIron || runClip, 0, coldOs))
        {
            auto& os = *colourOS[path];
            if (coldOs)
            {
                os.reset();
                colourFade.hold = 2 * colourDelay[path].len + 8;
            }

            float* chans[2] = { l, r };
            juce::dsp::AudioBlock<float> block (chans, 2, (size_t) n);
            auto up = os.processSamplesUp (block);
            const int un = (int) up.getNumSamples();
            float* ul = up.getChannelPointer (0);
            float* ur = up.getChannelPointer (1);

            auto inner = [&] (Fade& f, bool cold, auto&& resetFn, auto&& processFn)
            {
                if (cold) resetFn();
                const bool fading = ! f.steady();
                if (fading)
                {
                    std::copy (ul, ul + un, osDryL.begin());
                    std::copy (ur, ur + un, osDryR.begin());
                }
                processFn();
                if (fading) f.mix (ul, ur, osDryL.data(), osDryR.data(), un);
            };

            if (runTape)
                inner (tapeFade, coldTape, [&] { tape.reset(); }, [&]
                {
                    tape.set (tapeFade.active, sm[sTapeDrive], sm[sTapeBias] * 0.01f, sm[sTapeBump] * 0.01f, headroom);
                    tape.process (ul, ur, un);
                });

            if (runIron)
                inner (ironFade, coldIron, [&] { iron.reset(); }, [&]
                {
                    iron.set (ironFade.active, sm[sIronDrive], headroom);
                    iron.process (ul, ur, un);
                });

            if (runClip)
                inner (clipFade, coldClip, [] {}, [&]
                {
                    // the limiter's drive comes later, so the ceiling is expressed at this point in the chain
                    const float threshold = dbToGain (sm[sLimCeil] - sm[sLimDrive]);
                    pushPeak (meters.grClip, SoftClip::process (ul, ur, un, threshold, sm[sLimClip] * 0.01f));
                });

            os.processSamplesDown (block);
            colourFade.mix (l, r, tmpL.data(), tmpR.data(), n);
        }
        else
        {
            std::copy (tmpL.begin(), tmpL.begin() + n, l);
            std::copy (tmpR.begin(), tmpR.begin() + n, r);
        }
    }

    // ---- stereo image + limiter (Bus / Master) --------------------------------
    stage (imgFade, ! isMix && on (pImgOn), 0,
           [&] { image.reset(); },
           [&]
           {
               image.set (sm[sWidth], sm[sMonoBass]);
               image.process (l, r, n);
           });

    // the limiter always runs (as a plain delay when off) and smooths its own switching
    limiter.set (limOn, sm[sLimDrive], sm[sLimCeil], sm[sLimRel]);
    const float limGr = limiter.process (l, r, n);
    if (limOn) pushPeak (meters.grLim, limGr);

    // ---- output ---------------------------------------------------------------
    outGain.setTargetValue (bypassed ? 1.0f : dbToGain (pOutGain->load()));

    // Level-matched bypass: the bypassed signal is played at the loudness the processing had,
    // so the comparison is about tone and dynamics, not volume. The processed signal is never touched.
    matchGain.setTargetValue (bypassed && pMatch->load() > 0.5f ? dbToGain (meters.matchDb.load (std::memory_order_relaxed)) : 1.0f);
    deltaMix.setTargetValue (deltaListen.load (std::memory_order_relaxed) ? 1.0f : 0.0f);

    // Dither is the very last thing that happens, and only when the strip is on a bus or master.
    const int ditherMode = bypassed || isMix || deltaListen.load (std::memory_order_relaxed) ? 0 : juce::jlimit (0, 3, (int) pDither->load());
    if (ditherMode > 0) dither.set (ditherMode == 1 ? 24 : 16, ditherMode == 3);

    float pkL = 0.0f, pkR = 0.0f, tp = 0.0f;
    for (int i = 0; i < n; ++i)
    {
        duck = duckTarget > duck ? std::min (duckTarget, duck + duckStep) : std::max (duckTarget, duck - duckStep);
        const float g = outGain.getNextValue() * duck;
        l[i] *= g; r[i] *= g;

        const float wa = kOutHp.process (0, kOutShelf.process (0, l[i]));
        const float wb = kOutHp.process (1, kOutShelf.process (1, r[i]));
        eOut += (double) (wa * wa + wb * wb);

        const float mg = matchGain.getNextValue(), dm = deltaMix.getNextValue();
        l[i] = l[i] * mg - dm * dlyL[(size_t) i];
        r[i] = r[i] * mg - dm * dlyR[(size_t) i];
        if (ditherMode > 0) { l[i] = dither.process (0, l[i]); r[i] = dither.process (1, r[i]); }
        pkL = std::max (pkL, std::abs (l[i]));
        pkR = std::max (pkR, std::abs (r[i]));
        tp  = std::max (tp, outTruePeak.process (l[i], r[i]));
        vuState += vuCoef * (0.5f * (l[i] * l[i] + r[i] * r[i]) - vuState);
        corrLR  += corrCoef * (l[i] * r[i] - corrLR);
        corrLL  += corrCoef * (l[i] * l[i] - corrLL);
        corrRR  += corrCoef * (r[i] * r[i] - corrRR);
    }
    // loudness difference over the last few seconds; it holds still through silence and while bypassed
    if (! bypassed && duck >= 1.0f && eIn > 2.0e-7 * (double) n)
    {
        const double c = 1.0 - std::exp (-(double) n / (3.0 * sr));
        msIn  += c * (eIn  / (double) n - msIn);
        msOut += c * (eOut / (double) n - msOut);
        if (msIn > 1.0e-10 && msOut > 1.0e-12)
            meters.matchDb.store (juce::jlimit (-24.0f, 24.0f, (float) (10.0 * std::log10 (msOut / msIn))), std::memory_order_relaxed);
    }

    pushPeak (meters.outPeak[0], pkL);
    pushPeak (meters.outPeak[1], pkR);
    pushPeak (meters.truePeak, tp);
    scopeOut.push (l, r, n);
    meters.vuRms.store (std::sqrt (vuState), std::memory_order_relaxed);
    loudness.process (l, r, n);
}

//==============================================================================
void AtelierProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void AtelierProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

void AtelierProcessor::switchAB (int slot)
{
    slot = juce::jlimit (0, 1, slot);
    if (slot == abSlot) return;

    abState[abSlot] = apvts.copyState();
    if (abState[slot].isValid())
        apvts.replaceState (abState[slot].createCopy());    // an unused slot simply starts as a copy of the other
    abSlot = slot;
}

juce::AudioProcessorEditor* AtelierProcessor::createEditor()
{
    return new AtelierEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new AtelierProcessor();
}
