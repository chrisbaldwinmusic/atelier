#pragma once

#include "Common.h"
#include "ValveCircuits.h"
#include "OptoCircuit.h"
#include "FetCircuit.h"

namespace atelier
{
//==============================================================================
struct Gate
{
    void prepare (double sampleRate)
    {
        sr = sampleRate;
        detRel = msToCoef (10.0f, sr);
        att    = msToCoef (0.4f, sr);
        holdLen = (int) (0.03 * sr);
        reset();
    }

    void reset() noexcept   { env = 0.0f; gDb = 0.0f; open = true; hold = 0; }

    void set (float thresholdDb, float rangeDb, float releaseMs) noexcept
    {
        thr = thresholdDb; range = rangeDb; rel = msToCoef (releaseMs, sr);
    }

    /** Returns the largest gain reduction (dB, positive) in the block.
        Pass a key signal to open the gate from another track instead of from the audio itself. */
    float process (float* l, float* r, int n, const float* keyL = nullptr, const float* keyR = nullptr) noexcept
    {
        float maxGr = 0.0f;
        for (int i = 0; i < n; ++i)
        {
            const float pk = keyL != nullptr ? std::max (std::abs (keyL[i]), std::abs (keyR[i]))
                                             : std::max (std::abs (l[i]), std::abs (r[i]));
            env = pk > env ? pk : env + detRel * (pk - env);
            const float envDb = gainToDb (env);

            if (envDb > thr)             { open = true; hold = holdLen; }
            else if (envDb < thr - 3.0f) { if (hold > 0) --hold; else open = false; }

            const float target = open ? 0.0f : -range;
            gDb += (target > gDb ? att : rel) * (target - gDb);

            if (gDb < -0.01f)
            {
                const float g = dbToGain (gDb);
                l[i] *= g; r[i] *= g;
                maxGr = std::max (maxGr, -gDb);
            }
        }
        return maxGr;
    }

    double sr = 48000.0;
    float thr = -50.0f, range = 40.0f, rel = 0.001f, att = 0.05f, detRel = 0.002f;
    float env = 0.0f, gDb = 0.0f;
    int hold = 0, holdLen = 1440;
    bool open = true;
};

//==============================================================================
/** Split-band de-esser. A 12 dB/oct high-pass at the set frequency feeds the detector,
    so the body of the voice can't trigger it. The cut itself uses a first-order split an
    octave lower: its bands recombine without phase cancellation, so the result behaves
    like a dynamic high shelf and is bit-transparent whenever nothing is being reduced. */
struct DeEsser
{
    void prepare (double sampleRate)
    {
        sr = sampleRate;
        att = msToCoef (0.5f, sr);
        rel = msToCoef (40.0f, sr);
        reset();
    }

    void reset() noexcept   { hp.reset(); split.reset(); env = 0.0f; }

    void set (float freq, float thresholdDb) noexcept
    {
        hp.setHighPass (sr, freq, 0.707);
        split.setCutoff (sr, 0.5 * (double) freq);
        thr = thresholdDb;
    }

    float process (float* l, float* r, int n) noexcept
    {
        float maxGr = 0.0f;
        for (int i = 0; i < n; ++i)
        {
            const float pk = std::max (std::abs (hp.process (0, l[i])), std::abs (hp.process (1, r[i])));
            env += (pk > env ? att : rel) * (pk - env);

            const float hl = split.hp (0, l[i]);
            const float hr = split.hp (1, r[i]);
            const float gr = std::clamp ((gainToDb (env) - thr) * 0.8f, 0.0f, 18.0f);
            if (gr > 0.01f)
            {
                const float k = 1.0f - dbToGain (-gr);
                l[i] -= k * hl;
                r[i] -= k * hr;
                maxGr = std::max (maxGr, gr);
            }
        }
        return maxGr;
    }

    double sr = 48000.0;
    Biquad hp;
    OnePole split;
    float thr = -30.0f, att = 0.05f, rel = 0.001f, env = 0.0f;
};

//==============================================================================
/** Four compressors that differ in topology, detector and colour, not just timing.

    VCA      feed-forward, RMS detector, firm knee, no added colour. Precise and punchy.
    FET      feedback (the detector listens to its own output), peak detector, very fast.
             The gain element is a real junction FET (FetCircuit): clean once it is well on, gritty
             on loud material right where it starts to bite.
    Opto     feed-forward into a real light panel and photoresistive cell (OptoCell): the cell sets
             the timing, quick to let go from heavy reduction, then slower and slower, and slower
             still after long, heavy passages. A real 12AX7 make-up stage adds a touch of 2nd harmonic.
    Vari-Mu  feedback, slow RMS detector, very wide knee so the ratio rises with level.
             Push-pull valve gain cell adds 3rd harmonic as gain reduction increases. */
struct Compressor
{
    enum Model { vca = 0, fet, opto, variMu };

    struct Params
    {
        int   model = vca;
        float thresholdDb = -18.0f, ratio = 4.0f, attackMs = 10.0f, releaseMs = 200.0f;
        float makeupDb = 0.0f, mix = 1.0f, scHpf = 20.0f, extraKnee = 0.0f;
        bool  autoRelease = false;
        bool  external = false;     // detector listens to the sidechain input instead of the audio
    };

    void prepare (double sampleRate)
    {
        sr = sampleRate;
        makeupValve.prepare (sr);
        makeupValve.setInputScale (0.25);   // a full-scale sample puts 0.25 V on the grid: a touch of 2nd harmonic
        valves.prepare (sr);
        valves.setInputScale (4.0);         // a full-scale sample puts 4 V on each grid; see Vari-Mu below
        cell.prepare (sr);
        fetCell.setInputScale (0.2);        // a full-scale sample puts 200 mV across the divider; see FET below
        fetCell.prepare (sr);
        voltsPerDb = valves.controlFor (10.0) / 10.0;
        reset();
    }

    void reset() noexcept
    {
        scFilter.reset(); makeupValve.reset(); valves.reset(); cell.reset(); fetCell.reset();
        ms = 0.0f; envFast = envSlow = 0.0f; fb[0] = fb[1] = 0.0f;
    }

    void set (const Params& p) noexcept
    {
        float a = p.attackMs, r = p.releaseMs, rmsMs = 0.0f;
        const float ratio = std::max (1.0f, p.ratio);

        switch (p.model)
        {
            case fet:
                // The gain element is a real junction FET shunting the signal (FetCircuit). The side chain turns
                // the reduction it wants into a voltage on the FET's gate; the colour is the FET's own.
                feedback = true;  release = single;
                a = std::max (0.02f, a * 0.1f);             knee = 2.0f;  rmsMs = 0.0f;
                break;
            case opto:
                // The gain element is a real light panel and photoresistive cell (OptoCell). The side chain only
                // turns the reduction it wants into a voltage on the panel, quickly; the cell does the timing, and
                // the knobs speed it up or slow it down. At its own pace the cell takes 71 ms to reach 90 % of a
                // 12 dB reduction and 115 ms to let it back to 6 dB, so the attack knob is the first of those times
                // and the release knob the second. Lighter reduction attacks more slowly, heavier lets go faster.
                feedback = false; release = single;
                cell.setTiming (71.0 / std::max (1.0f, a), 115.0 / r);
                a = 0.5f; r = 5.0f;                         knee = 10.0f; rmsMs = 6.0f;
                break;
            case variMu:
                // The gain element is a real pair of remote-cutoff valves (VariMuCircuit). The side chain turns the
                // gain reduction it wants into a voltage on their grids, a fixed number of volts per dB, and the
                // valves do the rest: their curve makes the onset gentle and lets the ratio rise as they are pushed.
                feedback = true;  release = single;
                a = std::max (1.0f, a) * 2.0f; r *= 1.5f;   knee = 8.0f; rmsMs = 12.0f;
                break;
            default:
                feedback = false; release = single;
                knee = 3.0f; rmsMs = 3.0f;
                break;
        }

        // A feedback detector listens to its own output; with an external key there is no loop to close.
        if (p.external) feedback = false;

        if (p.autoRelease && release == single && p.model != opto) release = programme;     // the cell is programme-dependent already
        valveGain = p.model == variMu;
        optoGain = p.model == opto;
        fetGain = p.model == fet;
        knee += p.extraKnee;
        usesRms = rmsMs > 0.0f;
        rmsCoef = usesRms ? msToCoef (rmsMs, sr) : 1.0f;

        // Feed-forward: gr = (1 - 1/R) * over(input).  Feedback: gr = (R - 1) * over(output),
        // which settles at the same ratio but gets there the way the hardware does.
        slope = feedback ? ratio - 1.0f : 1.0f - 1.0f / ratio;

        if (feedback)
        {
            // the loop speeds the attack up by (1 + slope); pre-scale so the knob still means what it says,
            // and keep the one-sample loop comfortably stable
            attFast = std::min (msToCoef (a * (1.0f + slope), sr), 1.0f / (1.0f + slope));
        }
        else
        {
            attFast = msToCoef (a, sr);
        }

        relFast = msToCoef (release == single ? r : r * 0.4f, sr);
        attSlow = msToCoef (a * 8.0f, sr);
        relSlow = msToCoef (r * 4.0f, sr);

        thr    = p.thresholdDb;
        makeup = dbToGain (p.makeupDb);
        mix    = p.mix;

        useSc = p.scHpf > 20.5f;
        if (useSc) scFilter.setHighPass (sr, p.scHpf, 0.707);
    }

    float process (float* l, float* r, int n, const float* keyL = nullptr, const float* keyR = nullptr) noexcept
    {
        float maxGr = 0.0f;

        for (int i = 0; i < n; ++i)
        {
            const float inL = l[i], inR = r[i];

            // ---- detector ----
            float dl = feedback ? fb[0] : inL, dr = feedback ? fb[1] : inR;
            if (keyL != nullptr) { dl = keyL[i]; dr = keyR[i]; }
            if (useSc) { dl = scFilter.process (0, dl); dr = scFilter.process (1, dr); }

            const float pk = std::max (std::abs (dl), std::abs (dr));
            float levelDb;
            if (usesRms)
            {
                ms += rmsCoef * (pk * pk - ms);
                levelDb = 10.0f * std::log10 (std::max (ms, 1.0e-12f));
            }
            else
            {
                levelDb = gainToDb (pk);
            }

            // ---- gain computer (soft knee) ----
            const float over = levelDb - thr;
            float excess;
            if (2.0f * over <= -knee)       excess = 0.0f;
            else if (2.0f * over >= knee)   excess = over;
            else                            { const float t = over + knee * 0.5f; excess = t * t / (2.0f * knee); }
            const float target = slope * excess;

            // ---- ballistics ----
            envFast += (target > envFast ? attFast : relFast) * (target - envFast);
            float gr = envFast;

            if (release == programme)
            {
                envSlow += (target > envSlow ? attSlow : relSlow) * (target - envSlow);
                gr = 0.65f * envFast + 0.35f * envSlow;
            }

            // ---- gain element ----
            float wl, wr;
            if (valveGain)
            {
                const double control = voltsPerDb * (double) gr;
                wl = valves.process (0, inL, control);
                wr = valves.process (1, inR, control);
                gr = (float) valves.reduction (control);       // what the valves actually take off, for the meter
            }
            else if (fetGain)
            {
                const double gate = FetCircuit::gateFor ((double) gr);
                wl = fetCell.process (0, inL, gate);
                wr = fetCell.process (1, inR, gate);
            }
            else if (optoGain)
            {
                cell.step (OptoCell::voltsFor ((double) gr));
                const float g = (float) cell.relativeGain();
                wl = inL * g; wr = inR * g;
                gr = (float) cell.reduction();                  // what the cell actually takes off, for the meter
            }
            else
            {
                const float g = gr > 1.0e-4f ? dbToGain (-gr) : 1.0f;
                wl = inL * g; wr = inR * g;
            }
            fb[0] = wl; fb[1] = wr;

            if (optoGain)
            {
                // the opto's make-up amplifier: a real 12AX7 stage (TubeCircuit), the valve the classic optical
                // compressors use after their cell. It runs at the compressor's rate, as the gain cells do.
                wl = makeupValve.process (0, wl);
                wr = makeupValve.process (1, wr);
            }

            l[i] = inL + mix * (wl * makeup - inL);
            r[i] = inR + mix * (wr * makeup - inR);
            maxGr = std::max (maxGr, gr);
        }
        return maxGr;
    }

    enum Release { single, programme };

    double sr = 48000.0;
    Biquad scFilter;
    TubeCircuit makeupValve;
    Release release = single;
    bool useSc = false, usesRms = false, feedback = false, valveGain = false, optoGain = false, fetGain = false;
    VariMuCircuit valves;
    OptoCell cell;
    FetCircuit fetCell;
    double voltsPerDb = 1.6;
    float thr = -18.0f, slope = 0.75f, knee = 3.0f, makeup = 1.0f, mix = 1.0f;
    float attFast = 0.01f, relFast = 0.001f, attSlow = 0.001f, relSlow = 0.0001f, rmsCoef = 1.0f;
    float ms = 0.0f, envFast = 0.0f, envSlow = 0.0f, fb[2] {};
};

//==============================================================================
/** Look-ahead brickwall limiter with true-peak detection: the gain computer looks at
    the 4x-interpolated waveform, so peaks that fall between samples are caught too.
    A second detector on the output trims the target when dense material still overshoots.
    Gain path: required gain -> sliding minimum -> release -> moving average,
    which guarantees the gain has fully arrived when the peak leaves the delay. */
struct Limiter
{
    void prepare (double sampleRate)
    {
        sr = sampleRate;
        L = std::max (4, (int) std::round (0.0015 * sr));
        cap = L + 1;
        delayLen = L - 1 + Detector::delay;     // look-ahead plus the interpolator's own delay
        guardRel = msToCoef (2000.0f, sr);
        for (auto& d : delay) d.assign ((size_t) delayLen, 0.0f);
        avg.assign ((size_t) L, 1.0f);
        dqV.assign ((size_t) cap, 1.0f);
        dqI.assign ((size_t) cap, 0);
        reset();
    }

    void reset() noexcept
    {
        for (auto& d : delay) std::fill (d.begin(), d.end(), 0.0f);
        std::fill (avg.begin(), avg.end(), 1.0f);
        truePeak.reset(); guard.reset(); margin = 1.0f;
        sum = (double) L; ai = 0; dpos = 0;
        head = tail = count = 0; counter = 0; relEnv = 1.0f;
        primed = false; isActive = false; clampHold = 0;
    }

    /** Low-latency mode drops the look-ahead: no delay, instant attack on the sample peak.
        It still never exceeds the ceiling, but it is rougher than the look-ahead limiter. */
    void setLowLatency (bool shouldBeLowLatency) noexcept   { lowLat = shouldBeLowLatency; reset(); }

    int fullLatency() const noexcept    { return delayLen; }
    int latency() const noexcept        { return lowLat ? 0 : delayLen; }

    /** Pass active = false to run as a pure latency-matched delay. */
    void set (bool active, float driveDb, float ceilingDb, float releaseMs) noexcept
    {
        drive   = active ? dbToGain (driveDb) : 1.0f;
        ceiling = active ? dbToGain (ceilingDb) : 1.0e9f;
        relCoef = msToCoef (releaseMs, sr);
        driveCoef = msToCoef (5.0f, sr);

        if (! primed) { primed = true; driveSm = drive; }
        // audio already inside the look-ahead was never seen by the detector: don't hard-clip it
        else if (active && ! isActive) clampHold = latency();
        isActive = active;
    }

    float process (float* l, float* r, int n) noexcept
    {
        float minGain = 1.0f;

        if (lowLat)
        {
            for (int i = 0; i < n; ++i)
            {
                driveSm += driveCoef * (drive - driveSm);
                const float xl = l[i] * driveSm, xr = r[i] * driveSm;
                const float pk = std::max (std::abs (xl), std::abs (xr));
                const float g  = pk > ceiling ? ceiling / pk : 1.0f;
                relEnv = g < relEnv ? g : relEnv + relCoef * (g - relEnv);
                l[i] = xl * relEnv; r[i] = xr * relEnv;
                minGain = std::min (minGain, relEnv);
            }
            return -gainToDb (minGain);
        }

        for (int i = 0; i < n; ++i)
        {
            driveSm += driveCoef * (drive - driveSm);
            const float xl = l[i] * driveSm, xr = r[i] * driveSm;
            const float pk = truePeak.process (xl, xr);
            const float aim = ceiling * margin;
            const float g  = pk > aim ? aim / pk : 1.0f;

            // sliding minimum over the last L samples (monotonic deque)
            while (count > 0 && dqV[(size_t) ((tail - 1 + cap) % cap)] >= g) { tail = (tail - 1 + cap) % cap; --count; }
            dqV[(size_t) tail] = g; dqI[(size_t) tail] = counter; tail = (tail + 1) % cap; ++count;
            if (dqI[(size_t) head] <= counter - L) { head = (head + 1) % cap; --count; }
            const float m = dqV[(size_t) head];
            ++counter;

            relEnv = m < relEnv ? m : relEnv + relCoef * (m - relEnv);

            sum += (double) relEnv - (double) avg[(size_t) ai];
            avg[(size_t) ai] = relEnv;
            if (++ai >= L)
            {
                ai = 0;
                sum = 0.0;
                for (float v : avg) sum += v;   // cancel accumulated rounding
            }
            const float a = std::min (1.0f, (float) (sum / (double) L));

            const float dl = delay[0][(size_t) dpos], dr = delay[1][(size_t) dpos];
            delay[0][(size_t) dpos] = xl; delay[1][(size_t) dpos] = xr;
            if (++dpos >= delayLen) dpos = 0;

            const float clampAt = clampHold > 0 ? (--clampHold, 1.0e9f) : ceiling;
            l[i] = std::clamp (dl * a, -clampAt, clampAt);
            r[i] = std::clamp (dr * a, -clampAt, clampAt);

            // Overshoot guard: check the true peak of what actually came out. On very dense material the
            // changing gain itself can create small overs; when one is seen, aim a little lower for a while.
            if (isActive)
            {
                const float op = guard.process (l[i], r[i]);
                margin = op > ceiling ? std::max (0.5f, margin * ceiling / op) : margin + guardRel * (1.0f - margin);
            }
            minGain = std::min (minGain, a);
        }
        return -gainToDb (minGain);
    }

    double sr = 48000.0, sum = 0.0;
    int L = 72, cap = 73, delayLen = 77, ai = 0, dpos = 0, head = 0, tail = 0, count = 0;
    using Detector = TruePeakT<32>;     // stricter than the meter, so the meter never reads over
    Detector truePeak;
    Detector guard;
    float margin = 1.0f, guardRel = 0.0001f;
    std::int64_t counter = 0;
    float drive = 1.0f, driveSm = 1.0f, driveCoef = 0.01f, ceiling = 1.0f, relCoef = 0.001f, relEnv = 1.0f;
    int clampHold = 0;
    bool lowLat = false, primed = false, isActive = false;
    std::vector<float> delay[2], avg, dqV;
    std::vector<std::int64_t> dqI;
};
} // namespace atelier
