#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <vector>

namespace atelier
{
constexpr double kPi = 3.14159265358979323846;

inline float dbToGain (float db) noexcept   { return std::pow (10.0f, db * 0.05f); }
inline float gainToDb (float g) noexcept    { return 20.0f * std::log10 (std::max (g, 1.0e-7f)); }

/** One-pole smoothing coefficient for a time constant in milliseconds. */
inline float msToCoef (float ms, double sampleRate) noexcept
{
    return 1.0f - (float) std::exp (-1.0 / (0.001 * (double) std::max (ms, 0.001f) * sampleRate));
}

//==============================================================================
/** Stereo biquad (RBJ designs), transposed direct form II with double state. */
struct Biquad
{
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
    double z1[2] {}, z2[2] {};

    void reset() noexcept   { z1[0] = z1[1] = z2[0] = z2[1] = 0.0; }

    inline float process (int ch, float x) noexcept
    {
        const double y = b0 * x + z1[ch];
        z1[ch] = b1 * x - a1 * y + z2[ch];
        z2[ch] = b2 * x - a2 * y;
        return (float) y;
    }

    void set (double nb0, double nb1, double nb2, double na0, double na1, double na2) noexcept
    {
        const double inv = 1.0 / na0;
        b0 = nb0 * inv; b1 = nb1 * inv; b2 = nb2 * inv; a1 = na1 * inv; a2 = na2 * inv;
    }

    /** Gain of the filter at angular frequency w (radians per sample). Used to draw the EQ curve. */
    double magnitude (double w) const noexcept
    {
        const double c1 = std::cos (w), s1 = std::sin (w), c2 = std::cos (2.0 * w), s2 = std::sin (2.0 * w);
        const double nr = b0 + b1 * c1 + b2 * c2, ni = b1 * s1 + b2 * s2;
        const double dr = 1.0 + a1 * c1 + a2 * c2, di = a1 * s1 + a2 * s2;
        return std::sqrt ((nr * nr + ni * ni) / std::max (1.0e-30, dr * dr + di * di));
    }

    static double clampFreq (double sr, double f) noexcept   { return std::clamp (f, 5.0, 0.45 * sr); }

    void setPeak (double sr, double f, double q, double gainDb) noexcept
    {
        const double A = std::pow (10.0, gainDb / 40.0);
        const double w = 2.0 * kPi * clampFreq (sr, f) / sr;
        const double c = std::cos (w), alpha = std::sin (w) / (2.0 * q);
        set (1.0 + alpha * A, -2.0 * c, 1.0 - alpha * A, 1.0 + alpha / A, -2.0 * c, 1.0 - alpha / A);
    }

    void setLowShelf (double sr, double f, double q, double gainDb) noexcept
    {
        const double A = std::pow (10.0, gainDb / 40.0);
        const double w = 2.0 * kPi * clampFreq (sr, f) / sr;
        const double c = std::cos (w), beta = 2.0 * std::sqrt (A) * std::sin (w) / (2.0 * q);
        set (A * ((A + 1.0) - (A - 1.0) * c + beta),
             2.0 * A * ((A - 1.0) - (A + 1.0) * c),
             A * ((A + 1.0) - (A - 1.0) * c - beta),
             (A + 1.0) + (A - 1.0) * c + beta,
             -2.0 * ((A - 1.0) + (A + 1.0) * c),
             (A + 1.0) + (A - 1.0) * c - beta);
    }

    void setHighShelf (double sr, double f, double q, double gainDb) noexcept
    {
        const double A = std::pow (10.0, gainDb / 40.0);
        const double w = 2.0 * kPi * clampFreq (sr, f) / sr;
        const double c = std::cos (w), beta = 2.0 * std::sqrt (A) * std::sin (w) / (2.0 * q);
        set (A * ((A + 1.0) + (A - 1.0) * c + beta),
             -2.0 * A * ((A - 1.0) + (A + 1.0) * c),
             A * ((A + 1.0) + (A - 1.0) * c - beta),
             (A + 1.0) - (A - 1.0) * c + beta,
             2.0 * ((A - 1.0) - (A + 1.0) * c),
             (A + 1.0) - (A - 1.0) * c - beta);
    }

    void setLowPass (double sr, double f, double q) noexcept
    {
        const double w = 2.0 * kPi * clampFreq (sr, f) / sr;
        const double c = std::cos (w), alpha = std::sin (w) / (2.0 * q);
        set ((1.0 - c) * 0.5, 1.0 - c, (1.0 - c) * 0.5, 1.0 + alpha, -2.0 * c, 1.0 - alpha);
    }

    void setHighPass (double sr, double f, double q) noexcept
    {
        const double w = 2.0 * kPi * clampFreq (sr, f) / sr;
        const double c = std::cos (w), alpha = std::sin (w) / (2.0 * q);
        set ((1.0 + c) * 0.5, -(1.0 + c), (1.0 + c) * 0.5, 1.0 + alpha, -2.0 * c, 1.0 - alpha);
    }
};

//==============================================================================
struct OnePole
{
    double a = 1.0;
    double z[2] {};

    void reset() noexcept                               { z[0] = z[1] = 0.0; }
    void setCutoff (double sr, double f) noexcept       { a = 1.0 - std::exp (-2.0 * kPi * f / sr); }
    inline float lp (int ch, float x) noexcept          { z[ch] += a * (x - z[ch]); return (float) z[ch]; }
    inline float hp (int ch, float x) noexcept          { return x - lp (ch, x); }
};

//==============================================================================
/** Fixed-length stereo delay used for latency compensation. In/out may alias. */
struct StereoDelay
{
    std::vector<float> buf[2];
    int len = 0, pos = 0;

    void prepare (int delaySamples)
    {
        len = std::max (0, delaySamples);
        for (auto& b : buf) b.assign ((size_t) std::max (1, len), 0.0f);
        pos = 0;
    }

    void reset() noexcept   { for (auto& b : buf) std::fill (b.begin(), b.end(), 0.0f); pos = 0; }

    void process (const float* inL, const float* inR, float* outL, float* outR, int n) noexcept
    {
        if (len == 0)
        {
            if (outL != inL) std::copy (inL, inL + n, outL);
            if (outR != inR) std::copy (inR, inR + n, outR);
            return;
        }

        for (int i = 0; i < n; ++i)
        {
            const float l = inL[i], r = inR[i];
            outL[i] = buf[0][(size_t) pos];
            outR[i] = buf[1][(size_t) pos];
            buf[0][(size_t) pos] = l;
            buf[1][(size_t) pos] = r;
            if (++pos >= len) pos = 0;
        }
    }
};

//==============================================================================
/** Click-free switching for one stage. Turning a stage on or off crossfades between its
    output and the (time-aligned) signal that went into it. Changing a stepped choice,
    such as a valve type, fades the stage out, swaps the choice while it is silent in the
    blend, then fades it back in. */
struct Fade
{
    float value = 0.0f, target = 0.0f, step = 0.002f;
    int active = 0;
    int hold = 0;       // samples to stay at the current blend before moving (lets a delayed stage fill up first)
    bool primed = false;

    void prepare (double rate, float ms = 10.0f)    { step = (float) (1.0 / (0.001 * (double) ms * rate)); primed = false; value = target = 0.0f; hold = 0; }

    /** Call once per chunk. Returns true if the stage has to run; `cold` is set when it is
        starting from silence and its internal state should be cleared first. */
    bool begin (bool want, int choice, bool& cold) noexcept
    {
        cold = false;
        if (! primed)                { primed = true; active = choice; value = want ? 1.0f : 0.0f; cold = want; }
        else if (choice != active)   { if (value <= 0.0f) { active = choice; cold = want; } else want = false; }
        else if (value <= 0.0f && target <= 0.0f) cold = want;

        target = want ? 1.0f : 0.0f;
        return value > 0.0f || target > 0.0f;
    }

    bool steady() const noexcept    { return value >= 1.0f && target >= 1.0f; }

    /** Blends the processed signal (in place) with the dry one. */
    void mix (float* wetL, float* wetR, const float* dryL, const float* dryR, int n) noexcept
    {
        if (steady()) return;
        float v = value;
        for (int i = 0; i < n; ++i)
        {
            if (hold > 0)   --hold;
            else            v = target > v ? std::min (target, v + step) : std::max (target, v - step);
            wetL[i] = dryL[i] + v * (wetL[i] - dryL[i]);
            wetR[i] = dryR[i] + v * (wetR[i] - dryR[i]);
        }
        value = v;
    }
};

//==============================================================================
/** Inter-sample ("true") peak estimator: 4x windowed-sinc interpolation.
    Converters reconstruct peaks that fall between samples; this finds them.
    12 taps per phase matches the meter described in ITU-R BS.1770; more taps follow the
    reconstructed waveform more closely on dense, full-band material. */
template <int Taps>
struct TruePeakT
{
    static constexpr int taps = Taps, phases = 4, delay = Taps / 2;     // delay in samples at the base rate

    TruePeakT()
    {
        // h[n] = sinc((n - centre) / 4) * Kaiser window; phase 0 is the (delayed) sample itself.
        // Kaiser (about 50 dB) keeps the response flat much closer to the top of the band than a
        // wider window would, which is where the largest inter-sample peaks come from.
        auto bessel0 = [] (double x)
        {
            double sum = 1.0, term = 1.0;
            for (int k = 1; k < 40; ++k) { term *= (x / (2.0 * k)) * (x / (2.0 * k)); sum += term; }
            return sum;
        };
        constexpr double beta = 4.55;
        const int length = phases * taps, centre = length / 2;
        for (int p = 0; p < phases; ++p)
            for (int k = 0; k < taps; ++k)
            {
                const int n = p + phases * k;
                const double x = (double) (n - centre) / (double) phases;
                const double sinc = std::abs (x) < 1.0e-9 ? 1.0 : std::sin (kPi * x) / (kPi * x);
                const double t = (double) (n - centre) / (double) centre;      // -1 .. 1
                const double win = bessel0 (beta * std::sqrt (std::max (0.0, 1.0 - t * t))) / bessel0 (beta);
                coef[p][k] = (float) (sinc * win);
            }
        reset();
    }

    void reset() noexcept   { for (auto& h : hist) h.fill (0.0f); pos = 0; }

    /** Feed one stereo frame; returns the largest inter-sample magnitude across both channels. */
    inline float process (float l, float r) noexcept
    {
        hist[0][(size_t) pos] = l; hist[1][(size_t) pos] = r;
        float peak = 0.0f;
        for (int ch = 0; ch < 2; ++ch)
            for (int p = 0; p < phases; ++p)
            {
                float acc = 0.0f;
                int idx = pos;
                for (int k = 0; k < taps; ++k)      // newest sample pairs with the first tap
                {
                    acc += coef[p][k] * hist[ch][(size_t) idx];
                    if (--idx < 0) idx = taps - 1;
                }
                peak = std::max (peak, std::abs (acc));
            }
        if (++pos >= taps) pos = 0;
        return peak;
    }

private:
    float coef[phases][taps] {};
    std::array<float, taps> hist[2] {};
    int pos = 0;
};

using TruePeak = TruePeakT<12>;

//==============================================================================
/** Word-length reduction for the final master: TPDF dither, with optional noise shaping
    that moves the dither noise up towards frequencies where the ear is least sensitive. */
struct Dither
{
    void reset() noexcept   { for (int ch = 0; ch < 2; ++ch) e1[ch] = e2[ch] = 0.0; }

    /** @param bits 16 or 24. */
    void set (int bits, bool shaped) noexcept
    {
        const double q = std::ldexp (1.0, bits - 1);
        if (q != scale || shaped != shape) reset();
        scale = q; shape = shaped;
    }

    inline float process (int ch, float x) noexcept
    {
        double w = (double) x * scale;
        if (shape) w -= h1 * e1[ch] + h2 * e2[ch];          // noise transfer: 1 - h1 z^-1 - h2 z^-2

        const double d = uniform() - uniform();             // triangular, +/- 1 LSB
        const double y = std::floor (w + d + 0.5);
        e2[ch] = e1[ch];
        e1[ch] = y - w;
        return (float) (y / scale);
    }

private:
    inline double uniform() noexcept
    {
        seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
        return (double) seed * (1.0 / 4294967296.0);
    }

    static constexpr double h1 = 1.2, h2 = -0.4;
    double scale = 32768.0, e1[2] {}, e2[2] {};
    bool shape = false;
    std::uint32_t seed = 0x9E3779B9u;
};

//==============================================================================
/** Lock-free hand-off of recent audio from the audio thread to the analyser display. */
struct ScopeFifo
{
    static constexpr int size = 16384;

    void push (const float* l, const float* r, int n) noexcept
    {
        int w = write.load (std::memory_order_relaxed);
        for (int i = 0; i < n; ++i)
        {
            buffer[(size_t) w] = 0.5f * (l[i] + r[i]);
            w = (w + 1) & (size - 1);
        }
        write.store (w, std::memory_order_release);
    }

    /** Copies the most recent n samples (oldest first). Display-only: a torn read is harmless. */
    void copyLatest (float* dest, int n) const noexcept
    {
        int rd = (write.load (std::memory_order_acquire) - n) & (size - 1);
        for (int i = 0; i < n; ++i) { dest[i] = buffer[(size_t) rd]; rd = (rd + 1) & (size - 1); }
    }

private:
    std::array<float, size> buffer {};
    std::atomic<int> write { 0 };
};

//==============================================================================
/** ITU-R BS.1770 / EBU R128 loudness: momentary (400 ms), short-term (3 s),
    gated integrated loudness and loudness range (LRA). */
struct LoudnessMeter
{
    void prepare (double sr)
    {
        // Pre-filter (high shelf) and RLB high-pass, re-derived for any sample rate.
        {
            const double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
            const double K = std::tan (kPi * f0 / sr), Vh = std::pow (10.0, G / 20.0), Vb = std::pow (Vh, 0.4996667741545416);
            const double a0 = 1.0 + K / Q + K * K;
            shelf.set (Vh + Vb * K / Q + K * K, 2.0 * (K * K - Vh), Vh - Vb * K / Q + K * K, a0, 2.0 * (K * K - 1.0), 1.0 - K / Q + K * K);
        }
        {
            const double f0 = 38.13547087602444, Q = 0.5003270373238773;
            const double K = std::tan (kPi * f0 / sr);
            hp.set (1.0, -2.0, 1.0, 1.0 + K / Q + K * K, 2.0 * (K * K - 1.0), 1.0 - K / Q + K * K);
        }
        blockLen = std::max (1, (int) std::round (sr * 0.1));
        reset();
    }

    void reset()
    {
        shelf.reset(); hp.reset();
        std::fill (std::begin (blocks), std::end (blocks), 0.0);
        acc = 0.0; count = 0; idx = 0; filled = 0; momentary = shortTerm = -100.0f;
        resetIntegrated();
    }

    /** Restarts the integrated loudness and loudness-range measurement. */
    void resetIntegrated()
    {
        gateHist.fill (0); rangeHist.fill (0);
        integrated = -100.0f; range = 0.0f;
    }

    void process (const float* l, const float* r, int n) noexcept
    {
        for (int i = 0; i < n; ++i)
        {
            const float a = hp.process (0, shelf.process (0, l[i]));
            const float b = hp.process (1, shelf.process (1, r[i]));
            acc += (double) a * a + (double) b * b;

            if (++count >= blockLen)
            {
                blocks[idx] = acc / (double) blockLen;
                idx = (idx + 1) % numBlocks;
                acc = 0.0; count = 0;

                double s = 0.0, m = 0.0;
                for (int k = 0; k < numBlocks; ++k) s += blocks[k];
                for (int k = 1; k <= 4; ++k)        m += blocks[(idx - k + numBlocks) % numBlocks];
                shortTerm = toLufs (s / numBlocks);
                momentary = toLufs (m / 4.0);
                filled = std::min (filled + 1, numBlocks);

                // histograms (0.1 LU bins from -70 LUFS) drive the gated measurements
                if (filled >= 4 && momentary > histLo)
                    ++gateHist[(size_t) std::min (histBins - 1, (int) ((momentary - histLo) * 10.0f))];
                if (filled >= numBlocks && shortTerm > histLo)
                    ++rangeHist[(size_t) std::min (histBins - 1, (int) ((shortTerm - histLo) * 10.0f))];
                updateGated();
            }
        }
    }

    static float toLufs (double meanSquare) noexcept
    {
        return meanSquare > 1.0e-10 ? (float) (-0.691 + 10.0 * std::log10 (meanSquare)) : -100.0f;
    }

    float momentary = -100.0f, shortTerm = -100.0f, integrated = -100.0f, range = 0.0f;

private:
    static constexpr int numBlocks = 30, histBins = 800;
    static constexpr float histLo = -70.0f;

    static double binEnergy (int bin) noexcept    { return std::pow (10.0, ((double) histLo + ((double) bin + 0.5) * 0.1 + 0.691) / 10.0); }

    /** Mean energy of all bins from `from` upwards; returns the block count through `n`. */
    static double meanEnergy (const std::array<std::uint32_t, histBins>& h, int from, double& n) noexcept
    {
        double sum = 0.0; n = 0.0;
        for (int b = std::max (0, from); b < histBins; ++b)
            if (h[(size_t) b] != 0) { sum += binEnergy (b) * (double) h[(size_t) b]; n += (double) h[(size_t) b]; }
        return n > 0.0 ? sum / n : 0.0;
    }

    void updateGated() noexcept
    {
        double n = 0.0;

        // integrated: absolute gate at -70 (the histogram floor), then a relative gate 10 LU below the ungated mean
        const double ungated = meanEnergy (gateHist, 0, n);
        if (n > 0.0)
        {
            const int gateBin = (int) std::floor ((toLufs (ungated) - 10.0f - histLo) * 10.0f);
            const double gated = meanEnergy (gateHist, gateBin, n);
            integrated = n > 0.0 ? toLufs (gated) : -100.0f;
        }

        // loudness range: 10th to 95th percentile of short-term values above a -20 LU relative gate
        const double stMean = meanEnergy (rangeHist, 0, n);
        if (n > 0.0)
        {
            const int gateBin = std::max (0, (int) std::floor ((toLufs (stMean) - 20.0f - histLo) * 10.0f));
            double total = 0.0;
            for (int b = gateBin; b < histBins; ++b) total += (double) rangeHist[(size_t) b];
            if (total > 0.0)
            {
                double run = 0.0; int lo = gateBin, hi = gateBin; bool gotLo = false;
                for (int b = gateBin; b < histBins; ++b)
                {
                    run += (double) rangeHist[(size_t) b];
                    if (! gotLo && run >= 0.10 * total) { lo = b; gotLo = true; }
                    if (run >= 0.95 * total) { hi = b; break; }
                }
                range = (float) (hi - lo) * 0.1f;
            }
        }
    }

    std::array<std::uint32_t, histBins> gateHist {}, rangeHist {};
    int filled = 0;
    Biquad shelf, hp;
    double blocks[numBlocks] {};
    double acc = 0.0;
    int count = 0, idx = 0, blockLen = 4800;
};
} // namespace atelier
